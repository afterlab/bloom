// #757: a styled Text Source's frame (text_styled_frame.hpp).
#include "text_styled_frame.hpp"

#include "cpu_composition_resolution.hpp"
#include "operation_key.hpp"

#include <bloom/document/text_styling.hpp>
#include <bloom/render/cpu_image_primitives.hpp>
#include <bloom/render/text_raster.hpp>
#include <bloom/runtime/text_markup.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
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
    auto outlines = render::styledTextOutlines(faces, runs.runs, block, options, 0.0, top, cancel);
    if (!outlines)
        return Result{cancel && cancel() ? std::string{}
                                         : std::string("The text glyphs could not be built")};
    geometry.fills = std::move(*outlines.value());
    return Result{std::move(geometry)};
}

std::variant<SubtitlesFrame, SubtitlesFailure>
prepareStyledTextFrame(const CompiledText& text, const CompiledCompositionPlan& plan,
                       const ResolvedEvaluation& resolved, const CancellationToken& cancellation) {
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
                                      .overflow = text.layout.overflow};
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
    if (style.background != document::kTextBackgroundOff && !geometry.lines.empty()) {
        std::vector<render::Path> boxes;
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
        if (!addLayer(std::move(boxes), {}, backgroundColor->value))
            return failed("The text background could not be built");
    }
    // The outline, under every fill: a stroke twice the width centred on the glyph edge, so the
    // width shows outside the glyphs, as Subtitles draws it.
    if (width > 0.0) {
        std::vector<render::Path> all;
        for (const auto& paint : geometry.fills)
            all.insert(all.end(), paint.paths.begin(), paint.paths.end());
        const auto join = joinOf(style.outlineJoin);
        const render::PathStroke stroke{2.0 * width, render::PathStrokeAlign::Center, join,
                                        join == render::PathStrokeJoin::Round
                                            ? render::PathStrokeCap::Round
                                            : render::PathStrokeCap::Butt};
        if (!addLayer(std::move(all), stroke, outlineColor->value))
            return failed("The text outline could not be built");
    }
    for (auto& paint : geometry.fills) {
        const auto resolvedColor = paintColor(paint.paint);
        if (!resolvedColor)
            return invalid("A text run names no color");
        if (!addLayer(std::move(paint.paths), {}, *resolvedColor))
            return failed("The text fill could not be built");
    }
    if (cancel())
        return cancelled();

    // The window: the ink, or the clip box (Clip) and the ink with the text box (Grow), as the
    // plain Text Source's window is its box.
    std::optional<render::PathBounds> area = ink;
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
    if (area) {
        const auto left = std::floor(area->left);
        const auto top = std::floor(area->top);
        const auto right = std::ceil(area->right);
        const auto bottom = std::ceil(area->bottom);
        constexpr double kBound = static_cast<double>(std::int64_t{1} << 30);
        if (!(right > left) || !(bottom > top) || std::abs(left) > kBound ||
            std::abs(top) > kBound || std::abs(right) > kBound || std::abs(bottom) > kBound)
            return invalid("The text window is invalid");
        const auto window = render::ImageWindow::create(
            static_cast<std::int64_t>(left), static_cast<std::int64_t>(top),
            static_cast<std::uint64_t>(right - left), static_cast<std::uint64_t>(bottom - top));
        if (!window)
            return invalid("The text window is invalid");
        frame.window = *window.value();
        frame.bounds = boundsForWindow(*frame.window, hScale, vScale);
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
