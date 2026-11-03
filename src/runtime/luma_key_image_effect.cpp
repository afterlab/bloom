#include "luma_key_image_effect.hpp"
#include "cpu_composition_evaluator_support.hpp"
#include "image_effect.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>

namespace bloom::runtime::detail {
namespace {
// The Rec. 709 luma weights of render::imageCoverage's Luma selector (the track-matte Luma), as
// the binary32 values both routes multiply by.
constexpr float kRed = 0.2126F;
constexpr float kGreen = 0.7152F;
constexpr float kBlue = 0.0722F;

ImageEffectResult failed(const EvaluationDiagnosticCode code, const char* text) {
    ImageEffectResult result;
    result.diagnostic = EvaluationDiagnostic{code, DiagnosticSeverity::Error, {}, text, text};
    return result;
}

[[nodiscard]] std::string glslFloat(const float value) {
    char text[32];
    const auto length = std::snprintf(text, sizeof(text), "uintBitsToFloat(0x%08xu)",
                                      static_cast<unsigned>(std::bit_cast<std::uint32_t>(value)));
    return std::string(text, static_cast<std::size_t>(length));
}
} // namespace

std::optional<PreparedLumaKey> prepareLumaKey(const LumaKeyKernel& kernel,
                                              const std::optional<double> low,
                                              const std::optional<double> high) {
    if (!low || !high || !std::isfinite(*low) || !std::isfinite(*high) ||
        std::abs(*low) > document::kMaximumLumaKeyThreshold ||
        std::abs(*high) > document::kMaximumLumaKeyThreshold)
        return std::nullopt;
    const auto hard = [&] {
        return PreparedLumaKey{static_cast<float>(*low), 0.0F,
                               kernel.invert ? LumaKeyMode::HardInverted : LumaKeyMode::Hard};
    };
    if (!(*high > *low))
        return hard();
    const double slope = 1.0 / (*high - *low);
    const auto narrowed = static_cast<float>(slope);
    if (!std::isfinite(narrowed))
        return hard();
    return PreparedLumaKey{static_cast<float>(kernel.invert ? *high : *low),
                           kernel.invert ? -narrowed : narrowed, LumaKeyMode::Ramp};
}

float lumaKeyCoverage(const PreparedLumaKey& key, const float red, const float green,
                      const float blue, const float alpha) noexcept {
    const float luma = std::fma(kBlue, blue, std::fma(kGreen, green, kRed * red));
    const float d = std::fma(-key.pivot, alpha, luma);
    float coverage = 0.0F;
    switch (key.mode) {
    case LumaKeyMode::Ramp:
        coverage = std::min(std::max(d * key.slope, 0.0F), alpha);
        break;
    case LumaKeyMode::Hard:
        coverage = d >= 0.0F ? alpha : 0.0F;
        break;
    case LumaKeyMode::HardInverted:
        coverage = d < 0.0F ? alpha : 0.0F;
        break;
    }
    // Canonical zero, as the resident wrapper stores for zero coverage.
    return coverage == 0.0F ? 0.0F : coverage;
}

ImageEffectResult applyLumaKey(const PreparedLumaKey& key, const render::Rgba32fImage& input,
                               const std::size_t budget, const CancellationToken& cancellation,
                               const OperationIndex operation,
                               const EvaluationProgressCallback& progress) {
    if (cancellation.isCancellationRequested())
        return ImageEffectResult{{}, {}, true};
    auto builder = render::Rgba32fImageBuilder::create(*input.descriptor(), budget);
    if (!builder)
        return failed(EvaluationDiagnosticCode::PixelStorageBudgetExceeded,
                      "Luma Key output exceeds the pixel budget");
    const auto view = input.view();
    if (!view)
        return failed(EvaluationDiagnosticCode::InvalidPixel, "Luma Key input is invalid");
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
                          "Luma Key row is unavailable");
        for (std::size_t column = 0; column < source.value()->size(); ++column) {
            if ((column & 4095U) == 0 && cancellation.isCancellationRequested())
                return ImageEffectResult{{}, {}, true};
            const auto p = (*source.value())[column];
            const float coverage = lumaKeyCoverage(key, p.red(), p.green(), p.blue(), p.alpha());
            const auto pixel =
                render::Rgba32f::fromPremultiplied(coverage, coverage, coverage, coverage);
            if (!pixel)
                return failed(EvaluationDiagnosticCode::InvalidPixel,
                              "Luma Key produced an invalid coverage");
            (*target.value())[column] = *pixel.value();
        }
        reportProgress(progress, {EvaluationProgressStage::Operation, operation,
                                  static_cast<std::uint64_t>(y - window.originY() + 1), rows});
    }
    if (cancellation.isCancellationRequested())
        return ImageEffectResult{{}, {}, true};
    auto frozen = std::move(*builder.value()).freeze();
    if (!frozen)
        return failed(EvaluationDiagnosticCode::InvalidPixel, "Luma Key image publication failed");
    return {std::make_shared<const render::Rgba32fImage>(std::move(*frozen.value())), {}, false};
}

std::array<float, 4> lumaKeyProgramData(const PreparedLumaKey& key) noexcept {
    return {key.pivot, key.slope, static_cast<float>(static_cast<std::uint8_t>(key.mode)), 0.0F};
}

render::OcioGpuProgramResult buildLumaKeyProgram(const std::array<float, 4> data) {
    if (!std::isfinite(data[0]) || !std::isfinite(data[1]) ||
        (data[2] != 0.0F && data[2] != 1.0F && data[2] != 2.0F) || data[3] != 0.0F)
        return render::OcioGpuProgramResult::failure(render::OcioGpuProgramError::InvalidRequest);
    static const std::string text =
        "layout(set = 0, binding = 0, std430) readonly buffer BloomLumaKeyData { vec4 values[]; } "
        "bloom_luma_key_data;\n"
        "vec4 bloom_luma_key(ivec2 c) {\n"
        " vec4 p = imageLoad(bloom_ocio_input, c);\n"
        " vec4 k = bloom_luma_key_data.values[0];\n"
        " precise float luma = fma(" +
        glslFloat(kBlue) + ", p.b, fma(" + glslFloat(kGreen) + ", p.g, " + glslFloat(kRed) +
        " * p.r));\n"
        " precise float d = fma(-k.x, p.a, luma);\n"
        " float coverage = 0.0;\n"
        " if (k.z == 0.0) {\n"
        "  precise float v = d * k.y;\n"
        "  coverage = min(max(v, 0.0), p.a);\n"
        " } else if (k.z == 1.0) {\n"
        "  coverage = d >= 0.0 ? p.a : 0.0;\n"
        " } else {\n"
        "  coverage = d < 0.0 ? p.a : 0.0;\n"
        " }\n"
        " return vec4(coverage);\n"
        "}\n";
    render::OcioGpuProgramDesc program;
    program.stage = render::OcioGpuProgramStage::ProcessNeighborhood;
    program.functionName = "bloom_luma_key";
    program.resourcePrefix = "bloom_luma_key_";
    program.shaderText = text;
    program.semanticsId = "bloom.render.luma-key.v1";
    program.uniformBufferStorage = true;
    program.uniformBufferSize = sizeof(data);
    program.uniformBufferData.resize(sizeof(data));
    std::memcpy(program.uniformBufferData.data(), data.data(), sizeof(data));
    program.shaderTextDigest = render::computeOcioGpuShaderTextDigest(program.shaderText);
    program.resourceDigest = render::computeOcioGpuResourceDigest(program);
    program.contentIdentity = render::computeOcioGpuContentIdentity(
        {program.semanticsId, {}, {}, program.shaderTextDigest, program.resourceDigest});
    return render::OcioGpuProgramResult::success(std::move(program));
}
} // namespace bloom::runtime::detail
