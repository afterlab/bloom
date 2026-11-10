// #597 Shape Morph: one path morphed into another (docs/architecture/path-processing.md). The
// arithmetic is path_morph.cpp's, shared with the keyframes of a path value; this is the node:
// reading its operands, choosing the pairing, observing cancellation while the shapes align, and
// refusing with a diagnostic that names what was asked.

#include "path_morph.hpp"
#include "value_utility_path_support.hpp"

#include <bloom/document/parameter.hpp>
#include <bloom/document/shape_morph_nodes.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

namespace bloom::runtime::detail {
namespace {

// A diagnostic's text is built per failure and kept until the evaluator has copied it (it does so
// as soon as the kernel returns, on the same thread).
[[nodiscard]] ValueUtilityOutcome tooMany(ValueUtilityOutcome outcome,
                                          const pathmorph::Morphed& morphed,
                                          const document::MorphPairing pairing) {
    thread_local std::string summary;
    thread_local std::string detail;
    summary = "Shape Morph result is too large";
    char buffer[384];
    const char* const how = pairing == document::MorphPairing::ByOrder ? "By order"
                                                                      : "By group, then geometry";
    if (morphed.contour)
        std::snprintf(buffer, sizeof buffer,
                      "Shape Morph, pairing %s: a paired contour would hold %zu points and a "
                      "contour holds at most %zu. Simplify one of the shapes or resample it to "
                      "fewer points.",
                      how, morphed.requested, document::kMaximumPathAnchors);
    else
        std::snprintf(buffer, sizeof buffer,
                      "Shape Morph, pairing %s: the shapes aligned would hold %zu contours or points "
                      "and a path holds at most %zu contours and %zu points. Simplify one of the "
                      "shapes or resample it to fewer points.",
                      how, morphed.requested, document::kMaximumPathContours,
                      document::kMaximumPathValueAnchors);
    detail = buffer;
    outcome.failed = true;
    outcome.summary = summary;
    outcome.detail = detail;
    return outcome;
}

} // namespace

ValueUtilityOutcome evaluateValuePathMorph(const ValueUtilityInvocation& invocation) {
    using namespace pathsupport;
    auto outcome = shapedOutcome(invocation.operation);
    ValueUtilityReader reader(invocation);
    const auto& from = reader.path(0);
    const auto& to = reader.path(1);
    const double percent = reader.scalar(2);
    const auto pairing =
        selectorAt(invocation, 0, document::kMorphPairings,
                   document::MorphPairing::ByGroupThenGeometry);
    if (reader.malformed())
        return outcome;
    if (!std::isfinite(percent))
        return refuse(std::move(outcome), "Shape Morph amount is not finite",
                      "The amount must be a finite number of percent.");
    if (!coordinatesUsable(from) || !coordinatesUsable(to))
        return refuse(std::move(outcome), "Shape Morph path is out of range",
                      "Every coordinate must be finite and within a billion pixels.");

    pathmorph::AlignOptions options;
    options.pairing = pairing == document::MorphPairing::ByOrder
                          ? pathmorph::Pairing::ByOrder
                          : pathmorph::Pairing::ByGroupThenGeometry;
    options.cancellation = invocation.limits.cancellation;
    auto morphed = pathmorph::morphPathsChecked(from, to, percent / 100.0, options);
    switch (morphed.status) {
    case pathmorph::AlignStatus::Ok:
        break;
    case pathmorph::AlignStatus::Cancelled:
        // A cancelled evaluation keeps nothing.
        outcome.cancelled = true;
        return shapedOutcome(invocation.operation);
    case pathmorph::AlignStatus::TooMany:
        return tooMany(shapedOutcome(invocation.operation), morphed, pairing);
    }
    publish(outcome, std::move(morphed.value));
    return outcome;
}

} // namespace bloom::runtime::detail
