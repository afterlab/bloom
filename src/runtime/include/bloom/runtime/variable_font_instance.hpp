#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace bloom::runtime::fonts {

// One axis of a variable font's `fvar` table, in the font's own user units (a weight axis runs
// 100 to 900, a width axis 50 to 200, and so on).
struct VariableFontAxis final {
    std::string tag;
    double minimum = 0.0;
    double defaultValue = 0.0;
    double maximum = 0.0;
};

struct VariableFontAxisSetting final {
    std::string tag;
    double value = 0.0;
};

enum class VariableFontInstanceStatus : std::uint8_t {
    // The font has no variation tables: the bytes are used as they are.
    NotVariable,
    // The bytes were rewritten as a static font at the requested coordinates.
    Instanced,
    // A variable font this reader cannot instantiate (damaged tables, or a collection that has
    // no such font): the default instance is drawn.
    Unsupported,
};

struct VariableFontInstance final {
    VariableFontInstanceStatus status = VariableFontInstanceStatus::NotVariable;
    // The static font file, when status is Instanced.
    std::vector<std::uint8_t> bytes;
    // For Unsupported: why, in one phrase.
    std::string detail;
};

// The axes of a variable font; empty for a static font or one that cannot be read.
// `collectionIndex` is the font within a collection file (0 for a single-font file).
[[nodiscard]] std::vector<VariableFontAxis> variableFontAxes(std::span<const std::uint8_t> font,
                                                             std::uint32_t collectionIndex = 0);

// Rewrites a variable font as a static single-font file at one point of its design space: the
// axes start at their defaults, `namedInstance` (1-based, 0 for none) moves them to that named
// instance, and `settings` then override single axes (clamped to the axis range; tags the font
// does not have are ignored). `collectionIndex` is the font within a collection file (0 for a
// single-font file); the result is that one font.
//
// TrueType outlines (`glyf` with `gvar` deltas): outlines, composite offsets, advance widths
// (`HVAR`, else the `gvar` phantom points) and glyph bounds follow the coordinates, and hinting is
// kept: glyph programs, `fpgm` and `prep` are carried and the control values follow `cvar`.
// Compact font format outlines (`CFF2`): every charstring is run at the coordinates and written
// as a static `CFF ` table in whole units, with advances from `HVAR`; hint operators, whose stems
// would move with the outline, are not carried.
[[nodiscard]] VariableFontInstance
instantiateVariableFont(std::span<const std::uint8_t> font, std::uint32_t collectionIndex,
                        std::uint32_t namedInstance,
                        std::span<const VariableFontAxisSetting> settings);

} // namespace bloom::runtime::fonts
