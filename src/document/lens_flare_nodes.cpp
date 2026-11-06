#include <bloom/document/lens_flare_definition.hpp>

#include <bloom/document/parameter.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <variant>

namespace bloom::document {

namespace {

using Kind = LensFlareParameterKind;

ParameterValueKind valueKindOf(const Kind kind) noexcept {
    switch (kind) {
    case Kind::Choice:
    case Kind::Integer:
        return ParameterValueKind::Integer;
    case Kind::Scalar:
        return ParameterValueKind::Float64;
    case Kind::Vector2:
        return ParameterValueKind::Vec2d;
    case Kind::Color:
        return ParameterValueKind::Color4d;
    }
    return ParameterValueKind::Float64;
}

ParameterValue defaultValueOf(const LensFlareParameterSpec& spec) {
    switch (spec.kind) {
    case Kind::Choice:
    case Kind::Integer:
        return static_cast<std::int64_t>(spec.value[0]);
    case Kind::Scalar:
        return spec.value[0];
    case Kind::Vector2:
        return Vec2d{spec.value[0], spec.value[1]};
    case Kind::Color:
        return core::Color4d{spec.value[0], spec.value[1], spec.value[2], spec.value[3]};
    }
    return 0.0;
}

// One value of a fixed pseudo-random sequence in [0, 1): the same on every device and at every
// resolution, so a flare's pattern is part of its document.
[[nodiscard]] double sequence(const std::int64_t seed, const std::uint64_t index) noexcept {
    std::uint64_t z = (static_cast<std::uint64_t>(seed) * 0x9E3779B97F4A7C15ULL) +
                      ((index + 1U) * 0xBF58476D1CE4E5B9ULL);
    z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
    z ^= z >> 31U;
    return static_cast<double>(z >> 11U) * 0x1.0p-53;
}

// The coatings' colours: each ghost mixes towards one of them.
inline constexpr std::array<std::array<double, 3>, 4> kCoatings{
    {{1.0, 0.72, 0.45}, {0.55, 1.0, 0.7}, {0.5, 0.7, 1.0}, {1.0, 0.55, 0.85}}};

[[nodiscard]] LensFlareDisc disc(const LensFlareLayoutInput& input, const std::int64_t stream,
                                 const std::int64_t index, const double size,
                                 const double spread) {
    const auto at = [&](const std::uint64_t channel) {
        return sequence(input.seed, static_cast<std::uint64_t>(stream) * 257U +
                                        static_cast<std::uint64_t>(index) * 7U + channel);
    };
    // Along the axis: 1 is the light, 0 the frame's centre, -1 its mirror image.
    const double along = spread * (0.85 - 2.2 * at(0));
    const auto coating = kCoatings[static_cast<std::size_t>(at(2) * 4.0)];
    const double mix = 0.35 + 0.65 * at(3);
    LensFlareDisc item;
    item.x = input.lightX * along;
    item.y = input.lightY * along;
    item.radius = input.size * size * (0.5 + at(1));
    for (std::size_t channel = 0; channel < 3; ++channel)
        item.tint[channel] = 1.0 + mix * (coating[channel] - 1.0);
    return item;
}

} // namespace

std::string lensFlareSchemaKey(const std::string_view role) {
    std::string key(kLensFlareNodeType);
    key += '.';
    key += role;
    return key;
}

bool isLensFlareParameterValueValid(const std::string_view schemaKey,
                                    const ParameterValue& value) noexcept {
    const auto* spec = lensFlareParameterSpec(schemaKey);
    if (spec == nullptr)
        return false;
    const auto within = [spec](const double v) {
        return std::isfinite(v) && v >= spec->minimum && v <= spec->maximum;
    };
    switch (spec->kind) {
    case Kind::Choice:
    case Kind::Integer: {
        const auto* v = std::get_if<std::int64_t>(&value);
        return v != nullptr && within(static_cast<double>(*v));
    }
    case Kind::Scalar: {
        const auto* v = std::get_if<double>(&value);
        return v != nullptr && within(*v);
    }
    case Kind::Vector2: {
        const auto* v = std::get_if<Vec2d>(&value);
        return v != nullptr && within(v->x) && within(v->y);
    }
    case Kind::Color: {
        const auto* v = std::get_if<core::Color4d>(&value);
        return v != nullptr && v->isValid() && within(v->red) && within(v->green) &&
               within(v->blue);
    }
    }
    return false;
}

NodeDefinition lensFlareDefinition() {
    NodeDefinition definition{{std::string(kLensFlareNodeType), kLensFlareNodeSchemaVersion},
                              NodeLoweringKind::ImageEffect,
                              {{std::string(kLensFlareInputPort), SocketValueKind::Image}},
                              {{std::string(kLensFlareOutputPort), SocketValueKind::Image}},
                              {},
                              std::nullopt,
                              NodeCardinality::Many,
                              NodeCategory::Compositing};
    // The occlusion matte: a Mask, or an Image read through the channel selector.
    InputPortDefinition occlusion{std::string(kLensFlareOcclusionPort), SocketValueKind::Mask, false};
    occlusion.acceptsImageMatte = true;
    definition.inputs.push_back(std::move(occlusion));
    for (const auto& parameter : lensFlareParameters()) {
        const bool constant =
            parameter.kind == Kind::Choice || parameter.kind == Kind::Integer;
        definition.parameters.push_back({std::string(parameter.role),
                                         lensFlareSchemaKey(parameter.role),
                                         valueKindOf(parameter.kind), true, !constant,
                                         defaultValueOf(parameter)});
        if (constant)
            continue;
        const auto socket = parameter.kind == Kind::Scalar    ? SocketValueKind::Scalar
                            : parameter.kind == Kind::Vector2 ? SocketValueKind::Vector2
                                                              : SocketValueKind::Color;
        definition.inputs.push_back({std::string(parameter.role), socket, false});
    }
    return definition;
}

const std::vector<LensFlarePreset>& lensFlarePresets() {
    // size, rotation, chromatic; then each element. Colours are straight RGBA.
    struct Look final {
        std::string_view name;
        double size, rotation, chromatic;
        std::array<double, 3> color;
        double glowAmount, glowSize;
        double streakAmount, streakLength, streakWidth;
        std::int64_t streakCount;
        double ghostAmount, ghostSize, ghostSpread;
        std::int64_t ghostCount, seed;
        double ringAmount, ringSize, ringWidth;
        double irisAmount, irisSize, irisSpread;
        std::int64_t irisCount, irisBlades;
        double hoopAmount, hoopSize, hoopWidth;
        double anamorphicAmount, anamorphicLength, anamorphicWidth;
        std::array<double, 3> anamorphicColor;
    };
    static const std::array<Look, 6> looks{{
        {"Zoom Lens", 1.0, 0.0, 0.35, {1.0, 0.95, 0.85}, 1.0, 18.0, 0.5, 55.0, 0.4, 3, 0.3, 5.0, 1.0,
         6, 1, 0.12, 30.0, 3.0, 0.2, 7.0, 1.0, 4, 6, 0.12, 70.0, 4.0, 0.0, 120.0, 0.6,
         {0.35, 0.55, 1.0}},
        {"Prime Lens", 0.8, 15.0, 0.2, {1.0, 0.97, 0.92}, 1.2, 14.0, 0.3, 35.0, 0.3, 4, 0.15, 3.5,
         0.8, 3, 7, 0.05, 22.0, 2.0, 0.35, 6.0, 0.9, 5, 8, 0.05, 55.0, 3.0, 0.0, 120.0, 0.6,
         {0.35, 0.55, 1.0}},
        {"Long Lens", 1.4, 0.0, 0.6, {1.0, 0.85, 0.65}, 0.8, 26.0, 0.15, 30.0, 0.5, 2, 0.5, 8.0, 1.3,
         10, 21, 0.2, 45.0, 6.0, 0.1, 12.0, 1.2, 3, 5, 0.25, 95.0, 8.0, 0.0, 120.0, 0.6,
         {0.35, 0.55, 1.0}},
        {"Anamorphic", 1.0, 0.0, 0.25, {0.8, 0.88, 1.0}, 0.7, 12.0, 0.0, 55.0, 0.4, 3, 0.15, 6.0, 1.0,
         4, 3, 0.0, 30.0, 3.0, 0.05, 9.0, 1.0, 2, 6, 0.08, 80.0, 5.0, 1.2, 180.0, 0.5,
         {0.3, 0.5, 1.0}},
        {"Sun Star", 1.2, 0.0, 0.15, {1.0, 0.93, 0.78}, 1.4, 22.0, 1.1, 90.0, 0.35, 8, 0.1, 4.0, 0.9,
         3, 11, 0.05, 28.0, 2.0, 0.08, 5.0, 0.8, 2, 7, 0.0, 70.0, 4.0, 0.0, 120.0, 0.6,
         {0.35, 0.55, 1.0}},
        {"Soft Glow", 1.0, 0.0, 0.0, {1.0, 0.96, 0.9}, 1.0, 35.0, 0.0, 55.0, 0.4, 3, 0.0, 5.0, 1.0,
         0, 1, 0.0, 30.0, 3.0, 0.0, 7.0, 1.0, 0, 6, 0.0, 70.0, 4.0, 0.0, 120.0, 0.6,
         {0.35, 0.55, 1.0}},
    }};
    static const std::vector<LensFlarePreset> presets = [] {
        std::vector<LensFlarePreset> result;
        for (const auto& l : looks) {
            const auto rgb = [](const std::array<double, 3>& c) {
                return ParameterValue{core::Color4d{c[0], c[1], c[2], 1.0}};
            };
            result.push_back(
                {l.name,
                 {{"flareSize", l.size},
                  {"flareRotation", l.rotation},
                  {"chromatic", l.chromatic},
                  {"lightColor", rgb(l.color)},
                  {"glowAmount", l.glowAmount},
                  {"glowSize", l.glowSize},
                  {"streakAmount", l.streakAmount},
                  {"streakCount", l.streakCount},
                  {"streakLength", l.streakLength},
                  {"streakWidth", l.streakWidth},
                  {"ghostAmount", l.ghostAmount},
                  {"ghostCount", l.ghostCount},
                  {"ghostSize", l.ghostSize},
                  {"ghostSpread", l.ghostSpread},
                  {"seed", l.seed},
                  {"ringAmount", l.ringAmount},
                  {"ringSize", l.ringSize},
                  {"ringWidth", l.ringWidth},
                  {"irisAmount", l.irisAmount},
                  {"irisCount", l.irisCount},
                  {"irisBlades", l.irisBlades},
                  {"irisSize", l.irisSize},
                  {"irisSpread", l.irisSpread},
                  {"hoopAmount", l.hoopAmount},
                  {"hoopSize", l.hoopSize},
                  {"hoopWidth", l.hoopWidth},
                  {"anamorphicAmount", l.anamorphicAmount},
                  {"anamorphicLength", l.anamorphicLength},
                  {"anamorphicWidth", l.anamorphicWidth},
                  {"anamorphicColor", rgb(l.anamorphicColor)}}});
        }
        return result;
    }();
    return presets;
}

LensFlareLayout lensFlareLayout(const LensFlareLayoutInput& input) {
    LensFlareLayout layout;
    const double turn = input.rotationDegrees * std::numbers::pi / 180.0;
    const auto streaks = std::clamp<std::int64_t>(input.streakCount, 0, kMaximumLensFlareStreaks);
    layout.streaks.reserve(static_cast<std::size_t>(streaks));
    for (std::int64_t line = 0; line < streaks; ++line) {
        const double angle = turn + std::numbers::pi * static_cast<double>(line) /
                                        static_cast<double>(streaks);
        // Each line its own length, so a star reads as light rather than as a wheel.
        const double variation =
            0.7 + 0.3 * sequence(input.seed, 1000U + static_cast<std::uint64_t>(line));
        layout.streaks.push_back(
            {std::cos(angle), std::sin(angle), input.size * input.streakLength * variation});
    }
    const auto ghosts = std::clamp<std::int64_t>(input.ghostCount, 0, kMaximumLensFlareGhosts);
    layout.ghosts.reserve(static_cast<std::size_t>(ghosts));
    for (std::int64_t index = 0; index < ghosts; ++index)
        layout.ghosts.push_back(disc(input, 1, index, input.ghostSize, input.ghostSpread));
    const auto irises = std::clamp<std::int64_t>(input.irisCount, 0, kMaximumLensFlareIrises);
    layout.irises.reserve(static_cast<std::size_t>(irises));
    for (std::int64_t index = 0; index < irises; ++index)
        layout.irises.push_back(disc(input, 2, index, input.irisSize, input.irisSpread));
    const auto blades =
        std::clamp<std::int64_t>(input.irisBlades, kMinimumLensFlareBlades, kMaximumLensFlareBlades);
    layout.irisNormals.reserve(static_cast<std::size_t>(blades));
    for (std::int64_t edge = 0; edge < blades; ++edge) {
        const double angle =
            turn + 2.0 * std::numbers::pi * static_cast<double>(edge) / static_cast<double>(blades);
        layout.irisNormals.push_back({std::cos(angle), std::sin(angle)});
    }
    return layout;
}

bool lensFlareProjection(const double zoom, const double distance, double& scale) noexcept {
    const double depth = zoom + distance;
    if (!(depth > 0.0) || !(zoom > 0.0))
        return false;
    scale = zoom / depth;
    return std::isfinite(scale);
}

} // namespace bloom::document
