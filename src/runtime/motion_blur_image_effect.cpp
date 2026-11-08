// #484 Motion blur: the motion kernel's C++ dialect and CPU interpreter, its GPU program text, the
// shutter model, operand sampling and the prepared table. The pass planner is
// motion_blur_planner.cpp. docs/architecture/motion-blur.md is the specification.

#include "motion_blur_image_effect.hpp"

#include "cpu_composition_evaluator_support.hpp"
#include "cpu_composition_resolution.hpp"
#include "image_effect.hpp"

#include <bloom/document/motion_blur_nodes.hpp>
#include <bloom/runtime/composition_time.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <string_view>

namespace bloom::runtime::detail {

std::string_view motionKernelGlsl() noexcept;

// --- The kernel's C++ dialect (motion_kernel.glsl, "Each host defines the dialect first") -------

namespace motion_kernel {
namespace {

using uint = std::uint32_t;

struct vec2 final {
    float x = 0.0F;
    float y = 0.0F;
    constexpr vec2() noexcept = default;
    constexpr vec2(const float a, const float b) noexcept : x(a), y(b) {}
};

struct vec4 final {
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
    float w = 0.0F;
    constexpr vec4() noexcept = default;
    constexpr explicit vec4(const float v) noexcept : x(v), y(v), z(v), w(v) {}
    constexpr vec4(const float a, const float b, const float c, const float d) noexcept
        : x(a), y(b), z(c), w(d) {}
};

[[maybe_unused]] inline vec4 operator+(const vec4 a, const vec4 b) noexcept {
    return {a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w};
}
[[maybe_unused]] inline vec4 operator-(const vec4 a, const vec4 b) noexcept {
    return {a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w};
}
[[maybe_unused]] inline vec4 operator*(const vec4 a, const vec4 b) noexcept {
    return {a.x * b.x, a.y * b.y, a.z * b.z, a.w * b.w};
}
[[maybe_unused]] inline vec4 operator*(const vec4 a, const float s) noexcept {
    return {a.x * s, a.y * s, a.z * s, a.w * s};
}
[[maybe_unused]] inline vec4 operator*(const float s, const vec4 a) noexcept { return a * s; }

// GLSL's min and max (no NaN reaches them: every register is finite and limited).
[[maybe_unused]] inline float min(const float a, const float b) noexcept { return b < a ? b : a; }
[[maybe_unused]] inline float max(const float a, const float b) noexcept { return a < b ? b : a; }
[[maybe_unused]] inline int min(const int a, const int b) noexcept { return b < a ? b : a; }
[[maybe_unused]] inline int max(const int a, const int b) noexcept { return a < b ? b : a; }
[[maybe_unused]] inline vec4 min(const vec4 a, const vec4 b) noexcept {
    return {min(a.x, b.x), min(a.y, b.y), min(a.z, b.z), min(a.w, b.w)};
}
[[maybe_unused]] inline vec4 max(const vec4 a, const vec4 b) noexcept {
    return {max(a.x, b.x), max(a.y, b.y), max(a.z, b.z), max(a.w, b.w)};
}
[[maybe_unused]] inline float abs(const float v) noexcept { return std::fabs(v); }
[[maybe_unused]] inline float floor(const float v) noexcept { return std::floor(v); }
[[maybe_unused]] inline float sqrt(const float v) noexcept { return std::sqrt(v); }
[[maybe_unused]] inline bool isnan(const float v) noexcept { return std::isnan(v); }
[[maybe_unused]] inline bool isinf(const float v) noexcept { return std::isinf(v); }
[[maybe_unused]] inline float fma(const float a, const float b, const float c) noexcept {
    return std::fma(a, b, c);
}
[[maybe_unused]] inline vec4 fma(const vec4 a, const vec4 b, const vec4 c) noexcept {
    return {std::fma(a.x, b.x, c.x), std::fma(a.y, b.y, c.y), std::fma(a.z, b.z, c.z),
            std::fma(a.w, b.w, c.w)};
}

struct Context final {
    const float* table = nullptr;
    std::size_t tableFloats = 0;
    const float* atlas = nullptr;
    std::int64_t width = 0;
    std::int64_t height = 0;
};
thread_local Context context;

[[maybe_unused]] inline vec4 bmk_data(const int index) noexcept {
    if (index < 0)
        return {};
    const auto offset = static_cast<std::size_t>(index) * 4U;
    if (offset + 4U > context.tableFloats)
        return {};
    const float* t = context.table + offset;
    return {t[0], t[1], t[2], t[3]};
}

[[maybe_unused]] inline vec4 bmk_texel(const int x, const int y) noexcept {
    if (x < 0 || y < 0 || x >= context.width || y >= context.height)
        return {};
    const float* p = context.atlas + (static_cast<std::size_t>(y) *
                                          static_cast<std::size_t>(context.width) +
                                      static_cast<std::size_t>(x)) *
                                         4U;
    return {p[0], p[1], p[2], p[3]};
}

[[maybe_unused]] inline float bmk_div(const float a, const float b) noexcept { return a / b; }
[[maybe_unused]] inline uint bmk_bits(const float f) noexcept { return std::bit_cast<uint>(f); }
[[maybe_unused]] inline float bmk_float(const uint u) noexcept { return std::bit_cast<float>(u); }

#define BMK_FN [[maybe_unused]] inline
#define BMK_P
#define BMK_OUT(T) T&
#define BMK_INOUT(T) T&

#define BMK_IS_PASS(p) (pass == (p))
#include "motion_kernel.glsl"
#undef BMK_IS_PASS

#undef BMK_FN
#undef BMK_P
#undef BMK_OUT
#undef BMK_INOUT

} // namespace
} // namespace motion_kernel

namespace {

ImageEffectResult failed(const EvaluationDiagnosticCode code, const char* text) {
    ImageEffectResult result;
    result.diagnostic = EvaluationDiagnostic{code, DiagnosticSeverity::Error, {}, text, {}};
    return result;
}

constexpr int kLastPass = 26;

// One evaluated pixel as the GPU wrapper stores it: finite with alpha in [0, 1], else invalid;
// alpha 0 stored as zero.
[[nodiscard]] bool storePixel(const motion_kernel::vec4 p, float* out) noexcept {
    if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z) ||
        !std::isfinite(p.w) || p.w < 0.0F || p.w > 1.0F)
        return false;
    if (p.w == 0.0F) {
        out[0] = out[1] = out[2] = out[3] = 0.0F;
        return true;
    }
    out[0] = p.x;
    out[1] = p.y;
    out[2] = p.z;
    out[3] = p.w;
    return true;
}

// Splits a double into two floats whose sum is it to about 48 bits (the identity of a constant).
void appendDouble(std::vector<float>& data, const double value) {
    const auto high = static_cast<float>(value);
    data.push_back(high);
    data.push_back(static_cast<float>(value - static_cast<double>(high)));
}

void appendShutter(std::vector<float>& data, const CompiledShutter& shutter) {
    appendDouble(data, shutter.shutter);
    data.push_back(static_cast<float>(shutter.offset));
    appendDouble(data, shutter.phase);
    data.push_back(static_cast<float>(shutter.curve));
    const auto& customPoints = shutter.customCurve.firstContour().anchors;
    data.push_back(static_cast<float>(customPoints.size()));
    for (const auto& anchor : customPoints) {
        appendDouble(data, anchor.point.x);
        appendDouble(data, anchor.point.y);
    }
    data.push_back(static_cast<float>(shutter.quality));
    data.push_back(static_cast<float>(shutter.minSamples));
    data.push_back(static_cast<float>(shutter.maxSamples));
    data.push_back(shutter.jitter ? 1.0F : 0.0F);
    appendDouble(data, shutter.maxBlur);
    data.push_back(static_cast<float>(shutter.rate.numerator()));
    data.push_back(static_cast<float>(shutter.rate.denominator()));
}

[[nodiscard]] bool validShutter(const CompiledShutter& shutter) noexcept {
    return std::isfinite(shutter.shutter) && shutter.shutter >= 0.0 && shutter.shutter <= 4.0 &&
           shutter.offset >= document::kShutterOffsetCentred &&
           shutter.offset <= document::kShutterOffsetCustom && std::isfinite(shutter.phase) &&
           std::abs(shutter.phase) <= 4.0 && shutter.curve >= document::kShutterCurveBox &&
           shutter.curve <= document::kShutterCurveCustom &&
           shutter.quality >= document::kMotionQualityDraft &&
           shutter.quality <= document::kMotionQualityFinal && shutter.minSamples >= 1 &&
           shutter.maxSamples >= shutter.minSamples && shutter.maxSamples <= kMotionMaxSamples &&
           std::isfinite(shutter.maxBlur) && shutter.maxBlur >= 0.0 && shutter.maxBlur <= 4096.0 &&
           shutter.rate.numerator() > 0 && shutter.rate.denominator() > 0;
}

[[nodiscard]] std::int64_t powerOfTwoAtLeast(const std::int64_t value) noexcept {
    std::int64_t p = 1;
    while (p < value && p < kMotionMaxSamples)
        p *= 2;
    return p;
}

} // namespace

// --- Operands ---------------------------------------------------------------------------------------

namespace {

// The number of shutter rows a Transform path samples: the 2P + 1 points of P strata's starts,
// centres and ends, P the most samples its quality takes (a power of two for Final, so every
// power-of-two count's strata fall on rows).
[[nodiscard]] std::int64_t transformGridStrata(const CompiledShutter& shutter) noexcept {
    return shutter.quality == document::kMotionQualityFinal
               ? std::max(powerOfTwoAtLeast(shutter.maxSamples),
                          powerOfTwoAtLeast(shutter.minSamples))
               : motionQualitySamples(shutter.quality);
}

template <typename Parameter> [[nodiscard]] bool constantParameter(const Parameter& parameter) {
    return parameter.source.index() == 0;
}

} // namespace

std::optional<std::vector<double>>
sampleMotionOperands(const ImageEffectKernel& kernel, const CompiledCompositionPlan& plan,
                     const ResolvedEvaluation& resolved,
                     const std::optional<core::RationalTime> time) {
    if (!isMotionKernel(kernel) || !time)
        return std::nullopt;
    std::vector<double> values;
    values.push_back(time->toSeconds());
    // The frame index on the composition's frame grid (the jitter's seed).
    const auto* path = std::get_if<MotionPathBlurKernel>(&kernel);
    const auto* vector = std::get_if<VectorBlurKernel>(&kernel);
    const document::FrameRate rate = path      ? path->shutter.rate
                                     : vector ? vector->shutter.rate
                                               : plan.format().frameRate();
    const auto frames = static_cast<long double>(time->numerator()) *
                        static_cast<long double>(rate.numerator()) /
                        (static_cast<long double>(time->denominator()) *
                         static_cast<long double>(rate.denominator()));
    values.push_back(std::isfinite(static_cast<double>(frames))
                         ? static_cast<double>(std::floor(frames))
                         : 0.0);
    if (path == nullptr)
        return values;
    bool evaluable = true;
    const auto push = [&](const auto& parameter, const ResolvedEvaluation& at) {
        const auto sampled = resolveParameter(parameter, plan, at);
        if (!sampled) {
            evaluable = false;
            return;
        }
        using Value = std::decay_t<decltype(sampled->value)>;
        if constexpr (std::is_same_v<Value, document::Vec2d>) {
            values.push_back(sampled->value.x);
            values.push_back(sampled->value.y);
        } else {
            values.push_back(sampled->value);
        }
    };
    push(path->angle, resolved);
    push(path->length, resolved);
    push(path->centre, resolved);
    push(path->amount, resolved);
    const auto pushTransform = [&](const Transform2DKernel& transform,
                                   const ResolvedEvaluation& at) {
        push(transform.translate, at);
        push(transform.pivot, at);
        push(transform.scale, at);
        push(transform.rotation, at);
    };
    if (!path->transform) {
        for (int i = 0; i < 7; ++i)
            values.push_back(0.0);
        values.push_back(0.0);
        return evaluable ? std::optional{std::move(values)} : std::nullopt;
    }
    const auto& transform = *path->transform;
    pushTransform(transform, resolved);
    const bool animated = !constantParameter(transform.translate) ||
                          !constantParameter(transform.pivot) ||
                          !constantParameter(transform.scale) ||
                          !constantParameter(transform.rotation);
    if (!animated) {
        values.push_back(0.0);
        return evaluable ? std::optional{std::move(values)} : std::nullopt;
    }
    // The transform at each shutter row's time: the operation's clock time plus the row's offset.
    const auto strata = transformGridStrata(path->shutter);
    const auto rows = 2 * strata + 1;
    values.push_back(static_cast<double>(rows));
    const auto [open, close] = shutterInterval(path->shutter);
    const double frameSeconds =
        static_cast<double>(rate.denominator()) / static_cast<double>(rate.numerator());
    const CancellationToken token;
    for (std::int64_t row = 0; row < rows; ++row) {
        const double offset = open + (close - open) * static_cast<double>(row) /
                                         static_cast<double>(rows - 1);
        const auto at = secondsToTime(time->toSeconds() + offset * frameSeconds);
        if (!at)
            return std::nullopt;
        bool cancelled = false;
        const auto sampled = sampleParametersAt(plan, *at, token, cancelled);
        if (!sampled || cancelled)
            return std::nullopt;
        pushTransform(transform, *sampled);
    }
    return evaluable ? std::optional{std::move(values)} : std::nullopt;
}

// --- The prepared table -------------------------------------------------------------------------
//
// [variant (1 accumulate, 2 path, 3 vector stage, 4 optical flow), frame index, operands offset,
// row count], the kernel's constants, then (a path) at the operands offset [angle, length, centre x,
// centre y, amount, translate x, y, pivot x, y, scale x, y, rotation] and the rows of seven.

std::optional<PreparedColorGrade> prepareMotionGrade(const ImageEffectKernel& kernel,
                                                     const std::span<const double> values) {
    if (!isMotionKernel(kernel) || values.size() < 2 ||
        !std::ranges::all_of(values, [](const double v) { return std::isfinite(v); }))
        return std::nullopt;
    std::vector<float> data(4, 0.0F);
    data[1] = static_cast<float>(std::fmod(values[1], 16777216.0));
    if (const auto* accumulate = std::get_if<TemporalAccumulateKernel>(&kernel)) {
        if (static_cast<std::uint8_t>(accumulate->op) >
                static_cast<std::uint8_t>(AccumulateOperator::Under) ||
            !std::isfinite(accumulate->inputWeight) || !std::isfinite(accumulate->sampleWeight) ||
            std::abs(accumulate->inputWeight) > 1.0e6 || std::abs(accumulate->sampleWeight) > 1.0e6)
            return std::nullopt;
        data[0] = 1.0F;
        data.push_back(static_cast<float>(accumulate->op));
        appendDouble(data, accumulate->inputWeight);
        appendDouble(data, accumulate->sampleWeight);
    } else if (const auto* path = std::get_if<MotionPathBlurKernel>(&kernel)) {
        if (!validShutter(path->shutter) ||
            static_cast<std::uint8_t>(path->mode) >
                static_cast<std::uint8_t>(MotionPathMode::Transform) ||
            (path->mode == MotionPathMode::Transform) != path->transform.has_value() ||
            (path->transform && !validTransform2DKernel(*path->transform)) || values.size() < 15)
            return std::nullopt;
        const auto rows = static_cast<std::size_t>(values[14]);
        if (values[14] < 0.0 || values.size() != 15 + 7 * rows ||
            std::abs(values[3]) > 1.0e6 || std::abs(values[4]) > 1.0e6 ||
            std::abs(values[5]) > 1.0e6 || std::abs(values[6]) > 1.0e6)
            return std::nullopt;
        if ((path->mode == MotionPathMode::Spin && std::abs(values[6]) > 3600.0) ||
            (path->mode == MotionPathMode::Zoom && std::abs(values[6]) > 16.0))
            return std::nullopt;
        data[0] = 2.0F;
        data.push_back(static_cast<float>(path->mode));
        appendShutter(data, path->shutter);
        data.push_back(static_cast<float>(path->seed & 0xFFFFFFU));
        if (path->transform) {
            data.push_back(static_cast<float>(path->transform->extent));
            data.push_back(static_cast<float>(path->transform->filter));
        }
        while (data.size() % 4 != 0)
            data.push_back(0.0F);
        data[2] = static_cast<float>(data.size());
        data[3] = static_cast<float>(rows);
        for (std::size_t i = 2; i < 14; ++i)
            data.push_back(static_cast<float>(values[i]));
        for (std::size_t i = 15; i < values.size(); ++i)
            data.push_back(static_cast<float>(values[i]));
        // A spatial path with no extent is the identity.
        const bool still = (path->mode == MotionPathMode::Directional && values[3] == 0.0) ||
                           ((path->mode == MotionPathMode::Spin ||
                             path->mode == MotionPathMode::Zoom) &&
                            values[6] == 0.0) ||
                           path->shutter.shutter == 0.0;
        while (data.size() % 4 != 0)
            data.push_back(0.0F);
        return PreparedColorGrade{ColorGradeKind::Motion, std::move(data),
                                  still && path->mode != MotionPathMode::Transform};
    } else if (const auto* vector = std::get_if<VectorBlurKernel>(&kernel)) {
        if (!validShutter(vector->shutter) ||
            static_cast<std::uint8_t>(vector->stage) >
                static_cast<std::uint8_t>(VectorBlurStage::Consistency) ||
            !std::isfinite(vector->decode.scale) || !std::isfinite(vector->decode.offset) ||
            vector->decode.layout < 0 || vector->decode.layout > 2 || vector->decode.units < 0 ||
            vector->decode.units > 2 || vector->depthChannel < 0 || vector->depthChannel > 3 ||
            !std::isfinite(vector->softZ) || !(vector->softZ > 0.0) || vector->zMode < 0 ||
            vector->zMode > 2 || vector->layer < 0 || vector->layer > 2 || vector->border < 0 ||
            vector->border > 2 || !std::isfinite(vector->brightness) ||
            !std::isfinite(vector->threshold))
            return std::nullopt;
        data[0] = 3.0F;
        data.push_back(static_cast<float>(vector->stage));
        appendDouble(data, vector->decode.scale);
        appendDouble(data, vector->decode.offset);
        data.push_back(vector->decode.flipY ? 1.0F : 0.0F);
        data.push_back(static_cast<float>(vector->decode.layout));
        data.push_back(static_cast<float>(vector->decode.units));
        data.push_back(vector->decode.alphaValid ? 1.0F : 0.0F);
        appendShutter(data, vector->shutter);
        data.push_back(vector->curved ? 1.0F : 0.0F);
        data.push_back(static_cast<float>(vector->depthChannel));
        appendDouble(data, vector->softZ);
        data.push_back(static_cast<float>(vector->zMode));
        data.push_back(static_cast<float>(vector->layer));
        data.push_back(static_cast<float>(vector->border));
        appendDouble(data, vector->brightness);
        appendDouble(data, vector->threshold);
        for (const auto slot : vector->slotImages)
            data.push_back(slot >= 0 ? 1.0F : 0.0F);
        data.push_back(static_cast<float>(vector->seed & 0xFFFFFFU));
    } else if (const auto* flow = std::get_if<OpticalFlowKernel>(&kernel)) {
        if (static_cast<std::uint8_t>(flow->stage) >
                static_cast<std::uint8_t>(OpticalFlowStage::CutDetect) ||
            flow->method < 0 || flow->method > 1 || flow->quality < document::kMotionQualityDraft ||
            flow->quality > document::kMotionQualityFinal || flow->border < 0 || flow->border > 2 ||
            flow->layer < 0 || flow->layer > 2 || !std::isfinite(flow->cutThreshold))
            return std::nullopt;
        data[0] = 4.0F;
        data.push_back(static_cast<float>(flow->stage));
        data.push_back(static_cast<float>(flow->method));
        data.push_back(static_cast<float>(flow->quality));
        data.push_back(static_cast<float>(flow->border));
        data.push_back(static_cast<float>(flow->layer));
        appendDouble(data, flow->cutThreshold);
        for (const auto slot : flow->slotImages)
            data.push_back(slot >= 0 ? 1.0F : 0.0F);
        // A flow reads no frame index or time: it is the same at every frame of the same images.
        data[1] = 0.0F;
    }
    while (data.size() % 4 != 0)
        data.push_back(0.0F);
    return PreparedColorGrade{ColorGradeKind::Motion, std::move(data), false};
}

bool validMotionProgramData(const std::vector<float>& data) noexcept {
    if (data.size() < 4 * 20 || data.size() > kMotionTableFloats || data.size() % 4 != 0 ||
        !std::ranges::all_of(data, [](const float v) { return std::isfinite(v); }))
        return false;
    const float pass = data[0];
    return pass >= 1.0F && pass <= static_cast<float>(kLastPass) && pass == std::floor(pass) &&
           data[3] >= 0.0F && data[4] >= 0.0F;
}

const std::string& motionProgramText(const int pass) {
    static std::array<std::string, kLastPass + 1> texts;
    static std::array<std::once_flag, kLastPass + 1> built;
    const auto index = static_cast<std::size_t>(std::clamp(pass, 0, kLastPass));
    std::call_once(built[index], [&] {
        std::string out =
            "layout(set = 0, binding = 0, std430) readonly buffer BloomMotionData { vec4 values[]; "
            "} bloom_motion_data;\n"
            "precise float bloom_ocio_cpu_div(float a, float b);\n"
            "#define BMK_FN\n"
            "#define BMK_P precise\n"
            "#define BMK_OUT(T) out T\n"
            "#define BMK_INOUT(T) inout T\n"
            "#define BMK_IS_PASS(p) (BMK_ONLY_PASS == (p))\n";
        out += "const int BMK_ONLY_PASS = " + std::to_string(index) + ";\n";
        out += "vec4 bmk_data(int i) {\n"
               " if (i < 0 || i >= bloom_motion_data.values.length()) { return vec4(0.0); }\n"
               " return bloom_motion_data.values[i];\n"
               "}\n"
               "vec4 bmk_texel(int x, int y) {\n"
               " ivec2 size = imageSize(bloom_ocio_input);\n"
               " if (x < 0 || y < 0 || x >= size.x || y >= size.y) { return vec4(0.0); }\n"
               " return imageLoad(bloom_ocio_input, ivec2(x, y));\n"
               "}\n"
               "precise float bmk_div(float a, float b) { return bloom_ocio_cpu_div(a, b); }\n"
               "uint bmk_bits(float f) { return floatBitsToUint(f); }\n"
               "float bmk_float(uint u) { return uintBitsToFloat(u); }\n";
        out += motionKernelGlsl();
        out += "\nvec4 bloom_motion(ivec2 c) { return bmk_main(c.x, c.y); }\n";
        texts[index] = std::move(out);
    });
    return texts[index];
}

std::array<float, 4> motionKernelPixel(const std::span<const float> table,
                                       const std::span<const float> atlas, const std::int64_t width,
                                       const std::int64_t height, const int x, const int y) {
    motion_kernel::context = {table.data(), table.size(), atlas.data(), width, height};
    const auto p = motion_kernel::bmk_main(x, y);
    return {p.x, p.y, p.z, p.w};
}

// --- The CPU interpreter --------------------------------------------------------------------------

ImageEffectResult applyMotion(const ImageEffectKernel& kernel, const PreparedColorGrade& grade,
                              const std::span<const render::Rgba32fImage* const> inputs,
                              const MotionGeometry& geometry, const std::size_t budget,
                              const CancellationToken& cancellation, const OperationIndex operation,
                              const EvaluationProgressCallback& progress,
                              CpuRowBandExecutor* const rowBands) {
    if (cancellation.isCancellationRequested())
        return ImageEffectResult{{}, {}, true};
    if (grade.kind != ColorGradeKind::Motion || inputs.size() != motionInputCount(kernel))
        return failed(EvaluationDiagnosticCode::InternalInvariant, "Motion inputs are malformed");
    std::vector<MotionRect> windows;
    windows.reserve(inputs.size());
    for (const auto* image : inputs) {
        if (image == nullptr) {
            windows.push_back({});
            continue;
        }
        const auto w = image->descriptor()->dataWindow();
        windows.push_back({w.originX(), w.originY(), static_cast<std::int64_t>(w.extent().width()),
                           static_cast<std::int64_t>(w.extent().height())});
    }
    const auto plan = planMotion(kernel, grade, windows, geometry);
    if (!plan)
        return failed(EvaluationDiagnosticCode::InvalidParameter,
                      "Motion blur geometry cannot be represented");
    if (!plan->output) {
        ImageEffectResult empty;
        empty.empty = true;
        return empty;
    }
    // The buffers: an input reads its image's rows; every other buffer owns its pixels.
    struct Buffer final {
        MotionRect window{};
        std::vector<float> pixels;
        const render::Rgba32fImage* image = nullptr;
    };
    std::vector<Buffer> buffers(plan->buffers);
    std::vector<render::Rgba32fImageView> views;
    views.reserve(inputs.size());
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        buffers[i].window = windows[i];
        buffers[i].image = inputs[i];
    }
    // When each buffer is last read, so it can be released.
    std::vector<std::size_t> lastUse(plan->buffers, 0);
    for (std::size_t s = 0; s < plan->steps.size(); ++s) {
        const auto& step = plan->steps[s];
        if (step.kind == MotionStepKind::Run)
            lastUse[step.source] = s;
        for (const auto& placement : step.sources)
            lastUse[placement.buffer] = s;
    }
    lastUse[*plan->output] = plan->steps.size();
    std::size_t live = 0;
    const auto release = [&](const std::size_t s) {
        for (std::size_t b = inputs.size(); b < buffers.size(); ++b)
            if (lastUse[b] == s && !buffers[b].pixels.empty()) {
                live -= buffers[b].pixels.size() * sizeof(float);
                buffers[b].pixels = {};
            }
    };
    const auto allocate = [&](Buffer& buffer, const MotionRect window) -> bool {
        const auto count = static_cast<std::size_t>(window.width) *
                           static_cast<std::size_t>(window.height) * 4U;
        if (window.width <= 0 || window.height <= 0 ||
            count / 4U / static_cast<std::size_t>(window.width) !=
                static_cast<std::size_t>(window.height))
            return false;
        const auto bytes = count * sizeof(float);
        if (live > budget || bytes > budget - live)
            return false;
        buffer.window = window;
        buffer.pixels.assign(count, 0.0F);
        live += bytes;
        return true;
    };
    const auto total = static_cast<std::uint32_t>(plan->steps.size());
    reportProgress(progress, {EvaluationProgressStage::Operation, operation, 0, total});
    for (std::size_t s = 0; s < plan->steps.size(); ++s) {
        if (cancellation.isCancellationRequested())
            return ImageEffectResult{{}, {}, true};
        const auto& step = plan->steps[s];
        auto& target = buffers[step.target];
        if (step.kind == MotionStepKind::Place) {
            if (!allocate(target, step.window))
                return failed(EvaluationDiagnosticCode::PixelStorageBudgetExceeded,
                              "Motion blur exceeds the pixel budget");
            for (const auto& placement : step.sources) {
                const auto& source = buffers[placement.buffer];
                const auto& sw = source.window;
                if (sw.empty())
                    continue;
                // The source's pixels moved by the shift, clipped to the target.
                const auto x0 = std::max(step.window.x, sw.x + placement.shiftX);
                const auto y0 = std::max(step.window.y, sw.y + placement.shiftY);
                const auto x1 =
                    std::min(step.window.x + step.window.width, sw.x + sw.width + placement.shiftX);
                const auto y1 = std::min(step.window.y + step.window.height,
                                         sw.y + sw.height + placement.shiftY);
                std::optional<render::Rgba32fImageView> view;
                if (source.image != nullptr) {
                    auto created = source.image->view();
                    if (!created)
                        return failed(EvaluationDiagnosticCode::InvalidPixel,
                                      "Motion blur image is invalid");
                    view = *created.value();
                }
                for (auto y = y0; y < y1; ++y) {
                    float* out = target.pixels.data() +
                                 (static_cast<std::size_t>(y - step.window.y) *
                                      static_cast<std::size_t>(step.window.width) +
                                  static_cast<std::size_t>(x0 - step.window.x)) *
                                     4U;
                    const auto sy = y - placement.shiftY;
                    if (view) {
                        const auto row = view->row(sy);
                        if (!row)
                            return failed(EvaluationDiagnosticCode::InvalidPixel,
                                          "Motion blur row is unavailable");
                        for (auto x = x0; x < x1; ++x, out += 4) {
                            const auto& p = (*row.value())[static_cast<std::size_t>(
                                x - placement.shiftX - sw.x)];
                            out[0] = p.red();
                            out[1] = p.green();
                            out[2] = p.blue();
                            out[3] = p.alpha();
                        }
                    } else {
                        const float* in =
                            source.pixels.data() +
                            (static_cast<std::size_t>(sy - sw.y) *
                                 static_cast<std::size_t>(sw.width) +
                             static_cast<std::size_t>(x0 - placement.shiftX - sw.x)) *
                                4U;
                        std::memcpy(out, in, static_cast<std::size_t>(x1 - x0) * 4U * sizeof(float));
                    }
                }
            }
        } else {
            const auto& source = buffers[step.source];
            if (source.pixels.empty())
                return failed(EvaluationDiagnosticCode::InternalInvariant,
                              "Motion blur pass reads no pixels");
            if (!allocate(target, source.window))
                return failed(EvaluationDiagnosticCode::PixelStorageBudgetExceeded,
                              "Motion blur exceeds the pixel budget");
            const auto& w = source.window;
            const float* atlas = source.pixels.data();
            float* out = target.pixels.data();
            const auto& table = step.table;
            // Outside the active rectangle every pixel is copied through.
            const auto ax = static_cast<std::int64_t>(table[1]);
            const auto ay = static_cast<std::int64_t>(table[2]);
            const auto aw = static_cast<std::int64_t>(table[3]);
            const auto ah = static_cast<std::int64_t>(table[4]);
            const auto outcome = runRowBandPass(
                rowBands, cancellation, static_cast<std::uint32_t>(w.height), 0,
                [&](const std::int64_t y) -> std::optional<EvaluationDiagnostic> {
                    motion_kernel::context = {table.data(), table.size(), atlas, w.width,
                                              w.height};
                    const auto rowOffset = static_cast<std::size_t>(y) *
                                           static_cast<std::size_t>(w.width) * 4U;
                    const bool rowActive = y >= ay && y < ay + ah;
                    for (std::int64_t x = 0; x < w.width; ++x) {
                        const auto o = rowOffset + static_cast<std::size_t>(x) * 4U;
                        if (!rowActive || x < ax || x >= ax + aw) {
                            std::memcpy(out + o, atlas + o, 4U * sizeof(float));
                            continue;
                        }
                        if ((x & 255) == 0 && cancellation.isCancellationRequested())
                            return std::nullopt;
                        const auto p = motion_kernel::bmk_main(static_cast<int>(x),
                                                               static_cast<int>(y));
                        if (!storePixel(p, out + o))
                            return EvaluationDiagnostic{EvaluationDiagnosticCode::InvalidPixel,
                                                        DiagnosticSeverity::Error,
                                                        {},
                                                        "Motion blur produced an invalid pixel",
                                                        {}};
                    }
                    return std::nullopt;
                });
            if (outcome.cancelled || cancellation.isCancellationRequested())
                return ImageEffectResult{{}, {}, true};
            if (outcome.incomplete || outcome.failure) {
                ImageEffectResult result;
                result.diagnostic = outcome.failure
                                        ? *outcome.failure
                                        : EvaluationDiagnostic{
                                              EvaluationDiagnosticCode::AllocationFailure,
                                              DiagnosticSeverity::Error,
                                              {},
                                              "A motion blur row band could not complete",
                                              {}};
                return result;
            }
        }
        release(s);
        reportProgress(progress, {EvaluationProgressStage::Operation, operation,
                                  static_cast<std::uint32_t>(s + 1), total});
    }
    const auto& result = buffers[*plan->output];
    const auto window = render::ImageWindow::create(result.window.x, result.window.y,
                                                    static_cast<std::uint64_t>(result.window.width),
                                                    static_cast<std::uint64_t>(result.window.height));
    const auto display = render::ImageWindow::create(
        geometry.display.x, geometry.display.y, static_cast<std::uint64_t>(geometry.display.width),
        static_cast<std::uint64_t>(geometry.display.height));
    const render::Rgba32fImage* colour = nullptr;
    for (const auto* image : inputs)
        if (image != nullptr) {
            colour = image;
            break;
        }
    if (!window || !display || colour == nullptr)
        return failed(EvaluationDiagnosticCode::InvalidParameter, "Motion blur window is invalid");
    const auto descriptor = render::Rgba32fImageDescriptor::create(
        *window.value(), *display.value(), colour->descriptor()->pixelAspect());
    if (!descriptor)
        return failed(EvaluationDiagnosticCode::InvalidParameter, "Motion blur window is invalid");
    auto builder = render::Rgba32fImageBuilder::create(*descriptor.value(), budget > live
                                                                                 ? budget - live
                                                                                 : 0);
    if (!builder)
        return failed(EvaluationDiagnosticCode::PixelStorageBudgetExceeded,
                      "Motion blur output exceeds the pixel budget");
    auto& image = *builder.value();
    for (std::int64_t y = 0; y < result.window.height; ++y) {
        auto row = image.row(result.window.y + y);
        if (!row)
            return failed(EvaluationDiagnosticCode::InternalInvariant,
                          "Motion blur row is unavailable");
        const float* in = result.pixels.data() + static_cast<std::size_t>(y) *
                                                     static_cast<std::size_t>(result.window.width) *
                                                     4U;
        for (std::size_t x = 0; x < row.value()->size(); ++x, in += 4) {
            const auto pixel = render::Rgba32f::fromPremultiplied(in[0], in[1], in[2], in[3]);
            if (!pixel)
                return failed(EvaluationDiagnosticCode::InvalidPixel,
                              "Motion blur produced an invalid pixel");
            (*row.value())[x] = *pixel.value();
        }
    }
    reportProgress(progress, {EvaluationProgressStage::Operation, operation, total, total});
    auto frozen = std::move(image).freeze();
    if (!frozen)
        return failed(EvaluationDiagnosticCode::InvalidPixel, "Motion blur publication failed");
    return {std::make_shared<const render::Rgba32fImage>(std::move(*frozen.value())), {}, false};
}

} // namespace bloom::runtime::detail
