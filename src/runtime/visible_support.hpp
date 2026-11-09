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
//  * anything else -- a content-pivot layer (its matrix follows its input's bounds), a spatial or
//    temporal effect, an unknown kernel -- may read anywhere, and so may everything beneath it.
//
// The CPU reference and the GPU scene builder both clip a Layer Output's window to this rectangle
// and take the clipped window as the lattice its resample is reconstructed over, so the two routes
// keep producing the same pixels. Clipping never removes a pixel a consumer reads: every rule is a
// superset of what that consumer samples. An unbounded result (nullopt) leaves the window as it
// was, which is also why no operation whose bounds feed a pixel decision (a content pivot, a
// Transform 2D's centre, a motion kernel's centre) can have a clipped input: those consumers make
// everything beneath them unbounded.

#include "cpu_composition_evaluator_support.hpp"
#include "cpu_composition_resolution.hpp"
#include "layer_parent_transform.hpp"
#include "separable_image_effect.hpp"

#include <bloom/render/image.hpp>
#include <bloom/runtime/compiled_plan.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
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

// The visible window of every operation of `plan` for this frame (nullopt: unbounded). `frame` is
// the Composition Output's window at the render scale; `frameResolved` resolves each operation's
// parameters on its own clock, exactly as the evaluators do.
[[nodiscard]] inline std::vector<std::optional<render::ImageWindow>>
visibleSupportWindows(const CompiledCompositionPlan& plan, const ResolvedEvaluation& frameResolved,
                      const core::RationalTime requestTime, const std::size_t output,
                      const render::ImageWindow frame) {
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
    // one needs its input's bounds, which are known only while the frame is evaluated.
    std::vector<std::optional<LayerMatrix>> layerMatrices(count);
    std::vector<std::optional<LayerMatrix>> contentMatrices(count);
    std::vector<bool> matrixKnown(count, false);
    const auto matrixOf = [&](const std::size_t index) {
        const auto* layer = std::get_if<CompiledLayerOutput>(&operations[index]);
        if (layer == nullptr || !layer->framePivot)
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
        const document::Vec2d pivot{
            static_cast<double>(plan.format().width()) / 2.0 + anchor->value.x,
            static_cast<double>(plan.format().height()) / 2.0 + anchor->value.y};
        auto matrix = LayerMatrix::authored(position->value, pivot, scale->value, rotation->value);
        auto content = LayerMatrix::authored(
            position->value, pivot, {scale->value.x * layer->sourcePixelAspect, scale->value.y},
            rotation->value);
        if (layer->parent) {
            matrix = layerMatrices[layer->parent->value()]->times(matrix);
            content = layerMatrices[layer->parent->value()]->times(content);
        }
        layerMatrices[index] = matrix;
        contentMatrices[index] = content;
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
            if (std::holds_alternative<IdentityImageKernel>(kernel) ||
                std::holds_alternative<ImageCombineKernel>(kernel) ||
                std::holds_alternative<PointwiseImageKernel>(kernel) ||
                std::holds_alternative<CstKernel>(kernel) ||
                std::holds_alternative<FileTransformKernel>(kernel)) {
                everyInput(asked);
                continue;
            }
            if (const auto* blur = std::get_if<SeparableBlurKernel>(&kernel);
                blur != nullptr && blur->edge == BlurEdgeMode::Transparent) {
                const auto& resolved = resolvedForClock(frameResolved, clock);
                const auto radius = resolveParameter(blur->radius, plan, resolved);
                const auto prepared = prepareSeparableBlur(
                    *blur, radius ? std::optional(radius->value) : std::nullopt, hScale, vScale);
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
