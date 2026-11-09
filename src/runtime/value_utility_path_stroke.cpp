// #509 Path Stroke: a path drawn as a stroke, answered as the outline the stroke covers (and, with
// a core, a narrower outline for the glowing core). Pure function of its operands
// (docs/architecture/path-processing.md); the outline is drawn by Shape Source's production fill.

#include "value_utility_path_support.hpp"

#include <bloom/document/value_utility_nodes.hpp>
#include <bloom/render/path_raster.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <optional>
#include <utility>
#include <vector>

namespace {

using namespace bloom;
using document::PathContour;
using document::PathValue;
using document::Vec2d;
using runtime::ValueUtilityInvocation;
using runtime::ValueUtilityOutcome;
using runtime::detail::shapedOutcome;
using runtime::detail::ValueUtilityReader;
namespace support = runtime::detail::pathsupport;
using Clipper2Lib::PathD;
using Clipper2Lib::PathsD;

constexpr std::size_t kMaximumPolygons = 400000;
constexpr std::size_t kMaximumSamples = 400000;
constexpr int kDiscSides = 16;

// A stretch of the path being stroked: points, the distance of each along its source contour (so
// a wiggle is a function of the place on the path, not of where a trim begins) and the width
// envelope (0 to 1) at each.
struct Stretch {
    std::vector<Vec2d> points;
    std::vector<double> along;
    std::vector<double> envelope;
    bool closed = false;
};

[[nodiscard]] double lengthOf(const std::vector<Vec2d>& points) noexcept {
    double length = 0.0;
    for (std::size_t index = 1; index < points.size(); ++index)
        length += std::hypot(points[index].x - points[index - 1].x,
                             points[index].y - points[index - 1].y);
    return length;
}

[[nodiscard]] Vec2d lerp(const Vec2d a, const Vec2d b, const double t) noexcept {
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
}

// The part of a stretch between two distances from its start. Points and `along` are interpolated;
// the envelope is filled in later, once the final extent of the stretch is known.
[[nodiscard]] Stretch slice(const Stretch& source, const double from, const double to) {
    Stretch result;
    double distance = 0.0;
    for (std::size_t index = 0; index + 1 < source.points.size(); ++index) {
        const auto& a = source.points[index];
        const auto& b = source.points[index + 1];
        const double length = std::hypot(b.x - a.x, b.y - a.y);
        const double low = distance;
        const double high = distance + length;
        distance = high;
        if (!(length > 0.0) || high <= from || low >= to)
            continue;
        const double t0 = std::max(from, low) > low ? (std::max(from, low) - low) / length : 0.0;
        const double t1 = std::min(to, high) < high ? (std::min(to, high) - low) / length : 1.0;
        if (result.points.empty()) {
            result.points.push_back(lerp(a, b, t0));
            result.along.push_back(source.along[index] +
                                   (source.along[index + 1] - source.along[index]) * t0);
        }
        result.points.push_back(lerp(a, b, t1));
        result.along.push_back(source.along[index] +
                               (source.along[index + 1] - source.along[index]) * t1);
    }
    return result;
}

[[nodiscard]] double lattice(const std::int64_t index, const std::int64_t seed) noexcept {
    auto x = static_cast<std::uint64_t>(index) * 0x9E3779B97F4A7C15ULL ^
             (static_cast<std::uint64_t>(seed) * 0xBF58476D1CE4E5B9ULL + 0x94D049BB133111EBULL);
    x ^= x >> 30U;
    x *= 0xBF58476D1CE4E5B9ULL;
    x ^= x >> 27U;
    x *= 0x94D049BB133111EBULL;
    x ^= x >> 31U;
    return static_cast<double>(x >> 11U) / static_cast<double>(1ULL << 53U) * 2.0 - 1.0;
}

// Smooth one-dimensional value noise in -1 to 1: a pure function of the place and the seed.
[[nodiscard]] double noise(const double place, const std::int64_t seed) noexcept {
    const double base = std::floor(place);
    const double t = place - base;
    const double eased = t * t * (3.0 - 2.0 * t);
    const auto index = static_cast<std::int64_t>(base);
    const double a = lattice(index, seed);
    const double b = lattice(index + 1, seed);
    return a + (b - a) * eased;
}

[[nodiscard]] Vec2d unit(const Vec2d from, const Vec2d to) noexcept {
    const double length = std::hypot(to.x - from.x, to.y - from.y);
    return length > 0.0 ? Vec2d{(to.x - from.x) / length, (to.y - from.y) / length} : Vec2d{1, 0};
}

// A polygon wound the one way the union reads as solid, whichever way it was built.
void addPolygon(PathsD& polygons, PathD polygon) {
    double twice = 0.0;
    for (std::size_t index = 0; index < polygon.size(); ++index) {
        const auto& p = polygon[index];
        const auto& q = polygon[(index + 1) % polygon.size()];
        twice += p.x * q.y - q.x * p.y;
    }
    if (twice > 0.0)
        std::ranges::reverse(polygon);
    polygons.push_back(std::move(polygon));
}

void addDisc(PathsD& polygons, const Vec2d center, const double radius) {
    if (!(radius > 1e-9))
        return;
    PathD disc;
    disc.reserve(kDiscSides);
    for (int side = 0; side < kDiscSides; ++side) {
        const double angle = 2.0 * std::numbers::pi * side / kDiscSides;
        disc.emplace_back(center.x + radius * std::cos(angle), center.y + radius * std::sin(angle));
    }
    addPolygon(polygons, std::move(disc));
}

struct Style {
    double width = 0.0;
    document::OffsetJoin join = document::OffsetJoin::Round;
    document::StrokeCap cap = document::StrokeCap::Round;
};

// The outline of one stretch, as polygons to be united: a trapezoid for each segment, a join at
// each inner vertex and a cap at each end. The union is taken once, over everything.
void outline(const Stretch& stretch, const Style& style, PathsD& polygons) {
    const auto count = stretch.points.size();
    if (count < 2)
        return;
    const auto half = [&](const std::size_t index) {
        return 0.5 * style.width * stretch.envelope[index];
    };
    const auto normal = [](const Vec2d direction) { return Vec2d{-direction.y, direction.x}; };
    for (std::size_t index = 0; index + 1 < count; ++index) {
        const auto direction = unit(stretch.points[index], stretch.points[index + 1]);
        const auto n = normal(direction);
        const double h0 = half(index);
        const double h1 = half(index + 1);
        const auto& a = stretch.points[index];
        const auto& b = stretch.points[index + 1];
        addPolygon(polygons, {{a.x + n.x * h0, a.y + n.y * h0},
                              {b.x + n.x * h1, b.y + n.y * h1},
                              {b.x - n.x * h1, b.y - n.y * h1},
                              {a.x - n.x * h0, a.y - n.y * h0}});
    }
    // Joins: between the segment before a vertex and the segment after it.
    for (std::size_t vertex = stretch.closed ? 0 : 1; vertex + (stretch.closed ? 0 : 1) < count;
         ++vertex) {
        const std::size_t previous = vertex == 0 ? count - 2 : vertex - 1;
        const std::size_t next = vertex + 1 < count ? vertex + 1 : 1;
        const auto& p = stretch.points[vertex];
        const auto d1 = unit(stretch.points[previous], p);
        const auto d2 = unit(p, stretch.points[next]);
        const double h = half(vertex);
        if (!(h > 1e-9))
            continue;
        const double cross = d1.x * d2.y - d1.y * d2.x;
        const double dot = d1.x * d2.x + d1.y * d2.y;
        if (std::abs(cross) < 1e-9 && dot > 0.0)
            continue;
        if (style.join == document::OffsetJoin::Round) {
            addDisc(polygons, p, h);
            continue;
        }
        const auto n1 = normal(d1);
        const auto n2 = normal(d2);
        for (const double side : {1.0, -1.0}) {
            const Vec2d a{p.x + side * n1.x * h, p.y + side * n1.y * h};
            const Vec2d b{p.x + side * n2.x * h, p.y + side * n2.y * h};
            PathD wedge{{p.x, p.y}, {a.x, a.y}, {b.x, b.y}};
            if (style.join == document::OffsetJoin::Miter) {
                const double denominator = 1.0 + n1.x * n2.x + n1.y * n2.y;
                // The miter is as long as 1 / cos(half the turn) half-widths; past four it is cut.
                if (denominator > 2.0 / 16.0) {
                    const double scale = h / denominator * side;
                    wedge.insert(wedge.begin() + 2,
                                 {p.x + (n1.x + n2.x) * scale, p.y + (n1.y + n2.y) * scale});
                }
            }
            addPolygon(polygons, std::move(wedge));
        }
    }
    if (stretch.closed)
        return;
    const auto capAt = [&](const std::size_t index, const Vec2d outward) {
        const double h = half(index);
        const auto& p = stretch.points[index];
        if (style.cap == document::StrokeCap::Round) {
            addDisc(polygons, p, h);
        } else if (style.cap == document::StrokeCap::Square && h > 1e-9) {
            const auto n = normal(outward);
            addPolygon(polygons, {{p.x + n.x * h, p.y + n.y * h},
                                  {p.x + n.x * h + outward.x * h, p.y + n.y * h + outward.y * h},
                                  {p.x - n.x * h + outward.x * h, p.y - n.y * h + outward.y * h},
                                  {p.x - n.x * h, p.y - n.y * h}});
        }
    };
    const auto start = unit(stretch.points[0], stretch.points[1]);
    const auto end = unit(stretch.points[count - 2], stretch.points[count - 1]);
    capAt(0, {-start.x, -start.y});
    capAt(count - 1, end);
}

ValueUtilityOutcome pathStroke(const ValueUtilityInvocation& invocation) {
    auto outcome = shapedOutcome(invocation.operation);
    ValueUtilityReader reader(invocation);
    const auto& path = reader.path(0);
    const double width = reader.scalar(1);
    const double start = reader.scalar(2);
    const double end = reader.scalar(3);
    const double offset = reader.scalar(4);
    const double taperStart = reader.scalar(5);
    const double taperEnd = reader.scalar(6);
    const double dashLength = reader.scalar(7);
    const double dashGap = reader.scalar(8);
    const double dashOffset = reader.scalar(9);
    const double spacing = reader.scalar(10);
    const double wiggle = reader.scalar(11);
    const double frequency = reader.scalar(12);
    const double phase = reader.scalar(13);
    const std::int64_t seed = reader.integer(14);
    const double coreWidth = reader.scalar(15);
    const double tolerance = reader.scalar(16);
    const auto cap = runtime::detail::selectorAt(invocation, 0, document::kStrokeCaps,
                                                 document::StrokeCap::Round);
    const auto join = runtime::detail::selectorAt(invocation, 1, document::kOffsetJoins,
                                                  document::OffsetJoin::Round);
    if (reader.malformed())
        return outcome;
    for (const double value : {width, start, end, offset, taperStart, taperEnd, dashLength, dashGap,
                               dashOffset, spacing, wiggle, frequency, phase, coreWidth})
        if (!std::isfinite(value))
            return support::refuse(std::move(outcome), "Path Stroke operand is not finite",
                                   "Every number must be finite.");
    if (!support::toleranceUsable(tolerance))
        return support::refuse(std::move(outcome), "Path Stroke tolerance is not usable",
                               "The tolerance must be between 0.001 and 1000 pixels.");
    if (!support::coordinatesUsable(path) || std::abs(width) > document::kMaximumPathCoordinate)
        return support::refuse(std::move(outcome), "Path Stroke path is out of range",
                               "Every coordinate must be finite and within a billion pixels.");

    // Flatten every contour at the one flattening every route shares.
    std::vector<Stretch> sources;
    for (const auto& contour : path.contours) {
        if (contour.anchors.size() < 2)
            continue;
        auto points =
            render::flattenPathContour(support::renderPath(contour, contour.closed), tolerance);
        if (!points)
            return support::refuse(
                std::move(outcome), "Path Stroke is too detailed",
                "The path has more flattened segments than can be stroked; raise "
                "the tolerance.");
        Stretch stretch;
        stretch.closed = contour.closed;
        double along = 0.0;
        for (const auto& point : *points) {
            const Vec2d next{point.x, point.y};
            if (!stretch.points.empty()) {
                const auto& previous = stretch.points.back();
                const double step = std::hypot(next.x - previous.x, next.y - previous.y);
                if (!(step > 0.0))
                    continue;
                along += step;
            }
            stretch.points.push_back(next);
            stretch.along.push_back(along);
        }
        if (contour.closed && stretch.points.size() > 1) {
            const auto first = stretch.points.front();
            const auto& last = stretch.points.back();
            const double step = std::hypot(first.x - last.x, first.y - last.y);
            if (step > 0.0) {
                along += step;
                stretch.points.push_back(first);
                stretch.along.push_back(along);
            }
        }
        if (stretch.points.size() >= 2)
            sources.push_back(std::move(stretch));
    }
    double total = 0.0;
    for (const auto& stretch : sources)
        total += lengthOf(stretch.points);

    // Trim: the window of the whole length, simultaneously over every contour.
    const bool wrap =
        !sources.empty() && std::ranges::all_of(sources, [](const Stretch& s) { return s.closed; });
    const auto windows = support::windowsFor(start, end, offset, wrap);
    std::vector<Stretch> pieces;
    if (windows.size() == 1 && windows[0].first <= 0.0 && windows[0].second >= 1.0) {
        pieces = sources;
    } else {
        for (const auto& [low, high] : windows) {
            double base = 0.0;
            for (const auto& source : sources) {
                const double length = lengthOf(source.points);
                const double from = std::max(low * total, base) - base;
                const double to = std::min(high * total, base + length) - base;
                base += length;
                if (to > from) {
                    auto piece = slice(source, from, to);
                    if (piece.points.size() >= 2)
                        pieces.push_back(std::move(piece));
                }
            }
        }
        // A stretch that ran past the end of one closed contour and on from its start is one piece.
        if (pieces.size() == 2 && sources.size() == 1 && windows.size() == 2) {
            auto& tail = pieces[0];
            auto& head = pieces[1];
            for (std::size_t index = 1; index < head.points.size(); ++index) {
                tail.points.push_back(head.points[index]);
                tail.along.push_back(tail.along.back() +
                                     std::hypot(head.points[index].x - head.points[index - 1].x,
                                                head.points[index].y - head.points[index - 1].y));
            }
            pieces.pop_back();
        }
    }
    double drawn = 0.0;
    for (const auto& piece : pieces)
        drawn += lengthOf(piece.points);
    outcome.outputs[2] = drawn;

    // Wiggle: points are resampled finely enough to follow the noise, then moved along the normal.
    if (wiggle != 0.0 && frequency != 0.0) {
        const double wavelength = 100.0 / std::abs(frequency);
        const double step = std::clamp(wavelength / 8.0, 0.5, 16.0);
        for (auto& piece : pieces) {
            const double length = lengthOf(piece.points);
            const auto samples = static_cast<std::size_t>(std::ceil(length / step)) + 1;
            if (samples > kMaximumSamples)
                return support::refuse(std::move(outcome), "Path Stroke wiggle is too fine",
                                       "The wiggle would need more samples than can be stroked; "
                                       "lower the frequency.");
            Stretch resampled;
            resampled.closed = piece.closed;
            // A walk, not a search per sample: samples are in order.
            std::size_t cursor = 0;
            double base = 0.0;
            for (std::size_t sample = 0; sample < samples; ++sample) {
                const double distance = std::min(length, static_cast<double>(sample) * step);
                while (cursor + 2 < piece.points.size()) {
                    const double segment =
                        std::hypot(piece.points[cursor + 1].x - piece.points[cursor].x,
                                   piece.points[cursor + 1].y - piece.points[cursor].y);
                    if (distance <= base + segment)
                        break;
                    base += segment;
                    ++cursor;
                }
                const double segment =
                    std::hypot(piece.points[cursor + 1].x - piece.points[cursor].x,
                               piece.points[cursor + 1].y - piece.points[cursor].y);
                const double t =
                    segment > 0.0 ? std::clamp((distance - base) / segment, 0.0, 1.0) : 0.0;
                resampled.points.push_back(lerp(piece.points[cursor], piece.points[cursor + 1], t));
                resampled.along.push_back(piece.along[cursor] +
                                          (piece.along[cursor + 1] - piece.along[cursor]) * t);
            }
            if (resampled.points.size() < 2)
                continue;
            Stretch moved = resampled;
            for (std::size_t index = 0; index < resampled.points.size(); ++index) {
                const auto before = resampled.points[index == 0 ? 0 : index - 1];
                const auto after =
                    resampled.points[std::min(index + 1, resampled.points.size() - 1)];
                const auto direction = unit(before, after);
                const double shift =
                    wiggle *
                    noise(resampled.along[index] * std::abs(frequency) / 100.0 + phase, seed);
                moved.points[index] = {resampled.points[index].x - direction.y * shift,
                                       resampled.points[index].y + direction.x * shift};
            }
            piece = std::move(moved);
        }
    }

    // The width envelope along each piece: a taper at either end, as a share of the piece.
    const double startShare = std::clamp(taperStart / 100.0, 0.0, 1.0);
    const double endShare = std::clamp(taperEnd / 100.0, 0.0, 1.0);
    for (auto& piece : pieces) {
        const double length = lengthOf(piece.points);
        piece.envelope.assign(piece.points.size(), 1.0);
        double distance = 0.0;
        for (std::size_t index = 0; index < piece.points.size(); ++index) {
            if (index > 0)
                distance += std::hypot(piece.points[index].x - piece.points[index - 1].x,
                                       piece.points[index].y - piece.points[index - 1].y);
            double value = 1.0;
            if (startShare > 0.0 && length > 0.0)
                value = std::min(value, distance / (startShare * length));
            if (endShare > 0.0 && length > 0.0)
                value = std::min(value, (length - distance) / (endShare * length));
            piece.envelope[index] = std::clamp(value, 0.0, 1.0);
        }
        if (piece.closed && (startShare > 0.0 || endShare > 0.0))
            piece.closed = false; // a taper gives a closed contour a start and an end
    }

    // Brush spacing stamps dots; a dash cuts the pieces; neither leaves them whole.
    std::vector<Stretch> strokes;
    std::vector<std::pair<Vec2d, double>> stamps;
    if (spacing > 0.0) {
        for (const auto& piece : pieces) {
            const double length = lengthOf(piece.points);
            if (length / spacing > static_cast<double>(kMaximumSamples))
                return support::refuse(std::move(outcome), "Path Stroke spacing is too small",
                                       "The brush would stamp more dots than can be drawn; raise "
                                       "the spacing.");
            std::size_t index = 0;
            double base = 0.0;
            for (double at = 0.0; at <= length + 1e-9; at += spacing) {
                while (index + 2 < piece.points.size()) {
                    const double segment =
                        std::hypot(piece.points[index + 1].x - piece.points[index].x,
                                   piece.points[index + 1].y - piece.points[index].y);
                    if (at <= base + segment)
                        break;
                    base += segment;
                    ++index;
                }
                const double segment =
                    std::hypot(piece.points[index + 1].x - piece.points[index].x,
                               piece.points[index + 1].y - piece.points[index].y);
                const double t = segment > 0.0 ? std::clamp((at - base) / segment, 0.0, 1.0) : 0.0;
                stamps.emplace_back(lerp(piece.points[index], piece.points[index + 1], t),
                                    piece.envelope[index] +
                                        (piece.envelope[index + 1] - piece.envelope[index]) * t);
            }
        }
    } else if (dashLength > 0.0 && dashGap >= 0.0 && dashGap + dashLength > 0.0) {
        const double period = dashLength + dashGap;
        for (const auto& piece : pieces) {
            const double length = lengthOf(piece.points);
            if (length / period > static_cast<double>(kMaximumSamples))
                return support::refuse(
                    std::move(outcome), "Path Stroke dashes are too short",
                    "The pattern would cut more dashes than can be drawn; lengthen "
                    "the dash.");
            double first = -std::fmod(dashOffset, period);
            if (first > 0.0)
                first -= period;
            for (double at = first; at < length; at += period) {
                const double from = std::max(at, 0.0);
                const double to = std::min(at + dashLength, length);
                if (!(to > from))
                    continue;
                auto dash = slice(piece, from, to);
                if (dash.points.size() < 2)
                    continue;
                // Carry the envelope across: interpolated at the dash's ends, by distance.
                dash.envelope.assign(dash.points.size(), 1.0);
                std::vector<double> cumulative(piece.points.size(), 0.0);
                for (std::size_t index = 1; index < piece.points.size(); ++index)
                    cumulative[index] =
                        cumulative[index - 1] +
                        std::hypot(piece.points[index].x - piece.points[index - 1].x,
                                   piece.points[index].y - piece.points[index - 1].y);
                double position = from;
                for (std::size_t index = 0; index < dash.points.size(); ++index) {
                    if (index > 0)
                        position += std::hypot(dash.points[index].x - dash.points[index - 1].x,
                                               dash.points[index].y - dash.points[index - 1].y);
                    const auto upper =
                        std::upper_bound(cumulative.begin(), cumulative.end(), position);
                    const auto high = static_cast<std::size_t>(upper - cumulative.begin());
                    const std::size_t low = high == 0 ? 0 : high - 1;
                    const std::size_t next = std::min(low + 1, cumulative.size() - 1);
                    const double span = cumulative[next] - cumulative[low];
                    const double t = span > 0.0 ? (position - cumulative[low]) / span : 0.0;
                    dash.envelope[index] =
                        piece.envelope[low] + (piece.envelope[next] - piece.envelope[low]) * t;
                }
                strokes.push_back(std::move(dash));
                if (strokes.size() > kMaximumSamples)
                    return support::refuse(std::move(outcome), "Path Stroke dashes are too short",
                                           "The pattern would cut more dashes than can be drawn; "
                                           "lengthen the dash.");
            }
        }
    } else {
        strokes = pieces;
    }

    // The outline of the body, and of the core at its share of the width.
    const auto build = [&](const double scale) -> std::optional<PathsD> {
        if (!(width * scale > 0.0))
            return PathsD{};
        const Style style{width * scale, join, cap};
        PathsD polygons;
        for (const auto& stroke : strokes) {
            outline(stroke, style, polygons);
            if (polygons.size() > kMaximumPolygons)
                return std::nullopt;
        }
        for (const auto& [center, envelope] : stamps) {
            addDisc(polygons, center, 0.5 * style.width * envelope);
            if (polygons.size() > kMaximumPolygons)
                return std::nullopt;
        }
        return Clipper2Lib::Union(polygons, Clipper2Lib::FillRule::NonZero, 3);
    };
    try {
        const auto body = build(1.0);
        const auto core = coreWidth > 0.0 ? build(std::clamp(coreWidth, 0.0, 100.0) / 100.0)
                                          : std::optional<PathsD>(PathsD{});
        if (!body || !core)
            return support::refuse(
                shapedOutcome(invocation.operation), "Path Stroke is too detailed",
                "The stroke is made of more pieces than can be united; raise the "
                "tolerance or lengthen the dashes.");
        auto bodyValue = support::valueOf(*body);
        auto coreValue = support::valueOf(*core);
        if (!support::fitsValue(bodyValue) || !support::fitsValue(coreValue))
            return support::refuse(
                shapedOutcome(invocation.operation), "Path Stroke result is too large",
                "The stroke has a contour with more anchors than a path may hold; "
                "raise the tolerance.");
        support::publish(outcome, std::move(bodyValue), 0);
        support::publish(outcome, std::move(coreValue), 1);
    } catch (...) {
        return support::refuse(shapedOutcome(invocation.operation),
                               "Path Stroke could not be computed",
                               "The stroke could not be built.");
    }
    return outcome;
}

} // namespace

namespace bloom::runtime::detail {

ValueUtilityOutcome evaluateValuePathStroke(const ValueUtilityInvocation& invocation) {
    return pathStroke(invocation);
}

} // namespace bloom::runtime::detail
