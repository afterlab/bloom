// #757: a Text Source's markup as styled runs (text_markup.hpp).
#include <bloom/runtime/text_markup.hpp>

#include <bloom/core/utf8.hpp>
#include <bloom/document/cue_markup.hpp>
#include <bloom/document/text_styling.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <string>

namespace bloom::runtime {
namespace {

namespace markup = document::cue_markup;

// The most warnings one text names; the rest are counted in a last one.
constexpr std::size_t kNamedWarnings = 4;

[[nodiscard]] double decodeSrgb(const double encoded) noexcept {
    return encoded <= 0.04045 ? encoded / 12.92 : std::pow((encoded + 0.055) / 1.055, 2.4);
}

// The WebVTT default color classes (W3C WebVTT, "Default classes"), sRGB, as Subtitles reads them.
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

[[nodiscard]] std::optional<int> hexDigit(const char c) noexcept {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return std::nullopt;
}

// "#RGB", "#RRGGBB" or "#RRGGBBAA", sRGB-encoded with straight alpha, as linear RGBA.
[[nodiscard]] std::optional<core::Color4d> literalColor(const std::string_view name) {
    if (name.size() < 2 || name.front() != '#')
        return std::nullopt;
    const auto digits = name.substr(1);
    if (digits.size() != 3 && digits.size() != 6 && digits.size() != 8)
        return std::nullopt;
    std::array<double, 4> channels{0.0, 0.0, 0.0, 1.0};
    const auto width = digits.size() == 3 ? std::size_t{1} : std::size_t{2};
    for (std::size_t channel = 0; channel * width < digits.size(); ++channel) {
        int value = 0;
        for (std::size_t index = 0; index < width; ++index) {
            const auto digit = hexDigit(digits[channel * width + index]);
            if (!digit)
                return std::nullopt;
            value = value * 16 + *digit;
        }
        if (width == 1)
            value *= 17;
        channels[channel] = static_cast<double>(value) / 255.0;
    }
    return core::Color4d{decodeSrgb(channels[0]), decodeSrgb(channels[1]), decodeSrgb(channels[2]),
                         channels[3]};
}

void appendUtf8(std::u32string& out, const std::string_view text) {
    for (std::size_t cursor = 0; cursor < text.size();) {
        const auto scalar = core::decodeUtf8Scalar(text, cursor);
        if (!scalar.isValid() || scalar.length == 0)
            return;
        cursor += scalar.length;
        if (scalar.value != U'\r')
            out.push_back(scalar.value);
    }
}

struct RunState final {
    bool bold = false;
    bool italic = false;
    bool underline = false;
    std::uint32_t paint = kTextFillPaint;
};

class RunBuilder final {
  public:
    RunBuilder(const CompiledSubtitlesFaces& faces, TextMarkupRuns& out)
        : faces_(faces), out_(out) {}

    void text(const std::u32string& value, const RunState& state) {
        if (value.empty())
            return;
        const auto face = static_cast<std::size_t>((state.bold ? 1 : 0) + (state.italic ? 2 : 0));
        out_.usesBold = out_.usesBold || state.bold;
        render::StyledRun run{value, face, faces_.oblique[face], state.paint, state.underline};
        auto& runs = out_.runs;
        if (!runs.empty() && runs.back().face == run.face && runs.back().oblique == run.oblique &&
            runs.back().paint == run.paint && runs.back().underline == run.underline) {
            runs.back().text += run.text;
            return;
        }
        runs.push_back(std::move(run));
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
            for (const auto& name : node.classes)
                if (const auto paint = classPaint(name))
                    inner.paint = *paint;
                else
                    warn("the color class \"" + name + "\" names no accent or color");
            walk(node.children, inner);
        }
    }

    void warn(std::string message) {
        ++warnings_;
        if (out_.warnings.size() < kNamedWarnings)
            out_.warnings.push_back(std::move(message));
    }

    void finish() {
        if (warnings_ > kNamedWarnings)
            out_.warnings.push_back(std::to_string(warnings_ - kNamedWarnings) +
                                    " more markup problems");
    }

  private:
    [[nodiscard]] std::optional<std::uint32_t> classPaint(const std::string& name) {
        if (name.size() == 7 && name.starts_with("accent") && name[6] >= '1' &&
            name[6] < static_cast<char>('1' + document::kTextAccentCount))
            return kTextFirstAccentPaint + static_cast<std::uint32_t>(name[6] - '1');
        std::optional<core::Color4d> color = literalColor(name);
        if (!color)
            for (const auto& entry : kColorClasses)
                if (entry.name == name)
                    color =
                        core::Color4d{decodeSrgb(entry.color.red), decodeSrgb(entry.color.green),
                                      decodeSrgb(entry.color.blue), entry.color.alpha};
        if (!color)
            return std::nullopt;
        auto& literals = out_.literals;
        const auto found = std::ranges::find(literals, *color);
        if (found != literals.end())
            return kTextFirstLiteralPaint + static_cast<std::uint32_t>(found - literals.begin());
        literals.push_back(*color);
        return kTextFirstLiteralPaint + static_cast<std::uint32_t>(literals.size() - 1);
    }

    const CompiledSubtitlesFaces& faces_;
    TextMarkupRuns& out_;
    std::size_t warnings_ = 0;
};

} // namespace

TextMarkupRuns textMarkupRuns(const std::string_view content, const bool markup,
                              const CompiledSubtitlesFaces& faces) {
    TextMarkupRuns out;
    RunBuilder builder(faces, out);
    if (!markup) {
        std::u32string scalars;
        appendUtf8(scalars, content);
        builder.text(scalars, {});
        return out;
    }
    // What the cue-text rules would ignore draws as written: its '<' is escaped and the text read
    // again, so the second reading keeps every other tag.
    const auto first = markup::parse(content);
    std::vector<std::size_t> literalTags;
    for (const auto& issue : first.ignored)
        if (issue.offset < content.size() && content[issue.offset] == '<') {
            literalTags.push_back(issue.offset);
            builder.warn(issue.message + " is drawn as text");
        }
    std::string escaped;
    if (!literalTags.empty()) {
        escaped.reserve(content.size() + literalTags.size() * 3);
        std::size_t next = 0;
        for (std::size_t index = 0; index < content.size(); ++index) {
            if (next < literalTags.size() && literalTags[next] == index) {
                escaped += "&lt;";
                ++next;
                continue;
            }
            escaped.push_back(content[index]);
        }
    }
    const auto parsed = literalTags.empty() ? first : markup::parse(escaped);
    builder.walk(parsed.tree.children, {});
    builder.finish();
    return out;
}

} // namespace bloom::runtime
