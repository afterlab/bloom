// #752: a cue list on the wire (docs/architecture/project-format.md, "Cue Lists"). A Subtitles node
// whose cues are a linked file's list (styles, a speaker, a placement, NTSC-rate times) is saved,
// reopened and re-encoded byte for byte; edits of the written text show what the decoder refuses,
// and a newer minor's member on the list itself is retained while one inside a cue is refused.

#include "zip_container_test_support.hpp"

#include <bloom/document/color_settings.hpp>
#include <bloom/document/cue_list.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/node_definition_registry.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/document/subtitles_nodes.hpp>
#include <bloom/project/canonical_document.hpp>
#include <bloom/project/canonical_manifest.hpp>
#include <bloom/project/document_decode.hpp>
#include <bloom/project/open_archive.hpp>
#include <bloom/project/save_archive.hpp>
#include <bloom/project/strict_json_dom.hpp>
#include <bloom/project/zip_container.hpp>

#include <array>
#include <cstdint>
#include <iostream>
#include <memory>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {
using namespace bloom;
using namespace bloom::project;
int failures = 0;
void expect(const bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << message << '\n';
    }
}

ProjectIoOperationMemory memory(const std::uint64_t budget = 64U << 20U) {
    auto coordinator = ProjectIoMemoryCoordinator::create(budget);
    if (!coordinator)
        throw std::runtime_error("memory coordinator");
    auto operation = coordinator->createOperation(budget, budget);
    if (!operation)
        throw std::runtime_error("memory operation");
    return std::move(*operation);
}

document::ColorSettings neutralColorSettings() {
    std::array<std::uint8_t, 32> bytes{};
    std::iota(bytes.begin(), bytes.end(), std::uint8_t{0});
    return document::makeBloomNeutralColorSettingsV1(core::Sha256Digest::fromBytes(bytes));
}

core::RationalTime frames(const std::int64_t count) {
    // NTSC: 1001/30000 s per frame.
    return *core::RationalTime::create(count * 1001, 30000);
}

document::CueListValue authoredList() {
    document::CueListValue list;
    list.language = "en-US";
    document::CueFileProvenance file;
    file.portability = "project-relative";
    file.path = "subtitles/episode 1.vtt";
    file.format = "vtt";
    file.encoding = "utf-8";
    file.digest = "sha256:" + std::string(64, 'a');
    file.size = 1234;
    file.modifiedNanoseconds = -5; // a stamp is opaque: any value round-trips
    list.provenance = file;
    document::CueStyle loud;
    loud.name = "loud";
    loud.color = core::Color4d{1.0, 1.0, 0.0, 1.0};
    loud.outlineColor = core::Color4d{0.0, 0.0, 0.0, 0.5};
    loud.outlineWidth = 0.003;
    loud.bold = true;
    loud.fontFamily = "Noto Sans";
    loud.fontSize = 0.055;
    document::CueStyle quiet;
    quiet.name = "quiet voice";
    quiet.italic = false;
    quiet.backgroundColor = core::Color4d{0.1, 0.1, 0.1, 0.75};
    list.styles = {loud, quiet};
    const auto add = [&](const std::uint64_t id, const std::int64_t start, const std::int64_t end,
                         std::string text) {
        auto cue = std::make_shared<document::Cue>();
        cue->id = id;
        cue->start = frames(start);
        cue->end = frames(end);
        cue->text = std::move(text);
        list.cues.push_back(cue);
        return cue;
    };
    add(4, 0, 48, "Hello <i>there</i>");
    auto second = add(2, 30, 90, "<v Anna>Two &amp; <00:00:02.500>three</v>\nlines");
    second->speaker = "Anna";
    second->style = "loud";
    second->placement.anchor = document::CueAnchor::Top;
    second->placement.positionX = 0.25;
    second->placement.lineOffset = -2;
    second->placement.textAlign = document::CueTextAlign::Left;
    auto third = add(7, 120, 121, "<c.yellow>built-in class</c>");
    third->style = "quiet voice";
    third->placement.width = 0.5;
    list.nextCueId = 9;
    document::canonicalizeCueList(list);
    if (!list.isValid())
        throw std::runtime_error("authored list: " + list.problem().value_or(""));
    return list;
}

struct Authored final {
    std::unique_ptr<document::Document> document;
    document::CompositionId compositionId;
    document::ParameterId cuesParameter;
};

Authored authoredProject() {
    auto initial = document::makeNewProject("Untitled Project", "Main Composition",
                                            core::RationalTime::fromInteger(10));
    auto owned = std::make_unique<document::Document>(std::move(initial.project));
    const auto snapshot = owned->snapshot();
    auto draft = owned->draft(snapshot);
    auto& composition = *draft.project().findComposition(initial.initialCompositionId);
    const auto* definition =
        document::builtInNodeDefinitions().find(document::kSubtitlesNodeType, 1);
    const auto nodeId = draft.ids().allocateNode();
    if (definition == nullptr || !nodeId)
        throw std::runtime_error("Subtitles definition");
    std::vector<document::ParameterBinding> bindings;
    std::optional<document::ParameterId> cues;
    for (const auto& parameter : definition->parameters) {
        const auto id = draft.ids().allocateParameter();
        if (!id)
            throw std::runtime_error("parameter id");
        auto value = parameter.defaultValue;
        if (parameter.schemaKey == document::kCueListValueParameterSchemaKey) {
            value = authoredList();
            cues = *id;
        }
        if (!composition.parameters().insert(
                {*id, parameter.schemaKey, document::ConstantValueSource{value}}))
            throw std::runtime_error("Subtitles parameter " + parameter.role);
        bindings.push_back({parameter.role, *id});
    }
    if (!cues || !composition.graph().addNode({*nodeId, std::string(document::kSubtitlesNodeType),
                                                std::move(bindings), 1}))
        throw std::runtime_error("Subtitles node");
    if (!draft.validate().ok())
        throw std::runtime_error("fixture validation");
    if (!owned->commit(snapshot.revision(), std::move(draft)).committed())
        throw std::runtime_error("fixture publication");
    return {std::move(owned), initial.initialCompositionId, *cues};
}

std::vector<std::byte> archiveOf(const document::Snapshot& snapshot,
                                 const document::ColorSettings& settings) {
    const CanonicalManifestV1 manifest;
    const CanonicalDocumentV1 input{.snapshot = &snapshot, .colorSettings = &settings};
    auto saved = buildVerifiedSaveArchive(manifest, input, {}, memory());
    if (!saved || !saved.archive())
        throw std::runtime_error("cue list archive");
    const auto bytes = saved.archive()->bytes();
    return {bytes.begin(), bytes.end()};
}

std::string documentTextOf(const std::vector<std::byte>& archive) {
    auto entries = readZipContainer(archive, {}, memory());
    if (!entries)
        throw std::runtime_error("cue list entries");
    const auto bytes = entries.document()->documentBytes();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

const document::CueListValue* listIn(const document::Composition& composition,
                                     const document::ParameterId id) {
    const auto* record = composition.parameters().find(id);
    const auto* constant =
        record == nullptr ? nullptr : std::get_if<document::ConstantValueSource>(&record->source);
    return constant == nullptr ? nullptr : std::get_if<document::CueListValue>(&constant->value);
}

void roundTripAndReopen() {
    const auto authored = authoredProject();
    const auto settings = neutralColorSettings();
    const auto snapshot = authored.document->snapshot();
    const auto archive = archiveOf(snapshot, settings);
    const auto text = documentTextOf(archive);
    expect(text.find("\"kind\": \"cueList\"") != std::string::npos,
           "a cue list constant is written with the cueList discriminator");
    expect(text.find("\"kind\": \"file\"") != std::string::npos &&
               text.find("\"path\": \"subtitles/episode 1.vtt\"") != std::string::npos,
           "a linked list writes its file provenance");

    auto openedResult = openProjectArchive(archive, {}, memory());
    expect(openedResult.outcome() == OpenArchiveOutcome::Opened, "the project opens");
    if (openedResult.outcome() != OpenArchiveOutcome::Opened)
        return;
    auto opened = std::move(openedResult).takeOpened();
    expect(!opened.roundTrip, "nothing unknown is retained");
    const auto reopened = opened.document->snapshot();
    const auto* composition = reopened.project().findComposition(authored.compositionId);
    const auto* list = composition == nullptr ? nullptr : listIn(*composition, authored.cuesParameter);
    expect(list != nullptr && *list == authoredList(),
           "the cue list reopens equal: ids, NTSC times, text, styles, placement, provenance");
    if (list != nullptr)
        expect(list->nextCueId == 9 && list->cues.size() == 3 && list->cues[1]->id == 2,
               "ids and the next id are kept, the cues in (start, end, id) order");
    const auto reencoded = documentTextOf(archiveOf(reopened, opened.colorSettings));
    expect(reencoded == text, "re-encoding the reopened document reproduces identical bytes");
}

void decoding() {
    const auto authored = authoredProject();
    const auto baseline =
        documentTextOf(archiveOf(authored.document->snapshot(), neutralColorSettings()));
    const auto decode = [&](const std::string& text) {
        auto dom = parseStrictJsonDom(test::toBytes(text), {}, memory());
        if (!dom)
            throw std::runtime_error("cue list parse");
        return decodeDocumentEnvelope(dom.document()->root());
    };
    const auto edited = [&](const std::string_view from, const std::string_view to) {
        auto text = baseline;
        const auto at = text.find(from);
        if (at == std::string::npos)
            throw std::runtime_error("edit anchor not found: " + std::string(from));
        text.replace(at, from.size(), to);
        return text;
    };
    expect(decode(baseline).outcome() == DocumentDecodeOutcome::Decoded, "the baseline decodes");
    expect(decode(edited("\"id\": \"7\"", "\"id\": \"1\"")).outcome() ==
               DocumentDecodeOutcome::Decoded,
           "any distinct ids decode");
    expect(decode(edited("\"id\": \"7\"", "\"id\": \"2\"")).outcome() ==
               DocumentDecodeOutcome::Failed,
           "two cues sharing an id are refused");
    expect(decode(edited("\"id\": \"4\"", "\"id\": \"0\"")).outcome() ==
               DocumentDecodeOutcome::Failed,
           "a cue id of zero is refused");
    expect(decode(edited("\"text\": \"Hello", "\"text\": \"\\u0007Hello")).outcome() ==
               DocumentDecodeOutcome::Failed,
           "cue text with a control character is refused");
    expect(decode(edited("\"speaker\": \"Anna\"", "\"speaker\": \"\"")).outcome() ==
               DocumentDecodeOutcome::Failed,
           "an empty optional member is refused (it is omitted, never written empty)");
    expect(decode(edited("\"speaker\": \"Anna\"", "\"speaker\": \"Anna\", \"mood\": 1")).outcome() ==
               DocumentDecodeOutcome::Failed,
           "an unknown member inside a cue is refused");
    expect(decode(edited("\"anchor\": \"top\"", "\"anchor\": \"up\"")).outcome() ==
               DocumentDecodeOutcome::Failed,
           "an anchor that is not one of the nine is refused");
    expect(decode(edited("\"name\": \"loud\"", "\"name\": \"zzz\"")).outcome() ==
               DocumentDecodeOutcome::Failed,
           "styles out of name order (and a cue naming a missing style) are refused");
    expect(decode(edited("\"name\": \"loud\"", "\"name\": \"yellow\"")).outcome() ==
               DocumentDecodeOutcome::Failed,
           "a style redefining a built-in class is refused");
    expect(decode(edited("\"language\": \"en-US\"", "\"language\": \"en US\"")).outcome() ==
               DocumentDecodeOutcome::Failed,
           "a language that is not a BCP 47 tag is refused");
    expect(decode(edited("\"kind\": \"file\"", "\"kind\": \"download\"")).outcome() ==
               DocumentDecodeOutcome::Failed,
           "an unknown provenance kind is refused");

    // A newer minor: a member after the list's own members is retained; one inside a cue is not.
    const auto minorAnchor = std::string_view("\"minor\": 0");
    const auto newer = [&](std::string text) {
        const auto at = text.find(minorAnchor);
        text.replace(at, minorAnchor.size(),
                     "\"minor\": " + std::to_string(kCanonicalDocumentSchemaVersionV1.minor + 1));
        return text;
    };
    // The cue array closes on a line indented as its "cues" member is.
    const auto cuesAt = baseline.find("\"cues\": [");
    const auto lineStart = cuesAt == std::string::npos ? cuesAt : baseline.rfind('\n', cuesAt) + 1;
    const auto closing = cuesAt == std::string::npos
                             ? std::string{}
                             : "\n" + baseline.substr(lineStart, cuesAt - lineStart) + "]";
    const auto listClose =
        cuesAt == std::string::npos ? std::string::npos : baseline.find(closing, cuesAt);
    expect(listClose != std::string::npos, "the fixture's cue array is found");
    if (listClose != std::string::npos) {
        auto retained = baseline;
        retained.insert(listClose + closing.size(), ",\n\"zFuture\": true");
        const auto decoded = decode(newer(retained));
        expect(decoded.outcome() == DocumentDecodeOutcome::Decoded &&
                   decoded.classification() == DocumentClassification::EditableWithRoundTrip,
               "a newer minor's member on the cue list is retained");
    }
    expect(decode(newer(edited("\"speaker\": \"Anna\"", "\"speaker\": \"Anna\", \"mood\": 1")))
                   .outcome() == DocumentDecodeOutcome::Failed,
           "a newer minor's member inside a cue is refused: cue objects are closed");
}

} // namespace

int main() {
    try {
        roundTripAndReopen();
        decoding();
    } catch (const std::exception& error) {
        std::cerr << "cue list persistence fixture failure: " << error.what() << '\n';
        return 1;
    }
    return failures == 0 ? 0 : 1;
}
