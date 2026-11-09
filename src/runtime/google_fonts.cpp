#include <bloom/runtime/google_fonts.hpp>

#include <bloom/core/safe_parse.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>

namespace bloom::runtime::fonts {
namespace {

[[nodiscard]] std::string percentEncode(const std::string_view text, const bool spaceAsPlus) {
    constexpr std::string_view hex = "0123456789ABCDEF";
    std::string out;
    for (const char raw : text) {
        const auto c = static_cast<unsigned char>(raw);
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out += raw;
        } else if (c == ' ' && spaceAsPlus) {
            out += '+';
        } else {
            out += '%';
            out += hex[c >> 4U];
            out += hex[c & 0x0fU];
        }
    }
    return out;
}

[[nodiscard]] std::string number(const double value) {
    return core::formatScalar(value, std::int32_t{3}, true, std::int32_t{0});
}

[[nodiscard]] bool lowerCaseTag(const std::string& tag) {
    return !tag.empty() && tag.front() >= 'a' && tag.front() <= 'z';
}

[[nodiscard]] std::string_view trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\n' ||
                             text.front() == '\r'))
        text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\n' ||
                             text.back() == '\r'))
        text.remove_suffix(1);
    return text;
}

[[nodiscard]] std::string unquote(std::string_view text) {
    text = trim(text);
    if (text.size() >= 2 && (text.front() == '\'' || text.front() == '"') &&
        text.back() == text.front())
        text = text.substr(1, text.size() - 2);
    return std::string(text);
}

} // namespace

std::string googleFamilyStylesheetUrl(const std::string_view family,
                                      const std::string_view endpoint) {
    return std::string(endpoint) + "?family=" + percentEncode(family, true);
}

std::string googleStylesheetUrl(const document::FontId& id, const GoogleFamily* indexed,
                                const std::string_view subsetText, const std::string_view endpoint) {
    // The axes named in the request, as (tag, value). Google requires them sorted with lower-case
    // tags first, then upper-case, each group alphabetical.
    std::vector<std::pair<std::string, std::string>> axes;
    const auto* weightAxis = indexed != nullptr ? indexed->axis("wght") : nullptr;
    const bool italicFamily = indexed != nullptr ? indexed->hasItalic() : id.italic;
    if (indexed == nullptr || italicFamily)
        axes.emplace_back("ital", id.italic ? "1" : "0");
    if (weightAxis != nullptr) {
        axes.emplace_back("wght", number(std::clamp<double>(id.weight, weightAxis->minimum,
                                                            weightAxis->maximum)));
    } else if (indexed == nullptr || !indexed->styles.empty()) {
        std::int32_t weight = id.weight;
        if (indexed != nullptr) {
            // A static family serves only the weights it has: the nearest one of the slope.
            std::int32_t best = indexed->styles.front().weight;
            for (const auto& style : indexed->styles)
                if (style.italic == id.italic &&
                    std::abs(style.weight - id.weight) < std::abs(best - id.weight))
                    best = style.weight;
            weight = best;
        }
        axes.emplace_back("wght", std::to_string(weight));
    }
    if (indexed != nullptr)
        for (const auto& axis : id.axes)
            if (const auto* known = indexed->axis(axis.tag); known != nullptr && axis.tag != "wght")
                axes.emplace_back(axis.tag, number(std::clamp(axis.value, known->minimum,
                                                              known->maximum)));
    std::ranges::sort(axes, [](const auto& a, const auto& b) {
        if (lowerCaseTag(a.first) != lowerCaseTag(b.first))
            return lowerCaseTag(a.first);
        return a.first < b.first;
    });
    std::string url = std::string(endpoint) + "?family=" + percentEncode(id.family, true);
    if (!axes.empty()) {
        std::string tags;
        std::string values;
        for (const auto& [tag, value] : axes) {
            tags += (tags.empty() ? "" : ",") + tag;
            values += (values.empty() ? "" : ",") + value;
        }
        url += ":" + tags + "@" + values;
    }
    if (!subsetText.empty())
        url += "&text=" + percentEncode(subsetText, false);
    return url;
}

std::vector<GoogleStylesheetFace> parseGoogleStylesheet(const std::string_view css) {
    std::vector<GoogleStylesheetFace> faces;
    std::size_t position = 0;
    while (true) {
        const auto open = css.find("@font-face", position);
        if (open == std::string_view::npos)
            break;
        const auto begin = css.find('{', open);
        const auto end = css.find('}', begin == std::string_view::npos ? open : begin);
        if (begin == std::string_view::npos || end == std::string_view::npos)
            break;
        position = end + 1;
        GoogleStylesheetFace face;
        const auto body = css.substr(begin + 1, end - begin - 1);
        std::size_t at = 0;
        while (at < body.size()) {
            auto stop = body.find(';', at);
            if (stop == std::string_view::npos)
                stop = body.size();
            const auto declaration = body.substr(at, stop - at);
            at = stop + 1;
            const auto colon = declaration.find(':');
            if (colon == std::string_view::npos)
                continue;
            const auto property = trim(declaration.substr(0, colon));
            const auto value = trim(declaration.substr(colon + 1));
            if (property == "font-family") {
                face.family = unquote(value);
            } else if (property == "font-style") {
                face.italic = value == "italic" || value == "oblique";
            } else if (property == "font-weight") {
                const auto space = value.find(' ');
                const auto first = value.substr(0, space);
                std::from_chars(first.data(), first.data() + first.size(), face.weight);
                face.weightMaximum = face.weight;
                if (space != std::string_view::npos) {
                    const auto second = trim(value.substr(space + 1));
                    std::from_chars(second.data(), second.data() + second.size(),
                                    face.weightMaximum);
                }
            } else if (property == "src") {
                const auto urlAt = value.find("url(");
                if (urlAt != std::string_view::npos) {
                    const auto close = value.find(')', urlAt);
                    if (close != std::string_view::npos)
                        face.url = unquote(value.substr(urlAt + 4, close - urlAt - 4));
                }
            }
        }
        if (face.url.empty() || !isGoogleFontFileUrl(face.url))
            continue;
        face.version = googleVersionFromUrl(face.url);
        faces.push_back(std::move(face));
    }
    return faces;
}

const GoogleStylesheetFace* pickGoogleStylesheetFace(const std::vector<GoogleStylesheetFace>& faces,
                                                     const std::int32_t weight, const bool italic) {
    const GoogleStylesheetFace* best = nullptr;
    int bestDistance = 0;
    for (const auto& face : faces) {
        if (face.italic != italic)
            continue;
        const int distance = weight >= face.weight && weight <= face.weightMaximum
                                 ? 0
                                 : std::min(std::abs(weight - face.weight),
                                            std::abs(weight - face.weightMaximum));
        if (best == nullptr || distance < bestDistance) {
            best = &face;
            bestDistance = distance;
        }
    }
    if (best == nullptr && !faces.empty())
        best = &faces.front();
    return best;
}

std::string googleVersionFromUrl(const std::string_view url) {
    const auto isVersion = [](const std::string_view part) {
        return part.size() >= 2 && part.front() == 'v' &&
               std::ranges::all_of(part.substr(1), [](const char c) { return c >= '0' && c <= '9'; });
    };
    const auto query = url.find('?');
    const auto path = url.substr(0, query);
    std::size_t start = 0;
    while (start < path.size()) {
        auto end = path.find('/', start);
        if (end == std::string_view::npos)
            end = path.size();
        const auto part = path.substr(start, end - start);
        if (isVersion(part))
            return std::string(part);
        start = end + 1;
    }
    if (query != std::string_view::npos) {
        const auto tail = url.substr(query + 1);
        std::size_t at = 0;
        while (at < tail.size()) {
            auto stop = tail.find('&', at);
            if (stop == std::string_view::npos)
                stop = tail.size();
            const auto pair = tail.substr(at, stop - at);
            at = stop + 1;
            if (pair.starts_with("v=") && isVersion(pair.substr(2)))
                return std::string(pair.substr(2));
        }
    }
    return {};
}

bool isGoogleFontFileUrl(const std::string_view url) {
    constexpr std::string_view prefix = "https://fonts.gstatic.com/";
    return url.starts_with(prefix) && url.size() < 2048 &&
           std::ranges::none_of(url, [](const char c) { return c == ' ' || c == '\n'; });
}

} // namespace bloom::runtime::fonts
