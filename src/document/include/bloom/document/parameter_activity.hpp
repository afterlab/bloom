#pragma once

// Whether a node's parameter does anything for the node's current constant switches and modes:
// Gaussian Blur's vertical radius reads nothing until Separate axes is on, its channel radii nothing
// until Per-channel radius is on, and Denoise & Smooth's frame count only in Temporal mode, its
// radius in every mode but Temporal, its strength only while Auto strength is off and its sample
// region only while Auto strength is on (#524, #514). #757: a Text Source's background color,
// padding and corner radius only while Background is not Off, its outline color and join only while
// Outline width is not a constant 0.
//
// One rule for every reader: Properties and the Timeline hide an inactive row, and the compiler
// leaves an inactive parameter's animation curve out of the plan, so a curve keyed on a value the
// node does not read is neither lowered nor refused as a curve nothing references.

#include <bloom/document/project.hpp>

#include <string_view>

namespace bloom::document {

[[nodiscard]] bool isParameterInactive(const Composition& composition, const NodeRecord& node,
                                       std::string_view schemaKey);

// The same rule for one bound parameter, told apart by its role as well: a few rows share a generic
// operand key with rows that stay active, so the key alone cannot say. #591: while a space is
// connected to Function Plot, Axes or Axis Tick, the space places the drawing and their own Origin
// and Pixels Per Unit are not read (Axis Tick's Label Offset, the same kind of value, still is).
// Every reader that knows the role asks this one.
[[nodiscard]] bool isParameterInactive(const Composition& composition, const NodeRecord& node,
                                       std::string_view schemaKey, std::string_view role);

} // namespace bloom::document
