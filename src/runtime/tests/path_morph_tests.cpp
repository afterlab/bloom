// #597 The alignment step a morph between two different path values runs through
// (src/runtime/path_morph.hpp, docs/architecture/path-value.md "Morphing between different
// shapes").
//
// Every case checks the step's contract: the two answers have the same structure, each draws
// exactly the shape it came from (or, for a part with no partner, a point at that part's centroid),
// and the step is a pure function (the same inputs twice give the same bytes). Then the case's own
// answer: which contour pairs with which, where a closed contour starts, which way it runs, and
// what the morph looks like half way.

#include "../path_morph.hpp"
#include "../path_resample.hpp"

#include <bloom/document/path_value.hpp>
#include <bloom/runtime/task_scheduler.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <numbers>
#include <source_location>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace bloom;
using document::PathAnchor;
using document::PathContour;
using document::PathGroup;
using document::PathValue;
using document::Vec2d;
namespace morph = runtime::pathmorph;

class Expectations final {
  public:
    void expect(const bool condition, const std::string_view message,
                const std::source_location location = std::source_location::current()) {
        if (condition)
            return;
        ++failures_;
        std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
    }
    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

[[nodiscard]] PathAnchor corner(const double x, const double y) {
    return {{x, y}, std::nullopt, std::nullopt, document::PathAnchorMode::Corner};
}

[[nodiscard]] PathContour polygon(const std::vector<Vec2d>& points, const bool closed = true) {
    PathContour contour;
    contour.closed = closed;
    for (const auto point : points)
        contour.anchors.push_back(corner(point.x, point.y));
    return contour;
}

[[nodiscard]] PathContour square(const double cx, const double cy, const double half) {
    return polygon({{cx - half, cy - half},
                    {cx + half, cy - half},
                    {cx + half, cy + half},
                    {cx - half, cy + half}});
}

// A regular polygon of `sides` corners, the first at angle `start` (radians), counter-clockwise on
// screen when `turn` is +1.
[[nodiscard]] PathContour regular(const double cx, const double cy, const double radius,
                                  const std::size_t sides, const double start = 0.0,
                                  const double turn = 1.0) {
    std::vector<Vec2d> points;
    for (std::size_t index = 0; index < sides; ++index) {
        const double angle = start + turn * 2.0 * std::numbers::pi * static_cast<double>(index) /
                                         static_cast<double>(sides);
        points.push_back({cx + radius * std::cos(angle), cy + radius * std::sin(angle)});
    }
    return polygon(points);
}

// A circle of four Bezier quarters.
[[nodiscard]] PathContour circle(const double cx, const double cy, const double radius) {
    const double k = 0.5522847498307936 * radius;
    PathContour contour;
    contour.closed = true;
    contour.anchors = {{{cx + radius, cy},
                        Vec2d{cx + radius, cy - k},
                        Vec2d{cx + radius, cy + k},
                        document::PathAnchorMode::Mirrored},
                       {{cx, cy + radius},
                        Vec2d{cx + k, cy + radius},
                        Vec2d{cx - k, cy + radius},
                        document::PathAnchorMode::Mirrored},
                       {{cx - radius, cy},
                        Vec2d{cx - radius, cy + k},
                        Vec2d{cx - radius, cy - k},
                        document::PathAnchorMode::Mirrored},
                       {{cx, cy - radius},
                        Vec2d{cx - k, cy - radius},
                        Vec2d{cx + k, cy - radius},
                        document::PathAnchorMode::Mirrored}};
    return contour;
}

[[nodiscard]] PathValue valueOf(std::vector<PathContour> contours) {
    PathValue value;
    value.contours = std::move(contours);
    return value;
}

[[nodiscard]] bool sameStructure(const PathValue& a, const PathValue& b) {
    return document::pathKeysInterpolate(a, b);
}

[[nodiscard]] Vec2d centroidOf(const PathContour& contour) {
    // The vertices' mean is the centroid for the regular shapes and squares these tests use.
    Vec2d sum{0, 0};
    for (const auto& anchor : contour.anchors)
        sum = {sum.x + anchor.point.x, sum.y + anchor.point.y};
    const auto count = static_cast<double>(contour.anchors.size());
    return {sum.x / count, sum.y / count};
}

[[nodiscard]] bool near(const Vec2d a, const Vec2d b, const double tolerance = 1e-9) {
    return std::abs(a.x - b.x) <= tolerance && std::abs(a.y - b.y) <= tolerance;
}

[[nodiscard]] bool collapsedAt(const PathContour& contour, const Vec2d point) {
    return !contour.anchors.empty() &&
           std::ranges::all_of(contour.anchors, [&](const PathAnchor& anchor) {
               return near(anchor.point, point, 1e-6) && !anchor.inHandle && !anchor.outHandle;
           });
}

// A drawn contour (as the fill flattens it) is the original's when the dense flattenings of the two
// lie within a hair of each other both ways.
[[nodiscard]] bool drawsAs(const PathContour& aligned, const PathContour& original) {
    // A contour cut open draws the closed one's whole outline, closing segment included, so it is
    // compared with the original opened the same way at its own first anchor.
    PathContour reference = original;
    if (original.closed && !aligned.closed) {
        reference.anchors.push_back(original.anchors.front());
        reference.anchors.back().outHandle.reset();
        reference.anchors.front().inHandle.reset();
        reference.closed = false;
    }
    return aligned.closed == reference.closed &&
           runtime::pathresample::deviation(aligned, reference) <= 1e-6;
}

// The contract every alignment keeps.
void checkContract(Expectations& expectations, const PathValue& from, const PathValue& to,
                   const morph::Alignment& aligned, const std::string& label) {
    expectations.expect(aligned.status == morph::AlignStatus::Ok, label + ": aligns");
    expectations.expect(sameStructure(aligned.from, aligned.to),
                        label + ": the two answers have the same structure");
    expectations.expect(morph::alignPaths(from, to).from == aligned.from &&
                            morph::alignPaths(from, to).to == aligned.to,
                        label + ": the same inputs give the same answer");
    expectations.expect(aligned.from.groups == from.groups &&
                            aligned.from.fillRule == from.fillRule,
                        label + ": the first answer keeps the first value's groups and fill rule");
    expectations.expect(morph::morphPaths(from, to, 0.0) == from &&
                            morph::morphPaths(from, to, 1.0) == to,
                        label + ": amount 0 is the first value exactly and 1 the second");
}

void differentAnchorCounts(Expectations& expectations) {
    // A square (4) into an octagon (8) of the same centre: the square gains a corner-free anchor at
    // the middle of each side, and the start of the octagon nearest the square's first corner is
    // chosen, so nothing spins.
    const auto from = valueOf({square(0, 0, 50)});
    const auto to = valueOf({regular(0, 0, 60, 8, -3.0 * std::numbers::pi / 4.0)});
    const auto aligned = morph::alignPaths(from, to);
    checkContract(expectations, from, to, aligned, "4 to 8 anchors");
    expectations.expect(aligned.from.contours[0].anchors.size() == 8,
                        "4 to 8: the square is subdivided to eight anchors");
    expectations.expect(drawsAs(aligned.from.contours[0], from.contours[0]) &&
                            drawsAs(aligned.to.contours[0], to.contours[0]),
                        "4 to 8: each aligned contour draws its own shape");
    // The first corner (-50, -50) meets the octagon's corner in the same direction.
    expectations.expect(
        near(aligned.to.contours[0].anchors[0].point,
             {60 * std::cos(-3 * std::numbers::pi / 4), 60 * std::sin(-3 * std::numbers::pi / 4)}),
        "4 to 8: corners meet corners in the same direction");
    // Half way, every anchor lies between the square and the octagon: no point crosses the centre.
    const auto half = morph::morphPaths(from, to, 0.5);
    expectations.expect(
        half.has_value() &&
            std::ranges::all_of(half->contours[0].anchors,
                                [](const auto& anchor) {
                                    const double r = std::hypot(anchor.point.x, anchor.point.y);
                                    return r >= 50.0 && r <= 60.0 * std::numbers::sqrt2;
                                }),
        "4 to 8: half way, every point is between the two outlines");

    // A circle (four curved quarters) into a triangle: the triangle is subdivided to four anchors.
    const auto round = valueOf({circle(0, 0, 40)});
    const auto triangle = valueOf({regular(0, 0, 40, 3)});
    const auto toTriangle = morph::alignPaths(round, triangle);
    checkContract(expectations, round, triangle, toTriangle, "circle to triangle");
    expectations.expect(toTriangle.to.contours[0].anchors.size() == 4 &&
                            drawsAs(toTriangle.to.contours[0], triangle.contours[0]),
                        "circle to triangle: the triangle is subdivided at its longest side");
}

void contourCounts(Expectations& expectations) {
    // One square into three: the square pairs with the nearest of the same size, and the other two
    // grow from points at their own centroids.
    const auto one = valueOf({square(0, 0, 20)});
    const auto three = valueOf({square(-100, 0, 20), square(2, 1, 20), square(100, 0, 20)});
    const auto growing = morph::alignPaths(one, three);
    checkContract(expectations, one, three, growing, "1 to 3 contours");
    expectations.expect(growing.from.contours.size() == 3, "1 to 3: three contours");
    expectations.expect(drawsAs(growing.to.contours[0], three.contours[1]),
                        "1 to 3: the square pairs with the one nearest it");
    expectations.expect(collapsedAt(growing.from.contours[1], {-100, 0}) &&
                            collapsedAt(growing.from.contours[2], {100, 0}) &&
                            drawsAs(growing.to.contours[1], three.contours[0]) &&
                            drawsAs(growing.to.contours[2], three.contours[2]),
                        "1 to 3: the other two grow from points at their centroids");
    const auto halfGrown = morph::morphPaths(one, three, 0.5);
    expectations.expect(halfGrown.has_value() && halfGrown->contours.size() == 3 &&
                            near(centroidOf(halfGrown->contours[1]), {-100, 0}, 1e-6) &&
                            std::abs(halfGrown->contours[1].anchors[0].point.x - (-110)) < 1e-6,
                        "1 to 3: half way, a growing square is half its size about its centre");

    // Three into one: the far two shrink to their centroids.
    const auto shrinking = morph::alignPaths(three, one);
    checkContract(expectations, three, one, shrinking, "3 to 1 contours");
    expectations.expect(
        shrinking.to.contours.size() == 3 && collapsedAt(shrinking.to.contours[0], {-100, 0}) &&
            drawsAs(shrinking.to.contours[1], one.contours[0]) &&
            collapsedAt(shrinking.to.contours[2], {100, 0}),
        "3 to 1: the far two shrink to their centroids, the near one becomes the square");

    // An empty value into a shape, and back: the shape grows from its centroid.
    const PathValue empty;
    const auto emerging = morph::alignPaths(empty, one);
    checkContract(expectations, empty, one, emerging, "empty to shape");
    expectations.expect(emerging.from.contours.size() == 1 &&
                            collapsedAt(emerging.from.contours[0], {0, 0}),
                        "empty to shape: the shape grows from its centroid");
    const auto vanishing = morph::alignPaths(one, empty);
    checkContract(expectations, one, empty, vanishing, "shape to empty");
    expectations.expect(vanishing.to.contours.size() == 1 &&
                            collapsedAt(vanishing.to.contours[0], {0, 0}),
                        "shape to empty: the shape shrinks to its centroid");
}

void openAndClosed(Expectations& expectations) {
    // An open zig-zag into a closed square: the square is cut open at the corner (and in the
    // direction) nearest the line's anchors, its start repeated at its end, and both are open.
    const auto line =
        valueOf({polygon({{-50, -50}, {50, -50}, {50, 50}, {-50, 50}, {-50, -48}}, false)});
    const auto box = valueOf({square(0, 0, 50)});
    const auto aligned = morph::alignPaths(line, box);
    checkContract(expectations, line, box, aligned, "open to closed");
    expectations.expect(!aligned.from.contours[0].closed && !aligned.to.contours[0].closed,
                        "open to closed: both aligned contours are open");
    expectations.expect(aligned.to.contours[0].anchors.size() == 5 &&
                            near(aligned.to.contours[0].anchors.front().point, {-50, -50}) &&
                            near(aligned.to.contours[0].anchors.back().point, {-50, -50}),
                        "open to closed: the square is cut at the corner the line starts at");
    expectations.expect(drawsAs(aligned.to.contours[0], box.contours[0]),
                        "open to closed: the cut square draws the whole square");
    // And the other way: closed into open, the closed one (the first value) is cut.
    const auto back = morph::alignPaths(box, line);
    checkContract(expectations, box, line, back, "closed to open");
    expectations.expect(!back.from.contours[0].closed &&
                            drawsAs(back.from.contours[0], box.contours[0]) &&
                            near(back.from.contours[0].anchors.front().point, {-50, -50}),
                        "closed to open: the first value is cut where the line starts");
    // Two open lines that run opposite ways: the second is turned round, so the ends travel
    // together.
    const auto right = valueOf({polygon({{0, 0}, {100, 0}}, false)});
    const auto left = valueOf({polygon({{100, 10}, {50, 10}, {0, 10}}, false)});
    const auto lines = morph::alignPaths(right, left);
    checkContract(expectations, right, left, lines, "open to open");
    expectations.expect(near(lines.to.contours[0].anchors.front().point, {0, 10}) &&
                            near(lines.to.contours[0].anchors.back().point, {100, 10}),
                        "open to open: a line drawn the other way is turned round");
}

void startAndDirection(Expectations& expectations) {
    // The same hexagon started three corners later: the alignment finds the start, so the morph
    // stands still (every anchor meets itself).
    const auto hexagon = regular(10, 20, 50, 6);
    PathContour later = hexagon;
    std::ranges::rotate(later.anchors, later.anchors.begin() + 3);
    const auto from = valueOf({hexagon});
    const auto to = valueOf({later});
    expectations.expect(document::pathKeysInterpolate(from, to),
                        "a rotated start has the same structure");
    const auto rotated = morph::alignPaths(from, to);
    checkContract(expectations, from, to, rotated, "rotated start");
    expectations.expect(rotated.to.contours[0] == hexagon,
                        "rotated start: the start is found again");
    // The same hexagon drawn the other way round: the direction is found again too.
    PathContour reversed = hexagon;
    std::ranges::reverse(reversed.anchors);
    const auto backwards = valueOf({reversed});
    const auto turned = morph::alignPaths(from, backwards);
    checkContract(expectations, from, backwards, turned, "reversed direction");
    expectations.expect(turned.to.contours[0] == hexagon,
                        "reversed direction: the direction is found again");
    // A curved contour reversed swaps its handles: the circle started at another quarter and
    // reversed aligns back onto the circle exactly.
    auto circular = circle(0, 0, 30);
    auto backCircle = circular;
    std::ranges::reverse(backCircle.anchors);
    for (auto& anchor : backCircle.anchors)
        std::swap(anchor.inHandle, anchor.outHandle);
    std::ranges::rotate(backCircle.anchors, backCircle.anchors.begin() + 1);
    const auto circles = morph::alignPaths(valueOf({circular}), valueOf({backCircle}));
    expectations.expect(circles.to.contours[0] == circular,
                        "reversed circle: the start, direction and handles are found again");

    // Past the exact limit, the same answers through the correlation: 1500 corners started 377
    // later and then reversed.
    const auto many = regular(0, 0, 400, 1500, 0.1);
    PathContour manyLater = many;
    std::ranges::rotate(manyLater.anchors, manyLater.anchors.begin() + 377);
    const auto large = morph::alignPaths(valueOf({many}), valueOf({manyLater}));
    expectations.expect(large.status == morph::AlignStatus::Ok && large.to.contours[0] == many,
                        "1500 anchors: the start is found through the correlation");
    std::ranges::reverse(manyLater.anchors);
    const auto largeBack = morph::alignPaths(valueOf({many}), valueOf({manyLater}));
    expectations.expect(largeBack.status == morph::AlignStatus::Ok &&
                            largeBack.to.contours[0] == many,
                        "1500 anchors: the direction is found through the correlation");
}

void groupKeys(Expectations& expectations) {
    // Two parts, keyed "dot" and "bar", swap places between the values (and their order in the
    // list). By geometry alone the dot would pair with whatever now sits where it was; the keys
    // pair the dot with the dot.
    PathValue from = valueOf({square(-100, 0, 10), square(100, 0, 40)});
    from.groups = {{"dot", 0, 1, std::nullopt}, {"bar", 1, 1, std::nullopt}};
    PathValue to = valueOf({square(-100, 0, 40), square(100, 0, 10)});
    to.groups = {{"bar", 0, 1, std::nullopt}, {"dot", 1, 1, std::nullopt}};
    const auto keyed = morph::alignPaths(from, to);
    checkContract(expectations, from, to, keyed, "group keys");
    expectations.expect(
        drawsAs(keyed.to.contours[0], to.contours[1]) &&
            drawsAs(keyed.to.contours[1], to.contours[0]),
        "group keys: each part pairs with the part of the same key across the reorder");
    // Without the keys the same values pair by geometry: each square with the one where it stands.
    PathValue bareFrom = from;
    bareFrom.groups.clear();
    PathValue bareTo = to;
    bareTo.groups.clear();
    const auto bare = morph::alignPaths(bareFrom, bareTo);
    checkContract(expectations, bareFrom, bareTo, bare, "no groups");
    expectations.expect(drawsAs(bare.to.contours[0], to.contours[0]),
                        "no groups: the contours pair by where they stand");
    // By order, whatever the keys say.
    const auto ordered = morph::alignPaths(from, to, {morph::Pairing::ByOrder});
    expectations.expect(drawsAs(ordered.to.contours[0], to.contours[0]) &&
                            drawsAs(ordered.to.contours[1], to.contours[1]),
                        "by order: contour i pairs with contour i");
    // A group present in one value only: its contours pair with the other unkeyed ones, by
    // geometry.
    PathValue partial = valueOf({square(-100, 0, 10), square(100, 0, 40), square(0, 200, 5)});
    partial.groups = {{"dot", 0, 1, std::nullopt}, {"tail", 2, 1, core::Color4d{1, 0, 0, 1}}};
    const auto mixed = morph::alignPaths(partial, to);
    checkContract(expectations, partial, to, mixed, "a group in one value only");
    expectations.expect(drawsAs(mixed.to.contours[0], to.contours[1]) &&
                            drawsAs(mixed.to.contours[1], to.contours[0]) &&
                            collapsedAt(mixed.to.contours[2], {0, 200}),
                        "a group in one value only: keyed parts pair, the extra one shrinks away");
    const auto midway = morph::morphPaths(partial, to, 0.5);
    expectations.expect(midway.has_value() && midway->groups == partial.groups,
                        "during the morph the earlier value's groups and colours hold");
}

void identicalStructure(Expectations& expectations) {
    // Values that already correspond keep their anchors where they are on the first side, and the
    // whole step leaves a value aligned with itself unchanged.
    const auto value = valueOf({circle(0, 0, 30), square(5, 5, 5)});
    const auto self = morph::alignPaths(value, value);
    expectations.expect(self.from == value && self.to == value,
                        "a value aligned with itself is itself");
    const auto half = morph::morphPaths(value, value, 0.5);
    expectations.expect(half.has_value() && *half == value,
                        "a value morphed into itself stands still");
}

void limits(Expectations& expectations) {
    // A closed contour of 4096 anchors against an open one of 4096: cutting the closed one open
    // would need 4097, past the per-contour cap. The alignment says so; the morph answers nothing.
    const auto big = regular(0, 0, 500, document::kMaximumPathAnchors);
    std::vector<Vec2d> points;
    for (std::size_t index = 0; index < document::kMaximumPathAnchors; ++index)
        points.push_back({static_cast<double>(index), 0.0});
    const auto line = polygon(points, false);
    const auto aligned = morph::alignPaths(valueOf({big}), valueOf({line}));
    expectations.expect(aligned.status == morph::AlignStatus::TooMany && aligned.contour &&
                            aligned.requested == document::kMaximumPathAnchors + 1,
                        "past the contour cap the alignment says how many it would need");
    expectations.expect(!morph::morphPaths(valueOf({big}), valueOf({line}), 0.5).has_value(),
                        "and the morph answers nothing rather than a clamped shape");
    expectations.expect(morph::morphPaths(valueOf({big}), valueOf({line}), 0.0) == valueOf({big}),
                        "but its ends are still the values themselves");
}

// A genuinely cancelled token, taken from a scheduler task that is cancelled while it runs.
[[nodiscard]] runtime::CancellationToken cancelledToken() {
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
        TaskRequest("path morph cancel token",
                    {.kind = TaskOwnerKind::Composition, .id = TaskOwnerId::fromRaw(95)}),
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

// Cancellation reaches the alignment: a token that fired answers Cancelled with nothing kept, in the
// pairing of many contours and in the start search of long ones; and a token that fires while an
// alignment is running ends that very alignment (the loop below can only finish by the alignment
// itself answering Cancelled).
void cancellation(Expectations& expectations) {
    const auto fired = cancelledToken();
    morph::AlignOptions options;
    options.cancellation = &fired;

    const auto small = valueOf({regular(0, 0, 50, 8)});
    const auto other = valueOf({regular(0, 0, 80, 12)});
    const auto few = morph::alignPaths(small, other, options);
    expectations.expect(few.status == morph::AlignStatus::Cancelled && few.from.contours.empty() &&
                            few.to.contours.empty(),
                        "a cancelled alignment of two shapes answers Cancelled and keeps no contour");

    std::vector<PathContour> manyFrom, manyTo;
    for (int index = 0; index < 120; ++index) {
        manyFrom.push_back(regular(index * 3.0, 0, 5, 6));
        manyTo.push_back(regular(index * 3.0 + 1.0, 4, 6, 9));
    }
    const auto many = morph::alignPaths(valueOf(manyFrom), valueOf(manyTo), options);
    expectations.expect(many.status == morph::AlignStatus::Cancelled && many.from.contours.empty(),
                        "a cancelled alignment of 120 pairs answers Cancelled and keeps nothing");

    const auto longFrom = valueOf({regular(0, 0, 100, 1500)});
    const auto longTo = valueOf({regular(0, 0, 120, 1600)});
    const auto checked = morph::morphPathsChecked(longFrom, longTo, 0.5, options);
    expectations.expect(checked.status == morph::AlignStatus::Cancelled && checked.value.contours.empty(),
                        "a cancelled morph of two 1500-anchor contours answers Cancelled and no value");
    expectations.expect(!morph::morphPaths(small, other, 0.5, options).has_value(),
                        "morphPaths answers nothing for a cancelled alignment");
    expectations.expect(morph::morphPaths(small, other, 0.0, options) == small &&
                            morph::morphPaths(small, other, 1.0, options) == other,
                        "the ends need no alignment, so a cancelled token does not touch them");
    expectations.expect(morph::alignPaths(small, other).status == morph::AlignStatus::Ok,
                        "with no token the same inputs align");

    // A token that fires inside a running alignment.
    using namespace runtime;
    TaskSchedulerConfig config = TaskSchedulerConfig::defaults();
    config.cpuWorkerCount = 1;
    config.blockingIoWorkerCount = 1;
    TaskScheduler scheduler(config);
    std::atomic<bool> running{false};
    std::atomic<int> rounds{0};
    std::atomic<bool> endedByCancellation{false};
    auto submission = scheduler.submit<void>(
        TaskRequest("path morph alignment cancel",
                    {.kind = TaskOwnerKind::Composition, .id = TaskOwnerId::fromRaw(94)}),
        [&](TaskContext& context) {
            const auto token = context.cancellation();
            morph::AlignOptions live;
            live.cancellation = &token;
            running = true;
            while (rounds < 20000) {
                const auto answer = morph::alignPaths(longFrom, longTo, live);
                ++rounds;
                if (answer.status == morph::AlignStatus::Cancelled) {
                    endedByCancellation = answer.from.contours.empty() && answer.to.contours.empty();
                    break;
                }
            }
            return TaskResult<void>::succeeded();
        });
    expectations.expect(submission.accepted(), "the alignment task is accepted");
    if (!submission.accepted())
        return;
    const auto start = std::chrono::steady_clock::now();
    while (!running && std::chrono::steady_clock::now() - start < std::chrono::seconds(5))
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    submission.handle.cancel();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!submission.handle.tryTakeResult().has_value() &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    expectations.expect(endedByCancellation.load(),
                        "a token that fires while alignments run ends one of them with Cancelled and nothing kept");
}

} // namespace

int main() {
    Expectations expectations;
    differentAnchorCounts(expectations);
    contourCounts(expectations);
    openAndClosed(expectations);
    startAndDirection(expectations);
    groupKeys(expectations);
    identicalStructure(expectations);
    limits(expectations);
    cancellation(expectations);
    if (expectations.failures() != 0) {
        std::cerr << expectations.failures() << " path morph expectation(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "PASS: path morph alignment\n";
    return EXIT_SUCCESS;
}
