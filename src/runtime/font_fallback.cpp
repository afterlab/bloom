#include <bloom/runtime/font_fallback.hpp>

#include <bloom/runtime/google_catalogue.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <string>

namespace bloom::runtime::fonts {
namespace {

constexpr std::array<std::string_view, 6> kSans{"Arial",         "Helvetica", "Liberation Sans",
                                                 "DejaVu Sans",   "Noto Sans", "Segoe UI"};
constexpr std::array<std::string_view, 5> kSerif{"Times New Roman", "Times", "Liberation Serif",
                                                  "DejaVu Serif", "Noto Serif"};
constexpr std::array<std::string_view, 5> kMonospace{"Courier New", "Courier", "Liberation Mono",
                                                      "DejaVu Sans Mono", "Noto Sans Mono"};

[[nodiscard]] std::string lower(const std::string_view text) {
    std::string out(text);
    std::ranges::transform(out, out.begin(),
                           [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

[[nodiscard]] bool contains(const std::string& text, const std::string_view part) {
    return text.find(part) != std::string::npos;
}

} // namespace

std::string_view fontCategoryName(const FontCategory category) noexcept {
    switch (category) {
    case FontCategory::Sans:
        return "sans";
    case FontCategory::Serif:
        return "serif";
    case FontCategory::Display:
        return "display";
    case FontCategory::Handwriting:
        return "handwriting";
    case FontCategory::Monospace:
        return "monospace";
    }
    return "sans";
}

std::optional<FontCategory> fontCategoryFromName(const std::string_view name) noexcept {
    for (const auto category : {FontCategory::Sans, FontCategory::Serif, FontCategory::Display,
                                FontCategory::Handwriting, FontCategory::Monospace})
        if (fontCategoryName(category) == name)
            return category;
    return std::nullopt;
}

std::span<const std::string_view> fallbackFamilies(const FontCategory category) noexcept {
    switch (category) {
    case FontCategory::Serif:
        return kSerif;
    case FontCategory::Monospace:
        return kMonospace;
    case FontCategory::Sans:
    case FontCategory::Display:
    case FontCategory::Handwriting:
        return kSans;
    }
    return kSans;
}

FontCategory categoryForMissingFont(const document::FontId& id) {
    if (const auto* indexed = googleCatalogue().find(id.family))
        return indexed->category;
    const auto family = lower(id.family);
    const auto style = lower(id.style);
    for (const auto* text : {&family, &style})
        if (contains(*text, "mono") || contains(*text, "typewriter") || contains(*text, "courier") ||
            contains(*text, "consolas") || contains(*text, "code"))
            return FontCategory::Monospace;
    for (const auto* text : {&family, &style})
        if (contains(*text, "serif") && !contains(*text, "sans"))
            return FontCategory::Serif;
    for (const std::string_view name : {"times", "georgia", "garamond", "palatino", "cambria",
                                        "baskerville", "bookman", "century", "didot", "minion",
                                        "caslon"})
        if (contains(family, name))
            return FontCategory::Serif;
    return FontCategory::Sans;
}

} // namespace bloom::runtime::fonts
