#pragma once

// #486 Sharpen: the durable vocabulary of the unsharp mask node. An Image in and out, and the optional
// effect mask every filter takes.
//
//   amount     how much of the detail is added back: 1 adds it once, 0 changes nothing.
//   radius     the blur the detail is measured against, in full-resolution pixels, the same radius
//              as Gaussian Blur's (a sigma of radius / 3).
//   threshold  detail smaller than this, in linear light, is left alone, and larger detail starts
//              from it, so grain and flat areas stay clean with no jump at the threshold.
//
// docs/architecture/image-filters.md, "Sharpen", is the specification. Schema keys are
// "bloom.sharpen.<role>". Every value is animatable, drivable and has a socket of its own.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace bloom::document {

inline constexpr std::string_view kSharpenNodeType = "bloom.sharpen";
inline constexpr std::uint32_t kSharpenNodeSchemaVersion = 1;

struct SharpenParameterSpec final {
    std::string_view role;
    double value = 0.0;
    // The document domain.
    double minimum = 0.0;
    double maximum = 0.0;
    // The control's label and presentation: the span its slider covers, one displayed step, the
    // decimals shown, the unit and the display scale of a stored value.
    std::string_view label{};
    double softMaximum = 1.0;
    double step = 1.0;
    int decimals = 1;
    std::string_view unit{};
    double displayScale = 1.0;
};

namespace sharpen_parameters {

inline constexpr std::array kParameters{
    SharpenParameterSpec{"amount", 1.0, 0.0, 10.0, "Amount", 3.0, 1.0, 0, "%", 100.0},
    // The radius shares kMaximumBlurRadiusPixels (parameter.hpp).
    SharpenParameterSpec{"radius", 3.0, 0.0, 256.0, "Radius", 32.0, 0.1, 1, "px"},
    SharpenParameterSpec{"threshold", 0.0, 0.0, 1000.0, "Threshold", 0.5, 0.001, 3},
};

} // namespace sharpen_parameters

[[nodiscard]] constexpr std::span<const SharpenParameterSpec> sharpenParameters() noexcept {
    return sharpen_parameters::kParameters;
}

// The spec of a "bloom.sharpen.<role>" schema key, or null.
[[nodiscard]] constexpr const SharpenParameterSpec*
sharpenParameterSpec(const std::string_view schemaKey) noexcept {
    constexpr std::string_view prefix = "bloom.sharpen.";
    if (!schemaKey.starts_with(prefix))
        return nullptr;
    const auto role = schemaKey.substr(prefix.size());
    for (const auto& spec : sharpen_parameters::kParameters)
        if (spec.role == role)
            return &spec;
    return nullptr;
}

// Whether `value` lies in the parameter's domain; true for any other key.
[[nodiscard]] constexpr bool isSharpenScalarWithinDomain(const std::string_view key,
                                                         const double value) noexcept {
    const auto* spec = sharpenParameterSpec(key);
    return spec == nullptr || (value >= spec->minimum && value <= spec->maximum);
}

} // namespace bloom::document
