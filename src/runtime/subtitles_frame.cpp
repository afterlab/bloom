// #752: a Subtitles node's frame (subtitles_frame.hpp).
#include "subtitles_frame.hpp"

#include "cpu_composition_resolution.hpp"
#include "operation_key.hpp"

#include <bloom/document/subtitles_nodes.hpp>
#include <bloom/render/cpu_image_primitives.hpp>
#include <bloom/runtime/subtitles_layout.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <type_traits>
#include <utility>

namespace bloom::runtime::detail {
namespace {

[[nodiscard]] std::string timestampOf(const core::RationalTime time) {
    const auto milliseconds = static_cast<std::int64_t>(std::floor(time.toSeconds() * 1000.0));
    const auto hours = milliseconds / 3600000;
    const auto minutes = milliseconds / 60000 % 60;
    const auto seconds = milliseconds / 1000 % 60;
    const auto rest = milliseconds % 1000;
    const auto two = [](const std::int64_t value) {
        return (value < 10 ? "0" : "") + std::to_string(value);
    };
    auto fraction = std::to_string(rest);
    fraction.insert(0, 3 - fraction.size(), '0');
    return two(hours) + ":" + two(minutes) + ":" + two(seconds) + "." + fraction;
}

[[nodiscard]] std::string cueList(const document::CueListValue& list,
                                  const std::vector<std::size_t>& active) {
    std::string out;
    for (const auto index : active) {
        if (!out.empty())
            out += ", ";
        out += std::to_string(list.cues[index]->id);
    }
    return out;
}

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

void addCueToKey(OperationKey& key, const document::Cue& cue, const bool speaker) {
    key.add(cue.text);
    key.add(cue.style);
    key.add(speaker ? cue.speaker : std::string{});
    const auto& placement = cue.placement;
    key.add(placement.anchor.has_value());
    if (placement.anchor)
        key.add(*placement.anchor);
    for (const auto& value : {placement.positionX, placement.positionY, placement.width}) {
        key.add(value.has_value());
        if (value)
            key.add(*value);
    }
    key.add(placement.lineOffset.has_value());
    if (placement.lineOffset)
        key.add(*placement.lineOffset);
    key.add(placement.textAlign.has_value());
    if (placement.textAlign)
        key.add(*placement.textAlign);
}

// The fade multiplier of a cue at `time`: 1 outside its fades.
[[nodiscard]] double fadeOf(const CompiledSubtitlesSettings& settings, const document::Cue& cue,
                            const core::RationalTime time) {
    const double now = time.toSeconds();
    double alpha = 1.0;
    if (settings.fadeIn > 0.0) {
        const double into = now - cue.start.toSeconds();
        if (into < settings.fadeIn)
            alpha = std::min(alpha, std::max(0.0, into / settings.fadeIn));
    }
    if (settings.fadeOut > 0.0) {
        const double left = cue.end.toSeconds() - now;
        if (left < settings.fadeOut)
            alpha = std::min(alpha, std::max(0.0, left / settings.fadeOut));
    }
    return alpha;
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

} // namespace

std::variant<SubtitlesFrame, SubtitlesFailure>
prepareSubtitlesFrame(const CompiledSubtitles& subtitles, const CompiledCompositionPlan& plan,
                      const ResolvedEvaluation& resolved, const core::RationalTime time,
                      const render::ImageWindow frameWindow,
                      const CancellationToken& cancellation) {
    using Result = std::variant<SubtitlesFrame, SubtitlesFailure>;
    const auto invalid = [](std::string message) -> Result {
        return SubtitlesFailure{SubtitlesFailureKind::Invalid, std::move(message)};
    };
    const auto cancelled = []() -> Result {
        return SubtitlesFailure{SubtitlesFailureKind::Cancelled, "Subtitles preparation was cancelled"};
    };
    const auto& settings = subtitles.settings;
    SubtitlesFrame frame;
    frame.bounds = {0.0, 0.0, settings.frameWidth, settings.frameHeight};

    // The cue list this frame reads.
    const CompiledCueList* list = &subtitles.cues;
    if (subtitles.drivenCues) {
        list = resolvedValue<CompiledCueList>(*subtitles.drivenCues, resolved);
        if (list == nullptr)
            return invalid("Subtitles cues could not be resolved");
    }
    const auto active = list->activeAt(time);
    if (active.size() > kSubtitlesActiveCueLimit)
        return SubtitlesFailure{
            SubtitlesFailureKind::Budget,
            "At " + timestampOf(time) + " " + std::to_string(active.size()) +
                " cues show at once (cues " + cueList(*list->list, active) + "); at most " +
                std::to_string(kSubtitlesActiveCueLimit) + " may"};

    OperationKey key;
    key.add(std::string("subtitles-frame-v1"));
    key.add(settings.burnIn);
    if (!settings.burnIn || active.empty()) {
        frame.contentKey = key.digest();
        return frame;
    }

    // The styling at this frame.
    const auto size = resolveParameter(subtitles.size, plan, resolved);
    const auto outlineWidth = resolveParameter(subtitles.outlineWidth, plan, resolved);
    const auto lineHeight = resolveParameter(subtitles.lineHeight, plan, resolved);
    const auto letterSpacing = resolveParameter(subtitles.letterSpacing, plan, resolved);
    const auto opacity = resolveParameter(subtitles.opacity, plan, resolved);
    const auto fill = resolveParameter(subtitles.fill, plan, resolved);
    const auto outlineColor = resolveParameter(subtitles.outlineColor, plan, resolved);
    const auto boxColor = resolveParameter(subtitles.boxColor, plan, resolved);
    const auto offset = resolveParameter(subtitles.offset, plan, resolved);
    if (!size || !outlineWidth || !lineHeight || !letterSpacing || !opacity || !fill ||
        !outlineColor || !boxColor || !offset)
        return invalid("Subtitles styling could not be resolved");
    SubtitlesStyleValues style;
    style.size = size->value;
    style.outlineWidth = outlineWidth->value;
    style.lineHeight = lineHeight->value;
    style.letterSpacing = letterSpacing->value;
    style.opacity = std::clamp(opacity->value, 0.0, 1.0);
    style.fill = fill->value;
    style.outlineColor = outlineColor->value;
    style.boxColor = boxColor->value;
    style.offset = offset->value;
    if (!std::isfinite(style.size) || !(style.size > 0.0) || !std::isfinite(style.outlineWidth) ||
        !std::isfinite(style.lineHeight) || !(style.lineHeight > 0.0) ||
        !std::isfinite(style.letterSpacing) || !std::isfinite(style.offset.x) ||
        !std::isfinite(style.offset.y))
        return invalid("Subtitles styling is outside its domain");

    const auto cancel = [&cancellation] { return cancellation.isCancellationRequested(); };
    auto laid = layoutSubtitleCues(subtitles, *list->list, active, style, cancel);
    if (auto* message = std::get_if<std::string>(&laid))
        return message->empty() ? cancelled() : invalid(std::move(*message));
    auto& layouts = std::get<std::vector<SubtitleCueLayout>>(laid);
    std::size_t glyphs = 0;
    for (const auto& layout : layouts)
        glyphs += layout.block.glyphCount;
    if (glyphs > kSubtitlesGlyphLimit)
        return SubtitlesFailure{SubtitlesFailureKind::Budget,
                                "At " + timestampOf(time) + " the subtitles draw " +
                                    std::to_string(glyphs) + " glyphs (cues " +
                                    cueList(*list->list, active) + "); at most " +
                                    std::to_string(kSubtitlesGlyphLimit) + " may"};
    frame.glyphs = glyphs;

    // The identity: what the layers are built from, never the time or the ids.
    key.add(resolved.horizontalScale);
    key.add(resolved.verticalScale);
    key.add(frameWindow.originX());
    key.add(frameWindow.originY());
    key.add(frameWindow.extent().width());
    key.add(frameWindow.extent().height());
    for (const auto& face : subtitles.faces.faces)
        addFontToKey(key, face);
    for (const auto oblique : subtitles.faces.oblique)
        key.add(oblique);
    key.add(settings.box);
    key.add(settings.boxPadding);
    key.add(settings.boxRadius);
    key.add(settings.safeArea);
    key.add(settings.marginX);
    key.add(settings.marginY);
    key.add(settings.anchor);
    key.add(settings.maxWidth);
    key.add(settings.wrap);
    key.add(settings.maxLines);
    key.add(settings.overflow);
    key.add(settings.textAlign);
    key.add(settings.useCuePositions);
    key.add(settings.useFileStyles);
    key.add(settings.showSpeaker);
    key.add(settings.frameWidth);
    key.add(settings.frameHeight);
    key.add(settings.titleSafe);
    key.add(settings.actionSafe);
    for (const auto value : {style.size, style.outlineWidth, style.lineHeight, style.letterSpacing,
                             style.opacity, style.offset.x, style.offset.y})
        key.add(value);
    key.add(style.fill);
    key.add(style.outlineColor);
    key.add(style.boxColor);
    // The list's styles reach the picture through the cues that name them.
    key.add(settings.useFileStyles ? list->list->styles.size() : std::size_t{0});
    if (settings.useFileStyles)
        for (const auto& named : list->list->styles) {
            key.add(named.name);
            for (const auto& color : {named.color, named.backgroundColor, named.outlineColor}) {
                key.add(color.has_value());
                if (color)
                    key.add(*color);
            }
            for (const auto& value : {named.outlineWidth, named.fontSize}) {
                key.add(value.has_value());
                if (value)
                    key.add(*value);
            }
            for (const auto& value : {named.bold, named.italic, named.underline}) {
                key.add(value.has_value());
                if (value)
                    key.add(*value);
            }
        }
    const bool speaker = settings.showSpeaker == document::kSubtitlesSpeakerPrefix;
    key.add(active.size());
    std::vector<double> fades;
    for (const auto index : active) {
        const auto& cue = *list->list->cues[index];
        addCueToKey(key, cue, speaker);
        const auto fade = fadeOf(settings, cue, time);
        // The fade phase, only while a fade runs: a held cue keys alike on every frame.
        key.add(fade < 1.0);
        if (fade < 1.0)
            key.add(fade);
        fades.push_back(fade);
    }
    frame.contentKey = key.digest();

    // The layers: per cue in stacking order, its box, its outline, then its fills by paint.
    const auto hScale = resolved.horizontalScale;
    const auto vScale = resolved.verticalScale;
    std::optional<render::PathBounds> ink;
    const auto addLayer = [&](std::vector<render::Path> paths, const render::PathStroke stroke,
                              const core::Color4d& color, const double alpha) -> bool {
        if (paths.empty())
            return true;
        auto straight = color;
        straight.alpha = std::clamp(straight.alpha * alpha, 0.0, 1.0);
        if (straight.alpha <= 0.0)
            return true;
        const auto pixel = render::solidPixelFromStraightLinearRec709Scene(straight);
        if (!pixel)
            return false;
        auto raster = render::PathRaster::create(std::span<const render::Path>(paths), stroke,
                                                 hScale, vScale, cancel);
        if (!raster)
            return false;
        const bool isStroke = stroke.width > 0.0;
        const auto bounds = raster.value()->bounds(!isStroke, isStroke);
        if (bounds.right > bounds.left && bounds.bottom > bounds.top) {
            if (!ink)
                ink = bounds;
            else
                ink = render::PathBounds{std::min(ink->left, bounds.left),
                                         std::min(ink->top, bounds.top),
                                         std::max(ink->right, bounds.right),
                                         std::max(ink->bottom, bounds.bottom)};
        }
        frame.layers.push_back(
            {std::make_shared<const render::PathRaster>(std::move(*raster.value())), isStroke,
             *pixel.value()});
        return true;
    };
    const std::span<const render::TextFont> faces(subtitles.faces.faces);
    for (std::size_t cueIndex = 0; cueIndex < layouts.size(); ++cueIndex) {
        if (cancel())
            return cancelled();
        const auto& layout = layouts[cueIndex];
        const auto alpha = style.opacity * fades[cueIndex];
        const auto& block = layout.block;
        SubtitlesPlacedCue placed;
        placed.index = layout.index;
        placed.id = layout.id;
        placed.block = {layout.x - layout.padding, layout.y - layout.padding,
                        layout.x + block.width + layout.padding,
                        layout.y + block.height + layout.padding};
        placed.lines = block.lines.size();
        placed.size = layout.options.verticalSize;
        placed.shrunk = layout.shrunk;
        frame.cues.push_back(placed);
        // The box.
        if (settings.box != document::kSubtitlesBoxOff && !block.lines.empty()) {
            std::vector<render::Path> boxes;
            const auto radius = std::max(0.0, settings.boxRadius) * layout.options.verticalSize;
            if (settings.box == document::kSubtitlesBoxBlock) {
                const auto width = block.width + 2.0 * layout.padding;
                const auto height = block.height + 2.0 * layout.padding;
                boxes.push_back(translated(
                    render::rectanglePath(width, height, std::min(radius, std::min(width, height) / 2.0)),
                    placed.block.left, placed.block.top));
            } else {
                for (std::size_t line = 0; line < block.lines.size(); ++line) {
                    const auto& row = block.lines[line];
                    if (!(row.width > 0.0))
                        continue;
                    const auto top = layout.y + static_cast<double>(line) * block.lineStep -
                                     (line == 0 ? layout.padding : 0.0);
                    const auto bottom = layout.y + static_cast<double>(line + 1) * block.lineStep +
                                        (line + 1 == block.lines.size() ? layout.padding : 0.0);
                    const auto width = row.width + 2.0 * layout.padding;
                    const auto height = bottom - top;
                    boxes.push_back(translated(
                        render::rectanglePath(width, height,
                                              std::min(radius, std::min(width, height) / 2.0)),
                        layout.x + row.offset - layout.padding, top));
                }
            }
            if (!addLayer(std::move(boxes), {}, layout.boxColor, alpha))
                return cancel() ? cancelled() : invalid("The subtitle box could not be built");
        }
        auto outlines = render::styledTextOutlines(faces, layout.runs, block, layout.options,
                                                   layout.x, layout.y, cancel);
        if (!outlines)
            return cancel() ? cancelled() : invalid("The subtitle glyphs could not be built");
        // The outline, under every fill of the cue: a stroke twice the width, centred on the glyph
        // edge, so the width shows outside the glyph.
        if (layout.outlineWidth > 0.0) {
            std::vector<render::Path> all;
            for (const auto& paint : *outlines.value())
                all.insert(all.end(), paint.paths.begin(), paint.paths.end());
            const render::PathStroke stroke{2.0 * layout.outlineWidth,
                                            render::PathStrokeAlign::Center,
                                            render::PathStrokeJoin::Round,
                                            render::PathStrokeCap::Round};
            if (!addLayer(std::move(all), stroke, layout.outlineColor, alpha))
                return cancel() ? cancelled() : invalid("The subtitle outline could not be built");
        }
        for (auto& paint : *outlines.value()) {
            if (paint.paint >= layout.paints.size())
                return invalid("A subtitle run names no color");
            if (!addLayer(std::move(paint.paths), {}, layout.paints[paint.paint], alpha))
                return cancel() ? cancelled() : invalid("The subtitle fill could not be built");
        }
    }
    if (ink) {
        // The proxy pixels the ink touches, inside the frame.
        const auto left = std::max(std::floor(ink->left * hScale),
                                   static_cast<double>(frameWindow.originX()));
        const auto top = std::max(std::floor(ink->top * vScale),
                                  static_cast<double>(frameWindow.originY()));
        const auto right = std::min(std::ceil(ink->right * hScale),
                                    static_cast<double>(frameWindow.maxXExclusive()));
        const auto bottom = std::min(std::ceil(ink->bottom * vScale),
                                     static_cast<double>(frameWindow.maxYExclusive()));
        if (right > left && bottom > top) {
            const auto window = render::ImageWindow::create(
                static_cast<std::int64_t>(left), static_cast<std::int64_t>(top),
                static_cast<std::uint64_t>(right - left), static_cast<std::uint64_t>(bottom - top));
            if (!window)
                return invalid("The subtitles window is invalid");
            frame.window = *window.value();
        }
    }
    if (!frame.window)
        frame.layers.clear();
    return frame;
}

} // namespace bloom::runtime::detail
