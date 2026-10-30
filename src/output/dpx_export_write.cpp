#include <bloom/media/provider/encode_session.hpp>
#include <bloom/output/dpx_export_write.hpp>
#include <bloom/output/flat_exr_options.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>

namespace bloom::output {
namespace {
using namespace media::provider;

constexpr std::size_t kCancellationStride = 64U * 1024U;

[[nodiscard]] bool isRec709Primaries(const std::string_view id) noexcept {
    return id == "lin_rec709_scene" || id == "Linear Rec.709 (sRGB)";
}

// H.273 values compared the way the export verifier compares them: a missing tag is unspecified.
[[nodiscard]] bool sameTags(const ColourTags& a, const ColourTags& b) noexcept {
    const auto norm = [](const std::int32_t code) { return std::max(code, 2); };
    return norm(a.primaries) == norm(b.primaries) && norm(a.transfer) == norm(b.transfer);
}

// Straight alpha is dropped: the colour is what the frame shows over black (premultiplied).
[[nodiscard]] Result<FrameProduct> prepareRgb16(const render::Rgba32fImage& image,
                                                const PreparedFlatExrOutputV1* conversion,
                                                const ColourTags& tags,
                                                const runtime::CancellationToken& cancellation) {
    const auto* descriptor = image.descriptor();
    if (!descriptor || descriptor->dataWindow() != descriptor->displayWindow() ||
        descriptor->pixelAspect() != core::PixelAspectRatio::square())
        return Unavailable{Error::InvalidValue, "DPX needs equal windows and square pixels"};
    const auto window = descriptor->dataWindow();
    if (window.originX() != 0 || window.originY() != 0 ||
        window.extent().width() > Limits::dimension ||
        window.extent().height() > Limits::dimension || image.pixels().size() > Limits::pixels)
        return Unavailable{Error::Oversized, "DPX dimensions exceed provider limits"};
    FrameProduct result;
    result.format = PixelFormat::Rgba16;
    result.pts = {0, 1};
    result.colour = tags;
    CpuPlane plane;
    plane.width = static_cast<std::uint32_t>(window.extent().width());
    plane.height = static_cast<std::uint32_t>(window.extent().height());
    plane.stride = plane.width * 8U;
    plane.bytes.resize(image.pixels().size() * 8U);
    const auto pixels = image.pixels();
    std::vector<std::array<float, 4>> converted;
    std::size_t offset = 0;
    for (std::size_t first = 0; first < pixels.size(); first += kCancellationStride) {
        if (cancellation.isCancellationRequested())
            return Unavailable{Error::Cancelled, "DPX preparation cancelled"};
        const auto count = std::min(kCancellationStride, pixels.size() - first);
        converted.assign(count, {});
        if (conversion) {
            if (!conversion->apply(pixels.subspan(first, count), converted))
                return Unavailable{Error::InvalidValue, "DPX output colour conversion failed"};
        } else {
            for (std::size_t i = 0; i < count; ++i) {
                const auto& p = pixels[first + i];
                converted[i] = {p.red(), p.green(), p.blue(), 1.0F};
            }
        }
        for (const auto& pixel : converted) {
            for (std::size_t c = 0; c < 4; ++c) {
                const double value = c == 3 ? 1.0 : static_cast<double>(pixel[c]);
                const auto quantized = static_cast<std::uint32_t>(std::floor(
                    (std::isfinite(value) ? std::clamp(value, 0.0, 1.0) : 0.0) * 65535.0 + 0.5));
                plane.bytes[offset++] = static_cast<std::byte>(quantized & 255U);
                plane.bytes[offset++] = static_cast<std::byte>(quantized >> 8U);
            }
        }
    }
    result.planes.push_back(std::move(plane));
    return result;
}
} // namespace

ColourTags dpxColourTagsV1(const std::string_view outputColorSpaceId,
                           const std::string_view workingColorSpaceId) {
    const auto id = outputColorSpaceId.empty() ? workingColorSpaceId : outputColorSpaceId;
    // primaries, transfer (8 = linear), matrix (0 = RGB), range (2 = full)
    return {isRec709Primaries(id) ? 1 : 2, 8, 0, 2};
}

bool dpxEncoderAvailableV1() noexcept {
    try {
        EncodeSessionV1 session({});
        EncodeSettingsV1 settings;
        settings.width = 8;
        settings.height = 8;
        settings.videoCodec = "dpx";
        settings.profile = "rgb10";
        settings.container = "dpx";
        const auto refused = session.begin(settings);
        return !(refused && refused->reason == Error::Unavailable &&
                 refused->detail == "Encoder is not in the intake");
    } catch (...) {
        return true;
    }
}

DpxExportWriteResultV1
DpxExportWriterV1::run(const OutputAnalysisAttemptV1& attempt,
                       const std::filesystem::path& destination,
                       const runtime::CancellationToken& cancellation) const noexcept {
    if (!isDpxPresetV1(attempt.preset()) || !attempt.frame() || !attempt.approvable())
        return DpxExportWriteResultV1::failed(DpxExportWriteErrorCodeV1::ProviderFailed);
    try {
        const auto fail = [](const Unavailable& e) {
            return e.reason == Error::Cancelled
                       ? DpxExportWriteResultV1::cancelledResult()
                       : DpxExportWriteResultV1::failed(
                             e.reason == Error::Unavailable
                                 ? DpxExportWriteErrorCodeV1::ProviderMissing
                                 : DpxExportWriteErrorCodeV1::ProviderFailed);
        };
        const auto cancel = [&] { return cancellation.isCancellationRequested(); };
        const auto* conversion = attempt.report()->exr().get();
        const auto& intent = attempt.frame()->identity().colorIntent;
        const auto tags = dpxColourTagsV1(conversion ? conversion->options().outputColorSpaceId
                                                     : std::string_view{},
                                          intent.workingColorSpaceId);
        // A conversion to the working space itself is the identity.
        auto prepared = prepareRgb16(attempt.frame()->processImage(),
                                     conversion && conversion->processor() ? conversion : nullptr,
                                     tags, cancellation);
        if (const auto* e = std::get_if<Unavailable>(&prepared))
            return fail(*e);
        auto frame = std::get<FrameProduct>(std::move(prepared));
        EncodeSettingsV1 settings;
        settings.container = "dpx";
        settings.videoCodec = "dpx";
        settings.profile = attempt.preset() == OutputPresetV1::DpxRgb10V1 ? "rgb10" : "rgb16";
        settings.width = frame.planes[0].width;
        settings.height = frame.planes[0].height;
        settings.colour = tags;
        EncodeSessionV1 session({}, cancel);
        if (const auto e = session.begin(settings))
            return fail(*e);
        if (const auto e = session.video(std::move(frame)))
            return fail(*e);
        auto verified = session.finish();
        if (const auto* e = std::get_if<Unavailable>(&verified))
            return fail(*e);
        const auto qc = std::get<EncodeQcV1>(verified);
        auto observed = DpxExportWriteResultV1::failed(DpxExportWriteErrorCodeV1::TagsDiffer);
        observed.observedTags = qc.colour;
        if (qc.container != "dpx/dpx" || !sameTags(qc.colour, tags))
            return observed;
        std::ofstream file(destination, std::ios::binary | std::ios::trunc);
        if (!file)
            return DpxExportWriteResultV1::failed(DpxExportWriteErrorCodeV1::ProviderFailed);
        core::Sha256Hasher hasher;
        for (std::uint64_t offset = 0; offset < qc.bytes;) {
            auto reply = session.read(offset);
            if (const auto* e = std::get_if<Unavailable>(&reply))
                return fail(*e);
            const auto& chunk = std::get<EncodedChunkV1>(reply);
            if (chunk.bytes.size() > qc.bytes - offset || !hasher.update(chunk.bytes))
                return DpxExportWriteResultV1::failed(DpxExportWriteErrorCodeV1::ProviderFailed);
            file.write(reinterpret_cast<const char*>(chunk.bytes.data()),
                       static_cast<std::streamsize>(chunk.bytes.size()));
            offset += chunk.bytes.size();
        }
        file.close();
        if (!file || hasher.finalize() != qc.artifact)
            return DpxExportWriteResultV1::failed(DpxExportWriteErrorCodeV1::ProviderFailed);
        if (const auto e = session.close())
            return fail(*e);
        DpxExportWriteResultV1 result;
        result.written = true;
        result.artifactDigest = qc.artifact;
        result.semanticDigest = qc.firstFrame;
        result.artifactBytes = qc.bytes;
        result.observedTags = qc.colour;
        return result;
    } catch (const std::exception&) {
        return DpxExportWriteResultV1::failed(DpxExportWriteErrorCodeV1::ProviderFailed);
    }
}

} // namespace bloom::output
