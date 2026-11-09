#include <bloom/document/font_id.hpp>

#include <bloom/core/safe_parse.hpp>
#include <bloom/core/utf8.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace bloom::document {
namespace {

constexpr std::string_view kHex = "0123456789ABCDEF";

[[nodiscard]] bool needsEscape(const unsigned char byte) noexcept {
    return byte == '%' || byte == ';' || byte == '=' || byte == '|' || byte == ':' || byte < 0x20 ||
           byte == 0x7f;
}

void appendEscaped(std::string& out, const std::string_view value) {
    for (const char raw : value) {
        const auto byte = static_cast<unsigned char>(raw);
        if (needsEscape(byte)) {
            out += '%';
            out += kHex[byte >> 4U];
            out += kHex[byte & 0x0fU];
        } else {
            out += raw;
        }
    }
}

[[nodiscard]] int hexValue(const char c) noexcept {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return -1;
}

[[nodiscard]] std::optional<std::string> unescape(const std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] != '%') {
            out += text[index];
            continue;
        }
        if (index + 2 >= text.size())
            return std::nullopt;
        const int high = hexValue(text[index + 1]);
        const int low = hexValue(text[index + 2]);
        if (high < 0 || low < 0)
            return std::nullopt;
        out += static_cast<char>(high * 16 + low);
        index += 2;
    }
    return out;
}

[[nodiscard]] std::string formatNumber(const double value) {
    return core::formatScalar(value, std::int32_t{6}, true, std::int32_t{0});
}

[[nodiscard]] bool validTag(const std::string_view tag) noexcept {
    return tag.size() == 4 && std::ranges::all_of(tag, [](const char c) {
               return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
           });
}

[[nodiscard]] bool validVersion(const std::string_view version) noexcept {
    return version.size() <= 32 && std::ranges::all_of(version, [](const char c) {
               return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                      c == '.' || c == '-' || c == '_';
           });
}

} // namespace

std::string_view fontSourceName(const FontSource source) noexcept {
    switch (source) {
    case FontSource::System:
        return "system";
    case FontSource::Google:
        return "google";
    case FontSource::Builtin:
        return "builtin";
    }
    return "builtin";
}

std::optional<FontSource> fontSourceFromName(const std::string_view name) noexcept {
    if (name == "system")
        return FontSource::System;
    if (name == "google")
        return FontSource::Google;
    if (name == "builtin")
        return FontSource::Builtin;
    return std::nullopt;
}

bool isValidFontId(const FontId& id) noexcept {
    if (id.family.empty() || id.family.size() > kMaximumFontFamilyBytes ||
        id.style.size() > kMaximumFontStyleBytes || id.postscriptName.size() > 128 ||
        !validVersion(id.familyVersion) || id.weight < 1 || id.weight > 1000 ||
        id.axes.size() > kMaximumFontAxes || !core::isValidUtf8(id.family) ||
        !core::isValidUtf8(id.style) || !core::isValidUtf8(id.postscriptName))
        return false;
    for (std::size_t index = 0; index < id.axes.size(); ++index) {
        const auto& axis = id.axes[index];
        if (!validTag(axis.tag) || !std::isfinite(axis.value) || std::abs(axis.value) > 1.0e6)
            return false;
        if (index > 0 && !(id.axes[index - 1].tag < axis.tag))
            return false;
    }
    return true;
}

std::string encodeFontId(const FontId& id) {
    std::string text = "source=";
    text += fontSourceName(id.source);
    text += ";family=";
    appendEscaped(text, id.family);
    if (!id.style.empty()) {
        text += ";style=";
        appendEscaped(text, id.style);
    }
    text += ";weight=" + std::to_string(id.weight);
    text += id.italic ? ";italic=true" : ";italic=false";
    if (!id.axes.empty()) {
        text += ";axes=";
        for (std::size_t index = 0; index < id.axes.size(); ++index) {
            if (index > 0)
                text += '|';
            appendEscaped(text, id.axes[index].tag);
            text += ':';
            text += formatNumber(id.axes[index].value);
        }
    }
    if (!id.postscriptName.empty()) {
        text += ";ps=";
        appendEscaped(text, id.postscriptName);
    }
    if (!id.familyVersion.empty()) {
        text += ";version=";
        appendEscaped(text, id.familyVersion);
    }
    return text;
}

std::optional<FontId> decodeFontId(const std::string_view text) {
    if (text.empty() || text.size() > 4096 || !text.starts_with("source="))
        return std::nullopt;
    FontId id;
    bool haveSource = false;
    bool haveFamily = false;
    std::size_t start = 0;
    while (start <= text.size()) {
        auto end = text.find(';', start);
        if (end == std::string_view::npos)
            end = text.size();
        const auto member = text.substr(start, end - start);
        start = end + 1;
        if (member.empty())
            continue;
        const auto equals = member.find('=');
        if (equals == std::string_view::npos)
            return std::nullopt;
        const auto key = member.substr(0, equals);
        const auto raw = member.substr(equals + 1);
        auto value = unescape(raw);
        if (!value)
            return std::nullopt;
        if (key == "source") {
            const auto source = fontSourceFromName(*value);
            if (!source)
                return std::nullopt;
            id.source = *source;
            haveSource = true;
        } else if (key == "family") {
            id.family = *std::move(value);
            haveFamily = true;
        } else if (key == "style") {
            id.style = *std::move(value);
        } else if (key == "weight") {
            std::int32_t weight = 0;
            const auto parsed = std::from_chars(value->data(), value->data() + value->size(), weight);
            if (parsed.ec != std::errc{} || parsed.ptr != value->data() + value->size())
                return std::nullopt;
            id.weight = weight;
        } else if (key == "italic") {
            if (*value != "true" && *value != "false")
                return std::nullopt;
            id.italic = *value == "true";
        } else if (key == "axes") {
            std::size_t axisStart = 0;
            while (axisStart <= raw.size() && !raw.empty()) {
                auto axisEnd = raw.find('|', axisStart);
                if (axisEnd == std::string_view::npos)
                    axisEnd = raw.size();
                const auto axis = raw.substr(axisStart, axisEnd - axisStart);
                axisStart = axisEnd + 1;
                const auto colon = axis.find(':');
                if (colon == std::string_view::npos)
                    return std::nullopt;
                auto tag = unescape(axis.substr(0, colon));
                const auto number = axis.substr(colon + 1);
                const auto parsedValue = core::parseScalar(number);
                if (!tag || !parsedValue)
                    return std::nullopt;
                id.axes.push_back({*std::move(tag), *parsedValue});
            }
        } else if (key == "ps") {
            id.postscriptName = *std::move(value);
        } else if (key == "version") {
            id.familyVersion = *std::move(value);
        }
        // Any other member belongs to a newer Bloom and is ignored.
    }
    if (!haveSource || !haveFamily || !isValidFontId(id))
        return std::nullopt;
    return id;
}

std::string fontDisplayName(const FontId& id) {
    return id.style.empty() ? id.family : id.family + " " + id.style;
}

const FontId& defaultFontId() {
    static const FontId id{FontSource::Builtin, "DejaVu Sans", "Book", 400, false, {}, {}, {}};
    return id;
}

} // namespace bloom::document
