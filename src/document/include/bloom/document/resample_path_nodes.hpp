#pragma once

// #637 Resample Path (docs/architecture/path-processing.md). The node's type, its two selectors'
// vocabularies and the schema keys of the operands that change with the mode. The stored integers of
// both selectors are durable.

#include <bloom/document/value_utility_nodes.hpp>

#include <array>
#include <cstdint>
#include <string_view>

namespace bloom::document {

inline constexpr std::string_view kResamplePathNodeType = "bloom.resample-path";

inline constexpr std::string_view kResampleModeParameterSchemaKey = "bloom.path.resample-mode";
inline constexpr std::string_view kResampleOutputParameterSchemaKey = "bloom.path.resample-output";
// The operands the mode decides between: Properties shows the one the mode reads.
inline constexpr std::string_view kResampleCountSchemaKey = "bloom.resample.count";
inline constexpr std::string_view kResampleGapSchemaKey = "bloom.resample.gap";
inline constexpr std::string_view kResampleDensitySchemaKey = "bloom.resample.density";
inline constexpr std::string_view kResamplePercentageSchemaKey = "bloom.resample.percentage";
inline constexpr std::string_view kResampleStartOffsetSchemaKey = "bloom.resample.start-offset";

// How the number of points on each contour is decided: a set number, a set distance, a set number per
// 100 pixels of length, or a share of the contour's own point count.
enum class ResampleMode : std::uint8_t { FixedCount = 0, Gap = 1, ByLength = 2, Percentage = 3 };
inline constexpr std::array<ResampleMode, 4> kResampleModes{
    ResampleMode::FixedCount, ResampleMode::Gap, ResampleMode::ByLength, ResampleMode::Percentage};
// Straight segments between the points, or curves that keep the shape the contour had.
enum class ResampleOutput : std::uint8_t { Straight = 0, Curves = 1 };
inline constexpr std::array<ResampleOutput, 2> kResampleOutputs{ResampleOutput::Straight,
                                                                ResampleOutput::Curves};

// The fewest points a resampled contour keeps.
inline constexpr std::int64_t kMinimumResampleOpenPoints = 2;
inline constexpr std::int64_t kMinimumResampleClosedPoints = 3;

} // namespace bloom::document
