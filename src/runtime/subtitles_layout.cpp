// #752: the layout and checks of a Subtitles node's cues (subtitles_layout.hpp).
#include <bloom/runtime/subtitles_layout.hpp>

#include <bloom/core/utf8.hpp>
#include <bloom/document/cue_markup.hpp>
#include <bloom/document/subtitles_nodes.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <string_view>
#include <utility>

namespace bloom::runtime {
namespace {

namespace markup = document::cue_markup;

// A file's color is a straight sRGB-encoded display value; the render works in linear values.
[[nodiscard]] double decodeSrgb(const double encoded) noexcept {
    return encoded <= 0.04045 ? encoded / 12.92 : std::pow((encoded + 0.055) / 1.055, 2.4);
}
[[nodiscard]] core::Color4d linearFromFile(const core::Color4d& color) noexcept {
    return {decodeSrgb(color.red), decodeSrgb(color.green), decodeSrgb(color.blue), color.alpha};
}

// The WebVTT default color classes (W3C WebVTT, "Default classes"), sRGB.
struct ColorClass final {
    std::string_view name;
    core::Color4d color;
};
constexpr std::array<ColorClass, 8> kColorClasses{{{"white", {1, 1, 1, 1}},
                                                   {"lime", {0, 1, 0, 1}},
                                                   {"cyan", {0, 1, 1, 1}},
                                                   {"red", {1, 0, 0, 1}},
                                                   {"yellow", {1, 1, 0, 1}},
                                                   {"magenta", {1, 0, 1, 1}},
                                                   {"blue", {0, 0, 1, 1}},
                                                   {"black", {0, 0, 0, 1}}}};

[[nodiscard]] std::optional<core::Color4d> colorClass(const std::string_view name) {
    for (const auto& entry : kColorClasses)
        if (entry.name == name)
            return linearFromFile(entry.color);
    return std::nullopt;
}

void appendUtf8(std::u32string& out, const std::string_view text) {
    for (std::size_t cursor = 0; cursor < text.size();) {
        const auto scalar = core::decodeUtf8Scalar(text, cursor);
        if (!scalar.isValid() || scalar.length == 0)
            return;
        cursor += scalar.length;
        out.push_back(scalar.value);
    }
}

struct RunState final {
    bool bold = false;
    bool italic = false;
    bool underline = false;
    std::uint32_t paint = 0;
};

class RunBuilder final {
  public:
    RunBuilder(const CompiledSubtitles& subtitles, const document::CueListValue& list,
               std::vector<core::Color4d>& paints, core::Color4d& boxColor)
        : subtitles_(subtitles), list_(list), paints_(paints), boxColor_(boxColor) {}

    void text(const std::u32string& value, const RunState& state) {
        if (value.empty())
            return;
        const auto face = static_cast<std::size_t>((state.bold ? 1 : 0) + (state.italic ? 2 : 0));
        render::StyledRun run{value, face, subtitles_.faces.oblique[face], state.paint,
                              state.underline};
        if (!runs_.empty() && runs_.back().face == run.face &&
            runs_.back().oblique == run.oblique && runs_.back().paint == run.paint &&
            runs_.back().underline == run.underline) {
            runs_.back().text += run.text;
            return;
        }
        runs_.push_back(std::move(run));
    }

    void apply(const document::CueStyle& style, RunState& state) {
        if (style.color)
            state.paint = paint(linearFromFile(*style.color));
        if (style.bold)
            state.bold = *style.bold;
        if (style.italic)
            state.italic = *style.italic;
        if (style.underline)
            state.underline = *style.underline;
        if (style.backgroundColor)
            boxColor_ = linearFromFile(*style.backgroundColor);
    }

    void walk(const std::vector<markup::Node>& nodes, const RunState& state) {
        for (const auto& node : nodes) {
            auto inner = state;
            switch (node.kind) {
            case markup::NodeKind::Text: {
                std::u32string scalars;
                appendUtf8(scalars, node.text);
                text(scalars, state);
                continue;
            }
            case markup::NodeKind::Italic:
                inner.italic = true;
                break;
            case markup::NodeKind::Bold:
                inner.bold = true;
                break;
            case markup::NodeKind::Underline:
                inner.underline = true;
                break;
            case markup::NodeKind::RubyText:
            case markup::NodeKind::Timestamp:
                continue; // ruby text is kept, the base drawn; a timestamp draws nothing
            case markup::NodeKind::Class:
            case markup::NodeKind::Ruby:
            case markup::NodeKind::Voice:
            case markup::NodeKind::Language:
                break;
            }
            if (subtitles_.settings.useFileStyles) {
                for (const auto& name : node.classes) {
                    if (const auto* style = list_.findStyle(name))
                        apply(*style, inner);
                    else if (const auto color = colorClass(name))
                        inner.paint = paint(*color);
                    else if (name.starts_with("bg_"))
                        if (const auto background = colorClass(name.substr(3)))
                            boxColor_ = *background;
                }
            }
            walk(node.children, inner);
        }
    }

    [[nodiscard]] std::uint32_t paint(const core::Color4d& color) {
        for (std::size_t index = 0; index < paints_.size(); ++index)
            if (paints_[index] == color)
                return static_cast<std::uint32_t>(index);
        paints_.push_back(color);
        return static_cast<std::uint32_t>(paints_.size() - 1);
    }

    [[nodiscard]] std::vector<render::StyledRun> take() { return std::move(runs_); }

  private:
    const CompiledSubtitles& subtitles_;
    const document::CueListValue& list_;
    std::vector<core::Color4d>& paints_;
    core::Color4d& boxColor_;
    std::vector<render::StyledRun> runs_;
};

[[nodiscard]] render::TextAlignment alignmentOf(const std::int64_t stored) noexcept {
    return stored == 0 ? render::TextAlignment::Left
           : stored == 2 ? render::TextAlignment::Right
                         : render::TextAlignment::Center;
}

// A cue's runs, paints and per-cue styling before layout.
struct CueContent final {
    std::vector<render::StyledRun> runs;
    std::vector<core::Color4d> paints;
    core::Color4d outlineColor;
    double outlineWidth = 0.0;
    core::Color4d boxColor;
    double size = 0.0;
};

[[nodiscard]] CueContent cueContent(const CompiledSubtitles& subtitles,
                                    const document::CueListValue& list, const document::Cue& cue,
                                    const SubtitlesStyleValues& style) {
    CueContent content;
    content.paints.push_back(style.fill);
    content.outlineColor = style.outlineColor;
    content.outlineWidth = style.outlineWidth;
    content.boxColor = style.boxColor;
    content.size = style.size;
    RunBuilder builder(subtitles, list, content.paints, content.boxColor);
    RunState base;
    if (subtitles.settings.useFileStyles && !cue.style.empty()) {
        if (const auto* named = list.findStyle(cue.style)) {
            builder.apply(*named, base);
            if (named->outlineColor)
                content.outlineColor = linearFromFile(*named->outlineColor);
            if (named->outlineWidth)
                content.outlineWidth = *named->outlineWidth * subtitles.settings.frameHeight;
            if (named->fontSize && *named->fontSize > 0.0)
                content.size = *named->fontSize * subtitles.settings.frameHeight;
        } else if (const auto color = colorClass(cue.style)) {
            base.paint = builder.paint(*color);
        }
    }
    if (subtitles.settings.showSpeaker == document::kSubtitlesSpeakerPrefix &&
        !cue.speaker.empty()) {
        std::u32string prefix;
        appendUtf8(prefix, cue.speaker);
        prefix += U": ";
        builder.text(prefix, base);
    }
    builder.walk(markup::parse(cue.text).tree.children, base);
    content.runs = builder.take();
    return content;
}

[[nodiscard]] std::size_t anchorOf(const CompiledSubtitles& subtitles, const document::Cue& cue) {
    if (subtitles.settings.useCuePositions && cue.placement.anchor)
        return static_cast<std::size_t>(*cue.placement.anchor);
    return static_cast<std::size_t>(std::clamp<std::int64_t>(subtitles.settings.anchor, 0, 8));
}

} // namespace

render::PathBounds subtitlesSafeArea(const CompiledSubtitlesSettings& settings) {
    const auto width = settings.frameWidth;
    const auto height = settings.frameHeight;
    double insetX = 0.0;
    double insetY = 0.0;
    switch (settings.safeArea) {
    case document::kSubtitlesSafeTitle:
        insetX = width * (1.0 - settings.titleSafe) / 2.0;
        insetY = height * (1.0 - settings.titleSafe) / 2.0;
        break;
    case document::kSubtitlesSafeAction:
        insetX = width * (1.0 - settings.actionSafe) / 2.0;
        insetY = height * (1.0 - settings.actionSafe) / 2.0;
        break;
    case document::kSubtitlesSafeCustom:
        insetX = width * settings.marginX;
        insetY = height * settings.marginY;
        break;
    default:
        break;
    }
    return {insetX, insetY, width - insetX, height - insetY};
}

std::variant<std::vector<SubtitleCueLayout>, std::string>
layoutSubtitleCues(const CompiledSubtitles& subtitles, const document::CueListValue& list,
                   const std::span<const std::size_t> indices, const SubtitlesStyleValues& style,
                   const render::PathCancellation& cancelled) {
    const auto& settings = subtitles.settings;
    const auto safe = subtitlesSafeArea(settings);
    const std::span<const render::TextFont> faces(subtitles.faces.faces);
    std::vector<SubtitleCueLayout> out;
    out.reserve(indices.size());
    for (const auto index : indices) {
        if (cancelled && cancelled())
            return std::string{};
        if (index >= list.cues.size())
            return std::string("A cue index is outside the list");
        const auto& cue = *list.cues[index];
        auto content = cueContent(subtitles, list, cue, style);
        SubtitleCueLayout layout;
        layout.index = index;
        layout.id = cue.id;
        layout.paints = std::move(content.paints);
        layout.outlineColor = content.outlineColor;
        layout.outlineWidth = std::max(0.0, content.outlineWidth);
        layout.boxColor = content.boxColor;
        layout.runs = std::move(content.runs);
        std::u32string scalars;
        for (const auto& run : layout.runs)
            scalars += run.text;
        if (const auto script = render::firstShapedScript(scalars))
            layout.unshapedScript = std::string(*script);
        auto& options = layout.options;
        options.horizontalSize = std::clamp(content.size, 1.0, render::kMaximumTextPixelSize);
        options.verticalSize = options.horizontalSize;
        options.lineHeight = style.lineHeight;
        options.letterSpacing = style.letterSpacing;
        const bool positioned = settings.useCuePositions && cue.placement.width.has_value();
        options.maxWidth = std::max(
            1.0, positioned ? *cue.placement.width * settings.frameWidth
                            : (safe.right - safe.left) * std::clamp(settings.maxWidth, 0.0, 1.0));
        options.wrap = settings.wrap == document::kSubtitlesWrapGreedy ? render::TextWrapMode::Greedy
                       : settings.wrap == document::kSubtitlesWrapExplicit
                           ? render::TextWrapMode::Explicit
                           : render::TextWrapMode::Balanced;
        options.alignment =
            settings.useCuePositions && cue.placement.textAlign
                ? alignmentOf(static_cast<std::int64_t>(*cue.placement.textAlign))
                : alignmentOf(settings.textAlign);
        auto block = render::layoutStyledText(faces, layout.runs, options, cancelled);
        if (!block)
            return cancelled && cancelled() ? std::string{}
                                            : std::string("The cue's text could not be laid out");
        layout.naturalLines = block.value()->lines.size();
        const auto maxLines = static_cast<std::size_t>(std::max<std::int64_t>(1, settings.maxLines));
        if (settings.overflow == document::kSubtitlesOverflowShrink &&
            layout.naturalLines > maxLines) {
            // Shrink to fit: 5 % steps, never below 70 %; past the floor every line shows.
            const auto full = options.horizontalSize;
            for (int step = 1; step <= 7; ++step) {
                options.horizontalSize = full * std::max(0.7, 1.0 - 0.05 * step);
                options.verticalSize = options.horizontalSize;
                block = render::layoutStyledText(faces, layout.runs, options, cancelled);
                if (!block)
                    return cancelled && cancelled()
                               ? std::string{}
                               : std::string("The cue's text could not be laid out");
                layout.shrunk = true;
                if (block.value()->lines.size() <= maxLines)
                    break;
            }
        }
        layout.block = std::move(*block.value());
        layout.padding = settings.box == document::kSubtitlesBoxOff
                             ? 0.0
                             : std::max(0.0, settings.boxPadding) * options.verticalSize;
        out.push_back(std::move(layout));
    }

    // Placement and stacking. A cue with its own position stands alone; the others stack per
    // anchor, the earliest (in list order: start, end, id) nearest the anchored edge.
    std::map<std::size_t, double> stacked;
    for (auto& layout : out) {
        const auto& cue = *list.cues[layout.index];
        const auto anchor = anchorOf(subtitles, cue);
        const auto column = static_cast<double>(anchor % 3);
        const auto row = static_cast<double>(anchor / 3);
        const auto boxWidth = layout.block.width + 2.0 * layout.padding;
        const auto boxHeight = layout.block.height + 2.0 * layout.padding;
        double anchorX = safe.left + column * (safe.right - safe.left) / 2.0;
        double anchorY = safe.top + row * (safe.bottom - safe.top) / 2.0;
        const bool explicitPosition = settings.useCuePositions &&
                                      (cue.placement.positionX || cue.placement.positionY);
        if (settings.useCuePositions && cue.placement.positionX)
            anchorX = *cue.placement.positionX * settings.frameWidth;
        if (settings.useCuePositions && cue.placement.positionY)
            anchorY = *cue.placement.positionY * settings.frameHeight;
        double left = anchorX - column * boxWidth / 2.0;
        double top = anchorY - row * boxHeight / 2.0;
        // Whole lines from the anchored edge (a WebVTT line number, an ASS margin).
        if (settings.useCuePositions && cue.placement.lineOffset) {
            const auto lines = static_cast<double>(*cue.placement.lineOffset) * layout.block.lineStep;
            top += anchor / 3 == 2 ? -lines : lines;
        }
        if (!explicitPosition) {
            auto& used = stacked[anchor];
            top += anchor / 3 == 2 ? -used : used;
            used += boxHeight;
        }
        layout.x = left + layout.padding + style.offset.x;
        layout.y = top + layout.padding + style.offset.y;
    }
    return out;
}

std::variant<std::vector<SubtitlesPlacedBlock>, std::string>
placeSubtitleCues(const CompiledSubtitles& subtitles, const document::CueListValue& list,
                  const core::RationalTime time, const SubtitlesStyleValues& style,
                  const render::PathCancellation& cancelled) {
    std::vector<std::size_t> active;
    for (std::size_t index = 0; index < list.cues.size(); ++index) {
        const auto& cue = *list.cues[index];
        if (time < cue.start)
            break;
        if (time < cue.end)
            active.push_back(index);
    }
    std::vector<SubtitlesPlacedBlock> placed;
    if (active.empty())
        return placed;
    auto laid = layoutSubtitleCues(subtitles, list, active, style, cancelled);
    if (auto* message = std::get_if<std::string>(&laid))
        return std::move(*message);
    for (const auto& layout : std::get<std::vector<SubtitleCueLayout>>(laid))
        placed.push_back({layout.index, layout.id,
                          {layout.x - layout.padding, layout.y - layout.padding,
                           layout.x + layout.block.width + layout.padding,
                           layout.y + layout.block.height + layout.padding}});
    return placed;
}

SubtitlesStyleValues subtitlesConstantStyle(const CompiledSubtitles& subtitles) {
    SubtitlesStyleValues style;
    const auto scalar = [](const CompiledScalarParameter& parameter, double& target) {
        if (const auto* value = std::get_if<double>(&parameter.source))
            target = *value;
    };
    const auto color = [](const CompiledColorParameter& parameter, core::Color4d& target) {
        if (const auto* value = std::get_if<core::Color4d>(&parameter.source))
            target = *value;
    };
    scalar(subtitles.size, style.size);
    scalar(subtitles.outlineWidth, style.outlineWidth);
    scalar(subtitles.lineHeight, style.lineHeight);
    scalar(subtitles.letterSpacing, style.letterSpacing);
    scalar(subtitles.opacity, style.opacity);
    color(subtitles.fill, style.fill);
    color(subtitles.outlineColor, style.outlineColor);
    color(subtitles.boxColor, style.boxColor);
    if (const auto* value = std::get_if<document::Vec2d>(&subtitles.offset.source))
        style.offset = *value;
    return style;
}

std::vector<std::vector<SubtitleCueCheck>>
checkSubtitleCues(const CompiledSubtitles& subtitles, const document::CueListValue& list,
                  const SubtitlesStyleValues& style, const SubtitlesCheckSettings& settings) {
    std::vector<std::vector<SubtitleCueCheck>> checks(list.cues.size());
    const double rate = static_cast<double>(settings.frameRate.numerator()) /
                        static_cast<double>(settings.frameRate.denominator());
    const auto& node = subtitles.settings;
    const auto maxLines = static_cast<std::size_t>(std::max<std::int64_t>(1, node.maxLines));
    for (std::size_t index = 0; index < list.cues.size(); ++index) {
        const auto& cue = *list.cues[index];
        auto& found = checks[index];
        const std::array one{index};
        const auto laid = layoutSubtitleCues(subtitles, list, one, style);
        if (const auto* layouts = std::get_if<std::vector<SubtitleCueLayout>>(&laid);
            layouts != nullptr && !layouts->empty()) {
            const auto& layout = layouts->front();
            std::size_t widest = 0;
            for (const auto& line : layout.block.lines)
                widest = std::max(widest, line.characters);
            if (widest > static_cast<std::size_t>(settings.maxCharactersPerLine))
                found.push_back({"characters-per-line",
                                 "A line has " + std::to_string(widest) + " characters; at most " +
                                     std::to_string(settings.maxCharactersPerLine) + " read well"});
            if (layout.block.lines.size() > maxLines)
                found.push_back({"lines", "The cue shows " +
                                              std::to_string(layout.block.lines.size()) +
                                              " lines; Max lines is " + std::to_string(maxLines)});
            if (layout.unshapedScript)
                found.push_back(
                    {"unshaped-script",
                     "Cue " + std::to_string(cue.id) + " contains " + *layout.unshapedScript +
                         " text; Bloom lays text out without shaping, so it will not read "
                         "correctly"});
        }
        const double duration = cue.end.toSeconds() - cue.start.toSeconds();
        std::size_t characters = 0;
        std::u32string plain;
        appendUtf8(plain, markup::plainText(cue.text));
        for (const auto scalar : plain)
            if (scalar != U'\n')
                ++characters;
        if (duration > 0.0) {
            const double speed = static_cast<double>(characters) / duration;
            if (speed > settings.maxReadingSpeed) {
                const auto rounded = std::round(speed * 10.0) / 10.0;
                auto text = std::to_string(rounded);
                text.erase(text.find_last_not_of('0') + 1);
                if (!text.empty() && text.back() == '.')
                    text.pop_back();
                found.push_back({"reading-speed", "Reading speed is " + text +
                                                      " characters per second; at most " +
                                                      std::to_string(static_cast<std::int64_t>(
                                                          settings.maxReadingSpeed)) +
                                                      " read well"});
            }
        }
        if (duration + 1e-12 < settings.minDuration)
            found.push_back({"duration", "The cue shows for less than the minimum duration"});
        if (index + 1 < list.cues.size()) {
            const auto& next = *list.cues[index + 1];
            if (next.start < cue.end) {
                found.push_back({"overlap", "The cue overlaps cue " + std::to_string(next.id)});
            } else {
                const double gapFrames = (next.start.toSeconds() - cue.end.toSeconds()) * rate;
                if (gapFrames + 1e-9 < static_cast<double>(settings.minGapFrames))
                    found.push_back({"gap", "The gap to cue " + std::to_string(next.id) +
                                                " is shorter than " +
                                                std::to_string(settings.minGapFrames) + " frames"});
            }
        }
        if (index > 0 && cue.start < list.cues[index - 1]->end &&
            std::ranges::none_of(found, [](const auto& check) { return check.code == "overlap"; }))
            found.push_back({"overlap", "The cue overlaps cue " +
                                            std::to_string(list.cues[index - 1]->id)});
        if (!(cue.start < settings.compositionDuration) || settings.compositionDuration < cue.end)
            found.push_back({"outside-composition", "The cue reaches past the composition's end"});
    }
    return checks;
}

} // namespace bloom::runtime
