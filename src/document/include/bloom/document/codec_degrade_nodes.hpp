#pragma once

// #563 Codec Degrade: the durable vocabulary of the node that gives an image the look of lossy video
// compression. An Image in and out, and the optional effect mask every filter takes.
//
//   quality      how finely the blocks keep their detail, 100 being nearly lossless and 0 the coarsest
//                quantiser. Animatable.
//   persistence  P-frame persistence: how much of the previous decoded picture a predicted frame
//                keeps instead of refreshing it from the source. 0% refreshes every frame, 100%
//                freezes the picture until the next keyframe. Animatable.
//   blockSize    the width of a transform block, 8 or 16 pixels. Constant.
//   gopLength    the frames from one keyframe to the next: a keyframe at every multiple of it counted
//                from the composition's first frame. Constant.
//   chromaBleed  4:2:0 chroma: the colour is kept at half the resolution in each direction, so it
//                bleeds across edges. Constant.
//
// The node imitates a codec; it does not run one. docs/architecture/image-filters.md, "Codec
// Degrade", is the specification.
//
// Schema keys are "bloom.codec-degrade.<role>". Every Scalar is animatable, drivable and has a
// socket of its own; the Choices and the Boolean are constants with no socket.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace bloom::document {

inline constexpr std::string_view kCodecDegradeNodeType = "bloom.codec-degrade";
inline constexpr std::uint32_t kCodecDegradeNodeSchemaVersion = 1;

enum class CodecDegradeParameterKind : std::uint8_t { Choice, Scalar, Boolean };

struct CodecDegradeParameterSpec final {
    std::string_view role;
    CodecDegradeParameterKind kind = CodecDegradeParameterKind::Scalar;
    double value = 0.0;
    // The document domain.
    double minimum = 0.0;
    double maximum = 0.0;
    std::span<const std::string_view> choices{};
    // The control's label and presentation: the span its slider covers, one displayed step, the
    // decimals shown, the unit and the display scale of a stored value.
    std::string_view label{};
    double softMinimum = 0.0;
    double softMaximum = 1.0;
    double step = 1.0;
    int decimals = 0;
    std::string_view unit{};
    double displayScale = 1.0;
};

namespace codec_degrade_parameters {

using Kind = CodecDegradeParameterKind;

inline constexpr std::array<std::string_view, 2> kBlockSizes{"8 px", "16 px"};
inline constexpr std::array<std::string_view, 9> kGopLengths{
    "1 frame (all keyframes)", "2 frames", "4 frames", "6 frames", "8 frames",
    "12 frames",                        "15 frames", "24 frames", "30 frames"};
inline constexpr std::array<std::int64_t, 9> kGopFrames{1, 2, 4, 6, 8, 12, 15, 24, 30};

inline constexpr std::array kParameters{
    CodecDegradeParameterSpec{"quality", Kind::Scalar, 40.0, 0.0, 100.0, {}, "Quality", 0.0, 100.0,
                              1.0, 0},
    CodecDegradeParameterSpec{"persistence", Kind::Scalar, 0.5, 0.0, 1.0, {}, "P-frame persistence",
                              0.0, 1.0, 1.0, 0, "%", 100.0},
    CodecDegradeParameterSpec{"blockSize", Kind::Choice, 0.0, 0.0, 1.0, kBlockSizes, "Block size"},
    CodecDegradeParameterSpec{"gopLength", Kind::Choice, 5.0, 0.0, 8.0, kGopLengths, "GOP length"},
    CodecDegradeParameterSpec{"chromaBleed", Kind::Boolean, 1.0, 0.0, 1.0, {}, "Chroma bleed"},
};

} // namespace codec_degrade_parameters

[[nodiscard]] constexpr std::span<const CodecDegradeParameterSpec>
codecDegradeParameters() noexcept {
    return codec_degrade_parameters::kParameters;
}

// The spec of a "bloom.codec-degrade.<role>" schema key, or null.
[[nodiscard]] constexpr const CodecDegradeParameterSpec*
codecDegradeParameterSpec(const std::string_view schemaKey) noexcept {
    constexpr std::string_view prefix = "bloom.codec-degrade.";
    if (!schemaKey.starts_with(prefix))
        return nullptr;
    const auto role = schemaKey.substr(prefix.size());
    for (const auto& spec : codec_degrade_parameters::kParameters)
        if (spec.role == role)
            return &spec;
    return nullptr;
}

[[nodiscard]] constexpr bool isCodecDegradeScalarSchemaKey(const std::string_view key) noexcept {
    const auto* spec = codecDegradeParameterSpec(key);
    return spec != nullptr && spec->kind == CodecDegradeParameterKind::Scalar;
}
// A constant (Choice, Boolean): no socket and no animation.
[[nodiscard]] constexpr bool isCodecDegradeConstantSchemaKey(const std::string_view key) noexcept {
    const auto* spec = codecDegradeParameterSpec(key);
    return spec != nullptr && spec->kind != CodecDegradeParameterKind::Scalar;
}
// Whether the Scalar `value` lies in the parameter's domain; true for any other key.
[[nodiscard]] constexpr bool isCodecDegradeScalarWithinDomain(const std::string_view key,
                                                              const double value) noexcept {
    const auto* spec = codecDegradeParameterSpec(key);
    return spec == nullptr || (value >= spec->minimum && value <= spec->maximum);
}

// The Scalars in table order: CodecDegradeKernel::values holds one per entry.
inline constexpr std::size_t kCodecDegradeScalarCount = 2;
// The index of the Scalar `role` among the Scalars, or kCodecDegradeScalarCount for another role.
[[nodiscard]] constexpr std::size_t codecDegradeScalarIndex(const std::string_view role) noexcept {
    std::size_t index = 0;
    for (const auto& spec : codec_degrade_parameters::kParameters) {
        if (spec.kind != CodecDegradeParameterKind::Scalar)
            continue;
        if (spec.role == role)
            return index;
        ++index;
    }
    return index;
}

// The block width a Block size choice names, in pixels, or 0 for an index outside the choices.
[[nodiscard]] constexpr std::int64_t codecDegradeBlockPixels(const std::int64_t choice) noexcept {
    return choice == 0 ? 8 : choice == 1 ? 16 : 0;
}
// The frames a GOP length choice names, or 0 for an index outside the choices.
[[nodiscard]] constexpr std::int64_t codecDegradeGopFrames(const std::int64_t choice) noexcept {
    return choice >= 0 && choice < static_cast<std::int64_t>(codec_degrade_parameters::kGopFrames.size())
               ? codec_degrade_parameters::kGopFrames[static_cast<std::size_t>(choice)]
               : 0;
}

} // namespace bloom::document
