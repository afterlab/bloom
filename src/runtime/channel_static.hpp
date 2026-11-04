#pragma once

// #480: what the compiler knows about a channel expression without probing its file sources.
//
// An Image to Channel Set layer is static: its channel names, roles and pixels (the stored red,
// green, blue and alpha of an image operation) are known when the graph compiles. A file source's
// channels are only known once it is probed at evaluation. The compiler uses this view to decide
// how a Shuffle or Pass reads its slots:
//   - every slot a file channel or a constant: the ordinary gather (CompiledChannelShuffle), resolved
//     at evaluation;
//   - every slot an image channel or a constant: Channel Select and Channel Combine effects over the
//     image operations, exact GPU stored-channel gathers;
//   - a mix, or a slot whose source the compiler cannot decide, is refused with a diagnostic.

#include <bloom/render/channel_set_image.hpp>
#include <bloom/runtime/channel_expression.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bloom::runtime::detail {

enum class StaticChannelKind : std::uint8_t {
    // A channel of an Image to Channel Set source: `source` and `lane` (0 red .. 3 alpha).
    Image,
    // A file channel, or a name only a probe can resolve.
    File,
    Constant,
    // An image channel that a file channel of the same name may replace at evaluation.
    Ambiguous,
    // Certainly not in the set.
    Absent,
};

struct StaticChannel final {
    std::string name;
    render::ChannelDescriptor descriptor;
    StaticChannelKind kind = StaticChannelKind::File;
    std::uint32_t source = 0;
    std::uint8_t lane = 0;
    float constant = 0.0F;
};

struct StaticChannelSet final {
    // The channels the compiler can name, in resolution order.
    std::vector<StaticChannel> known;
    // Whether the set may also hold file channels the compiler cannot name.
    bool files = false;
};

// #480: the roles an input colour processor must never touch -- every role but colour, luminance,
// alpha and coverage (depth, normals, motion, position, UVs, IDs and Data).
[[nodiscard]] inline bool isDataChannelRole(const render::ChannelRole role) noexcept {
    using render::ChannelRole;
    return role != ChannelRole::ColorR && role != ChannelRole::ColorG &&
           role != ChannelRole::ColorB && role != ChannelRole::Alpha &&
           role != ChannelRole::Luminance && role != ChannelRole::Coverage;
}

// The root set of `expression` (its last step). An empty expression is an empty set.
[[nodiscard]] StaticChannelSet staticChannelSet(const CompiledChannelExpression& expression);

// A Shuffle slot ("0", "1", a channel name, or empty for the default colour layer's channel at
// `slotIndex`) against a static set.
[[nodiscard]] StaticChannel staticChannelSlot(const StaticChannelSet& set, std::string_view slot,
                                              std::size_t slotIndex);

// The descriptors of an Image to Channel Set layer: `<layer>.<name>` for each name, in lane order.
// Colour encoding gives the red, green and blue lanes colour roles and the alpha lane Alpha; Data
// keeps a name's data role (depth.Z is Depth, N.X is NormalX) and makes every colour role Data, so
// an Auto Shuffle reads the layer raw.
[[nodiscard]] std::vector<render::ChannelDescriptor>
imageChannelSetDescriptors(std::string_view layer, std::span<const std::string> names,
                           bool colourEncoding);

// One channel of a set as seen by the Pass picker.
struct PassCandidate final {
    std::string name;
    render::ChannelDescriptor descriptor;
};

// #480 Pass: the Shuffle mapping for `pass` over `channels` -- a channel name gives
// {c, c, c, "1"}; a layer gives its R/G/B, X/Y/Z, U/V/W (or the first three of its non-alpha
// channels) with "0" for a missing slot and "1" for alpha; empty reads the first data layer (else
// the first layer). Nullopt when the pass is not in the set.
[[nodiscard]] std::optional<std::array<std::string, 4>>
passMapping(std::span<const PassCandidate> channels, std::string_view pass);

} // namespace bloom::runtime::detail
