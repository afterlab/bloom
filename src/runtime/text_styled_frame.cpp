// #757: a styled Text Source's frame (text_styled_frame.hpp).
#include "text_styled_frame.hpp"

#include "cpu_composition_resolution.hpp"
#include "operation_key.hpp"

#include <bloom/document/text_styling.hpp>
#include <bloom/render/cpu_image_primitives.hpp>
#include <bloom/render/text_raster.hpp>
#include <bloom/core/utf8.hpp>
#include <bloom/runtime/text_animator.hpp>
#include <bloom/runtime/text_markup.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <numbers>
#include <limits>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <type_traits>
#include <utility>

namespace bloom::runtime::detail {
namespace {

void addFontToKey(OperationKey& key, const render::TextFont& font) {
    std::visit(
        [&key](const auto& face) {
            using Face = std::decay_t<decltype(face)>;
            if constexpr (std::is_same_v<Face, render::EmbeddedFace>) {
                key.add(std::string("embedded"));
                key.add(face);
            } else {
                const auto digest = face.contentDigest.toLowercaseHex();
                key.add(std::string(digest.data(), digest.size()));
                key.add(face.faceIndex);
            }
        },
        font);
}

[[nodiscard]] render::Path translated(render::Path path, const double x, const double y) {
    for (auto& anchor : path.anchors) {
        anchor.point = {anchor.point.x + x, anchor.point.y + y};
        if (anchor.inHandle)
            anchor.inHandle = render::PathPoint{anchor.inHandle->x + x, anchor.inHandle->y + y};
        if (anchor.outHandle)
            anchor.outHandle = render::PathPoint{anchor.outHandle->x + x, anchor.outHandle->y + y};
    }
    return path;
}


// #758: one Text Animator's values at the frame.
struct ResolvedTextAnimator final {
    std::vector<TextSelectorValues> selectors;
    double opacity = 1.0;
    document::Vec2d position{};
    document::Vec2d scale{1.0, 1.0};
    double rotation = 0.0;
    double skew = 0.0;
    bool fill = false;
    core::Color4d fillColor{};
    bool outline = false;
    core::Color4d outlineColor{};
    double outlineWidth = 0.0;
    double tracking = 0.0;
    std::int64_t revealBy = 0;
    std::int64_t revealUnits = 0;
    double reveal = 100.0;
    bool cursor = false;
    std::u32string cursorCharacter;
    bool cursorShown = false;
};

[[nodiscard]] std::optional<std::vector<ResolvedTextAnimator>>
resolveTextAnimators(const std::vector<CompiledTextAnimator>& animators,
                     const CompiledCompositionPlan& plan, const ResolvedEvaluation& resolved,
                     const core::RationalTime time) {
    std::vector<ResolvedTextAnimator> result;
    bool ok = true;
    const auto number = [&](const CompiledScalarParameter& parameter) {
        const auto value = resolveParameter(parameter, plan, resolved);
        ok = ok && value && std::isfinite(value->value);
        return value && std::isfinite(value->value) ? value->value : 0.0;
    };
    const auto vector = [&](const CompiledVec2Parameter& parameter) {
        const auto value = resolveParameter(parameter, plan, resolved);
        ok = ok && value && std::isfinite(value->value.x) && std::isfinite(value->value.y);
        return value ? value->value : document::Vec2d{};
    };
    const auto colorOf = [&](const CompiledColorParameter& parameter) {
        const auto value = resolveParameter(parameter, plan, resolved);
        ok = ok && value && value->value.isValid();
        return value ? value->value : core::Color4d{};
    };
    const auto choice = [&](const CompiledTextChoice& parameter, const std::int64_t last) {
        const auto value = resolveParameter(parameter.id, parameter.value, parameter.driven, resolved);
        ok = ok && value.has_value();
        return std::clamp<std::int64_t>(value ? value->value : 0, 0, last);
    };
    const auto flag = [&](const CompiledTextSwitch& parameter) {
        const auto value = resolveParameter(parameter.id, parameter.value, parameter.driven, resolved);
        ok = ok && value.has_value();
        return value && value->value;
    };
    for (const auto& animator : animators) {
        ResolvedTextAnimator values;
        for (const auto& selector : animator.selectors)
            values.selectors.push_back({.mode = choice(selector.mode, 2),
                                        .units = choice(selector.units, 3),
                                        .rangeBy = choice(selector.rangeBy, 1),
                                        .shape = choice(selector.shape, 5),
                                        .randomize = flag(selector.randomize),
                                        .start = number(selector.start),
                                        .end = number(selector.end),
                                        .offset = number(selector.offset),
                                        .amount = std::clamp(number(selector.amount), -100.0, 100.0),
                                        .easeHigh = std::clamp(number(selector.easeHigh), -100.0, 100.0),
                                        .easeLow = std::clamp(number(selector.easeLow), -100.0, 100.0),
                                        .seed = number(selector.seed)});
        values.opacity = std::clamp(number(animator.opacity), 0.0, 1.0);
        values.position = vector(animator.position);
        values.scale = vector(animator.scale);
        values.rotation = number(animator.rotation);
        values.skew = std::clamp(number(animator.skew), -85.0, 85.0);
        values.fill = animator.fill;
        values.fillColor = colorOf(animator.fillColor);
        values.outline = animator.outline;
        values.outlineColor = colorOf(animator.outlineColor);
        values.outlineWidth = number(animator.outlineWidth);
        values.tracking = number(animator.tracking);
        values.revealBy = choice(animator.revealBy, 1);
        values.revealUnits = choice(animator.revealUnits, 3);
        values.reveal = number(animator.reveal);
        values.cursor = animator.cursor;
        if (animator.cursor) {
            const std::string_view bytes = animator.cursorCharacter;
            for (std::size_t offset = 0; offset < bytes.size();) {
                const auto scalar = core::decodeUtf8Scalar(bytes, offset);
                if (scalar.length == 0)
                    break;
                if (scalar.value != U'\r' && scalar.value != U'\n')
                    values.cursorCharacter.push_back(scalar.value);
                offset += scalar.length;
            }
            values.cursorShown = !values.cursorCharacter.empty() &&
                                 textCursorShown(number(animator.cursorBlink), time.toSeconds());
        }
        result.push_back(std::move(values));
    }
    if (!ok)
        return std::nullopt;
    return result;
}

void addAnimatorToKey(OperationKey& key, const ResolvedTextAnimator& animator) {
    key.add(animator.selectors.size());
    for (const auto& selector : animator.selectors) {
        for (const auto value : {selector.mode, selector.units, selector.rangeBy, selector.shape})
            key.add(value);
        key.add(selector.randomize);
        for (const auto value : {selector.start, selector.end, selector.offset, selector.amount,
                                 selector.easeHigh, selector.easeLow,
                                 selector.randomize ? selector.seed : 0.0})
            key.add(value);
    }
    for (const auto value : {animator.opacity, animator.position.x, animator.position.y,
                             animator.scale.x, animator.scale.y, animator.rotation, animator.skew,
                             animator.outlineWidth, animator.tracking, animator.reveal})
        key.add(value);
    key.add(animator.fill);
    if (animator.fill)
        key.add(animator.fillColor);
    key.add(animator.outline);
    if (animator.outline)
        key.add(animator.outlineColor);
    key.add(animator.revealBy);
    key.add(animator.revealUnits);
    key.add(animator.cursor && animator.cursorShown);
    if (animator.cursor && animator.cursorShown)
        for (const auto scalar : animator.cursorCharacter)
            key.add(static_cast<std::uint32_t>(scalar));
}

// #758: one animated layer: its paths, the outline width it strokes (0 for a fill) and its
// straight color, opacity included.
struct AnimatedTextLayer final {
    std::vector<render::Path> paths;
    double outline = 0.0;
    core::Color4d color{};
};

[[nodiscard]] render::PathPoint mapClusterPoint(const render::PathPoint point,
                                                const std::array<double, 6>& m) noexcept {
    return {m[0] * point.x + m[2] * point.y + m[4], m[1] * point.x + m[3] * point.y + m[5]};
}

void mapClusterPaths(std::vector<render::Path>& paths, const std::array<double, 6>& m) {
    for (auto& path : paths)
        for (auto& anchor : path.anchors) {
            anchor.point = mapClusterPoint(anchor.point, m);
            if (anchor.inHandle)
                anchor.inHandle = mapClusterPoint(*anchor.inHandle, m);
            if (anchor.outHandle)
                anchor.outHandle = mapClusterPoint(*anchor.outHandle, m);
        }
}

[[nodiscard]] std::array<std::uint64_t, 5> layerIdentity(const double outline,
                                                         const core::Color4d& color) noexcept {
    return {std::bit_cast<std::uint64_t>(outline), std::bit_cast<std::uint64_t>(color.red),
            std::bit_cast<std::uint64_t>(color.green), std::bit_cast<std::uint64_t>(color.blue),
            std::bit_cast<std::uint64_t>(color.alpha)};
}

// #758: the per-cluster pass (docs/architecture/evaluation-primitives.md, "Text Animator"). Each
// animator in order gives every cluster its selectors' combined amount a, and blends its
// properties by it: opacity multiplies by 1 + a (O - 1), the offset adds a P, the scale multiplies
// by 1 + a (S - 1), rotation and skew add a R and a K, the fill and outline colors move a of the way
// (a clamped to [0, 1]) to the animator's when it changes them, the outline width adds a W and the
// tracking a T after the cluster. A cluster an animator's reveal has not reached draws nothing.
// The clusters then move by their line's tracking (realigned so the line keeps its alignment),
// turn, skew and scale about their baseline center, and are grouped into layers by outline width
// and color: the outlines first, then the fills, then each cursor.
template <typename PaintColor>
[[nodiscard]] std::variant<std::vector<AnimatedTextLayer>, std::string>
animateTextClusters(const StyledTextGeometry& geometry,
                    const std::vector<ResolvedTextAnimator>& animators,
                    const core::Color4d& fillColor, const core::Color4d& outlineColor,
                    const double outlineWidth, const std::int64_t alignment,
                    const PaintColor& paintColor, const CompiledSubtitlesFaces& faces,
                    const render::PathCancellation& cancel) {
    using Result = std::variant<std::vector<AnimatedTextLayer>, std::string>;
    const auto& records = geometry.clusters;
    const auto units = textClusterUnits(geometry.text, geometry.lineOf, geometry.lineCount);
    const auto count = records.size();
    std::vector<double> opacity(count, 1.0);
    std::vector<document::Vec2d> offset(count);
    std::vector<document::Vec2d> scale(count, document::Vec2d{1.0, 1.0});
    std::vector<double> rotation(count, 0.0);
    std::vector<double> skew(count, 0.0);
    std::vector<core::Color4d> fill(count);
    std::vector<core::Color4d> outline(count, outlineColor);
    std::vector<double> width(count, outlineWidth);
    std::vector<double> tracking(count, 0.0);
    std::vector<bool> shown(count, true);
    for (std::size_t index = 0; index < count; ++index) {
        const auto base = paintColor(records[index].paint);
        if (!base)
            return Result{std::string("A text run names no color")};
        fill[index] = *base;
    }
    const auto mix = [](const core::Color4d& from, const core::Color4d& to, const double t) {
        return core::Color4d{from.red + t * (to.red - from.red),
                             from.green + t * (to.green - from.green),
                             from.blue + t * (to.blue - from.blue),
                             from.alpha + t * (to.alpha - from.alpha)};
    };
    struct Cursor final {
        std::optional<std::size_t> after;
        std::u32string character;
    };
    std::vector<Cursor> cursors;
    for (const auto& animator : animators) {
        if (cancel && cancel())
            return Result{std::string{}};
        std::vector<double> amount(geometry.text.size(), animator.selectors.empty() ? 1.0 : 0.0);
        for (const auto& selector : animator.selectors)
            combineTextSelector(amount, textSelectorAmounts(selector, units), selector.mode);
        const auto visible =
            textRevealVisible(units, animator.revealUnits, animator.revealBy, animator.reveal);
        std::optional<std::size_t> lastShown;
        for (std::size_t index = 0; index < count; ++index) {
            const auto cluster = records[index].cluster;
            const auto a = cluster < amount.size() ? amount[cluster] : 0.0;
            opacity[index] *= std::clamp(1.0 + a * (animator.opacity - 1.0), 0.0, 1.0);
            offset[index].x += a * animator.position.x;
            offset[index].y += a * animator.position.y;
            scale[index].x *= 1.0 + a * (animator.scale.x - 1.0);
            scale[index].y *= 1.0 + a * (animator.scale.y - 1.0);
            rotation[index] += a * animator.rotation;
            skew[index] += a * animator.skew;
            const auto toward = std::clamp(a, 0.0, 1.0);
            if (animator.fill)
                fill[index] = mix(fill[index], animator.fillColor, toward);
            if (animator.outline)
                outline[index] = mix(outline[index], animator.outlineColor, toward);
            width[index] += a * animator.outlineWidth;
            tracking[index] += a * animator.tracking;
            const bool revealed = cluster < visible.size() && visible[cluster];
            shown[index] = shown[index] && revealed;
            if (revealed)
                lastShown = index;
        }
        if (animator.cursor && animator.cursorShown)
            cursors.push_back({lastShown, animator.cursorCharacter});
    }
    // Tracking: each cluster moves by the tracking of the clusters before it on its line, and the
    // line moves back by its alignment's share of what it grew.
    std::vector<double> shift(count, 0.0);
    for (std::size_t begin = 0; begin < count;) {
        auto end = begin;
        double grown = 0.0;
        while (end < count && records[end].line == records[begin].line) {
            shift[end] = grown;
            grown += tracking[end];
            ++end;
        }
        grown -= tracking[end - 1];
        const auto back = alignment == 1 ? grown / 2.0 : alignment == 2 ? grown : 0.0;
        for (auto index = begin; index < end; ++index)
            shift[index] -= back;
        begin = end;
    }
    std::vector<AnimatedTextLayer> outlines;
    std::vector<AnimatedTextLayer> fills;
    std::map<std::array<std::uint64_t, 5>, std::size_t> outlineOf;
    std::map<std::array<std::uint64_t, 5>, std::size_t> fillOf;
    const auto add = [](std::vector<AnimatedTextLayer>& layers,
                        std::map<std::array<std::uint64_t, 5>, std::size_t>& index,
                        const double stroke, const core::Color4d& color,
                        const std::vector<render::Path>& paths) {
        const auto identity = layerIdentity(stroke, color);
        auto found = index.find(identity);
        if (found == index.end()) {
            found = index.emplace(identity, layers.size()).first;
            layers.push_back({{}, stroke, color});
        }
        auto& target = layers[found->second].paths;
        target.insert(target.end(), paths.begin(), paths.end());
    };
    for (std::size_t index = 0; index < count; ++index) {
        if (cancel && cancel())
            return Result{std::string{}};
        if (!shown[index] || !(opacity[index] > 0.0))
            continue;
        const auto& record = records[index];
        auto paths = record.paths;
        const bool moved = shift[index] != 0.0 || offset[index].x != 0.0 ||
                           offset[index].y != 0.0 || scale[index].x != 1.0 ||
                           scale[index].y != 1.0 || rotation[index] != 0.0 || skew[index] != 0.0;
        if (moved) {
            // p' = A + o + R S K (p + t - A): t the tracking shift, A the baseline center, K the
            // skew (the top leans right for a positive angle), S the scale, R the turn (clockwise
            // on screen for a positive angle).
            const auto ax = record.x + record.advance / 2.0 + shift[index];
            const auto ay = record.baseline;
            const auto radians = rotation[index] * std::numbers::pi / 180.0;
            const auto cosine = std::cos(radians);
            const auto sine = std::sin(radians);
            const auto lean = -std::tan(skew[index] * std::numbers::pi / 180.0);
            // RSK as a 2x2: K = [1 lean; 0 1], S = diag(sx, sy), R = [c -s; s c].
            const auto sx = scale[index].x;
            const auto sy = scale[index].y;
            const auto k00 = sx;
            const auto k01 = sx * lean;
            const auto k11 = sy;
            const auto m00 = cosine * k00;
            const auto m01 = cosine * k01 - sine * k11;
            const auto m10 = sine * k00;
            const auto m11 = sine * k01 + cosine * k11;
            const auto px = shift[index] - ax;
            const auto py = -ay;
            const std::array<double, 6> m{m00, m10, m01, m11,
                                          ax + offset[index].x + m00 * px + m01 * py,
                                          ay + offset[index].y + m10 * px + m11 * py};
            mapClusterPaths(paths, m);
        }
        const auto strokeWidth = std::max(0.0, width[index]);
        if (strokeWidth > 0.0) {
            auto color = outline[index];
            color.alpha *= opacity[index];
            if (color.alpha > 0.0)
                add(outlines, outlineOf, strokeWidth, color, paths);
        }
        auto color = fill[index];
        color.alpha *= opacity[index];
        if (color.alpha > 0.0)
            add(fills, fillOf, 0.0, color, paths);
    }
    // Each cursor: its character after the last cluster its reveal shows (the line start when none
    // shows), in the text's fill color and first face.
    std::vector<AnimatedTextLayer> cursorLayers;
    for (const auto& cursor : cursors) {
        double x = 0.0;
        double baseline = geometry.top + geometry.ascent;
        if (cursor.after) {
            const auto& record = records[*cursor.after];
            x = record.x + record.advance + shift[*cursor.after];
            baseline = record.baseline;
        } else if (!geometry.lines.empty()) {
            x = geometry.lines.front().left;
            baseline = geometry.lines.front().top + geometry.ascent;
        }
        const std::vector<render::StyledRun> run{{cursor.character, 0, faces.oblique[0], 0, false}};
        const std::span<const render::TextFont> faceSpan(faces.faces);
        auto options = geometry.options;
        options.maxWidth = 0.0;
        options.alignment = render::TextAlignment::Left;
        const auto block = render::layoutStyledText(faceSpan, run, options, cancel);
        if (!block)
            return Result{cancel && cancel() ? std::string{}
                                             : std::string("The text cursor could not be laid out")};
        auto glyphs = render::styledTextOutlines(faceSpan, run, *block.value(), options, x,
                                                 baseline - block.value()->ascent, cancel);
        if (!glyphs)
            return Result{cancel && cancel() ? std::string{}
                                             : std::string("The text cursor could not be built")};
        AnimatedTextLayer layer{{}, 0.0, fillColor};
        for (auto& paint : *glyphs.value())
            layer.paths.insert(layer.paths.end(), paint.paths.begin(), paint.paths.end());
        if (!layer.paths.empty() && fillColor.alpha > 0.0)
            cursorLayers.push_back(std::move(layer));
    }
    std::vector<AnimatedTextLayer> layers = std::move(outlines);
    for (auto& layer : fills)
        layers.push_back(std::move(layer));
    for (auto& layer : cursorLayers)
        layers.push_back(std::move(layer));
    return Result{std::move(layers)};
}

[[nodiscard]] render::PathStrokeJoin joinOf(const std::int64_t stored) noexcept {
    return stored == document::kTextOutlineJoinMiter   ? render::PathStrokeJoin::Miter
           : stored == document::kTextOutlineJoinBevel ? render::PathStrokeJoin::Bevel
                                                       : render::PathStrokeJoin::Round;
}

} // namespace

std::variant<StyledTextGeometry, std::string>
layoutStyledTextSource(const StyledTextSourceInput& input, const render::PathCancellation& cancel) {
    using Result = std::variant<StyledTextGeometry, std::string>;
    if (input.faces == nullptr)
        return Result{std::string("Text faces are unavailable")};
    if (!std::isfinite(input.size) || !(input.size > 0.0) ||
        input.size > render::kMaximumTextPixelSize || !std::isfinite(input.lineHeight) ||
        !(input.lineHeight > 0.0) || !std::isfinite(input.letterSpacing) || input.alignment < 0 ||
        input.alignment > 2 || !std::isfinite(input.box.x) || !std::isfinite(input.box.y) ||
        input.box.x < 0.0 || input.box.y < 0.0)
        return Result{std::string("Text layout is invalid")};
    StyledTextGeometry geometry;
    auto runs = textMarkupRuns(input.content, input.markup, *input.faces);
    geometry.warnings = std::move(runs.warnings);
    geometry.literals = std::move(runs.literals);
    for (const auto& run : runs.runs)
        if (std::ranges::find(geometry.paints, run.paint) == geometry.paints.end())
            geometry.paints.push_back(run.paint);
    std::ranges::sort(geometry.paints);

    const auto alignment = static_cast<render::TextAlignment>(input.alignment);
    const bool hasBox = input.box.x > 0.0 && input.box.y > 0.0;
    render::StyledTextOptions options;
    options.horizontalSize = input.size;
    options.verticalSize = input.size;
    options.lineHeight = input.lineHeight;
    options.letterSpacing = input.letterSpacing;
    // The Text Source wraps only inside its box, greedily at the line break opportunities.
    options.maxWidth = input.wrap && hasBox ? input.box.x : 0.0;
    options.wrap = render::TextWrapMode::Greedy;
    options.alignment = alignment;
    options.fixedDigits = input.fixedDigits;
    const std::span<const render::TextFont> faces(input.faces->faces);
    auto laid = render::layoutStyledText(faces, runs.runs, options, cancel);
    if (!laid)
        return Result{cancel && cancel() ? std::string{}
                                         : std::string("The text could not be laid out")};
    auto block = std::move(*laid.value());
    geometry.glyphs = block.glyphCount;
    if (geometry.glyphs > kStyledTextGlyphLimit)
        return Result{"The text draws " + std::to_string(geometry.glyphs) + " glyphs; at most " +
                      std::to_string(kStyledTextGlyphLimit) +
                      " may with markup, an outline or a background"};
    geometry.lineStep = block.lineStep;

    // The Text Source's placement (text_raster.cpp): lines align inside the box, or inside the
    // widest line; with no box the anchor mode moves the block; inside a box the block sits at its
    // vertical alignment.
    const auto anchor = render::resolveTextAnchorMode(input.anchorMode, alignment);
    const double alignWidth = hasBox ? input.box.x : block.width;
    for (auto& line : block.lines) {
        const auto difference = alignWidth - line.width;
        line.offset = alignment == render::TextAlignment::Left     ? 0.0
                      : alignment == render::TextAlignment::Center ? difference / 2.0
                                                                   : difference;
        if (!hasBox && anchor != render::TextLayoutOptions::AnchorMode::Left)
            line.offset -= anchor == render::TextLayoutOptions::AnchorMode::Center
                               ? block.width / 2.0
                               : block.width;
    }
    const double natural = block.lineStep * static_cast<double>(block.lines.size());
    const double vertical = hasBox ? input.box.y - natural : 0.0;
    const double top = input.verticalAlignment == 1   ? vertical / 2.0
                       : input.verticalAlignment == 2 ? vertical
                                                      : 0.0;
    for (std::size_t index = 0; index < block.lines.size(); ++index)
        geometry.lines.push_back({block.lines[index].offset,
                                  top + static_cast<double>(index) * block.lineStep,
                                  block.lines[index].width});
    geometry.options = options;
    geometry.top = top;
    geometry.ascent = block.ascent;
    if (input.clusters) {
        // #758: the per-cluster record, and the fills grouped from it by paint in first-use order.
        for (const auto& run : runs.runs)
            for (const auto scalar : run.text)
                if (scalar != U'\r')
                    geometry.text.push_back(scalar);
        geometry.lineCount = block.lines.size();
        geometry.lineOf.assign(geometry.text.size(), 0);
        for (std::size_t line = 0; line < block.lines.size(); ++line) {
            const auto first = block.lines[line].firstCluster;
            const auto end = line + 1 < block.lines.size() ? block.lines[line + 1].firstCluster
                                                           : geometry.text.size();
            for (auto cluster = first; cluster < end && cluster < geometry.text.size(); ++cluster)
                geometry.lineOf[cluster] = line;
        }
        auto clusters =
            render::styledTextClusterOutlines(faces, runs.runs, block, options, 0.0, top, cancel);
        if (!clusters)
            return Result{cancel && cancel() ? std::string{}
                                             : std::string("The text glyphs could not be built")};
        geometry.clusters = std::move(*clusters.value());
        for (const auto& cluster : geometry.clusters) {
            auto found = std::ranges::find(geometry.fills, cluster.paint,
                                           &render::StyledPaintOutlines::paint);
            if (found == geometry.fills.end()) {
                geometry.fills.push_back({cluster.paint, {}});
                found = std::prev(geometry.fills.end());
            }
            found->paths.insert(found->paths.end(), cluster.paths.begin(), cluster.paths.end());
        }
        return Result{std::move(geometry)};
    }
    auto outlines = render::styledTextOutlines(faces, runs.runs, block, options, 0.0, top, cancel);
    if (!outlines)
        return Result{cancel && cancel() ? std::string{}
                                         : std::string("The text glyphs could not be built")};
    geometry.fills = std::move(*outlines.value());
    return Result{std::move(geometry)};
}

std::variant<SubtitlesFrame, SubtitlesFailure>
prepareStyledTextFrame(const CompiledText& text, const CompiledCompositionPlan& plan,
                       const ResolvedEvaluation& resolved, const core::RationalTime time,
                       const CancellationToken& cancellation) {
    using Result = std::variant<SubtitlesFrame, SubtitlesFailure>;
    const auto invalid = [](std::string message) -> Result {
        return SubtitlesFailure{SubtitlesFailureKind::Invalid, std::move(message)};
    };
    const auto cancelled = []() -> Result {
        return SubtitlesFailure{SubtitlesFailureKind::Cancelled, "Text preparation was cancelled"};
    };
    if (!text.style)
        return invalid("The text is not styled");
    const auto& style = *text.style;
    const auto size = resolveParameter(text.size, plan, resolved);
    const auto color = resolveParameter(text.color, plan, resolved);
    const auto content =
        resolveParameter(text.contentParameterId, text.content, text.drivenContent, resolved);
    const auto lineHeight = resolveParameter(text.layout.lineHeight, plan, resolved);
    const auto letterSpacing = resolveParameter(text.layout.letterSpacing, plan, resolved);
    const auto alignment = resolveParameter(text.layout.alignmentId, text.layout.alignment,
                                            text.layout.drivenAlignment, resolved);
    const auto outlineWidth = resolveParameter(style.outlineWidth, plan, resolved);
    const auto outlineColor = resolveParameter(style.outlineColor, plan, resolved);
    const auto backgroundColor = resolveParameter(style.backgroundColor, plan, resolved);
    const auto padding = resolveParameter(style.backgroundPadding, plan, resolved);
    const auto radius = resolveParameter(style.backgroundRadius, plan, resolved);
    if (!size || !color || !content || !lineHeight || !letterSpacing || !alignment ||
        !outlineWidth || !outlineColor || !backgroundColor || !padding || !radius)
        return invalid("Text styling could not be resolved");
    std::array<core::Color4d, document::kTextAccentCount> accents{};
    for (std::size_t index = 0; index < accents.size(); ++index) {
        const auto accent = resolveParameter(style.accents[index], plan, resolved);
        if (!accent)
            return invalid("Text accent colors could not be resolved");
        accents[index] = accent->value;
    }
    const double width = std::max(0.0, outlineWidth->value);
    const double pad = std::max(0.0, padding->value) * size->value;
    const double corner = std::max(0.0, radius->value);
    if (!std::isfinite(width) || !std::isfinite(pad) || !std::isfinite(corner))
        return invalid("Text styling is outside its domain");

    const auto cancel = [&cancellation] { return cancellation.isCancellationRequested(); };
    const StyledTextSourceInput input{.content = content->value,
                                      .markup = style.markup,
                                      .faces = &style.faces,
                                      .size = size->value,
                                      .lineHeight = lineHeight->value,
                                      .letterSpacing = letterSpacing->value,
                                      .alignment = alignment->value,
                                      .box = text.layout.box,
                                      .wrap = text.layout.wrap,
                                      .verticalAlignment = text.layout.verticalAlignment,
                                      .anchorMode = text.layout.anchorMode,
                                      .overflow = text.layout.overflow,
                                      .fixedDigits = style.fixedDigits,
                                      .clusters = !style.animators.empty()};
    auto laid = layoutStyledTextSource(input, cancel);
    if (auto* message = std::get_if<std::string>(&laid)) {
        if (message->empty())
            return cancelled();
        return SubtitlesFailure{message->starts_with("The text draws")
                                    ? SubtitlesFailureKind::Budget
                                    : SubtitlesFailureKind::Invalid,
                                std::move(*message)};
    }
    auto& geometry = std::get<StyledTextGeometry>(laid);
    const auto paintColor = [&](const std::uint32_t paint) -> std::optional<core::Color4d> {
        if (paint == kTextFillPaint)
            return color->value;
        if (paint >= kTextFirstAccentPaint && paint < kTextFirstLiteralPaint)
            return accents[paint - kTextFirstAccentPaint];
        const auto literal = static_cast<std::size_t>(paint - kTextFirstLiteralPaint);
        if (paint < kTextFirstLiteralPaint || literal >= geometry.literals.size())
            return std::nullopt;
        return geometry.literals[literal];
    };

    // The identity: what the layers are made from, and only the colors the runs use.
    const auto hScale = resolved.horizontalScale;
    const auto vScale = resolved.verticalScale;
    const bool hasBox = text.layout.box.x > 0.0 && text.layout.box.y > 0.0;
    const bool clips = hasBox && text.layout.overflow == 0;
    OperationKey key;
    key.add(std::string("text-styled-frame-v1"));
    key.add(hScale);
    key.add(vScale);
    for (const auto& face : style.faces.faces)
        addFontToKey(key, face);
    for (const auto oblique : style.faces.oblique)
        key.add(oblique);
    key.add(style.markup);
    key.add(content->value);
    for (const auto value : {size->value, lineHeight->value, letterSpacing->value})
        key.add(value);
    key.add(alignment->value);
    key.add(text.layout.box);
    key.add(text.layout.wrap);
    key.add(text.layout.verticalAlignment);
    key.add(text.layout.anchorMode);
    key.add(text.layout.overflow);
    key.add(width);
    if (width > 0.0) {
        key.add(outlineColor->value);
        key.add(style.outlineJoin);
    }
    key.add(style.background);
    if (style.background != document::kTextBackgroundOff) {
        key.add(backgroundColor->value);
        key.add(pad);
        key.add(corner);
    }
    for (const auto paint : geometry.paints) {
        const auto resolvedColor = paintColor(paint);
        if (!resolvedColor)
            return invalid("A text run names no color");
        key.add(paint);
        key.add(*resolvedColor);
    }
    // #758: fixed-width digits, and the chain's values at the frame (never the time: a blinking
    // cursor adds whether it shows).
    if (style.fixedDigits)
        key.add(std::string("fixed-digits"));
    auto animators = resolveTextAnimators(style.animators, plan, resolved, time);
    if (!animators)
        return invalid("Text Animator values could not be resolved");
    if (!animators->empty()) {
        key.add(std::string("text-animator-v1"));
        for (const auto& animator : *animators)
            addAnimatorToKey(key, animator);
    }
    SubtitlesFrame frame;
    frame.contentKey = key.digest();
    frame.glyphs = geometry.glyphs;

    // The box the text clips to: its text box, grown by what draws around the glyphs.
    std::optional<render::PathBounds> clip;
    if (clips) {
        const double margin =
            width + (style.background != document::kTextBackgroundOff ? pad : 0.0);
        clip = render::PathBounds{-margin, -margin, text.layout.box.x + margin,
                                  text.layout.box.y + margin};
    }
    std::optional<render::PathBounds> ink;
    // #758: while `measureOnly`, a layer adds its ink and is not kept (the still layout's bounds).
    bool measureOnly = false;
    const auto addLayer = [&](std::vector<render::Path> paths, const render::PathStroke stroke,
                              const core::Color4d& straight) -> bool {
        if (paths.empty() || !(straight.alpha > 0.0))
            return true;
        const auto pixel = render::solidPixelFromStraightLinearRec709Scene(straight);
        if (!pixel)
            return false;
        auto shared = std::make_shared<const std::vector<render::Path>>(std::move(paths));
        auto raster = clip ? render::PathRaster::transformed(
                                 std::span<const render::Path>(*shared), stroke,
                                 render::PathMatrix{1, 0, 0, 1, 0, 0}, hScale, vScale, cancel, clip)
                           : render::PathRaster::create(std::span<const render::Path>(*shared),
                                                        stroke, hScale, vScale, cancel);
        if (!raster)
            return false;
        const bool isStroke = stroke.width > 0.0;
        const auto bounds = raster.value()->bounds(!isStroke, isStroke);
        if (bounds.right > bounds.left && bounds.bottom > bounds.top)
            ink = !ink ? bounds
                       : render::PathBounds{std::min(ink->left, bounds.left),
                                            std::min(ink->top, bounds.top),
                                            std::max(ink->right, bounds.right),
                                            std::max(ink->bottom, bounds.bottom)};
        if (measureOnly)
            return true;
        frame.layers.push_back(
            {std::make_shared<const render::PathRaster>(std::move(*raster.value())),
             std::move(shared), stroke, isStroke, *pixel.value(), clip});
        return true;
    };
    const auto failed = [&](std::string message) {
        return cancel() ? cancelled() : invalid(std::move(message));
    };

    // The background box: per line around each line's advance, or one block around them all (the
    // text box itself when the text has one).
    std::vector<render::Path> boxes;
    if (style.background != document::kTextBackgroundOff && !geometry.lines.empty()) {
        const auto rounded = [&](const double left, const double top, const double right,
                                 const double bottom) {
            const auto w = right - left;
            const auto h = bottom - top;
            if (!(w > 0.0) || !(h > 0.0))
                return;
            boxes.push_back(translated(
                render::rectanglePath(w, h, std::min(corner, std::min(w, h) / 2.0)), left, top));
        };
        if (style.background == document::kTextBackgroundBlock) {
            double left = std::numeric_limits<double>::infinity();
            double right = -std::numeric_limits<double>::infinity();
            for (const auto& line : geometry.lines)
                if (line.width > 0.0) {
                    left = std::min(left, line.left);
                    right = std::max(right, line.left + line.width);
                }
            double top = geometry.lines.front().top;
            double bottom = geometry.lines.back().top + geometry.lineStep;
            if (hasBox) {
                left = 0.0;
                right = text.layout.box.x;
                top = 0.0;
                bottom = text.layout.box.y;
            }
            if (right > left)
                rounded(left - pad, top - pad, right + pad, bottom + pad);
        } else {
            for (std::size_t index = 0; index < geometry.lines.size(); ++index) {
                const auto& line = geometry.lines[index];
                if (!(line.width > 0.0))
                    continue;
                const auto top = line.top - (index == 0 ? pad : 0.0);
                const auto bottom =
                    line.top + geometry.lineStep + (index + 1 == geometry.lines.size() ? pad : 0.0);
                rounded(line.left - pad, top, line.left + line.width + pad, bottom);
            }
        }
    }
    // The outline, under every fill: a stroke twice the width centred on the glyph edge, so the
    // width shows outside the glyphs, as Subtitles draws it.
    const auto join = joinOf(style.outlineJoin);
    const auto outlineStroke = [join](const double strokeWidth) {
        return render::PathStroke{2.0 * strokeWidth, render::PathStrokeAlign::Center, join,
                                  join == render::PathStrokeJoin::Round
                                      ? render::PathStrokeCap::Round
                                      : render::PathStrokeCap::Butt};
    };
    // The still layers (a text with no animator draws exactly these): box, outline, fills.
    const auto stillLayers = [&]() -> std::optional<Result> {
        if (!addLayer(boxes, {}, backgroundColor->value))
            return failed("The text background could not be built");
        if (width > 0.0) {
            std::vector<render::Path> all;
            for (const auto& paint : geometry.fills)
                all.insert(all.end(), paint.paths.begin(), paint.paths.end());
            if (!addLayer(std::move(all), outlineStroke(width), outlineColor->value))
                return failed("The text outline could not be built");
        }
        for (const auto& paint : geometry.fills) {
            const auto resolvedColor = paintColor(paint.paint);
            if (!resolvedColor)
                return invalid("A text run names no color");
            if (!addLayer(paint.paths, {}, *resolvedColor))
                return failed("The text fill could not be built");
        }
        return std::nullopt;
    };
    std::optional<render::PathBounds> stillInk;
    if (animators->empty()) {
        if (auto failure = stillLayers())
            return std::move(*failure);
    } else {
        // #758: the still layout's ink places the text (its bounds), so nothing a selector moves,
        // scales or reveals moves the layer; the animated layers are what draws.
        measureOnly = true;
        if (auto failure = stillLayers())
            return std::move(*failure);
        stillInk = ink;
        ink.reset();
        measureOnly = false;
        const auto background = backgroundColor->value;
        auto animated = animateTextClusters(geometry, *animators, color->value,
                                            outlineColor->value, width, alignment->value,
                                            paintColor, style.faces, cancel);
        if (auto* message = std::get_if<std::string>(&animated)) {
            if (message->empty())
                return cancelled();
            return SubtitlesFailure{message->starts_with("The animated text draws")
                                        ? SubtitlesFailureKind::Budget
                                        : SubtitlesFailureKind::Invalid,
                                    std::move(*message)};
        }
        auto& layers = std::get<std::vector<AnimatedTextLayer>>(animated);
        // #758: the layers are per-cluster paints, drawn by one painted coverage pass however
        // many there are; only the glyph limit bounds them.
        frame.painted = true;
        if (!addLayer(boxes, {}, background))
            return failed("The text background could not be built");
        for (auto& layer : layers)
            if (!addLayer(std::move(layer.paths),
                          layer.outline > 0.0 ? outlineStroke(layer.outline) : render::PathStroke{},
                          layer.color))
                return failed("The animated text could not be built");
    }
    if (cancel())
        return cancelled();

    // The window: the ink, or the clip box (Clip) and the ink with the text box (Grow), as the
    // plain Text Source's window is its box.
    const auto windowOf = [&](const std::optional<render::PathBounds>& drawn)
        -> std::optional<std::optional<render::ImageWindow>> {
        std::optional<render::PathBounds> area = drawn;
        if (hasBox) {
            const render::PathBounds boxArea =
                clip ? *clip : render::PathBounds{0.0, 0.0, text.layout.box.x, text.layout.box.y};
            const auto scaled = render::PathBounds{boxArea.left * hScale, boxArea.top * vScale,
                                                   boxArea.right * hScale, boxArea.bottom * vScale};
            if (clip)
                area = scaled;
            else if (area)
                area = render::PathBounds{
                    std::min(area->left, scaled.left), std::min(area->top, scaled.top),
                    std::max(area->right, scaled.right), std::max(area->bottom, scaled.bottom)};
            else
                area = scaled;
        }
        if (!area)
            return std::optional<render::ImageWindow>{};
        const auto left = std::floor(area->left);
        const auto top = std::floor(area->top);
        const auto right = std::ceil(area->right);
        const auto bottom = std::ceil(area->bottom);
        constexpr double kBound = static_cast<double>(std::int64_t{1} << 30);
        if (!(right > left) || !(bottom > top) || std::abs(left) > kBound ||
            std::abs(top) > kBound || std::abs(right) > kBound || std::abs(bottom) > kBound)
            return std::nullopt;
        const auto window = render::ImageWindow::create(
            static_cast<std::int64_t>(left), static_cast<std::int64_t>(top),
            static_cast<std::uint64_t>(right - left), static_cast<std::uint64_t>(bottom - top));
        if (!window)
            return std::nullopt;
        return std::optional<render::ImageWindow>{*window.value()};
    };
    const auto drawnWindow = windowOf(ink);
    if (!drawnWindow)
        return invalid("The text window is invalid");
    frame.window = *drawnWindow;
    // #758: an animated text is placed by its still layout's window; it draws its animated ink.
    const auto placedWindow = animators->empty() ? drawnWindow : windowOf(stillInk);
    if (!placedWindow)
        return invalid("The text window is invalid");
    if (*placedWindow) {
        frame.bounds = boundsForWindow(**placedWindow, hScale, vScale);
        frame.bounds.pivotX = textBlockPivotX(text.layout, alignment->value);
    }
    // An empty text in a box keeps the box as its bounds, as the plain Text Source does, and
    // draws nothing.
    if (frame.layers.empty())
        frame.window.reset();
    if (!frame.window)
        frame.layers.clear();
    return frame;
}

} // namespace bloom::runtime::detail
