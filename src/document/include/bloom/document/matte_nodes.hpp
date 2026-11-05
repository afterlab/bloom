#pragma once

#include <bloom/document/node_definition_registry.hpp>

#include <cstdint>
#include <string_view>

// #383 matte workflow: the Mask Combine and Image to Mask nodes, and the optional mask input every
// grade, colour-management and filter effect carries. docs/architecture/image-filters.md
// ("Matte workflow") is the specification.
namespace bloom::document {

// Mask Combine: two Masks combined by the layer-mask modes, over the composition canvas.
inline constexpr std::string_view kMaskCombineNodeType = "bloom.mask-combine";
inline constexpr std::uint32_t kMaskCombineNodeSchemaVersion = 1;
inline constexpr std::string_view kMaskCombineFirstInputPort = "a";
inline constexpr std::string_view kMaskCombineSecondInputPort = "b";
inline constexpr std::string_view kMaskCombineOutputPort = "mask";
inline constexpr std::string_view kMaskCombineModeRole = "mode";
inline constexpr std::string_view kMaskCombineModeSchemaKey = "bloom.mask-combine.mode";
// The stored mode integers are durable and never reordered.
enum class MaskCombineMode : std::int64_t { Add = 0, Subtract = 1, Intersect = 2, Difference = 3 };
inline constexpr std::int64_t kMaskCombineModeCount = 4;

// Image to Mask: an Image's alpha, luma or one colour channel as a Mask over the composition canvas.
inline constexpr std::string_view kImageToMaskNodeType = "bloom.image-to-mask";
inline constexpr std::uint32_t kImageToMaskNodeSchemaVersion = 1;
inline constexpr std::string_view kImageToMaskInputPort = "input";
inline constexpr std::string_view kImageToMaskOutputPort = "mask";
inline constexpr std::string_view kImageToMaskChannelRole = "channel";
inline constexpr std::string_view kImageToMaskChannelSchemaKey = "bloom.image-to-mask.channel";

// The effect mask: an optional Mask (or Image) limiting where a grade, colour-management or filter
// effect applies, read through the matte selector (render::MatteSelection: 0-4 Alpha, Red, Green,
// Blue, Luma; 5-9 the same inverted). A Mask is always read as its coverage, so only the selector's
// invert half applies to one.
inline constexpr std::string_view kEffectMaskInputPort = "mask";
inline constexpr std::string_view kEffectMaskChannelRole = "maskChannel";
inline constexpr std::string_view kEffectMaskChannelSchemaKey = "bloom.effect-mask.channel";
// The matte selector's stored values (shared with Set Alpha's and Multiply Alpha's).
inline constexpr std::int64_t kMatteSelectorValueCount = 10;
inline constexpr std::int64_t kMatteSelectorInvertedOffset = 5;

// Whether `definition` (before its effect mask is appended) is an effect that takes one: an image
// effect in Grade, Color Management or Filters whose leading input is `input` and whose only output
// is an Image `image`, and which declares no `mask` input of its own.
[[nodiscard]] bool takesEffectMask(const NodeDefinition& definition) noexcept;
// Whether `definition` carries the effect mask (its last input and last parameter).
[[nodiscard]] bool hasEffectMask(const NodeDefinition& definition) noexcept;
// Appends the effect mask input and its channel parameter.
void appendEffectMask(NodeDefinition& definition);
// `definition` without its effect mask.
[[nodiscard]] NodeDefinition withoutEffectMask(NodeDefinition definition);

[[nodiscard]] NodeDefinition maskCombineDefinition();
[[nodiscard]] NodeDefinition imageToMaskDefinition();

} // namespace bloom::document
