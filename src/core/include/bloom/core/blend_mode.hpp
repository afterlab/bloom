#ifndef BLOOM_CORE_BLEND_MODE_HPP
#define BLOOM_CORE_BLEND_MODE_HPP

#include <array>
#include <cstdint>
#include <optional>

namespace bloom::core {

// How one layer's pixels combine with what is already beneath them.
//
// This lives in bloom_core, not in bloom_render or bloom_document, because three modules need the
// SAME closed vocabulary and the same stable integer mapping: the document schema validates and
// persists the stored integer, the CPU compositing kernel switches on the mode, and the runtime
// carries it from one to the other. A second enumeration in either of those modules would be two
// spellings of one contract, and a renumbering in one of them would silently re-interpret every
// saved document.
//
// The enumerator VALUES are the durable stored mapping (docs/architecture/color-management.md,
// "Blend modes"). Normal is 0, so a document that stores no blend mode at all -- every Layer Output
// written before the blend-mode slice -- decodes as Normal and renders exactly the picture it
// always did. Appending a mode is additive; renumbering or reusing a value is not, and would need a
// new schema key rather than a new number.
//
// The vocabulary has three families, each with one closed formula over premultiplied scene-linear
// values (docs/architecture/color-management.md, "Blend modes"):
//
//   - the SEPARABLE modes (Normal through Divide): a W3C Compositing and Blending Level 1 blend
//     function B(Cb, Cs) on un-premultiplied channels, folded with source-over alpha. None needs a
//     luminance, saturation, or hue model that would commit to a colorimetry the process space does
//     not fix;
//   - the channel EXTREMES (Minimum, Maximum): the per-component minimum or maximum of the two
//     premultiplied pixels, alpha included, which is what a depth or data pass is combined with;
//   - the Porter-Duff MATTE operators (In, Out, Atop, Xor, Stencil, Mask): co = cs*Fa + cb*Fb and
//     ao = as*Fa + ab*Fb with the operator's own coverage factors.
enum class BlendMode : std::uint8_t {
    Normal = 0,
    Add = 1,
    Multiply = 2,
    Screen = 3,
    Overlay = 4,
    Darken = 5,
    Lighten = 6,
    Difference = 7,
    Subtract = 8,
    Divide = 9,
    Minimum = 10,
    Maximum = 11,
    In = 12,
    Out = 13,
    Atop = 14,
    Xor = 15,
    Stencil = 16,
    Mask = 17,
};

// Every mode, in the authoring order every surface offers them in. It is also the stored-value
// order: the eight original modes kept their shipped order and every later mode is appended, so a
// renumbering cannot hide behind a reordering. One list, so the timeline dropdown, the Properties
// row, and the node card cannot present three different orders of the same vocabulary.
inline constexpr std::array<BlendMode, 18> kBlendModes{
    BlendMode::Normal,   BlendMode::Add,    BlendMode::Multiply, BlendMode::Screen,
    BlendMode::Overlay,  BlendMode::Darken, BlendMode::Lighten,  BlendMode::Difference,
    BlendMode::Subtract, BlendMode::Divide, BlendMode::Minimum,  BlendMode::Maximum,
    BlendMode::In,       BlendMode::Out,    BlendMode::Atop,     BlendMode::Xor,
    BlendMode::Stencil,  BlendMode::Mask,
};

// The groups a blend-mode dropdown separates with a hairline, in kBlendModes order: Normal on its
// own, the layer blends that brighten, darken, or add contrast, the arithmetic comparisons, the
// channel extremes, and the Porter-Duff mattes.
enum class BlendModeGroup : std::uint8_t {
    Normal,
    Blend,
    Arithmetic,
    Extremes,
    Matte,
};

[[nodiscard]] constexpr BlendModeGroup blendModeGroup(const BlendMode mode) noexcept {
    switch (mode) {
    case BlendMode::Normal:
        return BlendModeGroup::Normal;
    case BlendMode::Add:
    case BlendMode::Multiply:
    case BlendMode::Screen:
    case BlendMode::Overlay:
    case BlendMode::Darken:
    case BlendMode::Lighten:
        return BlendModeGroup::Blend;
    case BlendMode::Difference:
    case BlendMode::Subtract:
    case BlendMode::Divide:
        return BlendModeGroup::Arithmetic;
    case BlendMode::Minimum:
    case BlendMode::Maximum:
        return BlendModeGroup::Extremes;
    case BlendMode::In:
    case BlendMode::Out:
    case BlendMode::Atop:
    case BlendMode::Xor:
    case BlendMode::Stencil:
    case BlendMode::Mask:
        return BlendModeGroup::Matte;
    }
    return BlendModeGroup::Normal;
}

// True for the modes that combine colour through a separable blend function and composite alpha
// source-over (Normal through Divide); false for the extremes and the mattes, whose alpha follows
// their own formula.
[[nodiscard]] constexpr bool blendModeIsSeparable(const BlendMode mode) noexcept {
    return static_cast<std::uint8_t>(mode) <= static_cast<std::uint8_t>(BlendMode::Divide);
}

// True when a fully transparent source pixel leaves the backdrop pixel exactly as it was. Every
// separable mode does, and so do Atop, Xor and Stencil (their backdrop factor is 1 - as). Minimum,
// Maximum, In, Out and Mask do not: where the layer has no pixels they still change the backdrop
// (In, Out and Mask clear it, Minimum takes it to zero coverage, Maximum lifts a negative channel
// to zero), so a compositor must apply them across the whole stack window, not only inside the
// layer's own data window.
[[nodiscard]] constexpr bool
blendModeKeepsBackdropUnderTransparentSource(const BlendMode mode) noexcept {
    switch (mode) {
    case BlendMode::Minimum:
    case BlendMode::Maximum:
    case BlendMode::In:
    case BlendMode::Out:
    case BlendMode::Mask:
        return false;
    default:
        return true;
    }
}

inline constexpr BlendMode kDefaultBlendMode = BlendMode::Normal;

[[nodiscard]] constexpr std::int64_t blendModeStoredValue(const BlendMode mode) noexcept {
    return static_cast<std::int64_t>(static_cast<std::uint8_t>(mode));
}

// The only way a stored integer becomes a BlendMode. An unknown value is rejected rather than
// clamped or folded to Normal: a document carrying a mode this build does not implement is a
// document this build cannot render faithfully, and the document layer refuses it at validation
// instead of quietly drawing something else.
[[nodiscard]] constexpr std::optional<BlendMode>
blendModeFromStoredValue(const std::int64_t value) noexcept {
    for (const auto mode : kBlendModes) {
        if (blendModeStoredValue(mode) == value) {
            return mode;
        }
    }
    return std::nullopt;
}

} // namespace bloom::core

#endif // BLOOM_CORE_BLEND_MODE_HPP
