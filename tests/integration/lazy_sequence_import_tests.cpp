// Issue #473: importing an image sequence lists it and reads one member; every other member is
// read and digested when it is first needed. These tests prove that import reads one file however
// many members there are, that a member is digested on its first read (and recorded in the ADR 0025
// memo), that a member changed, touched or removed after import is reported and never served from
// old bytes, that a member whose bytes are not an image becomes a gap at its first read (ADR 0026
// / #373), and that a project saved with some members stamped and some digested reopens to the
// same asset, whose stamped members still read.
#include "generated_jpeg.hpp"
#include "image_source.hpp"
#include <bloom/commands/operations.hpp>
#include <bloom/core/sha256.hpp>
#include <bloom/document/color_settings.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/platform/source_identity.hpp>
#include <bloom/project/canonical_document.hpp>
#include <bloom/project/document_decode.hpp>
#include <bloom/project/strict_json_dom.hpp>
#include <bloom/runtime/operation_cache.hpp>

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <regex>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
namespace doc = bloom::document;
namespace runtime = bloom::runtime;
namespace commands = bloom::commands;
namespace platform = bloom::platform;
namespace project = bloom::project;
using bloom::core::RationalTime;

int failures = 0;
void expect(const bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

struct Folder {
    std::filesystem::path path;
    explicit Folder(const std::string& name) : path(std::filesystem::temp_directory_path() / name) {
        std::error_code error;
        std::filesystem::remove_all(path, error);
        std::filesystem::create_directories(path);
    }
    ~Folder() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
    Folder(const Folder&) = delete;
    Folder& operator=(const Folder&) = delete;
};

void writeBytes(const std::filesystem::path& path, const std::string_view bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}
void writeJpeg(const std::filesystem::path& path, const std::string_view trailer = {}) {
    std::string bytes(reinterpret_cast<const char*>(kGeneratedJpeg.data()), kGeneratedJpeg.size());
    bytes += trailer;
    writeBytes(path, bytes);
}
std::string frameName(const std::string& prefix, const int frame) {
    auto digits = std::to_string(frame);
    digits.insert(0, 4 - digits.size(), '0');
    return prefix + "." + digits + ".jpg";
}
bloom::core::Sha256Digest fileDigest(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(file)), {});
    return *bloom::core::Sha256Hasher::hash(std::as_bytes(std::span(bytes)));
}

doc::AssetRecord importSequence(const std::filesystem::path& picked,
                                const std::filesystem::path& base) {
    auto seed = doc::makeNewProject("Lazy", "Main", *RationalTime::create(1, 1));
    doc::Document document(std::move(seed.project));
    auto snapshot = document.snapshot();
    auto draft = document.draft(snapshot);
    commands::ImportAssets import({picked}, base);
    if (import.apply(draft).status != commands::OperationStatus::Applied)
        throw std::runtime_error("import: " + import.diagnostic());
    return draft.project().assets().front();
}

runtime::CompiledImageSource sourceOf(const doc::AssetRecord& asset) {
    runtime::CompiledImageSource source;
    source.asset = asset;
    return source;
}
// The member the composition frame `offset` selects (frame first + offset at 24 fps).
runtime::detail::ImageSourceSelection select(const runtime::CompiledImageSource& source,
                                             const std::filesystem::path& base,
                                             const std::int64_t offset,
                                             runtime::OperationCache* cache = nullptr) {
    return runtime::detail::selectImageSource(
        source, *RationalTime::create(offset, 24), *doc::FrameRate::create(24, 1), base, {},
        runtime::EvaluationColorIntent::LinearRec709Scene, cache);
}

// Import reads the picked member only: one full read of one file, for 3 members as for 48.
void importReadsOneMember() {
    for (const int members : {3, 48}) {
        Folder folder("bloom-lazy-sequence-count-" + std::to_string(members));
        for (int frame = 1; frame <= members; ++frame)
            writeJpeg(folder.path / frameName("count", frame));
        const auto before = platform::sourceReadWork();
        const auto asset = importSequence(folder.path / frameName("count", 1), folder.path);
        const auto after = platform::sourceReadWork();
        const auto label = std::to_string(members) + " members: ";
        expect(asset.kind == doc::AssetKind::Sequence &&
                   asset.manifest.members.size() == static_cast<std::size_t>(members),
               label + "the sequence imports with every member");
        expect(after.fullReads - before.fullReads == 1 &&
                   after.fullReadBytes - before.fullReadBytes == kGeneratedJpeg.size(),
               label + "import reads exactly one member's bytes");
        expect(asset.manifest.members.front().contentDigest &&
                   !asset.manifest.members.front().stamp,
               label + "the probed member carries its digest");
        bool stamped = true;
        for (std::size_t index = 1; index < asset.manifest.members.size(); ++index) {
            const auto& member = asset.manifest.members[index];
            const auto observed =
                bloom::media::observeImageSourceStamp(folder.path / member.locator.path);
            stamped = stamped && !member.contentDigest && member.stamp && observed &&
                      observed->size == member.stamp->size &&
                      observed->modifiedNanoseconds == member.stamp->modifiedNanoseconds;
        }
        expect(stamped, label + "every other member carries the stamp its file has");
        expect(asset.validate().ok() && asset.width > 0 && asset.height > 0,
               label + "the asset is valid and has the probed size");
    }
}

// A stamped member is read on first use, off any UI thread, and admitted by its stamp without a
// hash (ADR 0025, amended 2026-11-04: its digest is computed in the background); a revalidated
// reuse then reads nothing.
void firstReadDigests() {
    Folder folder("bloom-lazy-sequence-first-read");
    for (int frame = 1; frame <= 4; ++frame)
        writeJpeg(folder.path / frameName("first", frame),
                  std::string(1, static_cast<char>(frame)));
    const auto asset = importSequence(folder.path / frameName("first", 1), folder.path);
    const auto source = sourceOf(asset);
    const auto second = folder.path / frameName("first", 2);
    expect(!platform::observeSource(second).digest, "an unread member has no recorded digest");
    runtime::OperationCache cache;
    const auto before = platform::sourceReadWork();
    const auto selected = select(source, folder.path, 1, &cache);
    const auto afterFirst = platform::sourceReadWork();
    expect(selected.available && selected.path.filename() == second.filename() &&
               selected.expectedStamp.has_value(),
           "the stamped member is available on its first read");
    expect(afterFirst.fullReads - before.fullReads == 1, "its first read reads it once");
    expect(!platform::observeSource(second).digest && !selected.sourceRevision->contentVerified() &&
               selected.sourceRevision->captureStatistics().bytesHashed == 0,
           "the first read admits the member by its stamp without hashing it");
    expect(selected.digest == doc::sequenceMemberIdentity(asset.manifest.members[1]),
           "its cache identity is the provisional stamp identity");
    const auto again = select(source, folder.path, 1, &cache);
    expect(again.available && again.retainedRevision &&
               platform::sourceReadWork().fullReads == afterFirst.fullReads,
           "a revalidated reuse reads nothing");
    // The background pass hashes it later; the memo then vouches for the stamped bytes.
    const auto background = platform::hashSourceFile(second);
    expect(background.error == platform::SourceHashError::None &&
               background.digest == fileDigest(second) &&
               platform::observeSource(second).digest == fileDigest(second),
           "a background hash records the member's content digest");
    expect(select(source, folder.path, 2).digest != selected.digest,
           "two members of one size never share an identity");
}

// A member changed, touched or removed after import is reported by frame and never served stale,
// including from a revision captured before the change.
void changedFramesAreReported() {
    Folder folder("bloom-lazy-sequence-changed");
    for (int frame = 1; frame <= 5; ++frame)
        writeJpeg(folder.path / frameName("changed", frame));
    const auto asset = importSequence(folder.path / frameName("changed", 1), folder.path);
    const auto source = sourceOf(asset);
    runtime::OperationCache cache;
    expect(select(source, folder.path, 1, &cache).available, "frame 2 reads before the change");

    writeJpeg(folder.path / frameName("changed", 2), "rewritten");
    const auto rewritten = select(source, folder.path, 1, &cache);
    expect(!rewritten.available && !rewritten.retainedRevision &&
               rewritten.warning.find("Frame 2 changed since it was imported") != std::string::npos,
           "a rewritten frame is reported, not served from the retained revision");

    const auto touched = folder.path / frameName("changed", 3);
    std::filesystem::last_write_time(touched, std::filesystem::last_write_time(touched) +
                                                  std::chrono::seconds(10));
    const auto touchedSelection = select(source, folder.path, 2);
    expect(!touchedSelection.available &&
               touchedSelection.warning.find("Frame 3 changed") != std::string::npos,
           "a frame whose time changed is reported");

    std::filesystem::remove(folder.path / frameName("changed", 4));
    const auto removed = select(source, folder.path, 3);
    expect(!removed.available && removed.warning.find("Frame 4 is missing") != std::string::npos,
           "a removed frame is reported");
    expect(select(source, folder.path, 4).available, "an unchanged frame still reads");
}

// ADR 0026 / #373: a member whose bytes are not an image is found at its first read and treated as
// a gap: it holds the preceding frame, or shows nothing when the asset shows nothing for gaps.
void unreadableFrameIsAGap() {
    Folder folder("bloom-lazy-sequence-unreadable");
    writeJpeg(folder.path / frameName("bad", 1));
    writeJpeg(folder.path / frameName("bad", 2));
    writeBytes(folder.path / frameName("bad", 3), "not an image");
    writeJpeg(folder.path / frameName("bad", 4));
    auto asset = importSequence(folder.path / frameName("bad", 1), folder.path);
    expect(asset.manifest.members.size() == 4, "import does not read the unreadable member");
    const auto held = select(sourceOf(asset), folder.path, 2);
    expect(held.available && !held.missingFrame && held.path.filename() == frameName("bad", 2) &&
               held.warning.find("Frame 3 is unreadable and is treated as a gap") !=
                   std::string::npos,
           "the unreadable frame holds the preceding frame and says why");
    asset.interpretation.missingFrames = doc::AssetMissingFrames::Nothing;
    const auto nothing = select(sourceOf(asset), folder.path, 2);
    expect(nothing.available && nothing.missingFrame &&
               nothing.warning.find("Frame 3 is unreadable") != std::string::npos,
           "the unreadable frame shows nothing when gaps show nothing");
}

project::ProjectIoOperationMemory memory() {
    auto coordinator = project::ProjectIoMemoryCoordinator::create();
    if (!coordinator)
        throw std::runtime_error("memory coordinator");
    auto operation = coordinator->createOperation();
    if (!operation)
        throw std::runtime_error("memory operation");
    return std::move(*operation);
}
std::string encode(const doc::Project& value) {
    const doc::Document document{doc::Project(value)};
    const auto snapshot = document.snapshot();
    const auto settings = doc::makeBloomNeutralColorSettingsV1({});
    std::array<char, 64> payloadScratch{};
    std::array<std::size_t, 64> sortScratch{};
    const project::CanonicalDocumentV1 input{&snapshot, &settings, payloadScratch, sortScratch};
    const auto size = project::canonicalDocumentSize(input);
    if (!size)
        throw std::runtime_error("canonical size");
    std::string text(*size.value(), '\0');
    if (!project::encodeCanonicalDocument(input, text))
        throw std::runtime_error("canonical encode");
    return text;
}
std::optional<std::vector<doc::AssetRecord>> decode(const std::string& text) {
    auto operation = memory();
    auto parsed = project::parseStrictJsonDom(std::as_bytes(std::span(text)), {}, operation);
    if (!parsed)
        return std::nullopt;
    auto decoded = project::decodeDocumentEnvelope(parsed.document()->root());
    if (!decoded)
        return std::nullopt;
    return decoded.value()->assets;
}
// A project saved while only some members have digests reopens to the same asset, and its stamped
// members read as before.
void partialDigestsReopen() {
    Folder folder("bloom-lazy-sequence-reopen");
    for (int frame = 1; frame <= 3; ++frame)
        writeJpeg(folder.path / frameName("reopen", frame));
    auto seed = doc::makeNewProject("Lazy", "Main", *RationalTime::create(1, 1));
    doc::Document document(std::move(seed.project));
    auto snapshot = document.snapshot();
    auto draft = document.draft(snapshot);
    commands::ImportAssets import({folder.path / frameName("reopen", 2)}, folder.path);
    if (import.apply(draft).status != commands::OperationStatus::Applied)
        throw std::runtime_error("import");
    const auto asset = draft.project().assets().front();
    expect(!asset.manifest.members[0].contentDigest && asset.manifest.members[1].contentDigest &&
               !asset.manifest.members[2].contentDigest,
           "the picked middle member is the one digested");
    const auto text = encode(draft.project());
    expect(text.find("\"stamp\": {") != std::string::npos &&
               text.find("\"modified\": ") != std::string::npos &&
               text.find("\"contentDigest\"") != std::string::npos,
           "the document writes the stamps beside the digest");
    const auto reopened = decode(text);
    expect(reopened && reopened->size() == 1 && reopened->front() == asset,
           "the asset reopens exactly, stamps and digest included");
    if (reopened && !reopened->empty()) {
        const auto source = sourceOf(reopened->front());
        expect(select(source, folder.path, 0).available && select(source, folder.path, 2).available,
               "the reopened stamped members read");
        expect(encode(draft.project()) == text, "the canonical text is stable");
    }
}

// #495: a member may carry its stamp and the digest a full hash verified for the bytes that carried
// it. The three forms round-trip, and recording a digest beside a stamp keeps the member's
// identity.
void verifiedDigestsRoundTrip() {
    Folder folder("bloom-lazy-sequence-verified");
    for (int frame = 1; frame <= 3; ++frame)
        writeJpeg(folder.path / frameName("verified", frame));
    auto seed = doc::makeNewProject("Lazy", "Main", *RationalTime::create(1, 1));
    doc::Document document(std::move(seed.project));
    auto snapshot = document.snapshot();
    auto draft = document.draft(snapshot);
    commands::ImportAssets import({folder.path / frameName("verified", 2)}, folder.path);
    if (import.apply(draft).status != commands::OperationStatus::Applied)
        throw std::runtime_error("import");
    auto asset = draft.project().assets().front();
    auto& pending = asset.manifest.members[0];
    const auto pendingIdentity = doc::sequenceMemberIdentity(pending);
    // Frame 1 becomes verified, frame 2 stays legacy (digest only), frame 3 stays pending.
    pending.contentDigest = fileDigest(folder.path / frameName("verified", 1));
    expect(asset.validate().ok(), "a member carrying both a stamp and a digest validates");
    expect(doc::sequenceMemberIdentity(pending) == pendingIdentity,
           "recording a verified digest keeps the stamp-derived identity");
    expect(doc::sequenceMemberIdentity(asset.manifest.members[1]) ==
               *asset.manifest.members[1].contentDigest,
           "a digest-only member's identity stays its digest");
    auto neither = asset;
    neither.manifest.members[2].stamp.reset();
    expect(!neither.validate().ok(), "a member with neither a digest nor a stamp is refused");

    auto verified = draft.project();
    *verified.findAsset(asset.id) = asset;
    const auto text = encode(verified);
    const auto both = text.find("\"contentDigest\"");
    expect(both != std::string::npos && text.find("\"stamp\": {", both) != std::string::npos,
           "the verified member writes its digest before its stamp");
    const auto reopened = decode(text);
    expect(reopened && reopened->size() == 1 && reopened->front() == asset,
           "verified, legacy and pending members reopen exactly");
    expect(encode(verified) == text, "the canonical text is stable");
}
} // namespace

int main() {
    try {
        importReadsOneMember();
        firstReadDigests();
        changedFramesAreReported();
        unreadableFrameIsAGap();
        partialDigestsReopen();
        verifiedDigestsRoundTrip();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    return failures == 0 ? 0 : 1;
}
