#include <bloom/runtime/viewer_pick.hpp>

#include <bloom/document/drill_levels.hpp>
#include <bloom/document/project.hpp>
#include <bloom/render/path_raster.hpp>

#include "cpu_composition_evaluator_support.hpp"
#include "cpu_composition_resolution.hpp"
#include "shape_geometry.hpp"

#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>
#include <new>
#include <unordered_map>
#include <variant>

namespace bloom::runtime {
namespace {

// x' = a x + c y + e, y' = b x + d y + f.
struct Affine final {
    double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
    [[nodiscard]] document::Vec2d map(const document::Vec2d p) const noexcept {
        return {a * p.x + c * p.y + e, b * p.x + d * p.y + f};
    }
    [[nodiscard]] Affine then(const Affine& outer) const noexcept {
        // outer(this(p))
        return {outer.a * a + outer.c * b,           outer.b * a + outer.d * b,
                outer.a * c + outer.c * d,           outer.b * c + outer.d * d,
                outer.a * e + outer.c * f + outer.e, outer.b * e + outer.d * f + outer.f};
    }
    [[nodiscard]] double determinant() const noexcept { return a * d - b * c; }
    [[nodiscard]] std::optional<Affine> inverted() const noexcept {
        const auto det = determinant();
        if (!std::isfinite(det) || std::abs(det) < 1e-12)
            return std::nullopt;
        return Affine{
            d / det, -b / det, -c / det, a / det, (c * f - d * e) / det, (b * e - a * f) / det};
    }
};

// The content-to-parent affine a layer's evaluated bounds describe (its local box onto its
// polygon), or nullopt for an empty box.
[[nodiscard]] std::optional<Affine> contentAffine(const EvaluatedOperationBounds& bounds) {
    const auto width = bounds.local.right - bounds.local.left;
    const auto height = bounds.local.bottom - bounds.local.top;
    if (!(width > 0.0) || !(height > 0.0))
        return std::nullopt;
    const auto& p = bounds.polygon;
    const document::Vec2d x{(p[1].x - p[0].x) / width, (p[1].y - p[0].y) / width};
    const document::Vec2d y{(p[3].x - p[0].x) / height, (p[3].y - p[0].y) / height};
    const document::Vec2d origin{p[0].x - x.x * bounds.local.left - y.x * bounds.local.top,
                                 p[0].y - x.y * bounds.local.left - y.y * bounds.local.top};
    Affine affine{x.x, x.y, y.x, y.y, origin.x, origin.y};
    if (!std::isfinite(affine.determinant()))
        return std::nullopt;
    return affine;
}

[[nodiscard]] bool insideQuad(const std::array<document::Vec2d, 4>& quad, const Affine& parent,
                              const document::Vec2d point) {
    // Even-odd over the mapped quad (a parallelogram, possibly mirrored).
    std::array<document::Vec2d, 4> mapped{};
    for (std::size_t i = 0; i < 4; ++i)
        mapped[i] = parent.map(quad[i]);
    bool inside = false;
    for (std::size_t i = 0, j = 3; i < 4; j = i++) {
        const auto& a = mapped[i];
        const auto& b = mapped[j];
        if ((a.y > point.y) != (b.y > point.y) &&
            point.x < (b.x - a.x) * (point.y - a.y) / (b.y - a.y) + a.x)
            inside = !inside;
    }
    return inside;
}

// A shape's outline at one frame, in the shape's own (author) space.
struct Outline final {
    std::vector<std::vector<render::PathPoint>> contours;
    std::vector<bool> closed;
    bool fill = false;
    bool stroke = false;
    double halfWidth = 0.0;
    render::PathFillRule rule = render::PathFillRule::NonZero;
    bool valid = false;
};

[[nodiscard]] bool insideFill(const Outline& outline, const document::Vec2d p) {
    int winding = 0;
    int crossings = 0;
    for (const auto& contour : outline.contours) {
        const auto count = contour.size();
        if (count < 3)
            continue;
        for (std::size_t i = 0; i < count; ++i) {
            const auto& a = contour[i];
            const auto& b = contour[(i + 1) % count];
            if (a.y <= p.y) {
                if (b.y > p.y) {
                    const auto side = (b.x - a.x) * (p.y - a.y) - (p.x - a.x) * (b.y - a.y);
                    if (side > 0) {
                        ++winding;
                        ++crossings;
                    }
                }
            } else if (b.y <= p.y) {
                const auto side = (b.x - a.x) * (p.y - a.y) - (p.x - a.x) * (b.y - a.y);
                if (side < 0) {
                    --winding;
                    ++crossings;
                }
            }
        }
    }
    return outline.rule == render::PathFillRule::EvenOdd ? (crossings % 2) != 0 : winding != 0;
}

[[nodiscard]] double distanceToOutline(const Outline& outline, const document::Vec2d p) {
    double best = std::numeric_limits<double>::infinity();
    for (std::size_t index = 0; index < outline.contours.size(); ++index) {
        const auto& contour = outline.contours[index];
        if (contour.empty())
            continue;
        if (contour.size() == 1) {
            best = std::min(best, std::hypot(p.x - contour[0].x, p.y - contour[0].y));
            continue;
        }
        const auto segments = outline.closed[index] ? contour.size() : contour.size() - 1;
        for (std::size_t i = 0; i < segments; ++i) {
            const auto& a = contour[i];
            const auto& b = contour[(i + 1) % contour.size()];
            const auto dx = b.x - a.x, dy = b.y - a.y;
            const auto length = dx * dx + dy * dy;
            const auto t =
                length > 0.0 ? std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / length, 0.0, 1.0)
                             : 0.0;
            best = std::min(best, std::hypot(p.x - (a.x + t * dx), p.y - (a.y + t * dy)));
        }
    }
    return best;
}

} // namespace

// The geometry of one displayed frame: its plan, its value graph evaluated at the frame's time,
// and every outline a pick has asked for so far.
struct ViewerPicker::FrameGeometry final {
    document::Snapshot snapshot;
    document::CompositionId composition;
    core::RationalTime time;
    std::shared_ptr<const CompiledCompositionPlan> plan;
    std::optional<detail::ResolvedEvaluation> resolved;
    std::unordered_map<document::NodeId, std::size_t> shapes;
    mutable std::mutex mutex;
    mutable std::unordered_map<document::NodeId, Outline> outlines;

    FrameGeometry(document::Snapshot owner) : snapshot(std::move(owner)) {}

    [[nodiscard]] bool matches(const ViewerPickRequest& request) const {
        return &snapshot.project() == &request.snapshot.project() &&
               snapshot.revision() == request.snapshot.revision() &&
               composition == request.composition && time == request.time;
    }

    [[nodiscard]] Outline outline(const document::NodeId node,
                                  const CancellationToken& cancellation) const {
        {
            const std::lock_guard lock(mutex);
            if (const auto found = outlines.find(node); found != outlines.end())
                return found->second;
        }
        Outline result;
        const auto index = shapes.find(node);
        if (index != shapes.end()) {
            const auto& shape = std::get<CompiledShape>(plan->operations()[index->second]);
            const auto clock = plan->operationClock(OperationIndex::fromRaw(index->second));
            const auto& at = detail::resolvedForClock(*resolved, clock);
            const auto present = detail::timeForClock(*resolved, time, clock).has_value();
            const auto size = detail::resolveParameter(shape.size, *plan, at);
            const auto width = detail::resolveParameter(shape.strokeWidth, *plan, at);
            const auto fillSwitch = detail::resolvedFillEnabled(shape, at);
            const auto strokeSwitch = detail::resolvedStrokeEnabled(shape, at);
            const auto* path = detail::resolvedShapePath(shape, at);
            const auto line = detail::resolvedShapeLine(shape, *plan, at);
            if (present && size && width && fillSwitch && strokeSwitch && path && line &&
                std::isfinite(size->value.x) && std::isfinite(size->value.y) &&
                std::isfinite(width->value)) {
                result.fill = *fillSwitch && shape.kind != document::ShapeKind::Line;
                result.stroke = *strokeSwitch && width->value > 0.0;
                result.halfWidth = std::max(0.0, width->value) / 2.0;
                result.rule = detail::shapeFillRule(shape, *path);
                const auto tolerance = render::pathFlatteningTolerance(1.0, 1.0);
                const auto cancelled = [&cancellation] {
                    return cancellation.isCancellationRequested();
                };
                result.valid = true;
                for (const auto& contour :
                     detail::shapeRenderPaths(shape, size->value, *line, *path)) {
                    auto flat = render::flattenPathContour(contour, tolerance, cancelled);
                    if (!flat) {
                        result.valid = false;
                        break;
                    }
                    result.contours.push_back(std::move(*flat));
                    result.closed.push_back(contour.closed);
                }
            }
        }
        if (cancellation.isCancellationRequested())
            return result;
        const std::lock_guard lock(mutex);
        outlines.emplace(node, result);
        return result;
    }
};

namespace {

struct PickContext final {
    const document::Composition& composition;
    const ViewerPicker::FrameGeometry* frame = nullptr;
    std::unordered_map<document::LayerId, const EvaluatedOperationBounds*> bounds;
    const ViewerPickRequest& request;
    const CancellationToken& cancellation;
    std::size_t depth = 0;
};

} // namespace

namespace {
bool hitEntry(PickContext& context, const document::DrillEntry& entry,
              const std::vector<document::DrillEntry>& siblings, const Affine& parent,
              document::LayerId& leaf);

bool hitMerge(PickContext& context, const document::NodeId merge, const Affine& world,
              document::LayerId& leaf) {
    if (++context.depth > 256) {
        --context.depth;
        return false;
    }
    const auto entries = document::mergeEntryContents(context.composition, merge);
    bool hit = false;
    for (const auto& entry : entries) {
        if (entry.matteSource || context.cancellation.isCancellationRequested())
            continue;
        if (hitEntry(context, entry, entries, world, leaf)) {
            hit = true;
            break;
        }
    }
    --context.depth;
    return hit;
}

bool hitEntry(PickContext& context, const document::DrillEntry& entry,
              const std::vector<document::DrillEntry>& siblings, const Affine& parent,
              document::LayerId& leaf) {
    const auto found = context.bounds.find(entry.layer);
    if (found == context.bounds.end() || found->second->output.empty())
        return false;
    const auto& bounds = *found->second;
    const auto local = contentAffine(bounds);
    const auto point = context.request.point;
    bool hit = false;
    document::LayerId deepest = entry.layer;
    if (entry.contentMerge) {
        if (local)
            hit = hitMerge(context, *entry.contentMerge, local->then(parent), deepest);
    } else {
        const auto* node =
            entry.source ? context.composition.graph().findNode(*entry.source) : nullptr;
        std::optional<Outline> outline;
        if (node != nullptr && context.frame->shapes.contains(node->id))
            outline = context.frame->outline(node->id, context.cancellation);
        if (outline && outline->valid && local) {
            const auto world = local->then(parent);
            const auto inverse = world.inverted();
            if (inverse) {
                const auto content = inverse->map(point);
                const auto scale = std::sqrt(std::abs(world.determinant()));
                const auto reach = scale > 0.0 ? context.request.tolerance / scale : 0.0;
                hit = (outline->fill && insideFill(*outline, content)) ||
                      (outline->stroke &&
                       distanceToOutline(*outline, content) <= outline->halfWidth + reach);
            }
        } else if (!outline) {
            hit = insideQuad(bounds.polygon, parent, point);
        }
    }
    if (!hit)
        return false;
    // A track matte limits the layer to where its matte hits.
    if (const auto* boundary = context.composition.graph().findLayer(entry.layer);
        boundary != nullptr && boundary->trackMatte && boundary->trackMatte->source.isValid()) {
        const auto matte =
            std::ranges::find(siblings, boundary->trackMatte->source, &document::DrillEntry::layer);
        if (matte != siblings.end()) {
            document::LayerId ignored;
            const bool covered = hitEntry(context, *matte, siblings, parent, ignored);
            if (covered == boundary->trackMatte->inverted)
                return false;
        }
    }
    leaf = deepest;
    return true;
}
} // namespace

ViewerPicker::ViewerPicker(ViewerPickCompile compile) : compile_(std::move(compile)) {}

void ViewerPicker::setPickObserverForTesting(std::function<void()> observer) {
    observer_ = std::move(observer);
}

ViewerPicker::Statistics ViewerPicker::statistics() const {
    const std::lock_guard lock(mutex_);
    return statistics_;
}

ViewerPickResult ViewerPicker::pick(const ViewerPickRequest& request,
                                    const CancellationToken& cancellation) const {
    if (observer_)
        observer_();
    {
        const std::lock_guard lock(mutex_);
        ++statistics_.picks;
    }
    const auto refused = [](const ViewerPickStatus status, std::string text) {
        ViewerPickResult result;
        result.status = status;
        result.diagnostic = std::move(text);
        return result;
    };
    try {
        const auto* composition = request.snapshot.project().findComposition(request.composition);
        if (composition == nullptr)
            return refused(ViewerPickStatus::Unsupported, "The composition does not exist");
        const auto merge = document::drillLevelMerge(*composition, request.level);
        if (!merge)
            return refused(ViewerPickStatus::Unsupported, "The entered group no longer exists");
        std::shared_ptr<const FrameGeometry> frame;
        {
            const std::lock_guard lock(mutex_);
            if (frame_ && frame_->matches(request)) {
                frame = frame_;
                ++statistics_.reused;
            }
        }
        if (!frame) {
            SnapshotCompileResult compiled;
            if (request.plan && request.plan->sourceRevision() == request.snapshot.revision() &&
                request.plan->compositionId() == request.composition &&
                request.plan->projectId() == request.snapshot.project().id()) {
                compiled.status = SnapshotCompileStatus::Compiled;
                compiled.plan = request.plan;
            } else {
                {
                    const std::lock_guard lock(mutex_);
                    ++statistics_.compiles;
                }
                compiled =
                    compile_({.snapshot = request.snapshot, .compositionId = request.composition},
                             cancellation);
            }
            if (compiled.status == SnapshotCompileStatus::Cancelled ||
                cancellation.isCancellationRequested())
                return refused(ViewerPickStatus::Cancelled, "The pick was cancelled");
            if (compiled.status != SnapshotCompileStatus::Compiled || compiled.plan == nullptr)
                return refused(ViewerPickStatus::Unsupported,
                               compiled.diagnostics.empty()
                                   ? std::string("The composition cannot be compiled")
                                   : compiled.diagnostics.front().summary);
            const auto preflight = detail::preflight(
                compiled.plan,
                EvaluationRequest{.time = request.time,
                                  .output = compiled.plan->output(),
                                  .resolution = CompositionFormatResolution{},
                                  .quality = EvaluationQuality::Reference,
                                  .colorIntent = EvaluationColorIntent::LinearRec709Scene,
                                  .pixelStorageByteLimit = std::numeric_limits<std::size_t>::max()},
                cancellation, {}, nullptr, nullptr);
            if (preflight.cancelled || cancellation.isCancellationRequested())
                return refused(ViewerPickStatus::Cancelled, "The pick was cancelled");
            if (!preflight.resolved)
                return refused(ViewerPickStatus::Failed,
                               "The frame's values could not be evaluated");
            auto built = std::make_shared<FrameGeometry>(request.snapshot);
            built->composition = request.composition;
            built->time = request.time;
            built->plan = compiled.plan;
            built->resolved = std::move(*preflight.resolved);
            const auto operations = compiled.plan->operations();
            for (std::size_t index = 0; index < operations.size(); ++index)
                if (const auto* shape = std::get_if<CompiledShape>(&operations[index]))
                    built->shapes.try_emplace(shape->sourceNodeId, index);
            frame = built;
            const std::lock_guard lock(mutex_);
            frame_ = frame;
            ++statistics_.evaluations;
        }
        PickContext context{*composition, frame.get(), {}, request, cancellation};
        for (const auto& bounds : request.bounds)
            if (bounds.layerId.isValid())
                context.bounds.try_emplace(bounds.layerId, &bounds);
        // The level's own content-to-composition placement: each entered group's affine in turn.
        Affine world;
        for (const auto group : request.level) {
            const auto found = context.bounds.find(group);
            const auto local =
                found != context.bounds.end() ? contentAffine(*found->second) : std::nullopt;
            if (!local) {
                ViewerPickResult empty;
                empty.status = ViewerPickStatus::Picked;
                return empty;
            }
            world = local->then(world);
        }
        const auto entries = document::mergeEntryContents(*composition, *merge);
        ViewerPickResult result;
        result.status = ViewerPickStatus::Picked;
        for (const auto& entry : entries) {
            if (cancellation.isCancellationRequested())
                return refused(ViewerPickStatus::Cancelled, "The pick was cancelled");
            if (entry.matteSource)
                continue;
            document::LayerId leaf;
            if (hitEntry(context, entry, entries, world, leaf)) {
                result.child = entry.layer;
                result.leaf = leaf;
                break;
            }
        }
        if (cancellation.isCancellationRequested())
            return refused(ViewerPickStatus::Cancelled, "The pick was cancelled");
        return result;
    } catch (const std::bad_alloc&) {
        return refused(ViewerPickStatus::Failed, "The pick ran out of memory");
    }
}

} // namespace bloom::runtime
