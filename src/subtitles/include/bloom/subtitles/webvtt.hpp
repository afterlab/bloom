#pragma once

// #752: the W3C WebVTT file parsing algorithm, as the specification states it, over decoded text.
// Its result is the specification's own model (cues with their settings, regions, style sheets),
// which the web-platform-tests WebVTT parsing cases check; subtitles.hpp's load() maps it onto
// Bloom's cues and reports what that mapping drops or approximates.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bloom::subtitles::webvtt {

struct Region {
    std::string id;
    double width = 100.0;
    std::int64_t lines = 3;
    double anchorX = 0.0;
    double anchorY = 100.0;
    double viewportAnchorX = 0.0;
    double viewportAnchorY = 100.0;
    bool scrollUp = false;
    std::size_t line = 0;
};

enum class Direction : std::uint8_t { Horizontal, RightToLeft, LeftToRight };
enum class LineAlign : std::uint8_t { Start, Center, End };
enum class PositionAlign : std::uint8_t { Auto, LineLeft, Center, LineRight };
enum class TextAlign : std::uint8_t { Start, Center, End, Left, Right };

struct Cue {
    std::string id;
    std::int64_t startMilliseconds = 0;
    std::int64_t endMilliseconds = 0;
    std::string text;
    Direction vertical = Direction::Horizontal;
    std::optional<double> line; // nullopt is "auto"
    bool snapToLines = true;
    LineAlign lineAlign = LineAlign::Start;
    std::optional<double> position; // nullopt is "auto"
    PositionAlign positionAlign = PositionAlign::Auto;
    double size = 100.0;
    TextAlign align = TextAlign::Center;
    // An index into File::regions, or nullopt.
    std::optional<std::size_t> region;
    // The 1-based line of the timing line, and of the first text line.
    std::size_t timingLine = 0;
    std::size_t textLine = 0;
    // Settings the parser skipped (unknown names, malformed values), for the report.
    std::vector<std::string> skippedSettings;
};

struct StyleSheet {
    std::string text;
    std::size_t line = 0;
};

struct File {
    // False when the signature is wrong: the file was not processed.
    bool signatureValid = false;
    std::vector<Cue> cues;
    std::vector<Region> regions;
    std::vector<StyleSheet> styles;
    // Lines of blocks the parser read and returned nothing for: NOTE blocks and blocks whose timing
    // line did not parse ("note" or "malformed").
    struct IgnoredBlock {
        std::size_t line = 0;
        bool note = false;
        std::string firstLine;
    };
    std::vector<IgnoredBlock> ignored;
};

[[nodiscard]] File parse(std::string_view text);

} // namespace bloom::subtitles::webvtt
