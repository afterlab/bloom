#include "channel_image_effect.hpp"
#include "cpu_composition_evaluator_support.hpp"
#include "image_effect.hpp"
#include "operation_key.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <utility>

namespace bloom::runtime::detail {
namespace {

constexpr auto kLastSource = static_cast<std::uint8_t>(ImageChannelSource::One);

ImageEffectResult failed(const EvaluationDiagnosticCode code, const char* text) {
    ImageEffectResult result;
    result.diagnostic = EvaluationDiagnostic{code, DiagnosticSeverity::Error, {}, text, text};
    return result;
}

[[nodiscard]] float readSource(const render::Rgba32f pixel,
                               const ImageChannelSource source) noexcept {
    if (source == ImageChannelSource::Zero)
        return 0.0F;
    if (source == ImageChannelSource::One)
        return 1.0F;
    const auto channel = storedChannelOf(source);
    return channel ? render::storedImageChannel(pixel, *channel) : 0.0F;
}

// The resident program. Luma is the Rec. 709 luma of the stored colour in the same two-component
// Float32 arithmetic as ImageCombineV1's (the CPU reference's Float64 products, rounded once).
const std::string& channelSelectProgramText() {
    static const std::string text =
        "layout(set = 0, binding = 0, std430) readonly buffer BloomChannelSelectData { vec4 "
        "values[]; } bloom_channel_select_data;\n"
        "vec2 bloom_channel_two_sum(float a, float b) {\n"
        " precise float sum = a + b;\n"
        " precise float carried = sum - a;\n"
        " precise float error = (a - (sum - carried)) + (b - carried);\n"
        " return vec2(sum, error);\n"
        "}\n"
        "vec2 bloom_channel_add_wide(vec2 a, vec2 b) {\n"
        " vec2 sum = bloom_channel_two_sum(a.x, b.x);\n"
        " precise float error = sum.y + (a.y + b.y);\n"
        " return bloom_channel_two_sum(sum.x, error);\n"
        "}\n"
        "vec2 bloom_channel_scale_wide(vec2 a, float b) {\n"
        " precise float product = a.x * b;\n"
        " precise float residual = fma(a.x, b, -product);\n"
        " precise float error = residual + a.y * b;\n"
        " return bloom_channel_two_sum(product, error);\n"
        "}\n"
        "float bloom_channel_luma(vec3 rgb) {\n"
        " vec2 red = bloom_channel_scale_wide(vec2(0.2125999927520752, 7.247924815878548e-9), "
        "rgb.r);\n"
        " vec2 green = bloom_channel_scale_wide(vec2(0.7152000069618225, "
        "-6.9618225628786945e-9), rgb.g);\n"
        " vec2 blue = bloom_channel_scale_wide(vec2(0.0722000002861023, -2.86102294633217e-10), "
        "rgb.b);\n"
        " vec2 result = bloom_channel_add_wide(bloom_channel_add_wide(red, green), blue);\n"
        " precise float luma = result.x + result.y;\n"
        " return luma;\n"
        "}\n"
        "float bloom_channel_read(vec4 p, float source) {\n"
        " if (source == 0.0) { return p.r; }\n"
        " if (source == 1.0) { return p.g; }\n"
        " if (source == 2.0) { return p.b; }\n"
        " if (source == 3.0) { return p.a; }\n"
        " if (source == 4.0) { return bloom_channel_luma(p.rgb); }\n"
        " if (source == 5.0) { return 0.0; }\n"
        " return 1.0;\n"
        "}\n"
        "vec4 bloom_channel_select(ivec2 c) {\n"
        " vec4 p = imageLoad(bloom_ocio_input, c);\n"
        " vec4 m = bloom_channel_select_data.values[0];\n"
        " vec4 o = vec4(bloom_channel_read(p, m.x), bloom_channel_read(p, m.y), "
        "bloom_channel_read(p, m.z), bloom_channel_read(p, m.w));\n"
        " o.a = min(max(o.a, 0.0), 1.0);\n"
        " if (bloom_channel_select_data.values[1].x != 0.0) {\n"
        "  precise vec3 premultiplied = o.rgb * o.a;\n"
        "  o.rgb = premultiplied;\n"
        " }\n"
        " return o;\n"
        "}\n";
    return text;
}

} // namespace

std::optional<render::ImageCoverageChannel>
storedChannelOf(const ImageChannelSource source) noexcept {
    switch (source) {
    case ImageChannelSource::Red:
        return render::ImageCoverageChannel::Red;
    case ImageChannelSource::Green:
        return render::ImageCoverageChannel::Green;
    case ImageChannelSource::Blue:
        return render::ImageCoverageChannel::Blue;
    case ImageChannelSource::Alpha:
        return render::ImageCoverageChannel::Alpha;
    case ImageChannelSource::Luma:
        return render::ImageCoverageChannel::Luma;
    case ImageChannelSource::Zero:
    case ImageChannelSource::One:
        break;
    }
    return std::nullopt;
}

std::optional<PreparedChannelSelect>
prepareChannelSelect(const ChannelSelectKernel& kernel) noexcept {
    if (std::ranges::any_of(kernel.sources, [](const ImageChannelSource source) {
            return static_cast<std::uint8_t>(source) > kLastSource;
        }))
        return std::nullopt;
    const bool identity = !kernel.premultiply && kernel.sources[0] == ImageChannelSource::Red &&
                          kernel.sources[1] == ImageChannelSource::Green &&
                          kernel.sources[2] == ImageChannelSource::Blue &&
                          kernel.sources[3] == ImageChannelSource::Alpha;
    return PreparedChannelSelect{kernel.sources, kernel.premultiply, identity};
}

std::optional<PreparedChannelCombine>
prepareChannelCombine(const ChannelCombineKernel& kernel) noexcept {
    const auto channel = storedChannelOf(kernel.source);
    if (kernel.slot > 3 || !channel)
        return std::nullopt;
    return PreparedChannelCombine{kernel.slot, *channel, kernel.premultiply};
}

render::ImageResult<render::Rgba32f> channelSelectPixel(const PreparedChannelSelect& select,
                                                        const render::Rgba32f pixel) noexcept {
    std::array<float, 4> out{};
    for (std::size_t slot = 0; slot < out.size(); ++slot) {
        out[slot] = readSource(pixel, select.sources[slot]);
        if (!std::isfinite(out[slot]))
            return render::ImageResult<render::Rgba32f>::failure(
                render::ImageError::codeOnly(render::ImageErrorCode::NonFiniteResult));
    }
    out[3] = std::min(std::max(out[3], 0.0F), 1.0F);
    if (select.premultiply)
        for (std::size_t slot = 0; slot < 3; ++slot)
            out[slot] = out[slot] * out[3];
    return render::Rgba32f::fromPremultiplied(out[0], out[1], out[2], out[3]);
}

render::ImageResult<render::Rgba32f> channelCombinePixel(const PreparedChannelCombine& combine,
                                                         const render::Rgba32f base,
                                                         const render::Rgba32f second) noexcept {
    return render::setImageChannel(base, render::storedImageChannel(second, combine.source),
                                   combine.slot, combine.premultiply);
}

std::string channelSelectIdentity(const PreparedChannelSelect& select) {
    OperationKey key;
    key.add(std::string("channel-select-v1"));
    for (const auto source : select.sources)
        key.add(source);
    key.add(select.premultiply);
    return key.bytes();
}

std::string channelCombineIdentity(const PreparedChannelCombine& combine) {
    OperationKey key;
    key.add(std::string("channel-combine-v1"));
    key.add(static_cast<std::uint64_t>(combine.slot));
    key.add(combine.source);
    key.add(combine.premultiply);
    return key.bytes();
}

ImageEffectResult applyChannelSelect(const PreparedChannelSelect& select,
                                     const render::Rgba32fImage& input, const std::size_t budget,
                                     const CancellationToken& cancellation,
                                     const OperationIndex operation,
                                     const EvaluationProgressCallback& progress) {
    if (cancellation.isCancellationRequested())
        return ImageEffectResult{{}, {}, true};
    auto builder = render::Rgba32fImageBuilder::create(*input.descriptor(), budget);
    if (!builder)
        return failed(EvaluationDiagnosticCode::PixelStorageBudgetExceeded,
                      "Channel select output exceeds the pixel budget");
    const auto view = input.view();
    if (!view)
        return failed(EvaluationDiagnosticCode::InvalidPixel, "Channel select input is invalid");
    const auto window = input.descriptor()->dataWindow();
    const auto rows = window.extent().height();
    reportProgress(progress, {EvaluationProgressStage::Operation, operation, 0, rows});
    for (auto y = window.originY(); y < window.maxYExclusive(); ++y) {
        if (cancellation.isCancellationRequested())
            return ImageEffectResult{{}, {}, true};
        const auto source = view.value()->row(y);
        auto target = builder.value()->row(y);
        if (!source || !target)
            return failed(EvaluationDiagnosticCode::InternalInvariant,
                          "Channel select row is unavailable");
        for (std::size_t column = 0; column < source.value()->size(); ++column) {
            if ((column & 4095U) == 0 && cancellation.isCancellationRequested())
                return ImageEffectResult{{}, {}, true};
            const auto pixel = channelSelectPixel(select, (*source.value())[column]);
            if (!pixel)
                return failed(EvaluationDiagnosticCode::InvalidPixel,
                              "Channel select produced a non-finite value");
            (*target.value())[column] = *pixel.value();
        }
        reportProgress(progress, {EvaluationProgressStage::Operation, operation,
                                  static_cast<std::uint64_t>(y - window.originY() + 1), rows});
    }
    if (cancellation.isCancellationRequested())
        return ImageEffectResult{{}, {}, true};
    auto frozen = std::move(*builder.value()).freeze();
    if (!frozen)
        return failed(EvaluationDiagnosticCode::InvalidPixel,
                      "Channel select image publication failed");
    return {std::make_shared<const render::Rgba32fImage>(std::move(*frozen.value())), {}, false};
}

ImageEffectResult applyChannelCombine(const PreparedChannelCombine& combine,
                                      const render::Rgba32fImage& base,
                                      const render::Rgba32fImage* second, const std::size_t budget,
                                      const CancellationToken& cancellation,
                                      const OperationIndex operation,
                                      const EvaluationProgressCallback& progress) {
    if (cancellation.isCancellationRequested())
        return ImageEffectResult{{}, {}, true};
    if (second != nullptr && second->descriptor() == nullptr)
        return failed(EvaluationDiagnosticCode::InvalidPixel,
                      "Channel combine second input is unavailable");
    auto builder = render::Rgba32fImageBuilder::create(*base.descriptor(), budget);
    if (!builder)
        return failed(EvaluationDiagnosticCode::PixelStorageBudgetExceeded,
                      "Channel combine output exceeds the pixel budget");
    const auto view = base.view();
    if (!view)
        return failed(EvaluationDiagnosticCode::InvalidPixel, "Channel combine base is invalid");
    std::optional<render::Rgba32fImageView> secondView;
    std::optional<render::ImageWindow> secondWindow;
    if (second != nullptr) {
        auto fetched = second->view();
        if (!fetched)
            return failed(EvaluationDiagnosticCode::InvalidPixel,
                          "Channel combine second input is invalid");
        secondView.emplace(*fetched.value());
        secondWindow = second->descriptor()->dataWindow();
    }
    const auto window = base.descriptor()->dataWindow();
    const auto rows = window.extent().height();
    reportProgress(progress, {EvaluationProgressStage::Operation, operation, 0, rows});
    for (auto y = window.originY(); y < window.maxYExclusive(); ++y) {
        if (cancellation.isCancellationRequested())
            return ImageEffectResult{{}, {}, true};
        const auto source = view.value()->row(y);
        auto target = builder.value()->row(y);
        if (!source || !target)
            return failed(EvaluationDiagnosticCode::InternalInvariant,
                          "Channel combine row is unavailable");
        std::optional<std::span<const render::Rgba32f>> secondRow;
        if (secondWindow && y >= secondWindow->originY() && y < secondWindow->maxYExclusive()) {
            const auto fetched = secondView->row(y);
            if (!fetched)
                return failed(EvaluationDiagnosticCode::InternalInvariant,
                              "Channel combine second row is unavailable");
            secondRow = *fetched.value();
        }
        for (std::size_t column = 0; column < source.value()->size(); ++column) {
            if ((column & 4095U) == 0 && cancellation.isCancellationRequested())
                return ImageEffectResult{{}, {}, true};
            const auto x = window.originX() + static_cast<std::int64_t>(column);
            const auto tap =
                secondRow && x >= secondWindow->originX() && x < secondWindow->maxXExclusive()
                    ? (*secondRow)[static_cast<std::size_t>(x - secondWindow->originX())]
                    : render::Rgba32f::transparent();
            const auto pixel = channelCombinePixel(combine, (*source.value())[column], tap);
            if (!pixel)
                return failed(EvaluationDiagnosticCode::InvalidPixel,
                              "Channel combine produced a non-finite value");
            (*target.value())[column] = *pixel.value();
        }
        reportProgress(progress, {EvaluationProgressStage::Operation, operation,
                                  static_cast<std::uint64_t>(y - window.originY() + 1), rows});
    }
    if (cancellation.isCancellationRequested())
        return ImageEffectResult{{}, {}, true};
    auto frozen = std::move(*builder.value()).freeze();
    if (!frozen)
        return failed(EvaluationDiagnosticCode::InvalidPixel,
                      "Channel combine image publication failed");
    return {std::make_shared<const render::Rgba32fImage>(std::move(*frozen.value())), {}, false};
}

std::array<float, 8> channelSelectProgramData(const PreparedChannelSelect& select) {
    std::array<float, 8> data{};
    for (std::size_t slot = 0; slot < select.sources.size(); ++slot)
        data[slot] = static_cast<float>(static_cast<std::uint8_t>(select.sources[slot]));
    data[4] = select.premultiply ? 1.0F : 0.0F;
    return data;
}

render::OcioGpuProgramResult buildChannelSelectProgram(const std::array<float, 8>& data) {
    const bool valid = std::all_of(data.begin(), data.begin() + 4,
                                   [](const float value) {
                                       return value >= 0.0F &&
                                              value <= static_cast<float>(kLastSource) &&
                                              value == std::floor(value);
                                   }) &&
                       (data[4] == 0.0F || data[4] == 1.0F) && data[5] == 0.0F && data[6] == 0.0F &&
                       data[7] == 0.0F;
    if (!valid)
        return render::OcioGpuProgramResult::failure(render::OcioGpuProgramError::InvalidRequest);
    render::OcioGpuProgramDesc program;
    program.stage = render::OcioGpuProgramStage::ProcessNeighborhood;
    program.functionName = "bloom_channel_select";
    program.resourcePrefix = "bloom_channel_select_";
    program.semanticsId = "bloom.render.channel-select.v1";
    program.shaderText = channelSelectProgramText();
    program.uniformBufferStorage = true;
    program.uniformBufferSize = data.size() * sizeof(float);
    program.uniformBufferData.resize(data.size() * sizeof(float));
    std::memcpy(program.uniformBufferData.data(), data.data(), data.size() * sizeof(float));
    program.shaderTextDigest = render::computeOcioGpuShaderTextDigest(program.shaderText);
    program.resourceDigest = render::computeOcioGpuResourceDigest(program);
    program.contentIdentity = render::computeOcioGpuContentIdentity(
        {program.semanticsId, {}, {}, program.shaderTextDigest, program.resourceDigest});
    return render::OcioGpuProgramResult::success(std::move(program));
}

} // namespace bloom::runtime::detail
