#include <bloom/document/path_value.hpp>

#include <algorithm>
#include <cmath>
#include <string_view>
#include <unordered_map>

namespace bloom::document {
namespace {

[[nodiscard]] Vec2d lerp(const Vec2d a, const Vec2d b, const double t) noexcept {
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
}

// A neighbor's shortened handle stays collinear but is no longer symmetric, and an Auto neighbor
// must keep exactly this geometry, so neither keeps its mode.
void settleNeighbor(PathAnchor& anchor) noexcept {
    const auto mode =
        anchor.mode == PathAnchorMode::Corner ? inferPathAnchorMode(anchor) : anchor.mode;
    anchor.mode = mode == PathAnchorMode::Mirrored || mode == PathAnchorMode::Auto
                      ? PathAnchorMode::Aligned
                      : mode;
}

} // namespace

std::size_t contourSegmentCount(const PathContour& contour) noexcept {
    if (contour.anchors.size() < 2)
        return 0;
    return contour.closed ? contour.anchors.size() : contour.anchors.size() - 1;
}

std::array<Vec2d, 4> contourSegmentControls(const PathContour& contour, const std::size_t segment) {
    const auto& a = contour.anchors[segment];
    const auto& b = contour.anchors[(segment + 1) % contour.anchors.size()];
    return {a.point, a.outHandle.value_or(a.point), b.inHandle.value_or(b.point), b.point};
}

std::optional<std::size_t> subdivideContourSegment(PathContour& contour, const std::size_t segment,
                                                   const double t) {
    if (segment >= contourSegmentCount(contour) || contour.anchors.size() >= kMaximumPathAnchors ||
        !std::isfinite(t))
        return std::nullopt;
    const auto u = std::clamp(t, 0.0, 1.0);
    const auto first = segment;
    const auto second = (segment + 1) % contour.anchors.size();
    auto& a = contour.anchors[first];
    auto& b = contour.anchors[second];
    PathAnchor inserted;
    if (!a.outHandle && !b.inHandle) {
        inserted.point = lerp(a.point, b.point, u);
        inserted.mode = PathAnchorMode::Corner;
    } else {
        const auto controls = contourSegmentControls(contour, segment);
        const auto p01 = lerp(controls[0], controls[1], u);
        const auto p12 = lerp(controls[1], controls[2], u);
        const auto p23 = lerp(controls[2], controls[3], u);
        const auto p012 = lerp(p01, p12, u);
        const auto p123 = lerp(p12, p23, u);
        inserted.point = lerp(p012, p123, u);
        inserted.inHandle = p012;
        inserted.outHandle = p123;
        inserted.mode = PathAnchorMode::Aligned;
        if (a.outHandle) {
            a.outHandle = p01;
            settleNeighbor(a);
        }
        if (b.inHandle) {
            b.inHandle = p23;
            settleNeighbor(b);
        }
    }
    const auto at = first + 1;
    contour.anchors.insert(contour.anchors.begin() + static_cast<std::ptrdiff_t>(at), inserted);
    return at;
}

std::vector<std::pair<std::size_t, std::size_t>> matchPathGroups(const PathValue& from,
                                                                 const PathValue& to) {
    std::unordered_map<std::string_view, std::size_t> keys;
    keys.reserve(to.groups.size());
    for (std::size_t index = 0; index < to.groups.size(); ++index)
        keys.emplace(to.groups[index].key, index);
    std::vector<std::pair<std::size_t, std::size_t>> matches;
    for (std::size_t index = 0; index < from.groups.size(); ++index)
        if (const auto match = keys.find(from.groups[index].key); match != keys.end())
            matches.emplace_back(index, match->second);
    return matches;
}

bool pathValueHasFlatForm(const PathValue& value) noexcept {
    return value.isSimple() && (value.contours.empty() || !value.contours.front().anchors.empty() ||
                                value.contours.front().closed);
}

bool pathKeysInterpolate(const PathValue& from, const PathValue& to) noexcept {
    return from.contours.size() == to.contours.size() &&
           std::ranges::equal(
               from.contours, to.contours, [](const PathContour& a, const PathContour& b) {
                   return a.anchors.size() == b.anchors.size() && a.closed == b.closed;
               });
}

} // namespace bloom::document
