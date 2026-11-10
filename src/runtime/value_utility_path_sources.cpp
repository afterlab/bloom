// #509 Mask Path and Text Outline: paths read from what an artist already drew, so Path Stroke,
// Trim Path and the rest take them like any other Path value
// (docs/architecture/path-processing.md).
//
// Mask Path answers the masks of a layer (or a Mask node's one path) at the frame, one mask's
// contours after another in mask order. Text Outline answers a Text Source's glyph outlines, laid
// out exactly as the text draws them, from the same glyph source the text rasterizer reads: the
// host font-shaping preparation the GPU contract already names, so neither node adds a pixel
// operation. Both are bounded by the Path value's caps, refuse past them with a diagnostic, and
// poll cancellation in every loop that grows with the input.

#include "value_utility_path_support.hpp"

#include <bloom/document/value_utility_nodes.hpp>
#include <bloom/render/path_raster.hpp>
#include <bloom/render/text_raster.hpp>

#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

namespace {

using namespace bloom;
using document::PathValue;
using runtime::ValueUtilityInvocation;
using runtime::ValueUtilityOutcome;
using runtime::detail::shapedOutcome;
using runtime::detail::ValueUtilityReader;
namespace support = runtime::detail::pathsupport;

void publishWithCount(ValueUtilityOutcome& outcome, PathValue value) {
    outcome.outputs[1] = static_cast<std::int64_t>(value.contours.size());
    support::publish(outcome, std::move(value), 0);
}

ValueUtilityOutcome maskPath(const ValueUtilityInvocation& invocation) {
    auto outcome = shapedOutcome(invocation.operation);
    ValueUtilityReader reader(invocation);
    const auto which = reader.integer(0);
    if (reader.malformed())
        return outcome;
    // The masks follow the one operand, in mask order. 0 reads every mask; n reads the nth.
    const std::size_t masks = invocation.operands.size() - 1;
    PathValue result;
    bool first = true;
    if (which < 0)
        return support::refuse(std::move(outcome), "Mask Path mask is out of range",
                               "Mask is 0 for every mask, or 1 for the first, 2 for the second...");
    for (std::size_t index = 0; index < masks; ++index) {
        if (support::stopRequested(invocation))
            return support::cancelledOutcome(invocation);
        if (which != 0 && static_cast<std::uint64_t>(which) != index + 1)
            continue;
        const auto& mask = reader.path(index + 1);
        if (reader.malformed())
            return outcome;
        // The result keeps the fill rule of the first mask it holds.
        if (first) {
            result.fillRule = mask.fillRule;
            first = false;
        }
        for (const auto& contour : mask.contours) {
            if (support::stopRequested(invocation))
                return support::cancelledOutcome(invocation);
            if (result.contours.size() >= document::kMaximumPathContours)
                return support::refuse(shapedOutcome(invocation.operation),
                                       "Mask Path result is too large",
                                       "The masks hold more contours than a path may hold.");
            result.contours.push_back(contour);
        }
    }
    if (!support::fitsValue(result))
        return support::refuse(shapedOutcome(invocation.operation), "Mask Path result is too large",
                               "The masks hold more anchors than a path may hold.");
    publishWithCount(outcome, std::move(result));
    return outcome;
}

ValueUtilityOutcome textOutline(const ValueUtilityInvocation& invocation) {
    auto outcome = shapedOutcome(invocation.operation);
    // No text to read: the empty path (the node shows why in its readout).
    if (invocation.textSource == nullptr || invocation.operands.empty()) {
        publishWithCount(outcome, PathValue{});
        return outcome;
    }
    ValueUtilityReader reader(invocation);
    const auto content = reader.text(0);
    const double size = reader.scalar(1);
    const double lineHeight = reader.scalar(2);
    const double letterSpacing = reader.scalar(3);
    const auto alignment = reader.integer(4);
    if (reader.malformed())
        return outcome;
    const auto parameters = render::TextRasterParameters::create(size, size);
    if (!parameters || !std::isfinite(lineHeight) || !std::isfinite(letterSpacing) ||
        alignment < 0 || alignment > 2)
        return support::refuse(std::move(outcome), "Text Outline text is not usable",
                               "The text's size, line height, letter spacing or alignment cannot "
                               "be laid out.");
    // The text's own layout, in its own (author) space: the space a Shape Source on the same layer
    // draws a path in.
    const auto& fixed = *invocation.textSource;
    render::TextLayoutOptions layout;
    layout.alignment = static_cast<render::TextAlignment>(alignment);
    layout.lineHeight = lineHeight;
    layout.letterSpacing = letterSpacing;
    layout.multiline = true;
    layout.boxWidth = fixed.box.x;
    layout.boxHeight = fixed.box.y;
    layout.wrap = fixed.wrap;
    layout.verticalAlignment =
        static_cast<render::TextLayoutOptions::VerticalAlignment>(fixed.verticalAlignment);
    layout.anchorMode = render::resolveTextAnchorMode(fixed.anchorMode, layout.alignment);
    layout.overflow = static_cast<render::TextLayoutOptions::Overflow>(fixed.overflow);
    auto outlines =
        render::textOutlines(fixed.font, content, *parameters.value(), layout,
                             [&invocation] { return support::stopRequested(invocation); });
    if (!outlines) {
        if (support::stopRequested(invocation))
            return support::cancelledOutcome(invocation);
        return support::refuse(std::move(outcome), "Text Outline glyphs are unavailable",
                               "The text's glyph outlines could not be read from its font.");
    }
    const auto& paths = *outlines.value();
    if (paths.size() > document::kMaximumPathContours)
        return support::refuse(std::move(outcome), "Text Outline result is too large",
                               "The text has more glyph contours than a path may hold; shorten "
                               "it.");
    PathValue result;
    // Glyph outlines are filled by the non-zero rule, as the text draws them.
    result.fillRule = document::PathFillRule::NonZero;
    result.contours.reserve(paths.size());
    std::size_t total = 0;
    for (const auto& path : paths) {
        if (support::stopRequested(invocation))
            return support::cancelledOutcome(invocation);
        total += path.anchors.size();
        if (path.anchors.size() > document::kMaximumPathAnchors ||
            total > document::kMaximumPathValueAnchors)
            return support::refuse(shapedOutcome(invocation.operation),
                                   "Text Outline result is too large",
                                   "The text has more glyph anchors than a path may hold; shorten "
                                   "it.");
        if (path.anchors.empty())
            continue;
        document::PathContour contour;
        contour.closed = path.closed;
        contour.anchors.reserve(path.anchors.size());
        for (const auto& anchor : path.anchors) {
            document::PathAnchor each{{anchor.point.x, anchor.point.y}, std::nullopt, std::nullopt};
            if (anchor.inHandle)
                each.inHandle = document::Vec2d{anchor.inHandle->x, anchor.inHandle->y};
            if (anchor.outHandle)
                each.outHandle = document::Vec2d{anchor.outHandle->x, anchor.outHandle->y};
            each.mode = document::inferPathAnchorMode(each);
            contour.anchors.push_back(each);
        }
        result.contours.push_back(std::move(contour));
    }
    publishWithCount(outcome, std::move(result));
    return outcome;
}

} // namespace

namespace bloom::runtime::detail {

ValueUtilityOutcome evaluateValueMaskPath(const ValueUtilityInvocation& invocation) {
    return maskPath(invocation);
}

ValueUtilityOutcome evaluateValueTextOutline(const ValueUtilityInvocation& invocation) {
    return textOutline(invocation);
}

} // namespace bloom::runtime::detail
