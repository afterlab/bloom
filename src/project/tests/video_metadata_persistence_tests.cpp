// Issue #367: video-asset stream field order and timecode type persist.
#include <bloom/core/rational_time.hpp>
#include <bloom/document/asset.hpp>
#include <bloom/document/color_settings.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/project/canonical_document.hpp>
#include <bloom/project/document_decode.hpp>
#include <bloom/project/strict_json_dom.hpp>

#include <array>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>

namespace {

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
    auto seed = makeNewProject("Video metadata", "Main", core::RationalTime::fromInteger(2));
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
void roundTrip() {
    const auto document = makeDocument("01:00:00;00", 2);
    const auto text = encode(document);
    expect(text.find("\"fieldOrder\": 2") != std::string::npos &&
               text.find("\"timecodeType\": 2") != std::string::npos,
           "field order and drop-frame timecode type are written");
    auto operation = memory();
    auto parsed = parseStrictJsonDom(std::as_bytes(std::span(text)), {}, operation);
    expect(static_cast<bool>(parsed), "the document parses");
    if (!parsed)
        return;
    const auto decoded = decodeDocumentEnvelope(parsed.document()->root());
    expect(static_cast<bool>(decoded) &&
               decoded.value()->assets.front() ==
                   *document.snapshot().project().findAsset(AssetId::fromRaw(1)),
           "video metadata round trips exactly");
}
} // namespace
int main() {
    try {
        roundTrip();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return failures == 0 ? 0 : 1;
}
