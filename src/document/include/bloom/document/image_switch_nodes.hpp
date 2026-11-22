#pragma once

// #531 Image Switch and Channel Set Switch: the durable vocabulary of the two nodes that show one of
// several inputs, chosen per frame by an index. Bloom has no polymorphic port, so there is one node
// per transport kind: Image Switch switches Images, Channel Set Switch switches Channel Sets (render
// passes), and the same index drives both when passes must follow the beauty.
//
//   input1 ... input8        (Image Switch) the Images to choose from, in order; any may be empty.
//   channels1 ... channels8  (Channel Set Switch) the Channel Sets to choose from.
//   index  which input shows: its whole part counts from 1 (1 is the first input). Animatable and
//          drivable (an Integer or Scalar value node connects to its socket). Out of range it is
//          clamped to the first and the last connected input, or with Wrap taken around them.
//   wrap   the index counts around the inputs (9 is input 1 again with eight inputs) instead of
//          stopping at the first and the last.
//
// The inputs counted are input 1 up to the last connected one; an unconnected input below it shows
// nothing. Only the chosen input is evaluated. docs/architecture/image-switch.md is the
// specification. Schema keys are "bloom.image-switch.<role>", shared by both nodes.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace bloom::document {

inline constexpr std::string_view kImageSwitchNodeType = "bloom.image-switch";
inline constexpr std::string_view kChannelSetSwitchNodeType = "bloom.channel-set-switch";
inline constexpr std::uint32_t kImageSwitchNodeSchemaVersion = 1;

inline constexpr std::size_t kImageSwitchInputCount = 8;
inline constexpr std::array<std::string_view, kImageSwitchInputCount> kImageSwitchInputPorts{
    "input1", "input2", "input3", "input4", "input5", "input6", "input7", "input8"};
inline constexpr std::array<std::string_view, kImageSwitchInputCount> kChannelSetSwitchInputPorts{
    "channels1", "channels2", "channels3", "channels4",
    "channels5", "channels6", "channels7", "channels8"};
inline constexpr std::string_view kImageSwitchOutputPort = "image";
inline constexpr std::string_view kChannelSetSwitchOutputPort = "channels";
// Both switches' value output: the input shown, counted from 1, and 0 when it shows nothing.
inline constexpr std::string_view kImageSwitchSelectedPort = "selected";

inline constexpr std::string_view kImageSwitchIndexRole = "index";
inline constexpr std::string_view kImageSwitchWrapRole = "wrap";
inline constexpr std::string_view kImageSwitchIndexSchemaKey = "bloom.image-switch.index";
inline constexpr std::string_view kImageSwitchWrapSchemaKey = "bloom.image-switch.wrap";

inline constexpr double kDefaultImageSwitchIndex = 1.0;
// The stored index is any number within this magnitude (a driven index may count frames).
inline constexpr double kMaximumImageSwitchIndexMagnitude = 1.0e9;

[[nodiscard]] constexpr bool isImageSwitchNodeType(const std::string_view typeId) noexcept {
    return typeId == kImageSwitchNodeType || typeId == kChannelSetSwitchNodeType;
}

[[nodiscard]] constexpr bool isImageSwitchIndexWithinDomain(const double value) noexcept {
    return value >= -kMaximumImageSwitchIndexMagnitude &&
           value <= kMaximumImageSwitchIndexMagnitude;
}

// The position (from 0) in `count` inputs that an index selects, or nullopt when there is no input
// or the index is not finite: the whole part (rounded toward negative infinity) counts from 1 and is
// clamped to [1, count], or with `wrap` taken modulo `count`. The one rule the compiler (a
// constant index) and the evaluators (an animated or driven one) share.
[[nodiscard]] constexpr std::optional<std::size_t>
imageSwitchPosition(const std::size_t count, const double index, const bool wrap) noexcept {
    if (count == 0 || !(index > -9.0e15 && index < 9.0e15))
        return std::nullopt;
    auto whole = static_cast<std::int64_t>(index);
    if (static_cast<double>(whole) > index)
        --whole;
    const auto position = whole - 1;
    const auto size = static_cast<std::int64_t>(count);
    if (wrap)
        return static_cast<std::size_t>(((position % size) + size) % size);
    return static_cast<std::size_t>(position < 0 ? 0 : (position >= size ? size - 1 : position));
}

// The input ports of a switch node type, in order.
[[nodiscard]] constexpr const std::array<std::string_view, kImageSwitchInputCount>&
imageSwitchInputPorts(const std::string_view typeId) noexcept {
    return typeId == kChannelSetSwitchNodeType ? kChannelSetSwitchInputPorts
                                               : kImageSwitchInputPorts;
}

} // namespace bloom::document
