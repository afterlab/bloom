#ifndef BLOOM_RUNTIME_VISIBLE_SUPPORT_HPP
#define BLOOM_RUNTIME_VISIBLE_SUPPORT_HPP

// Private to src/runtime. #343: the part of each operation's image that the frame can show.
//
// A layer's transformed support is everything its resample could write, and nothing used to clip
// it: an imported elliptical gradient drawn in its own window covered about ten frames, and the
// track matte and the Merges above it inherited that window. This pass walks the plan from the
// Composition Output back to the leaves and gives every operation the rectangle of its own image
// that any consumer can read for a pixel of the frame:
//
//  * the Composition Output reads the frame (its display window at the render scale);
//  * a Merge, a track matte and the other pointwise effects read each input where they write;
//  * a separable blur with transparent edges reads its support around that;
//  * a frame-pivot Layer Output reads the inverse image of what is read of it, plus its bilinear
//    taps (exactly, with no margin, when it is the identity or a whole-pixel move);
//  * #642 B4: so does a content-pivot Layer Output above an oversampled picture whose input's
//    bounds the frame would publish with nothing clipped are known before it is drawn
//    (UnclippedPictures below); the CPU reference and the GPU route both pivot it on those bounds;
//  * anything else -- any other content-pivot layer (its matrix follows its input's bounds), a
//    spatial or temporal effect, an unknown kernel -- may read anywhere, and so may everything
//    beneath it.
//
// The CPU reference and the GPU scene builder both clip a Layer Output's window to this rectangle
// and take the clipped window as the lattice its resample is reconstructed over, so the two routes
// keep producing the same pixels. Clipping never removes a pixel a consumer reads: every rule is a
// superset of what that consumer samples. An unbounded result (nullopt) leaves the window as it
// was, which is also why no operation whose bounds feed a pixel decision (a content pivot, a
// Transform 2D's centre, a motion kernel's centre) can have a clipped input: those consumers make
// everything beneath them unbounded. The one exception is the B4 content pivot above: its pivot is
// taken from bounds computed before anything is clipped, never from the clipped frame's.

#include "cpu_composition_evaluator_support.hpp"
#include "cpu_composition_resolution.hpp"
#include "layer_mask_sampling.hpp"
#include "layer_parent_transform.hpp"
#include "separable_image_effect.hpp"
#include "shape_geometry.hpp"

#include <bloom/render/cpu_image_primitives.hpp>
#include <bloom/render/image.hpp>
#include <bloom/render/path_raster.hpp>
#include <bloom/runtime/cancellation.hpp>
#include <bloom/runtime/compiled_plan.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace bloom::runtime::detail {

// One operation's visible rectangle in absolute device pixels, [left, right) x [top, bottom).
struct VisibleRectangle final {
    std::int64_t left = 0;
    std::int64_t top = 0;
    std::int64_t right = 0;
    std::int64_t bottom = 0;
};

// Clips `window` to the visible rectangle of its operation. Nullopt when nothing of the window can
// be seen; the window unchanged when the operation is visible everywhere.
[[nodiscard]] inline std::optional<render::ImageWindow>
clipToVisible(const render::ImageWindow window, const std::optional<render::ImageWindow>& visible) {
    if (!visible)
        return window;
    const auto left = std::max(window.originX(), visible->originX());
    const auto top = std::max(window.originY(), visible->originY());
    const auto right = std::min(window.maxXExclusive(), visible->maxXExclusive());
    const auto bottom = std::min(window.maxYExclusive(), visible->maxYExclusive());
    if (left >= right || top >= bottom)
        return std::nullopt;
    const auto clipped =
        render::ImageWindow::create(left, top, static_cast<std::uint64_t>(right - left),
                                    static_cast<std::uint64_t>(bottom - top));
    return clipped ? std::optional{*clipped.value()} : std::nullopt;
}

// #642 B4: what an operation publishes for this frame when no visible window clips anything (its
// data window and the bounds downstream readers see, bounds[index].output), worked out without a
// pixel, by the same rules and the same calls the CPU reference makes. The data windows of image
// effects are their bounds, and a clipped frame's windows are smaller, so a content pivot taken
// from the clipped frame would move with the frame; taken from these, it is where it is when the
// whole picture is drawn. Only what an expanded Vector Artwork and the nodes allowed between it
// and its layer make is covered (vector-artwork.md, "Only the window the frame shows is drawn"):
// Shape and Solid sources (plain or gradient), Layer Outputs whose matrices are known, Merges,
// and the masks, track mattes, blurs and colour kernels; anything else is `supported == false`,
// and the content pivot above it is not clipped.
struct UnclippedPicture final {
    bool known = false;
    bool supported = false;
    std::optional<render::ImageWindow> window;
    ContentBounds bounds;
    // A vector chain (the leaf's geometry is covered by the layers above it), and a raster
    // Layer Output's chain: its first non-layer image and the composed content matrix (#502).
    bool vector = false;
    std::optional<std::pair<std::size_t, LayerMatrix>> chain;
};

class UnclippedPictures final {
  public:
    UnclippedPictures(const CompiledCompositionPlan& plan, const ResolvedEvaluation& frameResolved,
                      const core::RationalTime requestTime,
                      const std::vector<std::optional<LayerMatrix>>& contentMatrices,
                      const CancellationToken& cancellation)
        : plan_(plan), frameResolved_(frameResolved), requestTime_(requestTime),
          contentMatrices_(contentMatrices), cancellation_(cancellation),
          pictures_(plan.operations().size()) {}

    // Every operation `index` reads has a lower index, and every Layer Output's matrix below the
    // one asking is known by then (visibleSupportWindows asks in plan order).
    [[nodiscard]] const UnclippedPicture& at(const std::size_t index) {
        static const UnclippedPicture unsupported{};
        if (index >= pictures_.size())
            return unsupported;
        auto& picture = pictures_[index];
        if (!picture.known) {
            picture.known = true;
            UnclippedPicture result;
            result.known = true;
            evaluate(index, result);
            pictures_[index] = std::move(result);
        }
        return pictures_[index];
    }

  private:
    void evaluate(const std::size_t index, UnclippedPicture& out) {
        if (cancellation_.isCancellationRequested())
            return;
        const auto& operation = plan_.operations()[index];
        const auto operationIndex = OperationIndex::fromRaw(index);
        const auto clock = plan_.operationClock(operationIndex);
        const auto operandTime = timeForClock(frameResolved_, requestTime_, clock);
        const auto placementTime =
            timeForClock(frameResolved_, requestTime_, plan_.placementClock(operationIndex));
        // An operation on an absent clock produces and publishes nothing.
        if (!operandTime || !placementTime) {
            out.supported = true;
            return;
        }
        const auto& resolved = resolvedForClock(frameResolved_, clock);
        const double h = resolved.horizontalScale;
        const double v = resolved.verticalScale;
        if (const auto* shape = std::get_if<CompiledShape>(&operation)) {
            shapePicture(*shape, resolved, h, v, out);
            return;
        }
        if (const auto* solid = std::get_if<CompiledSolid>(&operation)) {
            solidPicture(*solid, resolved, h, v, out);
            return;
        }
        if (const auto* layer = std::get_if<CompiledLayerOutput>(&operation)) {
            layerPicture(index, *layer, resolved, *placementTime, h, v, out);
            return;
        }
        if (const auto* stack = std::get_if<CompiledMerge>(&operation)) {
            ContentBounds local;
            ContentBounds storage;
            for (const auto& entry : stack->entries) {
                if (entry.input.value() >= index)
                    return;
                const auto& input = at(entry.input.value());
                if (!input.supported)
                    return;
                local = unionBounds(local, input.bounds);
                if (input.window)
                    storage = unionBounds(storage, boundsForWindow(*input.window, 1.0, 1.0));
            }
            out.bounds = local;
            if (storage.empty()) {
                out.window = resolved.imageDescriptor.dataWindow();
            } else {
                const auto window = render::ImageWindow::create(
                    static_cast<std::int64_t>(storage.left), static_cast<std::int64_t>(storage.top),
                    static_cast<std::uint64_t>(storage.right - storage.left),
                    static_cast<std::uint64_t>(storage.bottom - storage.top));
                if (!window)
                    return;
                out.window = *window.value();
            }
            out.supported = true;
            return;
        }
        if (const auto* effect = std::get_if<CompiledImageEffect>(&operation))
            effectPicture(*effect, index, resolved, *operandTime, h, v, out);
        // Anything else (text, media, nested compositions, ...) is not covered.
    }

    void shapePicture(const CompiledShape& shape, const ResolvedEvaluation& resolved,
                      const double h, const double v, UnclippedPicture& out) {
        const auto size = resolveParameter(shape.size, plan_, resolved);
        const auto width = resolveParameter(shape.strokeWidth, plan_, resolved);
        const auto fillSwitch = resolvedFillEnabled(shape, resolved);
        const auto strokeSwitch = resolvedStrokeEnabled(shape, resolved);
        const auto* shapePath = resolvedShapePath(shape, resolved);
        const auto line = resolvedShapeLine(shape, plan_, resolved);
        if (!size || !width || !fillSwitch || !strokeSwitch || !shapePath || !line ||
            !std::isfinite(size->value.x) || !std::isfinite(size->value.y) || size->value.x < 0 ||
            size->value.y < 0 || !std::isfinite(width->value) || width->value < 0)
            return;
        // A leaf's geometry is a vector chain whether or not it draws.
        out.vector = true;
        const bool fillEnabled = *fillSwitch && shape.kind != document::ShapeKind::Line;
        const bool strokeEnabled = *strokeSwitch && width->value > 0;
        if (!fillEnabled && !strokeEnabled) {
            out.supported = true;
            return;
        }
        const auto paths = shapeRenderPaths(shape, size->value, *line, *shapePath);
        const auto raster = render::PathRaster::create(
            std::span<const render::Path>(paths),
            {strokeEnabled ? width->value : 0, static_cast<render::PathStrokeAlign>(shape.strokeAlign),
             static_cast<render::PathStrokeJoin>(shape.strokeJoin),
             static_cast<render::PathStrokeCap>(shape.strokeCap)},
            h, v, [this] { return cancellation_.isCancellationRequested(); });
        if (!raster)
            return;
        const auto extent = raster.value()->bounds(fillEnabled, strokeEnabled);
        out.bounds = {extent.left, extent.top, extent.right, extent.bottom};
        out.supported = true;
        if (out.bounds.empty())
            return;
        const auto left = std::floor(extent.left * h);
        const auto top = std::floor(extent.top * v);
        const auto right = std::ceil(extent.right * h);
        const auto bottom = std::ceil(extent.bottom * v);
        constexpr double maxCoordinate = 9007199254740991.0;
        const auto finiteCoordinate = [](const double value) {
            return std::isfinite(value) && std::abs(value) <= maxCoordinate;
        };
        if (!finiteCoordinate(left) || !finiteCoordinate(top) || !finiteCoordinate(right) ||
            !finiteCoordinate(bottom) ||
            right - left > static_cast<double>(std::numeric_limits<std::uint32_t>::max()) ||
            bottom - top > static_cast<double>(std::numeric_limits<std::uint32_t>::max())) {
            out.supported = false;
            return;
        }
        const auto window = render::ImageWindow::create(
            static_cast<std::int64_t>(left), static_cast<std::int64_t>(top),
            static_cast<std::uint32_t>(right - left), static_cast<std::uint32_t>(bottom - top));
        if (!window) {
            out.supported = false;
            return;
        }
        out.window = *window.value();
    }

    void solidPicture(const CompiledSolid& solid, const ResolvedEvaluation& resolved,
                      const double h, const double v, UnclippedPicture& out) {
        const auto width = resolveParameter(solid.width, plan_, resolved);
        const auto height = resolveParameter(solid.height, plan_, resolved);
        if (!width || !height || width->value < 1.0 || height->value < 1.0)
            return;
        // A plain solid is a vector leaf; a gradient is a raster.
        out.vector = !solid.gradient.has_value();
        const auto display = frameResolved_.imageDescriptor.displayWindow();
        double exactWidth = static_cast<double>(display.extent().width());
        double exactHeight = static_cast<double>(display.extent().height());
        const double authorWidth = width->value;
        const double authorHeight = height->value;
        if (authorWidth != plan_.format().width())
            exactWidth = authorWidth * h;
        if (authorHeight != plan_.format().height())
            exactHeight = authorHeight * v;
        const auto w = std::ceil(exactWidth);
        const auto hh = std::ceil(exactHeight);
        if (!std::isfinite(w) || !std::isfinite(hh) || w > 16777216.0 || hh > 16777216.0)
            return;
        const auto window = render::ImageWindow::create(0, 0, static_cast<std::uint64_t>(w),
                                                        static_cast<std::uint64_t>(hh));
        if (!window)
            return;
        out.window = *window.value();
        out.bounds = boundsForWindow(*out.window, h, v);
        out.bounds.right = authorWidth;
        out.bounds.bottom = authorHeight;
        out.supported = true;
    }

    void layerPicture(const std::size_t index, const CompiledLayerOutput& layer,
                      const ResolvedEvaluation& resolved, const core::RationalTime placementTime,
                      const double h, const double v, UnclippedPicture& out) {
        if (layer.input.value() >= index || layer.composedArtwork)
            return;
        const auto& input = at(layer.input.value());
        if (!input.supported || !contentMatrices_[index])
            return;
        const auto position = resolveParameter(layer.position, plan_, resolved);
        const auto anchor = resolveParameter(layer.anchor, plan_, resolved);
        const auto scale = resolveParameter(layer.scale, plan_, resolved);
        const auto rotation = resolveParameter(layer.rotation, plan_, resolved);
        const auto opacity = resolveParameter(layer.opacity, plan_, resolved);
        if (!position || !anchor || !scale || !rotation || !opacity)
            return;
        const auto& content = *contentMatrices_[index];
        out.vector = input.vector;
        if (!input.vector)
            out.chain = input.chain ? std::pair{input.chain->first, content.times(input.chain->second)}
                                    : std::pair{layer.input.value(), content};
        out.supported = true;
        // Outside its range, or with nothing to draw, a layer publishes no image and no bounds.
        if (placementTime < layer.inPoint || (layer.outPoint && placementTime >= *layer.outPoint) ||
            !input.window)
            return;
        const auto local = input.bounds;
        if (local.empty())
            return;
        const double contentScale = layer.contentScale;
        const bool scaledContent = contentScale != 1.0 || layer.outputScale != 1.0;
        const auto inputCentre = local.pivot();
        const document::Vec2d centre =
            layer.framePivot
                ? document::Vec2d{static_cast<double>(plan_.format().width()) / 2.0,
                                  static_cast<double>(plan_.format().height()) / 2.0}
                : document::Vec2d{inputCentre.x / contentScale, inputCentre.y / contentScale};
        const auto sourceWindow = *input.window;
        const auto bufferCentre = boundsForWindow(sourceWindow, h, v).centre();
        render::LayerTransform::Authored authored{
            .translationX = position->value.x - centre.x - anchor->value.x,
            .translationY = position->value.y - centre.y - anchor->value.y,
            .anchorX = centre.x - bufferCentre.x + anchor->value.x,
            .anchorY = centre.y - bufferCentre.y + anchor->value.y,
            .scaleX = scale->value.x * layer.sourcePixelAspect,
            .scaleY = scale->value.y,
            .rotationDegrees = rotation->value,
            .opacity = opacity->value,
        };
        if (!std::isfinite(authored.translationX * h) || !std::isfinite(authored.translationY * v)) {
            out.supported = false;
            return;
        }
        if (authored.scaleX == 0.0 || authored.scaleY == 0.0)
            return;
        const auto transform = render::LayerTransform::create(authored, sourceWindow, h, v);
        if (!transform) {
            out.supported = false;
            return;
        }
        std::optional<ParentedLayerTransform> parented;
        if (layer.parent || scaledContent) {
            parented.emplace(content, sourceWindow, h, v, static_cast<float>(opacity->value));
            if (!parented->finite()) {
                out.supported = false;
                return;
            }
            if (!parented->invertible())
                return;
        }
        std::optional<ParentedLayerTransform> concatenated;
        if (!input.vector && input.chain && input.chain->first < index) {
            const auto& source = at(input.chain->first);
            if (!source.supported) {
                out.supported = false;
                return;
            }
            if (source.window) {
                concatenated.emplace(content.times(input.chain->second), *source.window, h, v,
                                     static_cast<float>(opacity->value));
                if (!concatenated->finite()) {
                    out.supported = false;
                    return;
                }
                if (!concatenated->invertible())
                    return;
            }
        }
        const auto forward = [&](const double x, const double y) {
            return parented ? parented->forwardMap(x, y) : transform.value()->forwardMap(x, y);
        };
        const auto map = [&](const document::Vec2d point) {
            const auto mapped = forward(point.x * h - 0.5 - static_cast<double>(sourceWindow.originX()),
                                        point.y * v - 0.5 - static_cast<double>(sourceWindow.originY()));
            return document::Vec2d{(mapped.x + 0.5) / h, (mapped.y + 0.5) / v};
        };
        auto polygon = boundsCorners(local);
        for (auto& point : polygon)
            point = map(point);
        ContentBounds output{polygon[0].x, polygon[0].y, polygon[0].x, polygon[0].y};
        for (const auto point : polygon) {
            output.left = std::min(output.left, point.x);
            output.top = std::min(output.top, point.y);
            output.right = std::max(output.right, point.x);
            output.bottom = std::max(output.bottom, point.y);
        }
        out.bounds = output;
        constexpr double limit = 16777214.0;
        const double right = sourceWindow.extent().width();
        const double bottom = sourceWindow.extent().height();
        for (const auto point :
             {forward(-1.0, -1.0), forward(right, -1.0), forward(right, bottom), forward(-1.0, bottom)})
            if (!std::isfinite(point.x) || !std::isfinite(point.y) || std::abs(point.x) > limit ||
                std::abs(point.y) > limit) {
                out.supported = false;
                return;
            }
        const auto working = render::ImageWindow::create(-16777216, -16777216, 33554432, 33554432);
        out.window = concatenated ? concatenated->supportBounds(*working.value())
                     : parented   ? parented->supportBounds(*working.value())
                                  : transform.value()->supportBounds(*working.value());
    }

    void effectPicture(const CompiledImageEffect& effect, const std::size_t index,
                       const ResolvedEvaluation& resolved, const core::RationalTime operandTime,
                       const double h, const double v, UnclippedPicture& out) {
        if (effect.input.value() >= index || !effect.fieldImages.empty() || effect.maskInput ||
            effect.look)
            return;
        const auto& input = at(effect.input.value());
        if (!input.supported)
            return;
        const auto& kernel = effect.kernel;
        bool identity = effect.bypass || std::holds_alternative<IdentityImageKernel>(kernel);
        // The window an effect that is not the identity draws for its input's.
        std::optional<PreparedSeparableBlur> blur;
        if (!identity) {
            if (const auto* masks = std::get_if<LayerMaskKernel>(&kernel)) {
                const auto prepared = sampleLayerMasks(*masks, plan_, resolved, operandTime, h, v);
                if (!prepared)
                    return;
                identity = prepared->masks.empty();
            } else if (const auto* combine = std::get_if<ImageCombineKernel>(&kernel)) {
                // A track matte (or any combine but a Mix, which is the identity at a factor of
                // 0) draws over its base's window.
                if (combine->mode == ImageCombineMode::Mix)
                    return;
            } else if (const auto* separable = std::get_if<SeparableBlurKernel>(&kernel)) {
                const auto radius = resolveParameter(separable->radius, plan_, resolved);
                if (!radius)
                    return;
                std::optional<double> vertical;
                if (separable->separate) {
                    const auto sampled = resolveParameter(separable->radiusY, plan_, resolved);
                    if (!sampled)
                        return;
                    vertical = sampled->value;
                }
                blur = prepareSeparableBlur(*separable, radius->value, h, v, vertical);
                if (!blur)
                    return;
                identity = blur->identity();
            } else if (!(std::holds_alternative<LevelsKernel>(kernel) ||
                         std::holds_alternative<CurvesKernel>(kernel) ||
                         std::holds_alternative<HueSaturationKernel>(kernel) ||
                         std::holds_alternative<GradeKernel>(kernel) ||
                         std::holds_alternative<HighlightRollOffKernel>(kernel) ||
                         std::holds_alternative<AlphaAssociationKernel>(kernel) ||
                         std::holds_alternative<ColorizeKernel>(kernel) ||
                         std::holds_alternative<ColorToolKernel>(kernel))) {
                // The colour kernels above are never the identity and draw over their input's
                // window; anything else is not covered.
                return;
            }
        }
        out.supported = true;
        if (identity) {
            // Its image is its input's, as are its bounds and its vector chain.
            out.window = input.window;
            out.bounds = input.bounds;
            out.vector = input.vector;
            return;
        }
        // With no input image it is not applied: its bounds are its input's.
        out.bounds = input.bounds;
        if (!input.window)
            return;
        out.window = blur ? blurOutputWindow(*blur, *input.window) : input.window;
        if (!out.window) {
            out.supported = false;
            return;
        }
        out.bounds = boundsForWindow(*out.window, h, v);
    }

    const CompiledCompositionPlan& plan_;
    const ResolvedEvaluation& frameResolved_;
    core::RationalTime requestTime_;
    const std::vector<std::optional<LayerMatrix>>& contentMatrices_;
    const CancellationToken& cancellation_;
    std::vector<UnclippedPicture> pictures_;
};

// The visible window of every operation of `plan` for this frame (nullopt: unbounded). `frame` is
// the Composition Output's window at the render scale; `frameResolved` resolves each operation's
// parameters on its own clock, exactly as the evaluators do. #642 B4: `contentPivots`, when given,
// receives for every content-pivot Layer Output that is clipped the bounds of its input it pivots
// on (UnclippedPictures); the evaluators take those in place of the frame's own.
[[nodiscard]] inline std::vector<std::optional<render::ImageWindow>>
visibleSupportWindows(const CompiledCompositionPlan& plan, const ResolvedEvaluation& frameResolved,
                      const core::RationalTime requestTime, const std::size_t output,
                      const render::ImageWindow frame,
                      std::vector<std::optional<ContentBounds>>* contentPivots = nullptr,
                      const CancellationToken& cancellation = {}) {
    const auto& operations = plan.operations();
    const auto count = operations.size();
    struct Demand final {
        bool seen = false;
        bool unbounded = false;
        VisibleRectangle rectangle{};
    };
    std::vector<Demand> demand(count);
    constexpr std::int64_t kLimit = std::int64_t{1} << 40;
    const auto add = [&](const std::size_t index, const Demand& value) {
        if (index >= count)
            return;
        auto& target = demand[index];
        if (value.unbounded || target.unbounded) {
            target.seen = true;
            target.unbounded = true;
            return;
        }
        if (!target.seen) {
            target = value;
            return;
        }
        target.rectangle.left = std::min(target.rectangle.left, value.rectangle.left);
        target.rectangle.top = std::min(target.rectangle.top, value.rectangle.top);
        target.rectangle.right = std::max(target.rectangle.right, value.rectangle.right);
        target.rectangle.bottom = std::max(target.rectangle.bottom, value.rectangle.bottom);
    };
    const Demand unbounded{true, true, {}};
    const auto widened = [&](Demand value, const std::int64_t x, const std::int64_t y) {
        if (value.unbounded)
            return value;
        value.rectangle.left -= x;
        value.rectangle.right += x;
        value.rectangle.top -= y;
        value.rectangle.bottom += y;
        if (std::abs(value.rectangle.left) > kLimit || std::abs(value.rectangle.right) > kLimit ||
            std::abs(value.rectangle.top) > kLimit || std::abs(value.rectangle.bottom) > kLimit)
            return unbounded;
        return value;
    };
    const double hScale = frameResolved.horizontalScale;
    const double vScale = frameResolved.verticalScale;

    // A frame-pivot layer's matrix needs only its own parameters and its parent's; a content-pivot
    // one needs its input's bounds, which are known only while the frame is evaluated -- except
    // (#642 B4) above an oversampled picture, where they are worked out first (UnclippedPictures).
    std::vector<std::optional<LayerMatrix>> layerMatrices(count);
    std::vector<std::optional<LayerMatrix>> contentMatrices(count);
    if (contentPivots != nullptr)
        contentPivots->assign(count, std::nullopt);
    // Whether an oversampled picture (a content or output scale) is drawn at or beneath each
    // operation: only a content pivot above one takes its pivot from UnclippedPictures, so every
    // other plan keeps its windows exactly as they were.
    std::vector<bool> oversampled(count, false);
    for (std::size_t index = 0; index < count; ++index) {
        if (const auto* layer = std::get_if<CompiledLayerOutput>(&operations[index]))
            oversampled[index] = layer->contentScale != 1.0 || layer->outputScale != 1.0;
        if (!oversampled[index])
            forEachInput(operations[index], [&](const OperationIndex input) {
                if (input.value() < index && oversampled[input.value()])
                    oversampled[index] = true;
            });
    }
    UnclippedPictures pictures(plan, frameResolved, requestTime, contentMatrices, cancellation);
    const auto matrixOf = [&](const std::size_t index) {
        const auto* layer = std::get_if<CompiledLayerOutput>(&operations[index]);
        if (layer == nullptr)
            return;
        if (layer->parent &&
            (layer->parent->value() >= index || !layerMatrices[layer->parent->value()].has_value()))
            return;
        const auto& resolved =
            resolvedForClock(frameResolved, plan.operationClock(OperationIndex::fromRaw(index)));
        const auto position = resolveParameter(layer->position, plan, resolved);
        const auto anchor = resolveParameter(layer->anchor, plan, resolved);
        const auto scale = resolveParameter(layer->scale, plan, resolved);
        const auto rotation = resolveParameter(layer->rotation, plan, resolved);
        if (!position || !anchor || !scale || !rotation)
            return;
        // #502: a frame pivot is the frame's centre. #642 B4: a content pivot above an oversampled
        // picture is its input's centre with nothing clipped, at the frame's scale.
        document::Vec2d centre{static_cast<double>(plan.format().width()) / 2.0,
                               static_cast<double>(plan.format().height()) / 2.0};
        if (!layer->framePivot) {
            const auto input = layer->input.value();
            if (contentPivots == nullptr || input >= index || !oversampled[index])
                return;
            const auto& picture = pictures.at(input);
            if (!picture.supported)
                return;
            (*contentPivots)[index] = picture.bounds;
            const auto inputPivot = picture.bounds.pivot();
            centre = {inputPivot.x / layer->contentScale, inputPivot.y / layer->contentScale};
        }
        const document::Vec2d pivot{centre.x + anchor->value.x, centre.y + anchor->value.y};
        auto matrix = LayerMatrix::authored(position->value, pivot, scale->value, rotation->value);
        // #642: with a content scale its input is that many times larger.
        auto content = LayerMatrix::content(position->value, pivot, scale->value, rotation->value,
                                            layer->sourcePixelAspect, layer->contentScale);
        if (layer->parent) {
            matrix = layerMatrices[layer->parent->value()]->times(matrix);
            content = layerMatrices[layer->parent->value()]->times(content);
        }
        layerMatrices[index] = matrix;
        // #642: drawn into an oversampled group, its pixels land that many times larger.
        contentMatrices[index] = content.enlarged(layer->outputScale);
    };
    for (std::size_t index = 0; index < count; ++index)
        matrixOf(index);

    // What a resample through `matrix` reads of its input for the rectangle it is asked for: the
    // bilinear taps of the inverse image of every pixel centre, plus one pixel for rounding.
    const auto throughLayer = [&](const Demand& asked, const LayerMatrix& matrix) -> Demand {
        if (asked.unbounded)
            return asked;
        const auto& r = asked.rectangle;
        if (matrix.a == 1 && matrix.b == 0 && matrix.c == 0 && matrix.d == 1) {
            const double dx = matrix.x * hScale;
            const double dy = matrix.y * vScale;
            if (dx == std::floor(dx) && dy == std::floor(dy) && std::abs(dx) < kLimit &&
                std::abs(dy) < kLimit) {
                // A whole-pixel move reads each output pixel's own source pixel; its other tap
                // weighs exactly zero.
                const auto x = static_cast<std::int64_t>(dx);
                const auto y = static_cast<std::int64_t>(dy);
                return Demand{true, false, {r.left - x, r.top - y, r.right - x, r.bottom - y}};
            }
        }
        const double determinant = matrix.a * matrix.d - matrix.b * matrix.c;
        if (!std::isfinite(determinant) || determinant == 0.0)
            return unbounded;
        const double ia = matrix.d / determinant, ib = -matrix.b / determinant;
        const double ic = -matrix.c / determinant, id = matrix.a / determinant;
        double left = std::numeric_limits<double>::infinity(), top = left;
        double right = -left, bottom = -left;
        for (const double x :
             {static_cast<double>(r.left) + 0.5, static_cast<double>(r.right) - 0.5})
            for (const double y :
                 {static_cast<double>(r.top) + 0.5, static_cast<double>(r.bottom) - 0.5}) {
                const double px = x / hScale - matrix.x, py = y / vScale - matrix.y;
                const double u = (ia * px + ib * py) * hScale - 0.5;
                const double v = (ic * px + id * py) * vScale - 0.5;
                left = std::min(left, u);
                right = std::max(right, u);
                top = std::min(top, v);
                bottom = std::max(bottom, v);
            }
        if (!std::isfinite(left) || !std::isfinite(right) || !std::isfinite(top) ||
            !std::isfinite(bottom) || std::abs(left) > kLimit || std::abs(right) > kLimit ||
            std::abs(top) > kLimit || std::abs(bottom) > kLimit)
            return unbounded;
        return Demand{true,
                      false,
                      {static_cast<std::int64_t>(std::floor(left)) - 1,
                       static_cast<std::int64_t>(std::floor(top)) - 1,
                       static_cast<std::int64_t>(std::floor(right)) + 3,
                       static_cast<std::int64_t>(std::floor(bottom)) + 3}};
    };

    if (output < count)
        add(output, Demand{true,
                           false,
                           {frame.originX(), frame.originY(), frame.maxXExclusive(),
                            frame.maxYExclusive()}});
    for (std::size_t index = count; index-- > 0;) {
        // An operation nothing reads for a pixel (a parent lending only its matrix) is left as it
        // was.
        const Demand asked = demand[index].seen ? demand[index] : unbounded;
        const auto& operation = operations[index];
        const auto everyInput = [&](const Demand& value) {
            forEachInput(operation, [&](const OperationIndex input) {
                if (input.value() >= index)
                    return;
                add(input.value(), value);
            });
        };
        const auto clock = plan.operationClock(OperationIndex::fromRaw(index));
        if (!timeForClock(frameResolved, requestTime, clock)) {
            everyInput(unbounded);
            continue;
        }
        if (std::holds_alternative<CompiledCompositionOutput>(operation) ||
            std::holds_alternative<CompiledMerge>(operation)) {
            everyInput(asked);
            continue;
        }
        if (const auto* layer = std::get_if<CompiledLayerOutput>(&operation)) {
            // The parent lends its matrix, not its pixels.
            const auto& content = contentMatrices[index];
            if (layer->input.value() < index)
                add(layer->input.value(), content ? throughLayer(asked, *content) : unbounded);
            continue;
        }
        if (const auto* effect = std::get_if<CompiledImageEffect>(&operation)) {
            if (!effect->fieldImages.empty()) {
                everyInput(unbounded);
                continue;
            }
            const auto& kernel = effect->kernel;
            // #642: a layer's masks multiply its content by their coverage over the content's own
            // pixels, and the grades, keys and channel moves read only the pixel they write, so
            // each reads its inputs exactly where it is read.
            if (std::holds_alternative<IdentityImageKernel>(kernel) ||
                std::holds_alternative<ImageCombineKernel>(kernel) ||
                std::holds_alternative<PointwiseImageKernel>(kernel) ||
                std::holds_alternative<CstKernel>(kernel) ||
                std::holds_alternative<FileTransformKernel>(kernel) ||
                std::holds_alternative<LayerMaskKernel>(kernel) ||
                std::holds_alternative<LumaKeyKernel>(kernel) ||
                std::holds_alternative<LevelsKernel>(kernel) ||
                std::holds_alternative<CurvesKernel>(kernel) ||
                std::holds_alternative<HueSaturationKernel>(kernel) ||
                std::holds_alternative<GradeKernel>(kernel) ||
                std::holds_alternative<ChannelSelectKernel>(kernel) ||
                std::holds_alternative<ChannelCombineKernel>(kernel) ||
                std::holds_alternative<HighlightRollOffKernel>(kernel) ||
                std::holds_alternative<AlphaAssociationKernel>(kernel) ||
                std::holds_alternative<ColorizeKernel>(kernel) ||
                std::holds_alternative<ColorToolKernel>(kernel)) {
                everyInput(asked);
                continue;
            }
            // #642: a Transform 2D that only moves (a Drop Shadow's offset) reads each pixel's
            // source one move away, plus its resample's taps; its centre then decides nothing.
            if (const auto* transform = std::get_if<Transform2DKernel>(&kernel)) {
                const auto* scale = std::get_if<document::Vec2d>(&transform->scale.source);
                const auto* rotation = std::get_if<double>(&transform->rotation.source);
                const auto* skew = std::get_if<double>(&transform->skew.source);
                const auto& resolved = resolvedForClock(frameResolved, clock);
                const auto translate = resolveParameter(transform->translate, plan, resolved);
                if (!asked.unbounded && translate && scale != nullptr && scale->x == 1.0 &&
                    scale->y == 1.0 && rotation != nullptr && *rotation == 0.0 &&
                    skew != nullptr && *skew == 0.0 && std::isfinite(translate->value.x) &&
                    std::isfinite(translate->value.y) &&
                    std::abs(translate->value.x * hScale) < static_cast<double>(kLimit) &&
                    std::abs(translate->value.y * vScale) < static_cast<double>(kLimit)) {
                    const auto dx =
                        static_cast<std::int64_t>(std::floor(translate->value.x * hScale));
                    const auto dy =
                        static_cast<std::int64_t>(std::floor(translate->value.y * vScale));
                    Demand moved = asked;
                    moved.rectangle = {asked.rectangle.left - dx, asked.rectangle.top - dy,
                                       asked.rectangle.right - dx, asked.rectangle.bottom - dy};
                    everyInput(widened(moved, 2, 2));
                    continue;
                }
                everyInput(unbounded);
                continue;
            }
            // #642: a directional (Glow Streak) pass reads four taps up to three spacings along its
            // ray, each a bilinear sample.
            if (const auto* directional = std::get_if<DirectionalBlurKernel>(&kernel)) {
                const auto& resolved = resolvedForClock(frameResolved, clock);
                const auto spacing = resolveParameter(directional->spacing, plan, resolved);
                if (spacing && std::isfinite(spacing->value)) {
                    const double reach = 3.0 * std::abs(spacing->value) * std::max(hScale, vScale);
                    if (reach < static_cast<double>(kLimit)) {
                        const auto margin = static_cast<std::int64_t>(std::ceil(reach)) + 2;
                        everyInput(widened(asked, margin, margin));
                        continue;
                    }
                }
                everyInput(unbounded);
                continue;
            }
            if (const auto* blur = std::get_if<SeparableBlurKernel>(&kernel);
                blur != nullptr && blur->edge == BlurEdgeMode::Transparent) {
                const auto& resolved = resolvedForClock(frameResolved, clock);
                const auto radius = resolveParameter(blur->radius, plan, resolved);
                // #642 B4: a separate vertical radius reaches as far as the evaluators blur it.
                std::optional<double> vertical;
                if (blur->separate)
                    if (const auto sampled = resolveParameter(blur->radiusY, plan, resolved))
                        vertical = sampled->value;
                const auto prepared =
                    blur->separate && !vertical
                        ? std::nullopt
                        : prepareSeparableBlur(*blur,
                                               radius ? std::optional(radius->value) : std::nullopt,
                                               hScale, vScale, vertical);
                if (prepared) {
                    everyInput(widened(
                        asked, static_cast<std::int64_t>(prepared->horizontal.size() / 2) + 1,
                        static_cast<std::int64_t>(prepared->vertical.size() / 2) + 1));
                    continue;
                }
            }
            everyInput(unbounded);
            continue;
        }
        everyInput(unbounded);
    }

    std::vector<std::optional<render::ImageWindow>> result(count);
    for (std::size_t index = 0; index < count; ++index) {
        const auto& value = demand[index];
        if (!value.seen || value.unbounded || value.rectangle.right <= value.rectangle.left ||
            value.rectangle.bottom <= value.rectangle.top)
            continue;
        const auto window = render::ImageWindow::create(
            value.rectangle.left, value.rectangle.top,
            static_cast<std::uint64_t>(value.rectangle.right - value.rectangle.left),
            static_cast<std::uint64_t>(value.rectangle.bottom - value.rectangle.top));
        if (window)
            result[index] = *window.value();
    }
    return result;
}

// The track matte that bounds a Layer Output's pixels, per operation: an Alpha or Luma track matte
// (not inverted, at full strength) multiplies the layer by a matte that is zero outside the matte's
// own image, so nothing of the layer outside that image can be seen. It applies when the layer is
// read by nothing but that matte, the matte's operation comes earlier (the compiler lowers a matte
// source before the layer it mattes, so its window is known when the layer is drawn), and the matte
// is itself visible within bounds (so no consumer above reads its bounds for a pixel decision).
[[nodiscard]] inline std::vector<std::optional<std::size_t>>
matteBoundedLayers(const CompiledCompositionPlan& plan,
                   const std::vector<std::optional<render::ImageWindow>>& visible) {
    const auto& operations = plan.operations();
    const auto count = operations.size();
    std::vector<std::uint32_t> readers(count, 0);
    for (const auto& operation : operations)
        forEachInput(operation, [&](const OperationIndex input) {
            if (input.value() < count && readers[input.value()] < 2)
                ++readers[input.value()];
        });
    std::vector<std::optional<std::size_t>> result(count);
    for (std::size_t index = 0; index < count; ++index) {
        const auto* effect = std::get_if<CompiledImageEffect>(&operations[index]);
        const auto* combine =
            effect != nullptr ? std::get_if<ImageCombineKernel>(&effect->kernel) : nullptr;
        if (combine == nullptr || !effect->trackMatte || !effect->secondaryInput ||
            effect->maskInput || !effect->fieldImages.empty() || effect->bypass ||
            combine->mode != ImageCombineMode::MultiplyAlpha || combine->invert ||
            combine->drivenChannel ||
            (combine->channel != render::ImageCoverageChannel::Alpha &&
             combine->channel != render::ImageCoverageChannel::Luma) ||
            !combine->factor || index >= visible.size() || !visible[index])
            continue;
        const auto* strength = std::get_if<double>(&combine->factor->source);
        const auto base = effect->input.value();
        const auto matte = effect->secondaryInput->value();
        if (strength == nullptr || *strength != 1.0 || base >= index || matte >= base ||
            readers[base] != 1 || !std::holds_alternative<CompiledLayerOutput>(operations[base]))
            continue;
        result[base] = matte;
    }
    return result;
}

} // namespace bloom::runtime::detail

#endif // BLOOM_RUNTIME_VISIBLE_SUPPORT_HPP
