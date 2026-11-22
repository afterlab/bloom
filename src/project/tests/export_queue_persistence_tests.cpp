// ADR 0033: the export queue persists in run order and round-trips byte for byte. ADR 0036: the
// WebM and AV1 formats round-trip.
#include <bloom/document/document.hpp>
#include <bloom/document/export_queue.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/project/canonical_document.hpp>
#include <bloom/project/document_decode.hpp>
#include <bloom/project/document_reconstruct.hpp>
#include <bloom/project/strict_json_dom.hpp>

#include <algorithm>
#include <array>
#include <iostream>
#include <optional>
#include <regex>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>

namespace {
using namespace bloom;
using namespace bloom::project;
using namespace bloom::document;
int failures = 0;
void expect(const bool condition, const char* message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
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

ExportJob job(const std::uint64_t id, const CompositionId composition) {
    ExportJob value;
    value.id = ExportJobId::fromRaw(id);
    value.composition = composition;
    value.name = id == 1 ? "" : "Review \"cut\" {date}";
    value.render.range = id == 1 ? ExportRange{ExportRangeKind::WorkArea, 0, 0}
                                 : ExportRange{ExportRangeKind::Custom, 3, 17};
    value.enabled = id == 1;
    if (id != 1)
        value.labelColor = std::array<std::uint8_t, 3>{255, 0, 64};
    ExportOutput movie;
    movie.id = ExportOutputId::fromRaw(id * 10);
    movie.presetId = "builtin:h264-mp4";
    movie.settings.codecProfile = "high";
    movie.settings.videoBitrateKbps = 24000;
    movie.destination.path = "Exports/{name}.mp4";
    movie.importIntoAssets = id != 1;
    value.outputs.push_back(movie);
    ExportOutput frames;
    frames.id = ExportOutputId::fromRaw(id * 10 + 1);
    frames.presetId = "builtin:vfx-handoff-exr";
    frames.settings.format = ExportFormat::OpenExr;
    frames.settings.compression = "piz";
    frames.settings.outputColorSpaceId = "ACES2065-1";
    frames.settings.bakeLook = false;
    frames.settings.includeAudio = false;
    frames.settings.encoder = ExportEncoderPreference::Software;
    frames.settings.naming = {1001, 5, "{job}_<base>.{frame}.<ext>"};
    frames.destination = {"external", "file:///renders/plate.exr", ExportOverwritePolicy::Replace};
    frames.enabled = false;
    value.outputs.push_back(frames);
    return value;
}

Document makeDocument(const bool withQueue) {
    auto seed = makeNewProject("Export queue", "Main", core::RationalTime::fromInteger(2));
    if (withQueue)
        seed.project.setExportQueue(
            {{job(2, seed.initialCompositionId), job(1, seed.initialCompositionId)}});
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

std::string replaceOnce(std::string text, const std::string_view from, const std::string_view to) {
    const auto at = text.find(from);
    if (at == std::string::npos)
        throw std::logic_error("fixture anchor missing");
    text.replace(at, from.size(), to);
    return text;
}

void testRoundTrip() {
    const auto document = makeDocument(true);
    const auto text = encode(document);
    expect(text.find("\"exportQueue\"") != std::string::npos, "the queue is written");
    expect(text.find("\"exportJob\": \"2\"") != std::string::npos &&
               text.find("\"exportOutput\": \"21\"") != std::string::npos,
           "the allocator high water covers every export id");
    expect(text.find("\"id\": \"2\"") < text.find("\"id\": \"1\",\n          \"composition\""),
           "jobs are written in run order, not id order");
    auto operation = memory();
    auto parsed = parseStrictJsonDom(std::as_bytes(std::span(text)), {}, operation);
    expect(static_cast<bool>(parsed), "the document parses");
    if (!parsed)
        return;
    auto decoded = decodeDocumentEnvelope(parsed.document()->root());
    expect(static_cast<bool>(decoded), "the document decodes");
    if (!decoded)
        return;
    expect(decoded.value()->exportQueue == document.snapshot().project().exportQueue(),
           "every job and output member decodes exactly");
    auto envelope = *decoded.value();
    auto rebuilt = reconstructDocument(std::move(envelope));
    expect(static_cast<bool>(rebuilt), "the queue reconstructs into a valid project");
    if (!rebuilt)
        return;
    expect(encode(*rebuilt.value()->document) == text, "a full queue round-trips byte for byte");
}

std::optional<ExportQueue> decodeQueue(const std::string& text) {
    auto operation = memory();
    auto parsed = parseStrictJsonDom(std::as_bytes(std::span(text)), {}, operation);
    if (!parsed)
        return std::nullopt;
    auto decoded = decodeDocumentEnvelope(parsed.document()->root());
    if (!decoded)
        return std::nullopt;
    return decoded.value()->exportQueue;
}

// `useCachedFrames` is optional (absent means true) and written only when false.
void testUseCachedFrames() {
    const auto document = makeDocument(true);
    const auto text = encode(document);
    expect(text.find("useCachedFrames") == std::string::npos, "the default (true) is not written");
    const auto earlier = decodeQueue(text);
    expect(earlier && std::ranges::all_of(earlier->jobs,
                                          [](const auto& job) {
                                              return std::ranges::all_of(
                                                  job.outputs, [](const auto& output) {
                                                      return output.settings.useCachedFrames;
                                                  });
                                          }),
           "a document without the member opens with useCachedFrames on");

    auto seed = makeNewProject("Export queue", "Main", core::RationalTime::fromInteger(2));
    auto off = job(1, seed.initialCompositionId);
    off.outputs[0].settings.useCachedFrames = false;
    seed.project.setExportQueue({{off}});
    const Document withOff{std::move(seed.project)};
    const auto offText = encode(withOff);
    expect(offText.find("\"useCachedFrames\": false") != std::string::npos, "false is written");
    const auto decodedOff = decodeQueue(offText);
    expect(decodedOff && !decodedOff->jobs[0].outputs[0].settings.useCachedFrames &&
               decodedOff->jobs[0].outputs[1].settings.useCachedFrames,
           "false decodes per output");
    const auto explicitTrue =
        replaceOnce(offText, "\"useCachedFrames\": false", "\"useCachedFrames\": true");
    const auto decodedTrue = decodeQueue(explicitTrue);
    expect(decodedTrue && decodedTrue->jobs[0].outputs[0].settings.useCachedFrames,
           "an explicit true decodes");
    expect(
        !decodeQueue(replaceOnce(offText, "\"useCachedFrames\": false", "\"useCachedFrames\": 0")),
        "a non-boolean useCachedFrames is refused");
}

// #570: `loudness` is an optional member of an output's settings; absent means the audio is not
// normalized, so every earlier file opens and re-saves unchanged.
void testLoudness() {
    const auto plain = encode(makeDocument(true));
    expect(plain.find("loudness") == std::string::npos, "no target writes no member");

    auto seed = makeNewProject("Export queue", "Main", core::RationalTime::fromInteger(2));
    auto normalized = job(1, seed.initialCompositionId);
    normalized.outputs[0].settings.loudness = ExportLoudness{-14.0, -2.5};
    seed.project.setExportQueue({{normalized}});
    const Document withTarget{std::move(seed.project)};
    const auto text = encode(withTarget);
    expect(text.find("\"loudness\"") != std::string::npos &&
               text.find("\"targetLufs\": -14") != std::string::npos &&
               text.find("\"truePeakDbtp\": -2.5") != std::string::npos,
           "the target is written");
    const auto decoded = decodeQueue(text);
    expect(decoded && decoded->jobs[0].outputs[0].settings.loudness ==
                          std::optional(ExportLoudness{-14.0, -2.5}) &&
               !decoded->jobs[0].outputs[1].settings.loudness.has_value(),
           "the target round-trips per output");
    expect(!decodeQueue(replaceOnce(text, "\"targetLufs\": -14", "\"targetLufs\": -2")),
           "a target above -5 LUFS is refused");
    expect(!decodeQueue(replaceOnce(text, "\"truePeakDbtp\": -2.5", "\"truePeakDbtp\": 3")),
           "a ceiling above 0 dBTP is refused");
}

// #752: `subtitles` is an optional member of an output's settings; absent means no cue track is
// exported, so every earlier file opens and re-saves unchanged.
void testSubtitles() {
    const auto plain = encode(makeDocument(true));
    expect(plain.find("subtitles") == std::string::npos, "no subtitle export writes no member");

    auto seed = makeNewProject("Export queue", "Main", core::RationalTime::fromInteger(2));
    auto tracks = job(1, seed.initialCompositionId);
    tracks.outputs[0].settings.subtitles = ExportSubtitles{
        {ExportSubtitleFormat::Srt, ExportSubtitleFormat::Ttml}, true, std::nullopt};
    tracks.outputs[1].settings.subtitles =
        ExportSubtitles{{ExportSubtitleFormat::WebVtt},
                        false,
                        std::vector<NodeId>{NodeId::fromRaw(4), NodeId::fromRaw(12)}};
    seed.project.setExportQueue({{tracks}});
    const Document withTracks{std::move(seed.project)};
    const auto text = encode(withTracks);
    expect(text.find("\"subtitles\"") != std::string::npos &&
               text.find("\"sidecar\": [\n") != std::string::npos &&
               text.find("\"ttml\"") != std::string::npos &&
               text.find("\"embed\": true") != std::string::npos &&
               text.find("\"nodes\": [\n") != std::string::npos,
           "the subtitle settings are written");
    const auto decoded = decodeQueue(text);
    expect(decoded && *decoded == withTracks.snapshot().project().exportQueue(),
           "the subtitle settings round-trip per output");
    expect(decoded && encode(Document{[&] {
                          auto again = makeNewProject("Export queue", "Main",
                                                      core::RationalTime::fromInteger(2));
                          again.project.setExportQueue(*decoded);
                          return std::move(again.project);
                      }()}) == text,
           "the subtitle settings re-save byte for byte");
    for (const auto& output : tracks.outputs)
        expect(validateExportOutput(output).ok(), "the subtitle settings validate");
    expect(!decodeQueue(replaceOnce(text, "\"srt\"", "\"ass\"")),
           "an unknown sidecar format is refused");
    expect(!decodeQueue(replaceOnce(text, "\"embed\": true", "\"embed\": 1")),
           "a non-boolean embed is refused");

    auto nothing = tracks.outputs[0];
    nothing.settings.subtitles = ExportSubtitles{};
    expect(!validateExportOutput(nothing).ok(), "neither a sidecar nor a track is refused");
    auto twice = tracks.outputs[0];
    twice.settings.subtitles->sidecars = {ExportSubtitleFormat::Srt, ExportSubtitleFormat::Srt};
    expect(!validateExportOutput(twice).ok(), "a sidecar format given twice is refused");
    auto unordered = tracks.outputs[1];
    unordered.settings.subtitles->nodes =
        std::vector<NodeId>{NodeId::fromRaw(12), NodeId::fromRaw(4)};
    expect(!validateExportOutput(unordered).ok(), "unordered nodes are refused");
    auto none = tracks.outputs[1];
    none.settings.subtitles->nodes = std::vector<NodeId>{};
    expect(!validateExportOutput(none).ok(), "an empty node list is refused");
    // Embedding is a setting of any output: a format that carries no track is a preflight issue
    // (it names the sidecar formats), not an invalid document.
    auto mxf = tracks.outputs[0];
    mxf.settings.format = ExportFormat::DnxhrMxf;
    mxf.settings.codecProfile = "hq";
    expect(validateExportOutput(mxf).ok() && !exportFormatEmbedsSubtitles(ExportFormat::DnxhrMxf) &&
               exportFormatEmbedsSubtitles(ExportFormat::H264Mp4) &&
               exportFormatEmbedsSubtitles(ExportFormat::Vp9Webm),
           "embedding is persisted for any format and applies to MOV, MP4 and WebM");
}

// #760: `background` is an optional member of an output's settings; absent means transparent, so
// every earlier file opens and re-saves unchanged.
void testBackground() {
    const auto plain = encode(makeDocument(true));
    expect(plain.find("\"background\"") == std::string::npos,
           "a transparent output writes no background member");
    auto seed = makeNewProject("Export queue", "Main", core::RationalTime::fromInteger(2));
    auto backed = job(1, seed.initialCompositionId);
    backed.outputs[0].settings.background = ExportBackground::CompositionColor;
    seed.project.setExportQueue({{backed}});
    const Document withBackground{std::move(seed.project)};
    const auto text = encode(withBackground);
    expect(text.find("\"background\": \"composition\"") != std::string::npos,
           "a composition background is written");
    const auto decoded = decodeQueue(text);
    expect(decoded && *decoded == withBackground.snapshot().project().exportQueue() &&
               decoded->jobs[0].outputs[0].settings.background ==
                   ExportBackground::CompositionColor &&
               decoded->jobs[0].outputs[1].settings.background == ExportBackground::Transparent,
           "the background round-trips per output");
    expect(decoded && encode(Document{[&] {
                          auto again = makeNewProject("Export queue", "Main",
                                                      core::RationalTime::fromInteger(2));
                          again.project.setExportQueue(*decoded);
                          return std::move(again.project);
                      }()}) == text,
           "the background re-saves byte for byte");
    expect(!decodeQueue(replaceOnce(text, "\"background\": \"composition\"",
                                    "\"background\": \"black\"")),
           "an unknown background is refused");
    expect(!decodeQueue(replaceOnce(text, "\"background\": \"composition\"",
                                    "\"background\": \"transparent\"")),
           "transparent is the absent member, never written");
    expect(parseExportBackground("composition") == ExportBackground::CompositionColor &&
               parseExportBackground("transparent") == ExportBackground::Transparent &&
               !parseExportBackground("opaque") &&
               exportBackgroundToken(ExportBackground::CompositionColor) == "composition",
           "the background tokens");
}

void testRejects() {
    const auto text = encode(makeDocument(true));
    for (const auto& [from, to, message] :
         std::array<std::tuple<std::string_view, std::string_view, const char*>, 4>{{
             {"\"format\": \"h264-mp4\"", "\"format\": \"gif\"", "unknown format tokens"},
             {"\"overwrite\": \"ask\"", "\"overwrite\": \"maybe\"", "unknown overwrite policy"},
             {"\"kind\": \"custom\",\n            \"first\": \"3\",\n            \"last\": \"17\"",
              "\"kind\": \"custom\",\n            \"first\": \"3\"", "a custom range without last"},
             {"\"importIntoAssets\": true", "\"importIntoAssets\": 1", "a non-boolean flag"},
         }}) {
        const auto broken = replaceOnce(text, from, to);
        auto operation = memory();
        auto parsed = parseStrictJsonDom(std::as_bytes(std::span(broken)), {}, operation);
        expect(!parsed || !decodeDocumentEnvelope(parsed.document()->root()), message);
    }
    // A job whose composition does not exist decodes but never reconstructs.
    const auto orphan = replaceOnce(text, "\"composition\": \"1\",\n          \"name\": \"Review",
                                    "\"composition\": \"9\",\n          \"name\": \"Review");
    auto operation = memory();
    auto parsed = parseStrictJsonDom(std::as_bytes(std::span(orphan)), {}, operation);
    expect(static_cast<bool>(parsed), "the orphan fixture parses");
    if (!parsed)
        return;
    auto decoded = decodeDocumentEnvelope(parsed.document()->root());
    expect(static_cast<bool>(decoded), "the orphan fixture decodes");
    if (decoded) {
        auto envelope = *decoded.value();
        expect(!reconstructDocument(std::move(envelope)),
               "a job naming a missing composition is refused at reconstruction");
    }
}

// ADR 0036: the WebM and AV1 formats and their bit-depth profiles round-trip.
void testWebmFormats() {
    auto seed = makeNewProject("Export queue", "Main", core::RationalTime::fromInteger(2));
    auto webm = job(1, seed.initialCompositionId);
    const std::array formats{std::pair{ExportFormat::Vp9Webm, "10bit"},
                             std::pair{ExportFormat::Av1Webm, "8bit"},
                             std::pair{ExportFormat::Av1Mp4, "10bit"}};
    webm.outputs.clear();
    for (std::size_t index = 0; index < formats.size(); ++index) {
        ExportOutput output;
        output.id = ExportOutputId::fromRaw(10 + index);
        output.presetId = "builtin:custom";
        output.settings.format = formats[index].first;
        output.settings.codecProfile = formats[index].second;
        output.destination.path = "Exports/{name}";
        webm.outputs.push_back(output);
    }
    seed.project.setExportQueue({{webm}});
    const Document document{std::move(seed.project)};
    const auto text = encode(document);
    expect(text.find("\"minor\": 0\n  },\n  \"project\"") != std::string::npos,
           "the document is written at 1.0");
    for (const auto* token : {"\"vp9-webm\"", "\"av1-webm\"", "\"av1-mp4\"", "\"8bit\""})
        expect(text.find(token) != std::string::npos, "each WebM and AV1 token is written");
    const auto decoded = decodeQueue(text);
    expect(decoded && *decoded == document.snapshot().project().exportQueue(),
           "the WebM and AV1 outputs decode exactly");
    for (const auto& output : webm.outputs)
        expect(validateExportOutput(output).ok(), "a WebM or AV1 output validates");
    auto wrong = webm.outputs[0];
    wrong.settings.codecProfile = "main10";
    expect(!validateExportOutput(wrong).ok(), "an HEVC profile does not apply to VP9");
}

} // namespace

int main() {
    try {
        testRoundTrip();
        testLoudness();
        testSubtitles();
    testBackground();
        testRejects();
        testUseCachedFrames();
        testWebmFormats();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    return failures == 0 ? 0 : 1;
}
