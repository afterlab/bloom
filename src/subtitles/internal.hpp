#pragma once

// #752: what the subtitle readers share: line splitting, colors, and the builder that turns read
// cues into a valid document::CueListValue under one set of rules (docs/architecture/subtitles.md,
// "Timing rules" and "Malformed input").

#include <bloom/subtitles/subtitles.hpp>

#include <bloom/core/color.hpp>
#include <bloom/document/cue_list.hpp>

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bloom::subtitles::detail {

struct Line {
    std::string_view text;
    std::size_t number = 0;
};
// The lines of `text`; CR LF, CR and LF all end a line.
[[nodiscard]] std::vector<Line> splitLines(std::string_view text);

[[nodiscard]] std::string_view trim(std::string_view text) noexcept;
[[nodiscard]] std::string lower(std::string_view text);
[[nodiscard]] bool equalsIgnoringCase(std::string_view left, std::string_view right) noexcept;

// A CSS color: #rgb, #rgba, #rrggbb, #rrggbbaa, rgb(), rgba(), or a CSS named color. Components
// are sRGB-encoded in 0..1, alpha straight.
[[nodiscard]] std::optional<core::Color4d> parseCssColor(std::string_view text);
// "rrggbb", or "rrggbbaa" when not opaque, of a color's 8-bit sRGB components.
[[nodiscard]] std::string hexName(const core::Color4d& color);
// The 8-bit component, rounded.
[[nodiscard]] unsigned byteOf(double component) noexcept;

// Exact seconds from milliseconds.
[[nodiscard]] core::RationalTime milliseconds(std::int64_t value);

struct ReadCue {
    core::RationalTime start;
    core::RationalTime end;
    // Bloom cue markup (WebVTT cue text). The builder canonicalizes what does not read strictly.
    std::string text;
    std::string speaker;
    std::string style;
    document::CuePlacement placement;
    std::size_t line = 0;
    std::size_t column = 0;
};

class Builder final {
  public:
    explicit Builder(const LoadOptions& options) : options_(options) {}

    void report(std::size_t line, std::size_t column, std::string_view code, std::string message);
    // A feature dropped or approximated, counted: the report holds one row per feature, with how
    // many times it occurred and where first.
    void note(std::string_view code, std::string feature, std::size_t line, std::size_t column = 0);
    // A style entry; a later entry of the same name replaces the earlier one.
    void addStyle(document::CueStyle style);
    [[nodiscard]] bool hasStyle(std::string_view name) const;
    // The name of a generated style holding one color ("<prefix>-rrggbb"), adding it when new.
    [[nodiscard]] std::string colorStyle(std::string_view prefix, const core::Color4d& color);
    void addCue(ReadCue cue);
    void setLanguage(std::string language) { language_ = std::move(language); }
    [[nodiscard]] bool stopRequested() const noexcept { return options_.stop.stop_requested(); }
    [[nodiscard]] std::size_t cueCount() const noexcept { return cues_.size(); }

    [[nodiscard]] LoadOutcome finish(Format format, std::string encoding);
    [[nodiscard]] LoadFailure failure(std::size_t line, std::size_t column, std::string_view code,
                                      std::string message) const;

  private:
    const LoadOptions& options_;
    void flushNotes();

    std::vector<Report> reports_;
    struct Noted {
        std::string code;
        std::string feature;
        std::size_t line = 0;
        std::size_t column = 0;
        std::size_t count = 0;
    };
    std::vector<Noted> notes_;
    std::map<std::string, document::CueStyle> styles_;
    std::vector<ReadCue> cues_;
    std::string language_ = "und";
    bool tooMany_ = false;
};

// One format's reader over decoded text.
[[nodiscard]] LoadOutcome readSrt(std::string_view text, Builder& builder, std::string encoding);
[[nodiscard]] LoadOutcome readWebVtt(std::string_view text, Builder& builder, std::string encoding);
[[nodiscard]] LoadOutcome readTtml(std::string_view text, Builder& builder, std::string encoding);
[[nodiscard]] LoadOutcome readAss(std::string_view text, Builder& builder, std::string encoding);

[[nodiscard]] std::string writeSrt(const document::CueListValue& cues, std::vector<Report>& reports);
[[nodiscard]] std::string writeWebVtt(const document::CueListValue& cues,
                                      std::vector<Report>& reports);
[[nodiscard]] std::string writeTtml(const document::CueListValue& cues, const WriteOptions& options,
                                    std::vector<Report>& reports);

} // namespace bloom::subtitles::detail
