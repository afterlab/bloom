// #527 Dither. The table holds the amplitude, the domain, the pattern and each channel's placement;
// the geometry is inserted where it is applied. docs/architecture/image-filters.md, "Dither", is the
// specification; dither_kernel.glsl is the arithmetic, the same text on both hosts.

#include "dither_image_effect.hpp"

#include "cpu_composition_evaluator_support.hpp"
#include "image_effect.hpp"

#include <bloom/core/dither_pattern.hpp>
#include <bloom/document/color_cleanup_nodes.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <limits>
#include <string_view>

namespace bloom::runtime::detail {

std::string_view ditherKernelGlsl() noexcept;

namespace dither_kernel {
namespace {

// The kernel's C++ dialect (dither_kernel.glsl, "Each host defines the dialect first").
struct Context final {
    const float* table = nullptr;
    std::size_t floats = 0;
};
thread_local Context context;

[[maybe_unused]] inline float min(const float a, const float b) noexcept { return b < a ? b : a; }
[[maybe_unused]] inline float max(const float a, const float b) noexcept { return a < b ? b : a; }
[[maybe_unused]] inline float abs(const float v) noexcept { return std::fabs(v); }
[[maybe_unused]] inline float floor(const float v) noexcept { return std::floor(v); }
[[maybe_unused]] inline float fma(const float a, const float b, const float c) noexcept {
    return std::fma(a, b, c);
}
[[maybe_unused]] inline float ldexp(const float v, const int e) noexcept { return std::ldexp(v, e); }
[[maybe_unused]] inline float frexp(const float v, int& e) noexcept { return std::frexp(v, &e); }
[[maybe_unused]] inline float bdk_data(const int index) noexcept {
    if (index < 0 || static_cast<std::size_t>(index) >= context.floats)
        return 0.0F;
    return context.table[static_cast<std::size_t>(index)];
}
[[maybe_unused]] inline float bdk_div(const float a, const float b) noexcept { return a / b; }

#define BDK_FN [[maybe_unused]] inline
#define BDK_P
#define BDK_INOUT(T) T&

#include "dither_kernel.glsl"

#undef BDK_FN
#undef BDK_P
#undef BDK_INOUT

} // namespace
} // namespace dither_kernel

namespace {

// The table's places (dither_kernel.glsl).
constexpr std::size_t kAmplitude = 0;
constexpr std::size_t kDomain = 1;
constexpr std::size_t kSide = 2;
constexpr std::size_t kInverseCells = 3;
constexpr std::size_t kOffsets = 4;
// Red's fourth place: the target depth's steps, 2^bits - 1.
constexpr std::size_t kSteps = 7;
constexpr std::size_t kGeometry = 16;
// A full-resolution pixel coordinate must fit an int on both devices, with room for the shift.
constexpr double kCoordinateLimit = 1073741824.0; // 2^30

ImageEffectResult failed(const EvaluationDiagnosticCode code, const char* text) {
    ImageEffectResult result;
    result.diagnostic =
        EvaluationDiagnostic{code, DiagnosticSeverity::Error, {}, std::string(text), {}};
    return result;
}

[[nodiscard]] bool isPowerOfTwo(const std::int64_t value) noexcept {
    return value > 0 && (value & (value - 1)) == 0;
}

} // namespace

std::optional<PreparedColorGrade> prepareDitherGrade(const DitherKernel& kernel,
                                                     const std::span<const double> values) {
    const bool bayer = !kernel.blueNoise;
    const auto side = static_cast<int>(kernel.side);
    if (values.size() != 2 || kernel.seed < 0 || kernel.seed > 2147483647 ||
        (bayer && !core::dither::isBayerSide(side)) ||
        (!bayer && kernel.side != core::dither::kBlueNoiseSide) ||
        kernel.bits < 1 || kernel.bits > 16)
        return std::nullopt;
    const double amplitude = values[0];
    const double frame = values[1];
    if (!std::isfinite(amplitude) || amplitude < 0.0 ||
        amplitude > document::kMaximumDitherAmplitude || !std::isfinite(frame))
        return std::nullopt;
    const auto cells = static_cast<std::size_t>(side) * static_cast<std::size_t>(side);
    PreparedColorGrade grade{ColorGradeKind::Dither,
                             std::vector<float>(kDitherHeaderFloats + cells, 0.0F), false};
    auto& d = grade.data;
    const double steps = std::ldexp(1.0, static_cast<int>(kernel.bits)) - 1.0;
    d[kAmplitude] = static_cast<float>(amplitude / steps);
    d[kDomain] = kernel.stored ? 1.0F : 0.0F;
    d[kSide] = static_cast<float>(side);
    d[kInverseCells] = static_cast<float>(1.0 / static_cast<double>(cells));
    d[kSteps] = static_cast<float>(steps);
    // Held draws frame 0's placement on every frame.
    const auto framed = kernel.everyFrame ? static_cast<std::int64_t>(frame) : 0;
    for (int channel = 0; channel < 3; ++channel) {
        const auto o = core::dither::offset(kernel.seed, framed, kernel.monochrome ? 0 : channel, side);
        const auto at = kOffsets + 4U * static_cast<std::size_t>(channel);
        d[at] = static_cast<float>(o.x);
        d[at + 1] = static_cast<float>(o.y);
        d[at + 2] = static_cast<float>(o.rank);
    }
    if (bayer) {
        const auto ranks = core::dither::bayerMatrix(side);
        std::ranges::transform(ranks, d.begin() + static_cast<std::ptrdiff_t>(kDitherHeaderFloats),
                               [](const std::uint16_t r) { return static_cast<float>(r); });
    } else {
        const auto ranks = core::dither::blueNoiseTable();
        if (ranks.size() != cells)
            return std::nullopt;
        std::ranges::transform(ranks, d.begin() + static_cast<std::ptrdiff_t>(kDitherHeaderFloats),
                               [](const std::uint16_t r) { return static_cast<float>(r); });
    }
    // Every amplitude quantizes to the target depth (#676), so the node is never the identity.
    grade.identity = false;
    return grade;
}

std::optional<std::vector<float>> ditherProgramData(const PreparedColorGrade& grade,
                                                    const render::ImageWindow& dataWindow,
                                                    const double horizontalScale,
                                                    const double verticalScale) {
    if (grade.kind != ColorGradeKind::Dither || grade.data.size() <= kDitherHeaderFloats ||
        !std::isfinite(horizontalScale) || !std::isfinite(verticalScale) ||
        !(horizontalScale > 0.0) || !(verticalScale > 0.0))
        return std::nullopt;
    const double inverseX = 1.0 / horizontalScale;
    const double inverseY = 1.0 / verticalScale;
    const double originX = static_cast<double>(dataWindow.originX()) + 0.5;
    const double originY = static_cast<double>(dataWindow.originY()) + 0.5;
    const auto lastX = originX + static_cast<double>(dataWindow.extent().width());
    const auto lastY = originY + static_cast<double>(dataWindow.extent().height());
    // The origin and every pixel index must be exact in a binary32 (2^22 with the half), and the
    // full-resolution position must fit an int.
    const double exact = 4194304.0;
    if (!std::isfinite(inverseX) || !std::isfinite(inverseY) ||
        inverseX > static_cast<double>(std::numeric_limits<float>::max()) ||
        inverseY > static_cast<double>(std::numeric_limits<float>::max()) ||
        std::abs(originX) > exact || std::abs(lastX) > exact || std::abs(originY) > exact ||
        std::abs(lastY) > exact || std::abs(originX) * inverseX > kCoordinateLimit ||
        std::abs(lastX) * inverseX > kCoordinateLimit ||
        std::abs(originY) * inverseY > kCoordinateLimit ||
        std::abs(lastY) * inverseY > kCoordinateLimit)
        return std::nullopt;
    auto data = grade.data;
    data.insert(data.begin() + static_cast<std::ptrdiff_t>(kGeometry),
                {static_cast<float>(originX), static_cast<float>(originY),
                 static_cast<float>(inverseX), static_cast<float>(inverseY)});
    return data;
}

bool validDitherProgramData(const std::vector<float>& d) noexcept {
    if (d.size() <= kDitherProgramHeaderFloats ||
        !std::ranges::all_of(d, [](const float value) { return std::isfinite(value); }))
        return false;
    const float sideValue = d[kSide];
    if (!(sideValue >= 2.0F) || sideValue > 64.0F || sideValue != std::floor(sideValue))
        return false;
    const auto side = static_cast<std::int64_t>(sideValue);
    const auto cells = static_cast<std::size_t>(side * side);
    if (!isPowerOfTwo(side) || d.size() != kDitherProgramHeaderFloats + cells ||
        d[kInverseCells] != static_cast<float>(1.0 / static_cast<double>(cells)) ||
        !(d[kAmplitude] >= 0.0F) || (d[kDomain] != 0.0F && d[kDomain] != 1.0F) ||
        !(d[kGeometry + 2] > 0.0F) || !(d[kGeometry + 3] > 0.0F))
        return false;
    const auto whole = [](const float value, const float limit) {
        return value >= 0.0F && value < limit && value == std::floor(value);
    };
    const float steps = d[kSteps];
    if (!(steps >= 1.0F) || steps > 65535.0F || steps != std::floor(steps) ||
        !isPowerOfTwo(static_cast<std::int64_t>(steps) + 1))
        return false;
    for (std::size_t channel = 0; channel < 3; ++channel) {
        const float* o = d.data() + kOffsets + 4U * channel;
        if (!whole(o[0], sideValue) || !whole(o[1], sideValue) ||
            !whole(o[2], static_cast<float>(cells)))
            return false;
    }
    return std::all_of(d.begin() + static_cast<std::ptrdiff_t>(kDitherProgramHeaderFloats), d.end(),
                       [&](const float rank) { return whole(rank, static_cast<float>(cells)); });
}

bool ditherPixel(const std::span<const float> data, const std::int64_t x, const std::int64_t y,
                 std::array<float, 4>& pixel) noexcept {
    dither_kernel::context = {data.data(), data.size()};
    dither_kernel::bdk_dither(static_cast<int>(x), static_cast<int>(y), pixel[0], pixel[1], pixel[2],
                              pixel[3]);
    return std::isfinite(pixel[0]) && std::isfinite(pixel[1]) && std::isfinite(pixel[2]);
}

const std::string& ditherProgramText() {
    // The one kernel, over the table as a storage buffer. The wrapper makes alpha 0 transparent black
    // and refuses a non-finite result.
    static const std::string text = [] {
        std::string out =
            "layout(set = 0, binding = 0, std430) readonly buffer BloomDitherData { vec4 values[]; "
            "} bloom_dither_data;\n"
            "precise float bloom_ocio_cpu_div(float a, float b);\n"
            "#define BDK_FN\n"
            "#define BDK_P precise\n"
            "#define BDK_INOUT(T) inout T\n"
            "float bdk_data(int i) { return bloom_dither_data.values[i >> 2][i & 3]; }\n"
            "precise float bdk_div(float a, float b) { return bloom_ocio_cpu_div(a, b); }\n";
        out += ditherKernelGlsl();
        out += "\nvec4 bloom_dither(ivec2 c) {\n"
               " vec4 p = imageLoad(bloom_ocio_input, c);\n"
               " float r = p.r;\n"
               " float g = p.g;\n"
               " float b = p.b;\n"
               " bdk_dither(c.x, c.y, r, g, b, p.a);\n"
               " return vec4(r, g, b, p.a);\n"
               "}\n";
        return out;
    }();
    return text;
}

ImageEffectResult applyDither(const PreparedColorGrade& grade, const render::Rgba32fImage& input,
                              const double horizontalScale, const double verticalScale,
                              const std::size_t budget, const CancellationToken& cancellation,
                              const OperationIndex operation,
                              const EvaluationProgressCallback& progress,
                              CpuRowBandExecutor* const rowBands) {
    if (cancellation.isCancellationRequested())
        return ImageEffectResult{{}, {}, true};
    const auto& descriptor = *input.descriptor();
    const auto window = descriptor.dataWindow();
    const auto table = ditherProgramData(grade, window, horizontalScale, verticalScale);
    if (!table)
        return failed(EvaluationDiagnosticCode::InvalidParameter,
                      "Dither geometry cannot be represented");
    auto builder = render::Rgba32fImageBuilder::create(descriptor, budget);
    if (!builder)
        return failed(EvaluationDiagnosticCode::PixelStorageBudgetExceeded,
                      "Dither output exceeds the pixel budget");
    const auto view = input.view();
    if (!view)
        return failed(EvaluationDiagnosticCode::InvalidPixel, "Dither image is invalid");
    auto& image = *builder.value();
    const auto& sourceView = *view.value();
    const std::span<const float> data(*table);
    const auto originY = window.originY();
    const auto total = static_cast<std::uint32_t>(window.extent().height());
    reportProgress(progress, {EvaluationProgressStage::Operation, operation, 0, total});
    const auto invalid = [](const char* const text) {
        return EvaluationDiagnostic{EvaluationDiagnosticCode::InvalidPixel,
                                    DiagnosticSeverity::Error,
                                    {},
                                    text,
                                    {}};
    };
    const auto outcome = runRowBandPass(
        rowBands, cancellation, total, originY,
        [&](const std::int64_t y) -> std::optional<EvaluationDiagnostic> {
            const auto source = sourceView.row(y);
            auto target = image.row(y);
            if (!source || !target)
                return EvaluationDiagnostic{EvaluationDiagnosticCode::InternalInvariant,
                                            DiagnosticSeverity::Error,
                                            {},
                                            "Dither row is unavailable",
                                            {}};
            for (std::size_t column = 0; column < source.value()->size(); ++column) {
                if ((column & 255U) == 0 && cancellation.isCancellationRequested())
                    return std::nullopt;
                const auto& s = (*source.value())[column];
                std::array<float, 4> p{s.red(), s.green(), s.blue(), s.alpha()};
                if (!ditherPixel(data, static_cast<std::int64_t>(column), y - originY, p))
                    return invalid("Dither produced a non-finite value");
                const auto pixel = render::Rgba32f::fromPremultiplied(p[0], p[1], p[2], p[3]);
                if (!pixel)
                    return invalid("Dither produced an invalid pixel");
                (*target.value())[column] = *pixel.value();
            }
            return std::nullopt;
        });
    if (outcome.cancelled || cancellation.isCancellationRequested())
        return ImageEffectResult{{}, {}, true};
    if (outcome.incomplete || outcome.failure) {
        ImageEffectResult result;
        result.diagnostic = outcome.failure ? *outcome.failure
                                            : EvaluationDiagnostic{
                                                  EvaluationDiagnosticCode::AllocationFailure,
                                                  DiagnosticSeverity::Error,
                                                  {},
                                                  "A Dither row band could not complete",
                                                  {}};
        return result;
    }
    reportProgress(progress, {EvaluationProgressStage::Operation, operation, total, total});
    auto frozen = std::move(image).freeze();
    if (!frozen)
        return failed(EvaluationDiagnosticCode::InvalidPixel, "Dither publication failed");
    return {std::make_shared<const render::Rgba32fImage>(std::move(*frozen.value())), {}, false};
}

} // namespace bloom::runtime::detail
