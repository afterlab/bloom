#pragma once

// #486 Film Grain: the durable vocabulary of the node that adds seeded, repeatable grain to an
// image. An Image in and out, and the optional effect mask every filter takes.
//
//   intensity  the grain's standard deviation in linear light where the response is 100%.
//   size       how wide a grain is, in full-resolution pixels, whatever the proxy scale.
//   redAmount, greenAmount, blueAmount
//              how strong each channel's grain is, relative to the intensity.
//   shadows, midtones, highlights
//              the response to the pixel's own straight Rec. 709 luma: the share of the intensity
//              at luma 0, at mid grey (0.18) and at 1.0, linear between them. Above 1.0 the grain
//              grows with the luma, so it stays the same share of an HDR highlight.
//   animation  Every Frame draws new grain for each frame; Held keeps one pattern. Constant.
//   seed       which grain. The same seed and frame always give the same grain. Constant.
//   monochrome off gives each channel its own grain; on, every channel takes the red channel's
//              pattern, each still scaled by its own amount. Constant and optional: absent means
//              off.
//   mode       Film (the default) or Sensor Noise (#565). Constant and optional.
//   shadowWeighting, chromaBlotches, blotchSize, fixedPattern
//              what Sensor Noise adds: how much the noise favors the shadows, the strength and size
//              of its low-frequency chroma blotches, and the strength of a pattern that never
//              changes from frame to frame. Each is optional (a node saved before them opens with
//              them at their defaults) and is read only in Sensor Noise mode.
//
// The colour never clips: grain is added, in proportion to alpha so the image stays premultiplied,
// and alpha is kept. docs/architecture/image-filters.md, "Film Grain", is the specification.
//
// Schema keys are "bloom.film-grain.<role>". Every Scalar is animatable, drivable and has a socket
// of its own; the Choice, the Integer and the Boolean are constants with no socket.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace bloom::document {

inline constexpr std::string_view kFilmGrainNodeType = "bloom.film-grain";
inline constexpr std::uint32_t kFilmGrainNodeSchemaVersion = 1;

inline constexpr std::int64_t kFilmGrainEveryFrame = 0;
inline constexpr std::int64_t kFilmGrainHeld = 1;

// #565: the modes. Film is the node as it always was.
inline constexpr std::int64_t kFilmGrainFilm = 0;
inline constexpr std::int64_t kFilmGrainSensorNoise = 1;

// The response's knots in linear light: mid grey and 1.0.
inline constexpr double kFilmGrainMidGrey = 0.18;
inline constexpr double kFilmGrainWhite = 1.0;

enum class FilmGrainParameterKind : std::uint8_t { Choice, Integer, Scalar, Boolean };

struct FilmGrainParameterSpec final {
    std::string_view role;
    FilmGrainParameterKind kind = FilmGrainParameterKind::Scalar;
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
    int decimals = 1;
    std::string_view unit{};
    double displayScale = 1.0;
    // A node saved before the parameter existed opens with it at this default (Booleans always are).
    bool optional = false;
};

namespace film_grain_parameters {

using Kind = FilmGrainParameterKind;

inline constexpr std::array<std::string_view, 2> kAnimations{"Every Frame", "Held"};
inline constexpr std::array<std::string_view, 2> kModes{"Film", "Sensor Noise"};

inline constexpr std::array kParameters{
    FilmGrainParameterSpec{"intensity", Kind::Scalar, 0.02, 0.0, 1.0, {}, "Intensity", 0.0, 0.2, 0.1,
                           1, "%", 100.0},
    FilmGrainParameterSpec{"size", Kind::Scalar, 1.0, 0.25, 32.0, {}, "Size", 0.25, 8.0, 0.01, 2,
                           "px"},
    FilmGrainParameterSpec{"redAmount", Kind::Scalar, 1.0, 0.0, 10.0, {}, "Red amount", 0.0, 2.0, 1.0,
                           0, "%", 100.0},
    FilmGrainParameterSpec{"greenAmount", Kind::Scalar, 1.0, 0.0, 10.0, {}, "Green amount", 0.0, 2.0,
                           1.0, 0, "%", 100.0},
    FilmGrainParameterSpec{"blueAmount", Kind::Scalar, 1.0, 0.0, 10.0, {}, "Blue amount", 0.0, 2.0,
                           1.0, 0, "%", 100.0},
    FilmGrainParameterSpec{"shadows", Kind::Scalar, 0.1, 0.0, 10.0, {}, "Shadows response", 0.0, 2.0,
                           1.0, 0, "%", 100.0},
    FilmGrainParameterSpec{"midtones", Kind::Scalar, 1.0, 0.0, 10.0, {}, "Midtones response", 0.0,
                           2.0, 1.0, 0, "%", 100.0},
    FilmGrainParameterSpec{"highlights", Kind::Scalar, 1.0, 0.0, 10.0, {}, "Highlights response", 0.0,
                           2.0, 1.0, 0, "%", 100.0},
    FilmGrainParameterSpec{"animation", Kind::Choice, 0.0, 0.0, 1.0, kAnimations, "Grain changes"},
    FilmGrainParameterSpec{"seed", Kind::Integer, 1.0, 0.0, 2147483647.0, {}, "Seed"},
    FilmGrainParameterSpec{"monochrome", Kind::Boolean, 0.0, 0.0, 1.0, {}, "Monochrome"},
    // #565 Sensor Noise. Appended, so every earlier parameter keeps its place.
    FilmGrainParameterSpec{"mode", Kind::Choice, 0.0, 0.0, 1.0, kModes, "Mode", 0.0, 1.0, 1.0, 1, {},
                           1.0, true},
    FilmGrainParameterSpec{"shadowWeighting", Kind::Scalar, 1.0, 0.0, 10.0, {}, "Shadow weighting",
                           0.0, 4.0, 1.0, 0, "%", 100.0, true},
    FilmGrainParameterSpec{"chromaBlotches", Kind::Scalar, 0.5, 0.0, 10.0, {}, "Chroma blotches",
                           0.0, 2.0, 1.0, 0, "%", 100.0, true},
    FilmGrainParameterSpec{"blotchSize", Kind::Scalar, 24.0, 1.0, 512.0, {}, "Blotch size", 4.0,
                           128.0, 0.5, 1, "px", 1.0, true},
    FilmGrainParameterSpec{"fixedPattern", Kind::Scalar, 0.0, 0.0, 10.0, {}, "Fixed pattern", 0.0,
                           2.0, 1.0, 0, "%", 100.0, true},
};

} // namespace film_grain_parameters

[[nodiscard]] constexpr std::span<const FilmGrainParameterSpec> filmGrainParameters() noexcept {
    return film_grain_parameters::kParameters;
}

// The spec of a "bloom.film-grain.<role>" schema key, or null.
[[nodiscard]] constexpr const FilmGrainParameterSpec*
filmGrainParameterSpec(const std::string_view schemaKey) noexcept {
    constexpr std::string_view prefix = "bloom.film-grain.";
    if (!schemaKey.starts_with(prefix))
        return nullptr;
    const auto role = schemaKey.substr(prefix.size());
    for (const auto& spec : film_grain_parameters::kParameters)
        if (spec.role == role)
            return &spec;
    return nullptr;
}

[[nodiscard]] constexpr bool isFilmGrainScalarSchemaKey(const std::string_view key) noexcept {
    const auto* spec = filmGrainParameterSpec(key);
    return spec != nullptr && spec->kind == FilmGrainParameterKind::Scalar;
}
// A constant (Choice, Integer, Boolean): no socket and no animation.
[[nodiscard]] constexpr bool isFilmGrainConstantSchemaKey(const std::string_view key) noexcept {
    const auto* spec = filmGrainParameterSpec(key);
    return spec != nullptr && spec->kind != FilmGrainParameterKind::Scalar;
}
// Whether the Scalar `value` lies in the parameter's domain; true for any other key.
[[nodiscard]] constexpr bool isFilmGrainScalarWithinDomain(const std::string_view key,
                                                           const double value) noexcept {
    const auto* spec = filmGrainParameterSpec(key);
    return spec == nullptr || (value >= spec->minimum && value <= spec->maximum);
}

// The Scalars in table order: FilmGrainKernel::values holds one per entry.
inline constexpr std::size_t kFilmGrainScalarCount = 12;
// The index of the Scalar `role` among the Scalars, or kFilmGrainScalarCount for another role.
[[nodiscard]] constexpr std::size_t filmGrainScalarIndex(const std::string_view role) noexcept {
    std::size_t index = 0;
    for (const auto& spec : film_grain_parameters::kParameters) {
        if (spec.kind != FilmGrainParameterKind::Scalar)
            continue;
        if (spec.role == role)
            return index;
        ++index;
    }
    return index;
}

// The share of the intensity at the straight luma `luma` (negative reads as 0), shared by the
// renderer's documentation, its tests and the card's curve.
[[nodiscard]] constexpr double filmGrainResponse(const double luma, const double shadows,
                                                 const double midtones,
                                                 const double highlights) noexcept {
    const double value = luma > 0.0 ? luma : 0.0;
    if (value >= kFilmGrainWhite)
        return highlights * value;
    if (value >= kFilmGrainMidGrey)
        return midtones + (highlights - midtones) * ((value - kFilmGrainMidGrey) /
                                                     (kFilmGrainWhite - kFilmGrainMidGrey));
    return shadows + (midtones - shadows) * (value / kFilmGrainMidGrey);
}

} // namespace bloom::document
