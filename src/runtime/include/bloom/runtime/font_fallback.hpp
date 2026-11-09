#pragma once

#include <bloom/document/font_id.hpp>

#include <optional>
#include <span>
#include <string_view>

namespace bloom::runtime::fonts {

// The five categories the Google Fonts catalogue uses, which the picker filters by and the fallback
// is chosen from.
enum class FontCategory : std::uint8_t { Sans, Serif, Display, Handwriting, Monospace };

[[nodiscard]] std::string_view fontCategoryName(FontCategory category) noexcept;
[[nodiscard]] std::optional<FontCategory> fontCategoryFromName(std::string_view name) noexcept;

// The fallback table (docs/architecture/fonts.md): the first family of the list that is installed
// stands in for a font that cannot be resolved. It is the same table on Linux, macOS and Windows;
// each platform simply has different entries installed. Display and handwriting fonts, and any
// other kind, take the sans list. When none is installed Bloom's built-in face is used.
[[nodiscard]] std::span<const std::string_view> fallbackFamilies(FontCategory category) noexcept;

// The category a font that cannot be resolved belongs to: the Google catalogue's own category when
// its family is indexed, otherwise a reading of the recorded family and style (a family or style
// that says mono or typewriter is monospace; one that says serif without sans, or names a common
// serif family, is serif), otherwise sans.
[[nodiscard]] FontCategory categoryForMissingFont(const document::FontId& id);

} // namespace bloom::runtime::fonts
