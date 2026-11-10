// #589: a path as a graph value (docs/architecture/path-value.md). Compound contours, their fill
// rule and keyed groups; subdivision and arc length; whole-value key interpolation; the Path socket
// from a Path node into Shape Source through the real compiler and CPU evaluator; and compound
// values through save and reopen. The GPU route and the viewer and export routes have their own
// native tests.

#include <bloom/commands/animation_operations.hpp>
#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/data_block_operations.hpp>
#include <bloom/commands/layer_mask_operations.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/document/color_settings.hpp>
#include <bloom/document/data_block.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/node_definition_registry.hpp>
#include <bloom/document/path_value.hpp>
#include <bloom/document/shape.hpp>
#include <bloom/document/value_nodes.hpp>
#include <bloom/project/open_archive.hpp>
#include <bloom/project/save_archive.hpp>
#include <bloom/render/path_raster.hpp>
#include <bloom/runtime/animation_sampling.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/curve_compilation.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/runtime/value_graph_evaluation.hpp>

#include "path_morph.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace bloom;
using document::PathAnchor;
using document::PathContour;
using document::PathFillRule;
using document::PathGroup;
using document::PathValue;
using document::Vec2d;

int failures = 0;
void expect(const bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

// A square as one contour, clockwise on screen, or the other way round.
PathContour square(const double left, const double top, const double size,
                   const bool reversed = false) {
    PathContour contour;
    contour.closed = true;
    contour.anchors = {{{left, top}, {}, {}},
                       {{left + size, top}, {}, {}},
                       {{left + size, top + size}, {}, {}},
                       {{left, top + size}, {}, {}}};
    if (reversed)
        std::ranges::reverse(contour.anchors);
    return contour;
}

// A 40-pixel square with a 16-pixel one inside it.
PathValue framed(const PathFillRule rule = PathFillRule::EvenOdd, const double inner = 12.0) {
    PathValue value;
    value.contours = {square(4, 4, 40), square(inner, inner, 16)};
    value.fillRule = rule;
    return value;
}

// ---- The value ----

void simpleValuesAreOneContour() {
    expect(PathValue{}.isSimple() && PathValue{}.contours.empty() && PathValue{}.isValid(),
           "the empty value is simple, valid and has no contours");
    const auto one = PathValue::fromContour(square(0, 0, 4));
    expect(one.contours.size() == 1 && one.isSimple() && one.anchorCount() == 4,
           "fromContour gives a simple value of one contour");
    expect(one.firstContour() == square(0, 0, 4), "firstContour reads that contour");
    expect(PathValue{}.firstContour().anchors.empty() && !PathValue{}.firstContour().closed,
           "firstContour of the empty value is an empty open contour");
    expect(PathValue::fromContour({}, false).contours.empty(),
           "an empty open contour is the empty value");
    expect(PathValue::fromContour({}, true).contours.size() == 1,
           "an empty closed contour stays, as an older document could hold it");
    auto compound = framed();
    expect(!compound.isSimple() && compound.anchorCount() == 8, "two contours are compound");
    auto ruled = one;
    ruled.fillRule = PathFillRule::NonZero;
    expect(!ruled.isSimple(), "a fill rule makes a value compound");
    auto keyed = one;
    keyed.groups = {{"a", 0, 1, std::nullopt}};
    expect(!keyed.isSimple(), "a group makes a value compound");
}

void capsAndGroupsAreValidated() {
    auto contour = square(0, 0, 4);
    contour.anchors.resize(document::kMaximumPathAnchors);
    expect(PathValue::fromContour(contour).isValid(), "the anchor cap is reached per contour");
    contour.anchors.emplace_back();
    expect(!PathValue::fromContour(contour).isValid(), "the anchor cap is enforced per contour");

    // The per-value cap: two whole contours pass, far more than one contour's cap.
    contour.anchors.resize(document::kMaximumPathAnchors);
    PathValue big;
    big.contours = {contour, contour};
    expect(big.isValid(), "several contours may each reach the cap");
    big.contours.assign(document::kMaximumPathValueAnchors / document::kMaximumPathAnchors + 1,
                        contour);
    expect(!big.isValid(), "the whole value has its own anchor cap");

    PathValue tooMany;
    tooMany.contours.resize(document::kMaximumPathContours + 1);
    expect(!tooMany.isValid(), "the contour count is capped");

    auto nonFinite = PathValue::fromContour(square(0, 0, 4));
    nonFinite.contours[0].anchors[0].point.x = std::nan("");
    expect(!nonFinite.isValid(), "a non-finite anchor is refused in a contour");

    const auto grouped = [](std::vector<PathGroup> groups) {
        PathValue value;
        value.contours = {square(0, 0, 1), square(2, 0, 1), square(4, 0, 1)};
        value.groups = std::move(groups);
        return value;
    };
    expect(grouped({{"a", 0, 1, std::nullopt}, {"b", 1, 2, std::nullopt}}).isValid(),
           "ascending groups are valid");
    expect(grouped({{"a", 2, 1, std::nullopt}}).isValid(), "groups need not cover every contour");
    expect(!grouped({{"", 0, 1, std::nullopt}}).isValid(), "a key is not empty");
    expect(!grouped({{"a", 0, 1, std::nullopt}, {"a", 1, 1, std::nullopt}}).isValid(),
           "keys are unique");
    expect(!grouped({{"a", 0, 2, std::nullopt}, {"b", 1, 1, std::nullopt}}).isValid(),
           "groups do not overlap");
    expect(!grouped({{"b", 1, 1, std::nullopt}, {"a", 0, 1, std::nullopt}}).isValid(),
           "groups ascend");
    expect(!grouped({{"a", 2, 2, std::nullopt}}).isValid(), "a group stays inside the contours");
    expect(!grouped({{"a", 0, 0, std::nullopt}}).isValid(), "a group holds a contour");
    expect(
        !grouped({{std::string(document::kMaximumPathGroupKeyBytes + 1, 'k'), 0, 1, std::nullopt}})
             .isValid(),
        "a key has a length cap");
    expect(!grouped({{"\xff", 0, 1, std::nullopt}}).isValid(), "a key is UTF-8");
    expect(!grouped({{"a", 0, 1, core::Color4d{std::nan(""), 0, 0, 1}}}).isValid(),
           "a group color is finite");
    expect(grouped({{"a", 0, 1, core::Color4d{1, 0.5, 0, 1}}}).isValid(), "a group holds a color");
}

void keysCompareAcrossTwoPaths() {
    expect(document::pathGroupKeyFromSpan(3, 7) == "span:3:7", "a span key names its span");
    PathValue from;
    from.contours = {square(0, 0, 1), square(2, 0, 1), square(4, 0, 1)};
    from.groups = {{"first", 0, 1, std::nullopt},
                   {document::pathGroupKeyFromSpan(0, 2), 1, 1, std::nullopt},
                   {"only here", 2, 1, std::nullopt}};
    PathValue to;
    to.contours = {square(0, 0, 1), square(2, 0, 1), square(4, 0, 1), square(6, 0, 1)};
    to.groups = {{document::pathGroupKeyFromSpan(0, 2), 0, 2, core::Color4d{1, 0, 0, 1}},
                 {"first", 3, 1, std::nullopt}};
    const auto matched = document::matchPathGroups(from, to);
    expect(matched == std::vector<std::pair<std::size_t, std::size_t>>{{0, 1}, {1, 0}},
           "groups match by key, wherever they sit, in the first path's order");
    expect(document::matchPathGroups(from, PathValue{}).empty(), "no shared key, no pair");
}

void flatFormIsTheOneSpelling() {
    expect(document::pathValueHasFlatForm(PathValue{}), "the empty value is flat");
    expect(document::pathValueHasFlatForm(PathValue::fromContour(square(0, 0, 1))),
           "one contour is flat");
    expect(!document::pathValueHasFlatForm(framed()), "two contours are not");
    PathValue emptyOpen;
    emptyOpen.contours.emplace_back();
    expect(!document::pathValueHasFlatForm(emptyOpen),
           "one empty open contour is not, because flat reads back as no contours");
}

// ---- Subdivision ----

render::Path renderPath(const PathContour& contour) {
    render::Path path;
    path.closed = contour.closed;
    for (const auto& anchor : contour.anchors) {
        render::PathAnchor out;
        out.point = {anchor.point.x, anchor.point.y};
        if (anchor.inHandle)
            out.inHandle = render::PathPoint{anchor.inHandle->x, anchor.inHandle->y};
        if (anchor.outHandle)
            out.outHandle = render::PathPoint{anchor.outHandle->x, anchor.outHandle->y};
        path.anchors.push_back(out);
    }
    return path;
}

std::vector<std::uint8_t> coverageOf(const std::span<const render::Path> contours,
                                     const std::uint32_t width, const std::uint32_t height,
                                     const render::PathFillRule rule, const bool stroke = false,
                                     const render::PathStroke style = {}) {
    auto raster = render::PathRaster::create(contours, style, 1.0, 1.0);
    if (!raster)
        throw std::runtime_error("raster");
    std::vector<std::uint8_t> pixels(std::size_t{width} * height);
    for (std::uint32_t y = 0; y < height; ++y)
        if (!raster.value()->coverageRow(
                0, y, std::span(pixels).subspan(std::size_t{y} * width, width), rule, stroke))
            throw std::runtime_error("coverage row");
    return pixels;
}

// Largest coverage difference between two rasters, out of 255.
int largestDifference(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b) {
    int largest = a.size() == b.size() ? 0 : 255;
    for (std::size_t i = 0; largest != 255 && i < a.size(); ++i)
        largest = std::max(largest, std::abs(int{a[i]} - int{b[i]}));
    return largest;
}

PathContour curved() {
    PathContour contour;
    contour.closed = true;
    contour.anchors = {{{10.0, 40.0}, Vec2d{4.0, 60.0}, Vec2d{14.0, 8.0}},
                       {{60.0, 12.0}, Vec2d{40.0, 2.0}, Vec2d{84.0, 26.0}},
                       {{50.0, 60.0}, Vec2d{70.0, 70.0}, std::nullopt}};
    return contour;
}

void subdivisionKeepsTheShape() {
    const auto original = curved();
    const std::array<render::Path, 1> before{renderPath(original)};
    const auto reference = coverageOf(before, 96, 80, render::PathFillRule::NonZero);
    for (const double t : {0.5, 0.25, 0.1, 0.9}) {
        for (std::size_t segment = 0; segment < document::contourSegmentCount(original);
             ++segment) {
            auto split = original;
            const auto index = document::subdivideContourSegment(split, segment, t);
            expect(index == segment + 1 && split.anchors.size() == original.anchors.size() + 1,
                   "subdivision inserts the anchor after its segment's start");
            const std::array<render::Path, 1> after{renderPath(split)};
            const auto pixels = coverageOf(after, 96, 80, render::PathFillRule::NonZero);
            // Two flattenings of one curve, each within 1/32 pixel of it: a sample row a near-level
            // edge skims can flip, four samples of a pixel's sixteen, and nothing more.
            expect(largestDifference(reference, pixels) <= 4 * 255 / 16 + 1,
                   "subdividing at " + std::to_string(t) + " leaves the raster as it was");

            // Exactly the same curve: the two parts trace the original segment.
            const auto bezier = [](const std::array<Vec2d, 4>& c, const double u) {
                const double v = 1.0 - u;
                const auto at = [&](const double a, const double b, const double d,
                                    const double e) {
                    return v * v * v * a + 3 * v * v * u * b + 3 * v * u * u * d + u * u * u * e;
                };
                return Vec2d{at(c[0].x, c[1].x, c[2].x, c[3].x),
                             at(c[0].y, c[1].y, c[2].y, c[3].y)};
            };
            const auto whole = document::contourSegmentControls(original, segment);
            const auto left = document::contourSegmentControls(split, segment);
            const auto right = document::contourSegmentControls(split, segment + 1);
            for (int step = 0; step <= 16; ++step) {
                const double u = step / 16.0;
                const auto expected = bezier(whole, u);
                const auto actual =
                    u < t ? bezier(left, u / t) : bezier(right, (u - t) / (1.0 - t));
                expect(std::abs(expected.x - actual.x) < 1e-9 &&
                           std::abs(expected.y - actual.y) < 1e-9,
                       "the two parts trace the original segment exactly");
            }

            // The split point is on the curve: de Casteljau from the original control points.
            const auto controls = document::contourSegmentControls(original, segment);
            const auto lerp = [t](const Vec2d a, const Vec2d b) {
                return Vec2d{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
            };
            const auto p01 = lerp(controls[0], controls[1]);
            const auto p12 = lerp(controls[1], controls[2]);
            const auto p23 = lerp(controls[2], controls[3]);
            const auto point = lerp(lerp(p01, p12), lerp(p12, p23));
            const auto& inserted = split.anchors[segment + 1].point;
            expect(std::abs(inserted.x - point.x) < 1e-12 && std::abs(inserted.y - point.y) < 1e-12,
                   "the new anchor lies on the old curve");
        }
    }

    auto line = square(0, 0, 10);
    const auto index = document::subdivideContourSegment(line, 0, 0.5);
    expect(index == 1 && line.anchors[1].point == Vec2d{5.0, 0.0} && !line.anchors[1].inHandle &&
               !line.anchors[1].outHandle && !line.anchors[0].outHandle,
           "a straight segment stays straight, with no handles");
    auto closing = square(0, 0, 10);
    expect(document::subdivideContourSegment(closing, 3, 0.5) == 4 &&
               closing.anchors.back().point == Vec2d{0.0, 5.0},
           "a closed contour's closing segment subdivides");

    auto refused = square(0, 0, 10);
    expect(!document::subdivideContourSegment(refused, 4, 0.5), "no such segment");
    expect(!document::subdivideContourSegment(refused, 0, std::nan("")), "t must be finite");
    refused.anchors.resize(document::kMaximumPathAnchors);
    expect(!document::subdivideContourSegment(refused, 0, 0.5),
           "a contour at its cap refuses instead of growing past it");
}

// ---- Arc length ----

void arcLengthIsTheStrokedPolyline() {
    const auto square10 = renderPath(square(0, 0, 10));
    const auto closed = render::PathArcLength::create(square10, 1.0, 1.0);
    expect(closed && closed->length() == 40.0, "a closed square measures its closing segment");
    if (closed) {
        expect(closed->pointAt(0.0) == render::PathPoint{0.0, 0.0} &&
                   closed->pointAt(15.0) == render::PathPoint{10.0, 5.0},
               "a point is found by distance");
        expect(closed->pointAt(-3.0) == render::PathPoint{0.0, 0.0} &&
                   closed->pointAt(1000.0) == closed->pointAt(40.0),
               "distance clamps to the ends");
    }
    auto openSquare = square10;
    openSquare.closed = false;
    const auto open = render::PathArcLength::create(openSquare, 1.0, 1.0);
    expect(open && open->length() == 30.0, "an open contour has no closing segment");

    // A curve: the same polyline PathRaster flattened for its stroke and fill, at the same scales,
    // and the same arithmetic every time.
    const auto curve = renderPath(curved());
    for (const double scale : {0.5, 1.0, 3.0}) {
        const auto measured = render::PathArcLength::create(curve, scale, scale);
        auto raster = render::PathRaster::create(curve, {}, scale, scale);
        expect(measured.has_value() && static_cast<bool>(raster), "both flatten");
        if (!measured || !raster)
            continue;
        const auto points = measured->points();
        const auto drawn = raster.value()->flattenedContour();
        expect(points.size() == drawn.size() + 1 &&
                   std::ranges::equal(points.first(drawn.size()), drawn) &&
                   points.back() == points.front(),
               "arc length measures the polyline the raster draws, and its closing segment");
        const auto again = render::PathArcLength::create(curve, scale, scale);
        expect(again && again->length() == measured->length() &&
                   again->pointAt(measured->length() / 3) ==
                       measured->pointAt(measured->length() / 3),
               "the parameterization is deterministic");
    }

    // The flattened length converges on the curve's: a quarter circle Bezier measured fine.
    constexpr double kappa = 0.5522847498307935;
    render::Path quarter;
    quarter.anchors = {{{100.0, 0.0}, std::nullopt, render::PathPoint{100.0, 100.0 * kappa}},
                       {{0.0, 100.0}, render::PathPoint{100.0 * kappa, 100.0}, std::nullopt}};
    const auto arc = render::PathArcLength::create(quarter, 4.0, 4.0);
    expect(arc && std::abs(arc->length() - 100.0 * 3.14159265358979 / 2.0) < 0.05,
           "a quarter circle measures its arc");

    auto cancelled = render::PathArcLength::create(curve, 1.0, 1.0, [] { return true; });
    expect(!cancelled.has_value(), "measuring is cancellable");
}

// ---- Raster ----

constexpr std::uint8_t kFull = 255;

std::uint8_t at(const std::vector<std::uint8_t>& pixels, const std::uint32_t width,
                const std::uint32_t x, const std::uint32_t y) {
    return pixels[std::size_t{y} * width + x];
}

void compoundContoursFillTogether() {
    const std::array<render::Path, 2> same{renderPath(square(4, 4, 40)),
                                           renderPath(square(12, 12, 16))};
    const auto even = coverageOf(same, 48, 48, render::PathFillRule::EvenOdd);
    expect(at(even, 48, 6, 6) == kFull && at(even, 48, 20, 20) == 0 && at(even, 48, 2, 2) == 0,
           "even-odd cuts the inner contour as a hole");
    const auto nonzero = coverageOf(same, 48, 48, render::PathFillRule::NonZero);
    expect(at(nonzero, 48, 6, 6) == kFull && at(nonzero, 48, 20, 20) == kFull,
           "nonzero with one winding fills through");
    const std::array<render::Path, 2> opposite{renderPath(square(4, 4, 40)),
                                               renderPath(square(12, 12, 16, true))};
    const auto holed = coverageOf(opposite, 48, 48, render::PathFillRule::NonZero);
    expect(at(holed, 48, 6, 6) == kFull && at(holed, 48, 20, 20) == 0,
           "nonzero with opposite winding cuts a hole");

    // One contour is exactly the single-path raster, sample for sample.
    const auto curve = renderPath(curved());
    const std::array<render::Path, 1> one{curve};
    render::PathStroke stroke;
    stroke.width = 3.0;
    auto single = render::PathRaster::create(curve, stroke, 1.0, 1.0);
    expect(static_cast<bool>(single), "the single contour rasterizes");
    if (single) {
        std::vector<std::uint8_t> row(96);
        bool identical = true;
        for (const bool drawStroke : {false, true}) {
            const auto spanned =
                coverageOf(one, 96, 80, render::PathFillRule::NonZero, drawStroke, stroke);
            for (std::uint32_t y = 0; y < 80; ++y) {
                if (!single.value()->coverageRow(0, y, row, render::PathFillRule::NonZero,
                                                 drawStroke))
                    identical = false;
                identical = identical &&
                            std::memcmp(row.data(), spanned.data() + std::size_t{y} * 96, 96) == 0;
            }
        }
        expect(identical, "a span of one is the single-path raster");
    }

    // Both contours stroke, and any open contour centers the whole stroke.
    render::PathStroke inside;
    inside.width = 4.0;
    inside.align = render::PathStrokeAlign::Inside;
    const auto insideClosed = coverageOf(same, 48, 48, render::PathFillRule::NonZero, true, inside);
    expect(at(insideClosed, 48, 20, 3) == 0 && at(insideClosed, 48, 20, 5) == kFull,
           "closed contours keep an inside stroke inside");
    // Open at the outer contour's closing segment, its left edge.
    auto openOuter = renderPath(square(4, 4, 40));
    openOuter.closed = false;
    const std::array<render::Path, 2> mixed{openOuter, renderPath(square(12, 12, 16))};
    const auto centered = coverageOf(mixed, 48, 48, render::PathFillRule::NonZero, true, inside);
    expect(at(centered, 48, 20, 3) == kFull,
           "an open contour centers the whole stroke, on both sides of its edge");
    expect(at(centered, 48, 3, 20) == 0, "and an open contour has no closing segment to stroke");
    expect(at(centered, 48, 11, 20) == kFull && at(centered, 48, 20, 20) == 0,
           "the closed inner contour strokes centered too");
}

// ---- Keys ----

document::PathAnimationCurve curveOf(std::vector<std::pair<std::int64_t, PathValue>> keys) {
    document::PathAnimationCurve curve;
    curve.id = document::AnimationCurveId::fromRaw(1);
    std::uint64_t id = 1;
    for (auto& [second, value] : keys) {
        document::PathKeyframe key;
        key.id = document::KeyframeId::fromRaw(id++);
        key.time = core::RationalTime::fromInteger(second);
        key.value = std::move(value);
        curve.keyframes.push_back(std::move(key));
    }
    return curve;
}

PathValue sampled(const document::PathAnimationCurve& curve, const int quarter) {
    const auto value = runtime::samplePathCurve(runtime::compileAnimationCurve(curve),
                                                *core::RationalTime::create(quarter, 4));
    if (!value)
        throw std::runtime_error("the curve samples");
    return *value;
}

void keysInterpolateOrMorph() {
    auto start = framed(PathFillRule::EvenOdd);
    start.groups = {{"outer", 0, 1, core::Color4d{1, 0, 0, 1}}};
    auto end = framed(PathFillRule::NonZero, 20.0);
    end.groups = {{"other", 1, 1, std::nullopt}};
    expect(document::pathKeysInterpolate(start, end), "matching contours interpolate");
    const auto halfway = sampled(curveOf({{0, start}, {1, end}}), 2);
    expect(halfway.contours.size() == 2 &&
               halfway.contours[1].anchors[0].point == Vec2d{16.0, 16.0},
           "every contour's anchors interpolate");
    expect(halfway.fillRule == PathFillRule::EvenOdd && halfway.groups == start.groups,
           "the fill rule and groups are the earlier key's");
    expect(sampled(curveOf({{0, start}, {1, end}}), 4) == end, "the later key arrives exactly");

    // #597: keys that do not interpolate morph through the shared alignment step: exactly the
    // earlier key at its time, exactly the later at its own, and between them the morph itself (the
    // same value the Shape Morph node answers), carrying the earlier key's fill rule and groups.
    const auto morphs = [&](const PathValue& other, const std::string& why) {
        expect(!document::pathKeysInterpolate(start, other), why + ": the keys do not interpolate");
        const auto curve = curveOf({{0, start}, {1, other}});
        expect(sampled(curve, 0) == start, why + ": the earlier key is exact at its time");
        const auto between = sampled(curve, 3);
        const auto expected = runtime::pathmorph::morphPaths(start, other, 0.75);
        expect(expected.has_value() && between == *expected, why + ": the keys morph between");
        expect(between != start && between != other, why + ": the morph is neither key");
        expect(between.fillRule == start.fillRule && between.groups == start.groups,
               why + ": the morph carries the earlier key's fill rule and groups");
        expect(sampled(curve, 4) == other, why + ": the next key takes over exactly at its time");
        // Hold still holds.
        auto held = runtime::compileAnimationCurve(curve);
        held.keyframes.front().outgoingInterpolation = runtime::CompiledKeyframeInterpolation::Hold;
        const auto holding = runtime::samplePathCurve(held, *core::RationalTime::create(3, 4));
        expect(holding && *holding == start, why + ": a Hold key still holds");
    };
    auto fewerContours = start;
    fewerContours.contours.pop_back();
    morphs(fewerContours, "a different contour count");
    auto moreAnchors = start;
    moreAnchors.contours[1].anchors.push_back(
        {{30, 30}, std::nullopt, std::nullopt, document::PathAnchorMode::Corner});
    morphs(moreAnchors, "a different anchor count");
    auto openContour = start;
    openContour.contours[1].closed = false;
    morphs(openContour, "a different closed flag");

    // Keys that already interpolate give exactly the result they always gave: every point and
    // handle mixed by index, nothing aligned, byte for byte.
    {
        auto withHandles = start;
        withHandles.contours[0].anchors[1].outHandle = Vec2d{12, -6};
        auto other = end;
        other.contours[1].anchors[2].inHandle = Vec2d{9, 33};
        const auto curve = curveOf({{0, withHandles}, {1, other}});
        const auto value = sampled(curve, 1);
        PathValue expected;
        expected.fillRule = withHandles.fillRule;
        expected.groups = withHandles.groups;
        for (std::size_t c = 0; c < withHandles.contours.size(); ++c) {
            auto& out = expected.contours.emplace_back();
            out.closed = withHandles.contours[c].closed;
            for (std::size_t i = 0; i < withHandles.contours[c].anchors.size(); ++i) {
                const auto& a = withHandles.contours[c].anchors[i];
                const auto& b = other.contours[c].anchors[i];
                const auto mix = [](const Vec2d p, const Vec2d q) {
                    return Vec2d{p.x + (q.x - p.x) * 0.25, p.y + (q.y - p.y) * 0.25};
                };
                document::PathAnchor anchor;
                anchor.point = mix(a.point, b.point);
                if (a.inHandle || b.inHandle)
                    anchor.inHandle =
                        mix(a.inHandle.value_or(a.point), b.inHandle.value_or(b.point));
                if (a.outHandle || b.outHandle)
                    anchor.outHandle =
                        mix(a.outHandle.value_or(a.point), b.outHandle.value_or(b.point));
                anchor.mode = a.mode;
                out.anchors.push_back(anchor);
            }
        }
        expect(value == expected, "keys that interpolate are mixed by index exactly as before");
    }

    // One contour is the rule it always was.
    const auto one = PathValue::fromContour(square(0, 0, 8));
    const auto two = PathValue::fromContour(square(8, 0, 8));
    const auto midway = sampled(curveOf({{0, one}, {1, two}}), 2);
    expect(midway == PathValue::fromContour(square(4, 0, 8)), "one contour interpolates as before");
}

// ---- The socket, through the real compiler and evaluator ----

struct Fixture final {
    document::NewProject initial = document::makeNewProject(
        "Paths", "Main", core::RationalTime::fromInteger(4),
        *document::CompositionFormat::create(48, 48, core::PixelAspectRatio::square(),
                                             document::FrameRate::framesPerSecond24()));
    document::Document document{std::move(initial.project)};
    commands::CommandStack commands{document};
    document::CompositionId composition = initial.initialCompositionId;

    template <typename Operation, typename... Arguments>
    commands::CommandResult run(Arguments&&... arguments) {
        commands::Transaction transaction("Paths", document.snapshot().revision());
        transaction.emplace<Operation>(std::forward<Arguments>(arguments)...);
        auto result = commands.execute(std::move(transaction));
        if (!result.succeeded())
            throw std::runtime_error("fixture command refused: " +
                                     (result.operationFailures.empty()
                                          ? std::string("no detail")
                                          : result.operationFailures.front().issue.message));
        return result;
    }
    template <typename Operation, typename... Arguments> bool refused(Arguments&&... arguments) {
        commands::Transaction transaction("Paths", document.snapshot().revision());
        transaction.emplace<Operation>(std::forward<Arguments>(arguments)...);
        return !commands.execute(std::move(transaction)).succeeded();
    }
    [[nodiscard]] const document::Composition& main() const {
        return *document.snapshot().project().findComposition(composition);
    }
    [[nodiscard]] document::ParameterId parameter(const document::NodeId node,
                                                  const std::string_view role) const {
        for (const auto& binding : main().graph().findNode(node)->parameters)
            if (binding.role == role)
                return binding.parameterId;
        throw std::runtime_error("missing parameter");
    }
    document::NodeId addPathNode() {
        return *run<commands::AddNode>(composition, std::string(document::kPathValueNodeType),
                                       Vec2d{0, 0})
                    .outputId<document::NodeId>(commands::kAddNodeOutput);
    }
    document::NodeId addShape(const PathValue& path = PathValue::fromContour(square(0, 0, 1)),
                              const core::Color4d color = {1, 1, 1, 1}) {
        commands::ShapeLayerGeometry geometry;
        geometry.path = path;
        commands::ShapeLayerStyle style;
        style.fillColor = color;
        return *run<commands::AddShapeLayer>(composition, document::ShapeKind::Path, geometry,
                                             style)
                    .outputId<document::NodeId>("shapeNode");
    }
    void drive(const document::NodeId producer, const document::NodeId shape) {
        run<commands::ConnectPorts>(
            composition, document::OutputPortRef{producer, std::string(document::kValuePortName)},
            document::NodeInputRef{shape, "path"});
    }
    void key(const document::NodeId producer, const std::int64_t second, PathValue value) {
        run<commands::SetKeyframeAtTimeForParameter>(
            composition, parameter(producer, document::kValueParameterRole),
            core::RationalTime::fromInteger(second), std::move(value));
    }
    void set(const document::NodeId node, const std::string_view role,
             document::ParameterValue value) {
        run<commands::SetParameterSource>(composition, parameter(node, role),
                                          document::ConstantValueSource{std::move(value)});
    }
    // Makes the key at `second` of the producer's value a Hold key.
    void hold(const document::NodeId producer, const std::int64_t second) {
        const auto* record =
            main().parameters().find(parameter(producer, document::kValueParameterRole));
        const auto curveId = std::get<document::AnimationCurveSource>(record->source).curveId;
        const auto& curve =
            std::get<document::PathAnimationCurve>(*main().animationCurves().find(curveId));
        for (const auto& keyframe : curve.keyframes)
            if (keyframe.time == core::RationalTime::fromInteger(second))
                run<commands::SetKeyframeInterpolation>(composition, curveId, keyframe.id,
                                                        document::KeyframeInterpolation::Hold);
    }
};

struct Frame final {
    std::shared_ptr<const runtime::ProcessFrame> frame;
    runtime::OperationCacheStatistics statistics;

    [[nodiscard]] float alpha(const std::int64_t x, const std::int64_t y) const {
        const auto& image = frame->processImage();
        const auto window = image.descriptor()->dataWindow();
        if (x < window.originX() || y < window.originY() || x >= window.maxXExclusive() ||
            y >= window.maxYExclusive())
            return 0.0F;
        return image
            .pixels()[static_cast<std::size_t>(
                (y - window.originY()) * static_cast<std::int64_t>(window.extent().width()) +
                (x - window.originX()))]
            .alpha();
    }
    [[nodiscard]] bool operator==(const Frame& other) const {
        const auto a = frame->processImage().pixels();
        const auto b = other.frame->processImage().pixels();
        return a.size_bytes() == b.size_bytes() &&
               std::memcmp(a.data(), b.data(), a.size_bytes()) == 0;
    }
};

std::shared_ptr<const runtime::CompiledCompositionPlan> compile(const Fixture& fixture) {
    const runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
    auto compiled = compiler.compile({fixture.document.snapshot(), fixture.composition}, {});
    if (!compiled.plan)
        throw std::runtime_error(
            "the fixture does not compile: " +
            runtime::describeCompileFailure(compiled, fixture.document.snapshot().project(),
                                            fixture.composition));
    return compiled.plan;
}

Frame render(const runtime::CpuCompositionEvaluator& evaluator,
             const std::shared_ptr<const runtime::CompiledCompositionPlan>& plan, const int quarter,
             const runtime::CancellationToken& cancellation = {}) {
    Frame out;
    const auto result = evaluator.evaluate(
        plan,
        {*core::RationalTime::create(quarter, 4), plan->output(),
         runtime::CompositionFormatResolution{}, runtime::EvaluationQuality::Reference,
         runtime::EvaluationColorIntent::LinearRec709Scene, 1U << 26U},
        cancellation, {}, nullptr, &out.statistics);
    out.frame = result.frame();
    return out;
}

// A genuinely cancelled token, taken from a scheduler task that is cancelled while it runs.
runtime::CancellationToken cancelledToken() {
    using namespace runtime;
    TaskSchedulerConfig config = TaskSchedulerConfig::defaults();
    config.cpuWorkerCount = 1;
    config.blockingIoWorkerCount = 1;
    TaskScheduler scheduler(config);
    std::mutex mutex;
    std::condition_variable condition;
    bool entered = false;
    bool release = false;
    CancellationToken token;
    auto submission = scheduler.submit<void>(
        TaskRequest("path value cancel token",
                    {.kind = TaskOwnerKind::Composition, .id = TaskOwnerId::fromRaw(97)}),
        [&](TaskContext& context) {
            {
                std::lock_guard lock(mutex);
                token = context.cancellation();
                entered = true;
            }
            condition.notify_all();
            std::unique_lock lock(mutex);
            condition.wait(lock, [&] { return release; });
            return TaskResult<void>::succeeded();
        });
    if (!submission.accepted())
        throw std::logic_error("cancel token task was refused");
    {
        std::unique_lock lock(mutex);
        condition.wait(lock, [&] { return entered; });
    }
    submission.handle.cancel();
    {
        std::lock_guard lock(mutex);
        release = true;
    }
    condition.notify_all();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!submission.handle.tryTakeResult().has_value() &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return token;
}

void theSocketIsDeclared() {
    using document::SocketValueKind;
    const auto& definitions = document::builtInNodeDefinitions();
    const auto* shape = definitions.find(document::kShapeSourceNodeType, 1);
    expect(shape != nullptr, "Shape Source is registered");
    const auto input =
        shape ? std::ranges::find_if(shape->inputs,
                                     [](const auto& port) { return port.name == "path"; })
              : decltype(shape->inputs.end()){};
    expect(shape && input != shape->inputs.end() && input->valueKind == SocketValueKind::Path,
           "Shape Source reads a Path socket");
    const auto* node =
        definitions.find(document::kPathValueNodeType, document::kValueNodeSchemaVersion);
    expect(node != nullptr && node->outputs.size() == 1 &&
               node->outputs.front().valueKind == SocketValueKind::Path,
           "the Path node outputs a Path socket");
    expect(document::socketKindForParameterValueKind(document::ParameterValueKind::Path) ==
               SocketValueKind::Path,
           "a path parameter's socket kind is Path");
    for (const auto other : {SocketValueKind::Scalar, SocketValueKind::Vector2,
                             SocketValueKind::Image, SocketValueKind::String}) {
        expect(!document::isAcceptedSocketConnection(SocketValueKind::Path, other) &&
                   !document::isAcceptedSocketConnection(other, SocketValueKind::Path),
               "nothing promotes into or out of Path");
    }
    expect(document::isAcceptedSocketConnection(SocketValueKind::Path, SocketValueKind::Path),
           "Path connects to Path");

    Fixture fixture;
    const auto producer = fixture.addPathNode();
    const auto solid =
        *fixture
             .run<commands::AddNode>(fixture.composition,
                                     std::string(document::kSolidSourceNodeType), Vec2d{0, 0})
             .outputId<document::NodeId>(commands::kAddNodeOutput);
    expect(fixture.refused<commands::ConnectPorts>(
               fixture.composition,
               document::OutputPortRef{producer, std::string(document::kValuePortName)},
               document::NodeInputRef{solid, "width"}),
           "a Path output does not connect to a Scalar operand");
}

void aDrivenPathRendersThroughTheEvaluator() {
    Fixture fixture;
    const auto producer = fixture.addPathNode();
    const auto shape = fixture.addShape();
    fixture.set(producer, document::kValueParameterRole, framed());
    fixture.drive(producer, shape);
    runtime::CpuCompositionEvaluator evaluator;
    const auto plan = compile(fixture);
    const auto frame = render(evaluator, plan, 0);
    expect(frame.frame != nullptr, "the driven shape renders");
    if (!frame.frame)
        return;
    expect(frame.alpha(6, 6) == 1.0F && frame.alpha(20, 20) == 0.0F && frame.alpha(2, 2) == 0.0F,
           "the driven path replaces the parameter and cuts its hole");

    // The same value as the shape's own constant is the same picture.
    Fixture constant;
    constant.addShape(framed());
    const auto same = render(runtime::CpuCompositionEvaluator{}, compile(constant), 0);
    expect(same.frame && same == frame, "driven and constant are the same pixels");
}

void theValuesFillRuleWins() {
    const auto holeAt = [](const PathFillRule rule, const std::int64_t parameter) {
        Fixture fixture;
        const auto shape = fixture.addShape(framed(rule));
        fixture.set(shape, "fillRule", parameter);
        const auto frame = render(runtime::CpuCompositionEvaluator{}, compile(fixture), 0);
        return frame.frame && frame.alpha(20, 20) == 0.0F && frame.alpha(6, 6) == 1.0F;
    };
    constexpr std::int64_t nonZero = 0;
    constexpr std::int64_t evenOdd = 1;
    expect(holeAt(PathFillRule::EvenOdd, nonZero),
           "the value's even-odd beats a nonzero parameter");
    expect(!holeAt(PathFillRule::NonZero, evenOdd),
           "the value's nonzero beats an even-odd parameter");
    expect(holeAt(PathFillRule::Inherit, evenOdd), "inherit follows an even-odd parameter");
    expect(!holeAt(PathFillRule::Inherit, nonZero), "inherit follows a nonzero parameter");
}

void aKeyedPathAnimatesPerFrame() {
    Fixture fixture;
    const auto producer = fixture.addPathNode();
    const auto shape = fixture.addShape();
    fixture.drive(producer, shape);
    fixture.key(producer, 0, framed(PathFillRule::EvenOdd, 12.0));
    fixture.key(producer, 1, framed(PathFillRule::EvenOdd, 20.0));
    auto oneContour = framed(PathFillRule::EvenOdd, 20.0);
    oneContour.contours.pop_back();
    fixture.key(producer, 2, oneContour);
    const auto plan = compile(fixture);
    runtime::CpuCompositionEvaluator evaluator;
    const auto start = render(evaluator, plan, 0);
    const auto middle = render(evaluator, plan, 2);
    const auto end = render(evaluator, plan, 4);
    const auto holding = render(evaluator, plan, 6);
    const auto last = render(evaluator, plan, 8);
    expect(start.frame && middle.frame && end.frame && holding.frame && last.frame,
           "every frame renders");
    if (!start.frame || !middle.frame || !end.frame || !holding.frame || !last.frame)
        return;
    expect(start.alpha(14, 14) == 0.0F && start.alpha(34, 34) == 1.0F, "the first key's hole");
    expect(end.alpha(14, 14) == 1.0F && end.alpha(34, 34) == 0.0F, "the second key's hole");
    expect(middle.alpha(18, 18) == 0.0F && middle.alpha(14, 14) == 1.0F && !(middle == start) &&
               !(middle == end),
           "the hole moves between the keys");
    // #597: toward a key of a different shape (one contour fewer: the hole shrinks to its centre)
    // the path morphs instead of holding.
    expect(!(holding == end) && !(holding == last),
           "the path morphs toward a key of a different shape");
    expect(last.alpha(34, 34) == 1.0F, "and the next key arrives exactly at its time");
    {
        // A Hold key still holds up to the next, whatever its shape.
        Fixture held;
        const auto heldProducer = held.addPathNode();
        held.drive(heldProducer, held.addShape());
        held.key(heldProducer, 1, framed(PathFillRule::EvenOdd, 20.0));
        held.key(heldProducer, 2, oneContour);
        held.hold(heldProducer, 1);
        const auto heldPlan = compile(held);
        runtime::CpuCompositionEvaluator heldEvaluator;
        expect(render(heldEvaluator, heldPlan, 6) == render(heldEvaluator, heldPlan, 4),
               "a Hold key holds up to a key of a different shape");
    }

    // Cache identity: the time the path holds its value is one operation, and a changed frame is
    // evaluated anew.
    // The second key made Hold: the frames after it hold one value.
    fixture.hold(producer, 1);
    const auto holdingPlan = compile(fixture);
    runtime::CpuCompositionEvaluator cached;
    (void)render(cached, holdingPlan, 4);
    const auto repeat = render(cached, holdingPlan, 4);
    expect(repeat.statistics.misses == 0 && repeat.statistics.hits > 0,
           "the same frame is served from the operation cache");
    const auto held = render(cached, holdingPlan, 5);
    expect(!held.statistics.evaluatedNodes.empty() || held.statistics.hits > 0,
           "a held frame resolves through the cache");
    expect(held == end, "and is the same picture");
    const auto moved = render(cached, holdingPlan, 3);
    expect(moved.statistics.misses > 0, "a frame whose path moved is evaluated");

    // Invalidation: editing the producer changes the next compile's picture.
    fixture.key(producer, 0, framed(PathFillRule::EvenOdd, 6.0));
    const auto replanned = compile(fixture);
    const auto edited = render(cached, replanned, 0);
    expect(edited.frame && !(edited == start), "an edited key invalidates the frame it reaches");
    expect(edited.statistics.misses > 0, "the edit is a cache miss");
}

void severalConsumersReadOneOutput() {
    Fixture fixture;
    const auto producer = fixture.addPathNode();
    fixture.set(producer, document::kValueParameterRole, framed());
    const auto first = fixture.addShape();
    const auto second = fixture.addShape();
    fixture.drive(producer, first);
    fixture.drive(producer, second);
    fixture.set(second, "fillRule", std::int64_t{0});
    const auto plan = compile(fixture);
    const auto frame = render(runtime::CpuCompositionEvaluator{}, plan, 0);
    expect(frame.frame && frame.alpha(20, 20) == 0.0F && frame.alpha(6, 6) == 1.0F,
           "two shapes read one Path output and both draw it");

    // The value graph holds one output for the producer, read twice.
    std::size_t readers = 0;
    std::optional<runtime::ValueOutputIndex> output;
    for (const auto& operation : plan->operations()) {
        const auto* lowered = std::get_if<runtime::CompiledShape>(&operation);
        if (lowered == nullptr || !lowered->drivenPath)
            continue;
        ++readers;
        expect(!output || *output == *lowered->drivenPath, "both read the same output");
        output = lowered->drivenPath;
    }
    expect(readers == 2, "both shapes lowered a driven path");
}

void aOneContourShapeIsTheShapeItAlwaysWas() {
    // A driven one-contour path with the shape's own fill rule is the shape's constant path.
    for (const std::int64_t rule : {std::int64_t{0}, std::int64_t{1}}) {
        const auto contour = PathValue::fromContour(curved());
        Fixture constant;
        const auto plain = constant.addShape(contour, {0.5, 0.25, 1, 1});
        constant.set(plain, "fillRule", rule);
        const auto expected = render(runtime::CpuCompositionEvaluator{}, compile(constant), 0);

        Fixture driven;
        const auto producer = driven.addPathNode();
        driven.set(producer, document::kValueParameterRole, contour);
        const auto shape =
            driven.addShape(PathValue::fromContour(square(1, 1, 1)), {0.5, 0.25, 1, 1});
        driven.set(shape, "fillRule", rule);
        driven.drive(producer, shape);
        const auto actual = render(runtime::CpuCompositionEvaluator{}, compile(driven), 0);
        expect(expected.frame && actual.frame && expected == actual,
               "a driven one-contour path is the constant shape, pixel for pixel");
    }
}

void anUnconnectedShapeKeepsItsOwnPath() {
    Fixture fixture;
    const auto producer = fixture.addPathNode();
    fixture.set(producer, document::kValueParameterRole, framed());
    fixture.addShape(PathValue::fromContour(square(0, 0, 8)));
    const auto frame = render(runtime::CpuCompositionEvaluator{}, compile(fixture), 0);
    // The layer centers its content on its position: the small square covers the frame's center,
    // where the unconnected framed value has its hole, and nothing of the frame's edge.
    expect(frame.frame && frame.alpha(24, 24) == 1.0F && frame.alpha(6, 6) == 0.0F,
           "an unconnected socket leaves the parameter's path in charge");
}

void cancellationIsObserved() {
    Fixture fixture;
    const auto producer = fixture.addPathNode();
    const auto shape = fixture.addShape();
    fixture.drive(producer, shape);
    fixture.key(producer, 0, framed(PathFillRule::EvenOdd, 12.0));
    fixture.key(producer, 1, framed(PathFillRule::EvenOdd, 20.0));
    const auto plan = compile(fixture);
    runtime::CpuCompositionEvaluator evaluator;
    const auto frame = render(evaluator, plan, 2, cancelledToken());
    expect(frame.frame == nullptr, "a cancelled evaluation publishes no frame");
    const auto after = render(evaluator, plan, 2);
    expect(after.frame != nullptr, "the same evaluator renders the frame afterwards");
}

void theBudgetRefusesInsteadOfGrowing() {
    Fixture fixture;
    const auto producer = fixture.addPathNode();
    fixture.set(producer, document::kValueParameterRole, framed());
    const auto second = fixture.addPathNode();
    fixture.set(second, document::kValueParameterRole, framed());
    const auto shape = fixture.addShape();
    const auto other = fixture.addShape();
    fixture.drive(producer, shape);
    fixture.drive(second, other);
    const auto plan = compile(fixture);

    const auto evaluate = [&](const std::size_t anchors) {
        runtime::ValueGraphMemoization memoization;
        memoization.maxRetainedPathAnchors = anchors;
        return runtime::evaluateValueGraph(
            plan->valueOperations(), plan->valueOutputCount(), core::RationalTime{},
            plan->format().frameRate(),
            runtime::ValueGraphCurves{plan->scalarCurves(), plan->vec2Curves(),
                                      plan->color4Curves(), plan->vec3Curves()},
            memoization);
    };
    const auto enough = evaluate(16);
    expect(enough.diagnostics.empty(), "two eight-anchor paths fit sixteen anchors");
    const auto short_ = evaluate(15);
    const bool named = std::ranges::any_of(short_.diagnostics, [](const auto& diagnostic) {
        return diagnostic.summary == "Value graph path budget exceeded";
    });
    expect(named, "over the budget, the evaluation says so");
    const auto empties = std::ranges::count_if(short_.outputs, [](const auto& value) {
        const auto* path = std::get_if<runtime::CompiledPathValue>(&value);
        return path != nullptr && path->value->contours.empty();
    });
    expect(empties >= 1, "and the output that did not fit falls back to the empty path");
}

void timeDependenceFollowsTheCurve() {
    Fixture constant;
    const auto producer = constant.addPathNode();
    const auto shape = constant.addShape();
    constant.set(producer, document::kValueParameterRole, framed());
    constant.drive(producer, shape);
    const auto still = compile(constant);
    runtime::CpuCompositionEvaluator evaluator;
    (void)render(evaluator, still, 0);
    const auto again = render(evaluator, still, 3);
    expect(again.statistics.misses == 0,
           "a constant driven path does not re-evaluate at another time");
}

void surfacesThatHoldOneContourRefuseCompound() {
    Fixture fixture;
    const auto layer =
        *fixture
             .run<commands::AddSolidLayer>(fixture.composition, "Masked", core::Color4d{1, 1, 1, 1})
             .outputId<document::LayerId>(commands::kAddSolidLayerLayerOutput);
    expect(fixture.refused<commands::AddLayerMask>(fixture.composition, layer, framed()),
           "a layer mask refuses a compound path");
    auto ruled = PathValue::fromContour(square(0, 0, 8));
    ruled.fillRule = PathFillRule::EvenOdd;
    expect(fixture.refused<commands::AddLayerMask>(fixture.composition, layer, ruled),
           "a layer mask refuses a fill rule");
    auto keyed = PathValue::fromContour(square(0, 0, 8));
    keyed.groups = {{"a", 0, 1, std::nullopt}};
    expect(fixture.refused<commands::AddLayerMask>(fixture.composition, layer, keyed),
           "a layer mask refuses groups");
    expect(!fixture.refused<commands::AddLayerMask>(fixture.composition, layer,
                                                    PathValue::fromContour(square(0, 0, 8))),
           "a layer mask takes one contour as ever");
}

// ---- Save and reopen ----

project::ProjectIoOperationMemory memory() {
    auto coordinator = project::ProjectIoMemoryCoordinator::create();
    if (!coordinator)
        throw std::runtime_error("memory coordinator");
    auto operation = coordinator->createOperation();
    if (!operation)
        throw std::runtime_error("memory operation");
    return std::move(*operation);
}

void compoundValuesSaveAndReopen() {
    Fixture fixture;
    const auto producer = fixture.addPathNode();
    auto keyed = framed();
    keyed.groups = {{"outer", 0, 1, core::Color4d{1, 0.5, 0.25, 1}},
                    {document::pathGroupKeyFromSpan(0, 3), 1, 1, std::nullopt}};
    fixture.key(producer, 0, keyed);
    auto later = keyed;
    later.contours[1] = square(20, 20, 16);
    fixture.key(producer, 1, later);
    const auto driven = fixture.addShape();
    fixture.drive(producer, driven);
    fixture.addShape(framed(PathFillRule::NonZero));
    // Every remaining spelling: an empty open contour among others, and alone.
    PathValue withEmpty;
    withEmpty.contours = {PathContour{}, square(0, 0, 4)};
    fixture.addShape(withEmpty);
    PathValue emptyOpen;
    emptyOpen.contours.emplace_back();
    fixture.addShape(emptyOpen);

    document::DataBlockRecord block;
    block.id = document::DataBlockRecordId::fromRaw(1);
    block.kind = document::DataBlockKind::Path;
    block.typeId = "bloom.test.data-block";
    block.schemaVersion = {1, 0};
    block.mediaType = "application/vnd.bloom.test-data-block";
    block.provenance.source = document::DataBlockProducer{"bloom.test", "1", {}};
    block.provenance.createdAt = "2026-09-17T00:00:00Z";
    block.payload = keyed;
    fixture.run<commands::CreateDataBlock>(block);

    const auto snapshot = fixture.document.snapshot();
    expect(snapshot.project().validate().ok(), "the compound project validates");
    const auto settings = document::makeBloomNeutralColorSettingsV1({});
    auto saved = project::buildVerifiedSaveArchive(
        {}, {.snapshot = &snapshot, .colorSettings = &settings}, {}, memory());
    expect(static_cast<bool>(saved), "the compound project saves and verifies");
    if (!saved)
        return;
    auto opening = project::openProjectArchive(saved.archive()->bytes(), {}, memory());
    expect(opening.outcome() == project::OpenArchiveOutcome::Opened, "it reopens");
    if (opening.outcome() != project::OpenArchiveOutcome::Opened)
        return;
    auto opened = std::move(opening).takeOpened();
    const auto again = opened.document->snapshot();
    const auto& before = *snapshot.project().findComposition(fixture.composition);
    const auto& after = *again.project().findComposition(fixture.composition);
    expect(std::ranges::equal(before.parameters().records(), after.parameters().records()),
           "every contour, fill rule and group on a parameter equals itself");
    expect(
        std::ranges::equal(before.animationCurves().records(), after.animationCurves().records()),
        "and on a key");
    expect(
        std::ranges::equal(snapshot.project().typedDataBlocks(), again.project().typedDataBlocks()),
        "and in a data block");
    // A node's bindings persist in role order; each still names its parameter.
    const auto sameBindings = [](const document::NodeRecord& a, const document::NodeRecord& b) {
        auto left = a.parameters;
        auto right = b.parameters;
        const auto byRole = [](const auto& x, const auto& y) { return x.role < y.role; };
        std::ranges::sort(left, byRole);
        std::ranges::sort(right, byRole);
        return a.typeId == b.typeId && a.schemaVersion == b.schemaVersion && left == right;
    };
    expect(std::ranges::all_of(before.graph().nodes(),
                               [&](const document::NodeRecord& node) {
                                   const auto* reopened = after.graph().findNode(node.id);
                                   return reopened != nullptr && sameBindings(node, *reopened);
                               }),
           "the Path node and its binding survive");
    const auto first = render(runtime::CpuCompositionEvaluator{}, compile(fixture), 2);
    const runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
    const auto recompiled = compiler.compile({again, fixture.composition}, {});
    expect(static_cast<bool>(recompiled.plan), "the reopened project compiles");
    if (recompiled.plan)
        expect(first.frame &&
                   render(runtime::CpuCompositionEvaluator{}, recompiled.plan, 2) == first,
               "the reopened project renders the same frame");
}

} // namespace

int main() {
    try {
        simpleValuesAreOneContour();
        capsAndGroupsAreValidated();
        keysCompareAcrossTwoPaths();
        flatFormIsTheOneSpelling();
        subdivisionKeepsTheShape();
        arcLengthIsTheStrokedPolyline();
        compoundContoursFillTogether();
        keysInterpolateOrMorph();
        theSocketIsDeclared();
        aDrivenPathRendersThroughTheEvaluator();
        theValuesFillRuleWins();
        aKeyedPathAnimatesPerFrame();
        severalConsumersReadOneOutput();
        aOneContourShapeIsTheShapeItAlwaysWas();
        anUnconnectedShapeKeepsItsOwnPath();
        cancellationIsObserved();
        theBudgetRefusesInsteadOfGrowing();
        timeDependenceFollowsTheCurve();
        surfacesThatHoldOneContourRefuseCompound();
        compoundValuesSaveAndReopen();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    return failures == 0 ? 0 : 1;
}
