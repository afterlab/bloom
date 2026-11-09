#include <bloom/runtime/google_catalogue.hpp>

#include <bloom/core/safe_parse.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <string>

namespace bloom::runtime::fonts {
namespace {

[[nodiscard]] std::string lower(const std::string_view text) {
    std::string out(text);
    std::ranges::transform(out, out.begin(),
                           [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

[[nodiscard]] std::vector<std::string_view> split(const std::string_view text, const char separator) {
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    while (start <= text.size()) {
        auto end = text.find(separator, start);
        if (end == std::string_view::npos)
            end = text.size();
        parts.push_back(text.substr(start, end - start));
        start = end + 1;
    }
    return parts;
}

[[nodiscard]] bool parseNumber(const std::string_view text, std::int32_t& out) {
    const auto result = std::from_chars(text.data(), text.data() + text.size(), out);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}
[[nodiscard]] bool parseNumber(const std::string_view text, double& out) {
    const auto value = core::parseScalar(text);
    if (value)
        out = *value;
    return value.has_value();
}

} // namespace

const GoogleFontAxis* GoogleFamily::axis(const std::string_view tag) const noexcept {
    const auto found = std::ranges::find(axes, tag, &GoogleFontAxis::tag);
    return found == axes.end() ? nullptr : &*found;
}

bool GoogleFamily::hasItalic() const noexcept {
    return std::ranges::any_of(styles, [](const GoogleFontStyle& style) { return style.italic; });
}

GoogleCatalogue GoogleCatalogue::parse(const std::string_view text) {
    GoogleCatalogue catalogue;
    for (const auto line : split(text, '\n')) {
        if (line.empty())
            continue;
        if (line.front() == '#') {
            const auto equals = line.find('=');
            if (equals == std::string_view::npos)
                continue;
            const auto key = line.substr(1, equals - 1);
            const auto value = std::string(line.substr(equals + 1));
            if (key == "index-date")
                catalogue.info_.indexDate = value;
            else if (key == "source-revision")
                catalogue.info_.sourceRevision = value;
            continue;
        }
        const auto fields = split(line, '\t');
        if (fields.size() != 5 || fields[0].empty())
            continue;
        const auto category = fontCategoryFromName(fields[1]);
        if (!category)
            continue;
        GoogleFamily family;
        family.family = std::string(fields[0]);
        family.category = *category;
        family.version = std::string(fields[2]);
        for (const auto token : split(fields[3], ',')) {
            if (token.empty())
                continue;
            const bool italic = token.back() == 'i';
            std::int32_t weight = 0;
            if (parseNumber(italic ? token.substr(0, token.size() - 1) : token, weight))
                family.styles.push_back({weight, italic});
        }
        for (const auto token : split(fields[4], ',')) {
            const auto parts = split(token, ':');
            GoogleFontAxis axis;
            if (parts.size() == 3 && parseNumber(parts[1], axis.minimum) &&
                parseNumber(parts[2], axis.maximum)) {
                axis.tag = std::string(parts[0]);
                family.axes.push_back(std::move(axis));
            }
        }
        catalogue.families_.push_back(std::move(family));
    }
    std::ranges::sort(catalogue.families_, [](const GoogleFamily& a, const GoogleFamily& b) {
        return lower(a.family) < lower(b.family);
    });
    return catalogue;
}

const GoogleFamily* GoogleCatalogue::find(const std::string_view family) const noexcept {
    const auto wanted = lower(family);
    const auto found = std::ranges::lower_bound(
        families_, wanted, {},
        [](const GoogleFamily& entry) { return lower(entry.family); });
    if (found == families_.end() || lower(found->family) != wanted)
        return nullptr;
    return &*found;
}

const GoogleCatalogue& googleCatalogue() {
    static const GoogleCatalogue catalogue = [] {
        std::string text;
        for (const auto chunk : detail::googleCatalogueChunks())
            text += chunk;
        return GoogleCatalogue::parse(text);
    }();
    return catalogue;
}

} // namespace bloom::runtime::fonts
