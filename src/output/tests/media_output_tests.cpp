#include <algorithm>
#include <array>
#include <bloom/media/provider/ffmpeg_manifest.hpp>
#include <bloom/output/display_output.hpp>
#include <bloom/output/media_output.hpp>
#include <bloom/output/tiff_export_write.hpp>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <vector>
namespace {
void check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
bloom::output::MediaOutputAnalysisV1 analyze(bloom::output::OutputPresetV1 preset,
                                             bloom::media::provider::EncodeSettingsV1 settings) {
    auto result = bloom::output::analyzeMediaOutputV1(preset, std::move(settings));
    check(std::holds_alternative<bloom::output::MediaOutputAnalysisV1>(result),
          "media analysis succeeds");
    return std::get<bloom::output::MediaOutputAnalysisV1>(std::move(result));
}
// The codec-boundary half used after GPU display colour must reproduce the CPU reference exactly
// when it is handed the CPU reference's own display values (associated with the process alpha):
// same un-association, clamp, round-half-up quantization, byte order and plane digest.
void testDisplayPayloadQuantizationMatchesReference() {
    namespace o = bloom::output;
    namespace r = bloom::render;
    const auto display = o::PreparedOutputDisplayV1::prepare({});
    check(display != nullptr, "neutral display prepared");
    constexpr std::uint32_t width = 7;
    constexpr std::uint32_t height = 3;
    const auto associated = [](const float red, const float green, const float blue,
                               const float alpha) {
        const auto pixel = r::Rgba32f::fromPremultiplied(red, green, blue, alpha);
        check(pixel.value() != nullptr, "valid associated pixel");
        return *pixel.value();
    };
    std::vector<r::Rgba32f> process;
    for (std::uint32_t index = 0; index < width * height; ++index) {
        // Opaque, half and transparent coverage over SDR, HDR and negative values. Power-of-two
        // alphas keep association exact, so equality is bitwise.
        const float alpha = index % 3 == 0 ? 1.0F : (index % 3 == 1 ? 0.5F : 0.0F);
        const float value = -0.25F + static_cast<float>(index) * 0.11F;
        process.push_back(alpha == 0.0F ? r::Rgba32f::transparent()
                                        : associated(value * alpha, value * 0.5F * alpha,
                                                     (1.5F - value) * alpha, alpha));
    }
    const auto windowResult = r::ImageWindow::create(0, 0, width, height);
    check(windowResult.value() != nullptr, "window");
    const auto window = *windowResult.value();
    const auto descriptor =
        r::Rgba32fImageDescriptor::create(window, window, bloom::core::PixelAspectRatio::square());
    check(static_cast<bool>(descriptor), "descriptor");
    auto builder = r::Rgba32fImageBuilder::create(*descriptor.value(),
                                                  descriptor.value()->layout().pixelStorageBytes);
    check(static_cast<bool>(builder), "builder");
    for (std::uint32_t row = 0; row < height; ++row) {
        auto destination = builder.value()->row(row);
        check(static_cast<bool>(destination), "row");
        std::copy_n(process.begin() + static_cast<std::ptrdiff_t>(row * width), width,
                    destination.value()->begin());
    }
    auto frozen = std::move(*builder.value()).freeze();
    check(static_cast<bool>(frozen), "frozen");
    const auto& image = *frozen.value();
    std::vector<r::Rgba32f> displayAssociated;
    for (const auto pixel : image.pixels()) {
        const float alpha = pixel.alpha();
        if (alpha == 0.0F) {
            displayAssociated.push_back(pixel);
            continue;
        }
        const auto converted = display->processor().referenceToDisplay(
            {static_cast<double>(pixel.red() / alpha), static_cast<double>(pixel.green() / alpha),
             static_cast<double>(pixel.blue() / alpha), static_cast<double>(alpha)});
        check(converted.has_value(), "reference display conversion");
        displayAssociated.push_back(associated(static_cast<float>(converted->red) * alpha,
                                               static_cast<float>(converted->green) * alpha,
                                               static_cast<float>(converted->blue) * alpha, alpha));
    }
    namespace p = bloom::media::provider;
    const auto reference = o::prepareMediaRgba16V1(image, {1, 24}, {}, display.get());
    const auto boundary =
        o::prepareMediaRgba16FromDisplayV1(displayAssociated, width, height, {1, 24});
    check(std::holds_alternative<p::FrameProduct>(reference) &&
              std::holds_alternative<p::FrameProduct>(boundary),
          "both preparations succeed");
    const auto& left = std::get<p::FrameProduct>(reference);
    const auto& right = std::get<p::FrameProduct>(boundary);
    check(left.planes.size() == 1 && right.planes.size() == 1 &&
              left.planes[0].bytes == right.planes[0].bytes &&
              left.planes[0].digest == right.planes[0].digest && left.pts == right.pts &&
              left.format == right.format && left.colour == right.colour &&
              left.planes[0].stride == right.planes[0].stride,
          "GPU display payload quantizes exactly like the CPU reference");
    check(std::holds_alternative<p::Unavailable>(
              o::prepareMediaRgba16FromDisplayV1(displayAssociated, width + 1, height, {1, 24})),
          "a payload that does not match the frame geometry is refused");
}
} // namespace
// The container tags a video export declares, from its display name (exact H.273 code points).
void testOutputColourTags() {
    namespace o = bloom::output;
    const auto tags = [](const std::string_view display) {
        const auto t = o::outputColourTagsV1(display);
        return std::array<std::int32_t, 4>{t.primaries, t.transfer, t.matrix, t.range};
    };
    using Tags = std::array<std::int32_t, 4>;
    // Bloom Neutral's display is lower case; it was tagged {2, 2, 1, 1} (unspecified) before.
    check(tags("srgb_rec709_display") == Tags{1, 13, 1, 1}, "Neutral sRGB display tags sRGB");
    check(tags("sRGB - Display") == Tags{1, 13, 1, 1}, "ACES sRGB display tags sRGB");
    check(tags("Rec.1886 Rec.709 - Display") == Tags{1, 1, 1, 1}, "Rec.709 display tags BT.709");
    check(tags("Rec.2100-PQ - Display") == Tags{9, 16, 9, 1}, "PQ display tags Rec.2100 PQ");
    check(tags("Rec.2100-HLG - Display") == Tags{9, 18, 9, 1}, "HLG display tags Rec.2100 HLG");
    check(tags("Display P3 - Display") == Tags{12, 13, 1, 1}, "Display P3 tags P3-D65 sRGB");
    check(tags("Unknown") == Tags{2, 2, 1, 1}, "an unknown display leaves primaries unspecified");
    // #465: a TIFF carries full-range RGB, not the encoder default BT.709 limited range.
    const auto tiff = o::tiffExportEncodeSettingsV1(16, 8, "sRGB - Display");
    check(tiff.videoCodec == "tiff" && tiff.width == 16 && tiff.height == 8 &&
              tiff.colour == bloom::media::provider::ColourTags{1, 13, 0, 2},
          "a TIFF export declares sRGB primaries and transfer, the RGB matrix and full range");
}
// #465: the VA-API probe answer, with HEVC Main 10 probed apart from 8-bit HEVC.
void testVaapiProbeParsing() {
    namespace o = bloom::output;
    using Probe = o::VaapiEncodeProbeV1;
    check(o::parseVaapiEncodeProbeV1("bloom-vaapi-probe-v2\n/dev/dri/renderD128 1 1 0\n") ==
              Probe{"/dev/dri/renderD128", true, true, false},
          "a node that opens only 8-bit HEVC does not report Main 10");
    check(o::parseVaapiEncodeProbeV1("bloom-vaapi-probe-v2\n/dev/dri/renderD128 1 1 0\n"
                                     "/dev/dri/renderD129 1 1 1\n") ==
              Probe{"/dev/dri/renderD129", true, true, true},
          "a node that also opens Main 10 is preferred");
    check(o::parseVaapiEncodeProbeV1("bloom-vaapi-probe-v2\n/dev/dri/renderD128 1 0 0\n"
                                     "/dev/dri/renderD129 0 1 1\n") ==
              Probe{"/dev/dri/renderD128", true, false, false},
          "a node without H.264 encode is never chosen");
    check(o::parseVaapiEncodeProbeV1("bloom-vaapi-probe-v1\n/dev/dri/renderD128 1 1\n") == Probe{},
          "an answer in another format is refused");
}
int main() {
    try {
        testOutputColourTags();
        testVaapiProbeParsing();
        namespace p = bloom::media::provider;
        namespace o = bloom::output;
        p::EncodeSettingsV1 s;
        s.width = 256;
        s.height = 128;
        s.frames = 48;
        s.audioCodec = "pcm_s16le";
        s.audioSamples = 96000;
        const auto report = analyze(o::OutputPresetV1::ProResMovV1, s);
        const auto revision = bloom::color::ocioBuiltInContentRevision(
            bloom::color::OcioConfigLocatorKind::BloomBuiltIn, bloom::color::kAcesCgV1ConfigUri);
        if (revision) {
            const auto display = o::PreparedOutputDisplayV1::prepare(
                {.workingColorSpaceId = "ACEScg",
                 .ocioConfigRevision = *revision,
                 .ocioConfigUri = bloom::color::kAcesCgV1ConfigUri});
            check(display != nullptr, "ACES display processor available");
            // Issue #409: an output naming no view takes the Viewer's default pair, never the
            // config's own tone-mapped default view.
            check(display->displayName() == "sRGB - Display" &&
                      display->viewName() == "Un-tone-mapped" &&
                      display->description().find("view: Un-tone-mapped") != std::string::npos,
                  "an output naming no view records the Viewer's un-tone-mapped default");
            const auto named = o::PreparedOutputDisplayV1::prepare(
                {.workingColorSpaceId = "ACEScg",
                 .ocioConfigRevision = *revision,
                 .ocioConfigUri = bloom::color::kAcesCgV1ConfigUri},
                "sRGB - Display", "Un-tone-mapped");
            check(named && named->digest() == display->digest(),
                  "naming the Viewer's default pair is the same output as naming none");
            const auto toneMapped = o::PreparedOutputDisplayV1::prepare(
                {.workingColorSpaceId = "ACEScg",
                 .ocioConfigRevision = *revision,
                 .ocioConfigUri = bloom::color::kAcesCgV1ConfigUri},
                "sRGB - Display", "ACES 1.0 - SDR Video");
            check(toneMapped && toneMapped->viewName() == "ACES 1.0 - SDR Video" &&
                      toneMapped->digest() != display->digest(),
                  "an explicitly named tone-mapped view is kept");
            const auto displayOnly = o::PreparedOutputDisplayV1::prepare(
                {.workingColorSpaceId = "ACEScg",
                 .ocioConfigRevision = *revision,
                 .ocioConfigUri = bloom::color::kAcesCgV1ConfigUri},
                "Rec.1886 Rec.709 - Display", {});
            check(displayOnly && displayOnly->displayName() == "Rec.1886 Rec.709 - Display" &&
                      displayOnly->viewName() == "Un-tone-mapped",
                  "a named display with no view takes the un-tone-mapped view it offers");
            const auto rec709 = o::PreparedOutputDisplayV1::prepare(
                {.workingColorSpaceId = "ACEScg",
                 .ocioConfigRevision = *revision,
                 .ocioConfigUri = bloom::color::kAcesCgV1ConfigUri},
                "Rec.1886 Rec.709 - Display", "ACES 1.0 - SDR Video");
            check(rec709 && rec709->digest() != display->digest(),
                  "Rec.709 review pair has its own identity");
        }
#if !defined(__APPLE__)
        const auto hex = report.digest.toLowercaseHex();
#endif
        std::size_t recordBytes = std::string_view("BloomMediaOutputAnalysisV1").size() + 1 + 32;
        for (const auto& facet : report.facets)
            recordBytes += 3 + facet.description.size();
#if defined(__APPLE__)
        // The exact oracle below is pinned to the FFmpeg provider's ProRes wording. macOS uses the
        // native Apple ProRes note, so its record is deliberately different: keep the structural
        // and authority checks and leave the frozen Linux/FFmpeg oracle to that platform.
        check(recordBytes > 0 &&
                  report.display->processor().identity().canonicalBytes().size() == 224,
              "identity record sizes pinned");
#else
        check(recordBytes == 759 &&
                  report.display->processor().identity().canonicalBytes().size() == 224,
              "identity record sizes pinned");
        // Independent Python byte oracle: domain + 04 + frozen settings hash (EncodeSettingsV1
        // version 2) + eleven explicit (facet byte,state byte,NUL-terminated UTF-8 description)
        // tuples: 759 bytes. See tests/color5_identity_oracle.py. Issue #424: the settings hash
        // binds video tolerance profile v3 (6fcfd3e3...; v2's 7e8bea87... gave 1253f309...).
        check(std::string(hex.data(), hex.size()) ==
                  "2d6eeb234758d5328c33943f27c9faaf5ee63f80befd73650bed22d95c60b350",
              "media analysis frozen oracle");
#endif
        check(report.implementationNote.starts_with(p::kProResExportNote) &&
                  !report.appleAuthorized && !report.deliveryQualified,
              "ProRes wording and authority");
        check(report.facets[3].preservation == o::OutputPreservationStateV1::Omitted,
              "422 alpha omitted");
        auto silent = s;
        silent.audioCodec.clear();
        silent.audioSamples = 0;
        check(analyze(o::OutputPresetV1::ProResMovV1, silent).facets[4].preservation ==
                  o::OutputPreservationStateV1::Omitted,
              "audio-off analysis discloses omitted channels");
        s.profile = "4444";
        const auto alpha = analyze(o::OutputPresetV1::ProResMovV1, s);
        check(alpha.digest != report.digest &&
                  alpha.facets[3].preservation == o::OutputPreservationStateV1::Approximated,
              "profile and alpha bind analysis");
        s.videoCodec.clear();
        s.frames = 0;
        s.container = "wav";
        const auto pcm = analyze(o::OutputPresetV1::PcmWavV1, s);
        check(pcm.determinism == p::MediaDeterminismV1::ByteExact &&
                  pcm.toleranceProfile == p::Digest{},
              "PCM byte-exact identity");
        s.bwfDescription = "BWF metadata";
        check(analyze(o::OutputPresetV1::PcmWavV1, s).digest != pcm.digest,
              "BWF metadata binds approval");
        p::EncodeSettingsV1 h264;
        h264.width = 256;
        h264.height = 128;
        h264.frames = 48;
        h264.videoCodec = "h264";
        h264.profile = "high";
        h264.container = "mov";
        h264.audioCodec = "pcm_s16le";
        h264.audioSamples = 96000;
        const auto review = analyze(o::OutputPresetV1::H264MovV1, h264);
        check(review.determinism == p::MediaDeterminismV1::DecodedSemanticTolerance &&
                  review.toleranceProfile != p::Digest{} &&
                  review.implementationNote.starts_with("Review deliverable — not for archival"),
              "H.264 review analysis records lossy tolerance and archival warning");
        check(std::holds_alternative<p::Unavailable>(
                  o::analyzeMediaOutputV1(o::OutputPresetV1::ProResMovV1, s)),
              "codec/preset mismatch refused");
        // ADR 0036: VP9/AV1 with Opus in WebM and AV1 with AAC in MP4, 8 or 10 bits.
        auto web = h264;
        web.videoCodec = "vp9";
        web.profile = "10bit";
        web.container = "webm";
        web.audioCodec = "opus";
        const auto vp9 = analyze(o::OutputPresetV1::Vp9WebmV1, web);
        check(vp9.determinism == p::MediaDeterminismV1::DecodedSemanticTolerance &&
                  vp9.implementationNote.starts_with("VP9 web delivery — not for archival"),
              "VP9 WebM analysis records lossy tolerance and archival warning");
        auto av1 = web;
        av1.videoCodec = "av1";
        av1.container = "mp4";
        av1.audioCodec = "aac";
        (void)analyze(o::OutputPresetV1::Av1Mp4V1, av1);
        for (const auto& [preset, settings, why] :
             {std::tuple{o::OutputPresetV1::Av1Mp4V1, web, "AV1 MP4 refuses VP9 in WebM"},
              std::tuple{o::OutputPresetV1::Av1WebmV1, av1, "AV1 WebM refuses an MP4 container"}}) {
            check(std::holds_alternative<p::Unavailable>(o::analyzeMediaOutputV1(preset, settings)),
                  why);
        }
        auto opusInMp4 = av1;
        opusInMp4.audioCodec = "opus";
        check(std::holds_alternative<p::Unavailable>(
                  o::analyzeMediaOutputV1(o::OutputPresetV1::Av1Mp4V1, opusInMp4)),
              "AV1 MP4 carries AAC, not Opus");
        check(o::compositeFrameReuseEligibleV1(o::OutputPresetV1::Vp9WebmV1, "8bit") &&
                  o::compositeFrameReuseEligibleV1(o::OutputPresetV1::Av1Mp4V1, "8bit") &&
                  !o::compositeFrameReuseEligibleV1(o::OutputPresetV1::Av1WebmV1, "10bit"),
              "8-bit VP9/AV1 may reuse cached frames; 10-bit renders fresh");
        testDisplayPayloadQuantizationMatchesReference();
        // Composite-cache reuse is limited to outputs coarser than half precision.
        check(o::compositeFrameReuseEligibleV1(o::OutputPresetV1::ProResMovV1, "hq") &&
                  o::compositeFrameReuseEligibleV1(o::OutputPresetV1::H264Mp4V1, "high") &&
                  o::compositeFrameReuseEligibleV1(o::OutputPresetV1::HevcMovV1, "main10") &&
                  o::compositeFrameReuseEligibleV1(o::OutputPresetV1::DnxhrMxfV1, "dnxhr_hq") &&
                  o::compositeFrameReuseEligibleV1(o::OutputPresetV1::PngRgba8SrgbV1, {}),
              "lossy video and 8-bit PNG may reuse cached frames");
        check(!o::compositeFrameReuseEligibleV1(o::OutputPresetV1::ProResMovV1, "4444") &&
                  !o::compositeFrameReuseEligibleV1(o::OutputPresetV1::ProResMovV1, "4444xq") &&
                  !o::compositeFrameReuseEligibleV1(o::OutputPresetV1::DnxhrMxfV1, "dnxhr_hqx") &&
                  !o::compositeFrameReuseEligibleV1(o::OutputPresetV1::DnxhrMxfV1, "dnxhr_444") &&
                  !o::compositeFrameReuseEligibleV1(o::OutputPresetV1::TiffRgba16SrgbV1, {}) &&
                  !o::compositeFrameReuseEligibleV1(
                      o::OutputPresetV1::FlatExrRgba32fLinRec709SceneV1, {}) &&
                  !o::compositeFrameReuseEligibleV1(o::OutputPresetV1::PcmWavV1, "pcm_s16le"),
              "12-bit and deeper, 16-bit, float and audio outputs always render fresh");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
