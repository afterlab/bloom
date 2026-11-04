#pragma once

// #479 Separate Channels and Combine Channels: the durable vocabulary of the two image channel
// nodes. They work on any Image (footage, a nested composition, a generator, a processed result)
// and on render passes such as mist, Z-depth, normals, motion vectors and IDs: every value is the
// stored Float32 channel, unclamped and never colour converted (only alpha stays within [0, 1]).
//
// Separate Channels has one Image input and four Image outputs, red, green, blue and alpha, each
// a grey image (R = G = B = the channel, A = 1) over the input's data window. Combine Channels has
// four optional Image inputs in the same order, a source channel per input and an alpha mode, and
// one Image output.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace bloom::document {

inline constexpr std::string_view kSeparateChannelsNodeType = "bloom.separate-channels";
inline constexpr std::string_view kCombineChannelsNodeType = "bloom.combine-channels";
inline constexpr std::uint32_t kImageChannelNodeSchemaVersion = 1;

inline constexpr std::string_view kSeparateChannelsInputPort = "input";
inline constexpr std::string_view kCombineChannelsOutputPort = "image";
inline constexpr std::size_t kImageChannelCount = 4;
// Separate Channels' outputs and Combine Channels' inputs, in component order.
inline constexpr std::array<std::string_view, kImageChannelCount> kImageChannelPortNames{
    "red", "green", "blue", "alpha"};
// Combine Channels' per-input source channel parameters, in the same order.
inline constexpr std::array<std::string_view, kImageChannelCount> kCombineChannelsSourceRoles{
    "redSource", "greenSource", "blueSource", "alphaSource"};
inline constexpr std::string_view kCombineChannelsSourceSchemaKey = "bloom.combine-channels.source";
inline constexpr std::string_view kCombineChannelsAlphaModeRole = "alphaMode";
inline constexpr std::string_view kCombineChannelsAlphaModeSchemaKey =
    "bloom.combine-channels.alpha-mode";

// The source channel enumeration (the dropdown's stored values): the stored red, green, blue or
// alpha value, or the Rec. 709 luma of the stored colour.
inline constexpr std::int64_t kImageChannelSourceRed = 0;
inline constexpr std::int64_t kImageChannelSourceGreen = 1;
inline constexpr std::int64_t kImageChannelSourceBlue = 2;
inline constexpr std::int64_t kImageChannelSourceAlpha = 3;
inline constexpr std::int64_t kImageChannelSourceLuma = 4;
inline constexpr std::int64_t kImageChannelSourceCount = 5;
// Red, green and blue read their own channel; alpha reads red, the channel every grey image
// (a Separate Channels output, a render pass, a Mask) carries its value in, so Separate Channels
// into Combine Channels reproduces the image exactly.
inline constexpr std::array<std::int64_t, kImageChannelCount> kCombineChannelsSourceDefaults{
    kImageChannelSourceRed, kImageChannelSourceGreen, kImageChannelSourceBlue,
    kImageChannelSourceRed};

// The alpha association the combined channels declare. Premultiplied (the default) takes red,
// green and blue as already associated with the alpha, as Bloom stores every image and as
// Separate Channels outputs them. Straight multiplies red, green and blue by the alpha.
inline constexpr std::int64_t kCombineChannelsPremultiplied = 0;
inline constexpr std::int64_t kCombineChannelsStraight = 1;

[[nodiscard]] constexpr bool isImageChannelNodeType(const std::string_view typeId) noexcept {
    return typeId == kSeparateChannelsNodeType || typeId == kCombineChannelsNodeType;
}

} // namespace bloom::document
