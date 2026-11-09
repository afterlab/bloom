#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bloom::document {

// Where a font comes from. A text node stores this beside the family so a shared project names the
// font without carrying any font file (docs/architecture/fonts.md).
enum class FontSource : std::uint8_t {
    // An installed font of the machine, matched by PostScript name first, then family and style.
    System,
    // A family of the Google Fonts catalogue, fetched into the per-user cache on demand.
    Google,
    // One of the few faces Bloom's own interface and last-resort text use.
    Builtin,
};

// One variable-axis value other than weight and italic (which the font ID states directly), for
// example `wdth` 110.
struct FontAxisValue final {
    std::string tag;
    double value = 0.0;

    friend bool operator==(const FontAxisValue&, const FontAxisValue&) = default;
};

// The durable identity of a font a text node uses.
struct FontId final {
    FontSource source = FontSource::Builtin;
    std::string family;
    // The style name the artist sees: a weight and slope ("Bold Italic") or a named instance.
    std::string style;
    // The requested weight (1..1000) and slope. Matching uses these when the style name is not
    // found, so a project keeps working when a family spells a style differently.
    std::int32_t weight = 400;
    bool italic = false;
    // Other variable-axis values, sorted by tag, each tag four ASCII letters.
    std::vector<FontAxisValue> axes;
    // System fonts only: the PostScript name, the most exact system match.
    std::string postscriptName;
    // Google fonts only: the family version the project was last saved with (for example "v51").
    std::string familyVersion;

    friend bool operator==(const FontId&, const FontId&) = default;
};

inline constexpr std::size_t kMaximumFontFamilyBytes = 256;
inline constexpr std::size_t kMaximumFontStyleBytes = 128;
inline constexpr std::size_t kMaximumFontAxes = 16;

[[nodiscard]] std::string_view fontSourceName(FontSource source) noexcept;
[[nodiscard]] std::optional<FontSource> fontSourceFromName(std::string_view name) noexcept;

// Whether every member is within its bounds (valid UTF-8, sorted unique axis tags, finite values).
[[nodiscard]] bool isValidFontId(const FontId& id) noexcept;

// The stored text of a font ID, for example
// `source=google;family=Roboto;style=Bold;weight=700;italic=false;version=v51`. Members appear in
// this fixed order and an empty one is left out; the characters `%`, `;`, `=`, `|` and `:` and
// control characters inside a value are written as `%XX`. Encoding then decoding gives back the
// same ID, and decoding then encoding gives back the same text.
[[nodiscard]] std::string encodeFontId(const FontId& id);

// Reads stored text. Unknown members are ignored so a newer Bloom's extra members do not stop an
// older one from opening the project; a missing source or family, or a member out of bounds, gives
// nullopt.
[[nodiscard]] std::optional<FontId> decodeFontId(std::string_view text);

// "Family Style" for display (the style is left out when it is empty).
[[nodiscard]] std::string fontDisplayName(const FontId& id);

// The ID text a text node holds when nothing else was chosen: the built-in DejaVu Sans Book.
[[nodiscard]] const FontId& defaultFontId();

} // namespace bloom::document
