// Document 1.34 (issue #381): every Interpret Footage member and a stream's coded pixel aspect
// ratio round-trip, a document claiming 1.33 cannot carry them, and 1.33 migrates by version alone.
// The Import members (Skip, a sequence's imported frame range, a still's duration) do too.
#include <bloom/core/rational_time.hpp>
#include <bloom/document/asset.hpp>
#include <bloom/document/color_settings.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/project/canonical_document.hpp>
#include <bloom/project/document_decode.hpp>
#include <bloom/project/document_migration.hpp>
#include <bloom/project/strict_json_dom.hpp>

#include <array>
#include <iostream>
#include <regex>
#include <span>
#include <stdexcept>
#include <string>

namespace {

// Document 1.29 (ADR 0033) members a downgraded fixture must not carry.
[[maybe_unused]] void stripExportQueueV1_29(std::string& text) {
    text = std::regex_replace(
        text, std::regex(R"(,\s*"exportQueue"\s*:\s*\{\s*"jobs"\s*:\s*\[\]\s*\})"), "");
    text = std::regex_replace(
        text, std::regex(R"(,\s*"exportJob"\s*:\s*"0",\s*"exportOutput"\s*:\s*"0")"), "");
}
using namespace bloom;
using namespace bloom::project;
using namespace bloom::document;
int failures = 0;
void expect(const bool condition, const char* message) {
    if (!condition) {
        ++failures;
        std::cerr << message << '\n';
    }
}
ProjectIoOperationMemory memory() {
    auto coordinator = ProjectIoMemoryCoordinator::create();
    if (!coordinator)
        throw std::runtime_error("memory coordinator");
    auto operation = coordinator->createOperation();
    if (!operation)
        throw std::runtime_error("memory operation");
    return std::move(*operation);
}
AssetVideoStream stream(std::uint32_t id, std::uint32_t kind, std::string timecode,
                        std::uint32_t fieldOrder) {
    AssetVideoStream s;
    s.id = id;
    s.kind = kind;
    s.codec = kind == 1 ? "h264" : "aac";
    s.timecode = std::move(timecode);
    s.timebase = *core::RationalTime::create(1, 24);
    s.framePeriod = *core::RationalTime::create(1, 24);
    s.duration = *core::RationalTime::create(2, 1);
    if (kind == 1) {
        s.width = 64;
        s.height = 48;
        s.primaries = s.transfer = s.matrix = s.range = 1;
        s.fieldOrder = fieldOrder;
    } else {
        s.sampleRate = 48000;
        s.channelLayout = {"FL", "FR"};
    }
    s.timecodeType = timecodeTypeFor(s.timecode);
    return s;
}
Document makeDocument(std::string timecode, std::uint32_t fieldOrder) {
    auto seed = makeNewProject("Interpret Footage", "Main", core::RationalTime::fromInteger(2));
    AssetRecord asset;
    asset.id = AssetId::fromRaw(1);
    asset.kind = AssetKind::Video;
    asset.locator = {"file", "project-relative", "clip.mov", "file:///clip.mov"};
    asset.width = 64;
    asset.height = 48;
    asset.frames = 48;
    asset.duration = *core::RationalTime::create(2, 1);
    asset.name = "clip";
    asset.videoStreams = {stream(0, 1, std::move(timecode), fieldOrder), stream(1, 2, {}, 0),
                          stream(2, 2, {}, 0)};
    asset.rate = 48000;
    asset.channels = 2;
    if (!seed.project.addAsset(std::move(asset)))
        throw std::logic_error("video asset");
    return Document{std::move(seed.project)};
}
std::string encode(const Document& document) {
    const auto snapshot = document.snapshot();
    const auto settings = makeBloomNeutralColorSettingsV1({});
    std::array<char, 64> payloadScratch{};
    std::array<std::size_t, 64> sortScratch{};
    const CanonicalDocumentV1 input{&snapshot, &settings, payloadScratch, sortScratch};
    const auto size = canonicalDocumentSize(input);
    if (!size)
        throw std::runtime_error("canonical size");
    std::string text(*size.value(), '\0');
    if (!encodeCanonicalDocument(input, text))
        throw std::runtime_error("canonical encode");
    return text;
}
std::string replaceRegex(const std::string& text, const char* pattern, const char* replacement) {
    const auto result = std::regex_replace(text, std::regex(pattern), replacement);
    if (result == text)
        throw std::logic_error(std::string("fixture anchor missing: ") + pattern);
    return result;
}
std::vector<AssetRecord> decodeText(const std::string& text) {
    auto operation = memory();
    auto parsed = parseStrictJsonDom(std::as_bytes(std::span(text)), {}, operation);
    if (!parsed)
        throw std::runtime_error("parse");
    auto decoded = decodeDocumentEnvelope(parsed.document()->root());
    if (!decoded)
        throw std::runtime_error("decode");
    return decoded.value()->assets;
}
bool decodes(const std::string& text) {
    auto operation = memory();
    auto parsed = parseStrictJsonDom(std::as_bytes(std::span(text)), {}, operation);
    return parsed && static_cast<bool>(decodeDocumentEnvelope(parsed.document()->root()));
}
Document interpreted(AssetInterpretation interpretation) {
    auto seed = makeNewProject("Interpret Footage", "Main", core::RationalTime::fromInteger(2));
    auto document = makeDocument("01:00:00:00", 2);
    auto project = document.snapshot().project();
    auto asset = *project.findAsset(AssetId::fromRaw(1));
    asset.interpretation = std::move(interpretation);
    asset.videoStreams.front().pixelAspect = *core::PixelAspectRatio::create(4, 3);
    if (!asset.validate().ok())
        throw std::logic_error("interpreted asset");
    (void)seed;
    *project.findAsset(asset.id) = asset;
    return Document{std::move(project)};
}
void roundTrip() {
    AssetInterpretation value;
    value.frameRate = *FrameRate::create(24000, 1001);
    value.startTimecode = CompositionTimecode{*core::RationalTime::create(3600, 1), true};
    value.alphaMode = AssetAlphaMode::Premultiplied;
    value.invertAlpha = true;
    value.matteColor = {1.0, 0.5, 0.25};
    value.fieldOrder = AssetFieldOrder::LowerFirst;
    value.deinterlace = true;
    value.pixelAspect = *core::PixelAspectRatio::create(10, 11);
    value.loopCount = 3;
    const auto document = interpreted(value);
    const auto text = encode(document);
    for (const auto* token : {"\"frameRate\"", "\"startTimecode\"", "\"premultiplied\"",
                              "\"invertAlpha\": true", "\"matteColor\"", "\"lowerFirst\"",
                              "\"deinterlace\": true", "\"loopCount\": 3", "\"pixelAspect\""})
        expect(text.find(token) != std::string::npos, token);
    const auto decoded = decodeText(text);
    expect(decoded.front() == *document.snapshot().project().findAsset(AssetId::fromRaw(1)),
           "every 1.34 member and the stream pixel aspect round-trip exactly");
    expect(encode(document) == text, "the canonical text is stable");

    // Defaults write nothing new: the 1.33 interpretation shape.
    const auto plain = encode(makeDocument("01:00:00:00", 2));
    expect(plain.find("\"alphaMode\"") == std::string::npos &&
               plain.find("\"loopCount\"") == std::string::npos &&
               plain.find("\"missingFrames\"") == std::string::npos,
           "default members are absent");
    expect(!decodes(replaceRegex(text, R"("minor": 38(\s*\}))", "\"minor\": 33$1")),
           "a document claiming 1.33 cannot carry the 1.34 members");
    expect(!decodes(replaceRegex(text, R"("loopCount": 3)", "\"loopCount\": 1")),
           "a default value written out is refused");
    expect(!decodes(replaceRegex(text, R"("lowerFirst")", "\"sideways\"")),
           "an unknown token is refused");
}
void sequenceMembers() {
    // A sequence asset with an assumed rate and missing frames that show nothing.
    auto seed = makeNewProject("Interpret Footage", "Main", core::RationalTime::fromInteger(2));
    AssetRecord asset;
    asset.id = AssetId::fromRaw(1);
    asset.kind = AssetKind::Sequence;
    asset.locator = {"file", "project-relative", "shot.[0001-0003].png", "file:///shot"};
    asset.width = 4;
    asset.height = 4;
    asset.name = "shot";
    asset.manifest.pattern = "shot.####.png";
    asset.manifest.padding = 4;
    asset.manifest.first = 1;
    asset.manifest.last = 3;
    for (const auto frame : {1, 3})
        asset.manifest.members.push_back(
            {frame,
             {"file", "project-relative", "shot." + std::to_string(frame) + ".png", "file:///s"},
             bloom::core::Sha256Digest{},
             std::nullopt});
    asset.manifest.gaps = {2};
    asset.interpretation.frameRate = *FrameRate::create(25, 1);
    asset.interpretation.missingFrames = AssetMissingFrames::Nothing;
    if (!seed.project.addAsset(asset))
        throw std::logic_error("sequence asset");
    const Document document{std::move(seed.project)};
    const auto text = encode(document);
    expect(text.find("\"missingFrames\": \"nothing\"") != std::string::npos,
           "missing frames is written");
    expect(decodeText(text).front().interpretation == asset.interpretation,
           "a sequence interpretation round-trips");
    auto still = asset;
    still.kind = AssetKind::Image;
    expect(!still.validate().ok(), "a still cannot carry a frame rate or missing frames");
}
// The Import options and preferences (#381): Skip, an imported frame range and a still's
// duration round-trip, only where they belong, and Skip plays the frames a sequence has.
void importMembers() {
    auto seed = makeNewProject("Import", "Main", core::RationalTime::fromInteger(2));
    AssetRecord asset;
    asset.id = AssetId::fromRaw(1);
    asset.kind = AssetKind::Sequence;
    asset.locator = {"file", "project-relative", "shot.0002.png", "file:///s"};
    asset.width = 4;
    asset.height = 4;
    asset.name = "shot";
    asset.manifest.pattern = "shot.####.png";
    asset.manifest.padding = 4;
    asset.manifest.first = 2;
    asset.manifest.last = 5;
    for (const auto frame : {2, 3, 5})
        asset.manifest.members.push_back(
            {frame,
             {"file", "project-relative", "shot." + std::to_string(frame) + ".png", "file:///s"},
             bloom::core::Sha256Digest{},
             std::nullopt});
    asset.manifest.gaps = {4};
    asset.manifest.range = AssetSequenceRange{2, 9};
    asset.interpretation.missingFrames = AssetMissingFrames::Skip;
    expect(asset.validate().ok(), "a skipping sequence with an imported range is valid");
    expect(footageFrameCount(asset) == 3, "a skipping sequence is as long as its frames");
    const auto third = footageSequenceFrame(asset, 2);
    expect(third.member != nullptr && third.member->frame == 5,
           "Skip plays the frames it has back to back");
    auto holding = asset;
    holding.interpretation.missingFrames = AssetMissingFrames::HoldPrevious;
    expect(footageFrameCount(holding) == 4 && footageSequenceFrame(holding, 2).member->frame == 3,
           "Hold keeps the numbered timing and holds the frame before a gap");
    holding.interpretation.missingFrames = AssetMissingFrames::Nothing;
    expect(footageSequenceFrame(holding, 2).member == nullptr &&
               footageSequenceFrame(holding, 2).frame == 4,
           "Show nothing selects no member for a gap");
    auto outside = asset;
    outside.manifest.range = AssetSequenceRange{3, 9};
    expect(!outside.validate().ok(), "the frames must lie within the imported range");

    AssetRecord still;
    still.id = AssetId::fromRaw(2);
    still.kind = AssetKind::Image;
    still.locator = {"file", "project-relative", "logo.png", "file:///logo.png"};
    still.width = 4;
    still.height = 4;
    still.name = "logo";
    still.interpretation.stillDuration = core::RationalTime::fromInteger(5);
    expect(still.validate().ok() && footageDuration(still, FrameRate::framesPerSecond24()) ==
                                        std::optional{core::RationalTime::fromInteger(5)},
           "a still's duration is its length");
    auto timed = asset;
    timed.interpretation.stillDuration = still.interpretation.stillDuration;
    expect(!timed.validate().ok(), "only a still has a still duration");
    auto ranged = still;
    ranged.manifest.range = AssetSequenceRange{1, 2};
    expect(!ranged.validate().ok(), "only a sequence has an imported frame range");
    auto forever = still;
    forever.interpretation.stillDuration = core::RationalTime::fromInteger(86401);
    expect(!forever.validate().ok(), "a still lasts at most 24 hours");

    if (!seed.project.addAsset(asset) || !seed.project.addAsset(still))
        throw std::logic_error("import assets");
    const Document document{std::move(seed.project)};
    const auto text = encode(document);
    for (const auto* token : {"\"missingFrames\": \"skip\"", "\"stillDuration\"", "\"range\""})
        expect(text.find(token) != std::string::npos, token);
    const auto decoded = decodeText(text);
    expect(decoded.size() == 2 && decoded[0] == asset && decoded[1] == still,
           "Skip, the frame range and the still duration round-trip exactly");
    // A 1.33 reader keeps unknown trailing members aside and reads the 1.33 meaning.
    const auto older = decodeText(replaceRegex(text, R"("minor": 38(\s*\}))", "\"minor\": 33$1"));
    expect(older.size() == 2 &&
               older[0].interpretation.missingFrames == AssetMissingFrames::HoldPrevious &&
               !older[0].manifest.range && !older[1].interpretation.stillDuration,
           "a document claiming 1.33 does not read them");
    expect(!decodes(replaceRegex(text, R"("skip")", "\"sometimes\"")),
           "an unknown missing-frames token is refused");
}
void migration() {
    const auto current = encode(makeDocument("01:00:00:00", 2));
    const auto older = replaceRegex(current, R"("minor": 38(\s*\}))", "\"minor\": 33$1");
    auto operation = memory();
    auto parsed = parseStrictJsonDom(std::as_bytes(std::span(older)), {}, operation);
    expect(static_cast<bool>(parsed), "1.33 document parses");
    if (!parsed)
        return;
    const auto migrated = migrateDocumentDom(parsed.document()->root(), {1, 33}, {1, 34},
                                             kProductionDocumentMigrationSteps, {}, operation);
    expect(migrated.outcome() == MigrationOutcome::Migrated && migrated.stepsApplied() == 1 &&
               migrated.migratedRoot() != nullptr,
           "1.33 -> 1.34 is one step");
    if (migrated.migratedRoot() == nullptr)
        return;
    const auto decoded = decodeDocumentEnvelope(*migrated.migratedRoot());
    expect(static_cast<bool>(decoded) &&
               decoded.value()->assets.front().interpretation == AssetInterpretation{},
           "a migrated asset has the default interpretation (the 1.33 playback)");
}
} // namespace

int main() {
    try {
        roundTrip();
        sequenceMembers();
        importMembers();
        migration();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return failures == 0 ? 0 : 1;
}
