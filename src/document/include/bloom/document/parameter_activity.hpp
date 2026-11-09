#pragma once

// Whether a node's parameter does anything for the node's current constant switches and modes:
// Gaussian Blur's vertical radius reads nothing until Separate axes is on, its channel radii nothing
// until Per-channel radius is on, and Denoise & Smooth's frame count only in Temporal mode, its
// radius in every mode but Temporal, its strength only while Auto strength is off and its sample
// region only while Auto strength is on (#524, #514).
//
// One rule for every reader: Properties and the Timeline hide an inactive row, and the compiler
// leaves an inactive parameter's animation curve out of the plan, so a curve keyed on a value the
// node does not read is neither lowered nor refused as a curve nothing references.

#include <bloom/document/project.hpp>

#include <string_view>

namespace bloom::document {

[[nodiscard]] bool isParameterInactive(const Composition& composition, const NodeRecord& node,
                                       std::string_view schemaKey);

} // namespace bloom::document
