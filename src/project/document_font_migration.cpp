// Fonts are not assets (docs/architecture/fonts.md). Opening a document that still has Font assets,
// or a text font stored as an asset id, a face number or a "family|style|digest" reference, turns
// each into the font ID the text node now stores and drops the Font assets. The picture is the same:
// an embedded face stays the same built-in face, and an installed font is looked up again by its
// family and style.

#include <bloom/project/document_decode.hpp>

#include <bloom/document/font_id.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/document/parameter_enums.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <string>
#include <unordered_map>

namespace bloom::project {
namespace {

using document::FontId;
using document::FontSource;

struct EmbeddedFace final {
    std::string_view family;
    std::string_view style;
    std::int32_t weight;
    std::string_view digest;
};
constexpr std::array<EmbeddedFace, 4> kEmbedded{{
    {"DejaVu Sans", "Book", 400, "7da195a74c55bef988d0d48f9508bd5d849425c1770dba5d7bfc6ce9ed848954"},
    {"Inter", "Regular", 400, "40d692fce188e4471e2b3cba937be967878f631ad3ebbbdcd587687c7ebe0c82"},
    {"Inter", "Medium", 500, "97ad806f526e41546d46365bb3a393145f75b7b1568913db74549ad8b8dba872"},
    {"Inter", "SemiBold", 600, "78a843fade9d4612a5567302fb595b56976eb5fcebf4fea5a5912d638bafcde3"},
}};

[[nodiscard]] std::string lower(const std::string_view text) {
    std::string out(text);
    std::ranges::transform(out, out.begin(),
                           [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

[[nodiscard]] bool has(const std::string& text, const std::string_view part) {
    return text.find(part) != std::string::npos;
}

// The weight and slope a style name states ("Bold Italic" -> 700, italic).
void readStyle(const std::string_view style, FontId& id) {
    const auto text = lower(style);
    id.italic = has(text, "italic") || has(text, "oblique");
    id.weight = has(text, "thin") || has(text, "hairline")      ? 100
                : has(text, "extralight") || has(text, "ultralight") ? 200
                : has(text, "light")                            ? 300
                : has(text, "semibold") || has(text, "demibold") ? 600
                : has(text, "extrabold") || has(text, "ultrabold") ? 800
                : has(text, "black") || has(text, "heavy")      ? 900
                : has(text, "bold")                             ? 700
                : has(text, "medium")                           ? 500
                                                                : 400;
}

[[nodiscard]] FontId fontFor(const std::string_view family, const std::string_view style,
                             const bool builtin) {
    FontId id;
    id.source = builtin ? FontSource::Builtin : FontSource::System;
    id.family = std::string(family);
    id.style = std::string(style);
    readStyle(style, id);
    return id;
}

[[nodiscard]] FontId embeddedFont(const std::size_t index) {
    const auto& face = kEmbedded[index];
    FontId id = fontFor(face.family, face.style, true);
    id.weight = face.weight;
    return id;
}

// "family|style|digest": the reference an earlier build stored.
[[nodiscard]] std::optional<FontId> fromLegacyReference(const std::string_view text) {
    const auto first = text.find('|');
    const auto second = first == std::string_view::npos ? first : text.find('|', first + 1);
    if (second == std::string_view::npos || first == 0)
        return std::nullopt;
    const auto family = text.substr(0, first);
    const auto style = text.substr(first + 1, second - first - 1);
    const auto digest = text.substr(second + 1);
    for (std::size_t index = 0; index < kEmbedded.size(); ++index)
        if (kEmbedded[index].digest == digest)
            return embeddedFont(index);
    return fontFor(family, style, false);
}

[[nodiscard]] std::optional<FontId>
fontForAsset(const document::AssetRecord& asset) {
    if (asset.fontFamily.empty())
        return std::nullopt;
    if (asset.locator.portability == "builtin" && asset.fontIndex < kEmbedded.size() &&
        kEmbedded[asset.fontIndex].family == asset.fontFamily)
        return embeddedFont(asset.fontIndex);
    return fontFor(asset.fontFamily, asset.fontStyle, false);
}

} // namespace

void migrateLegacyFontAssets(DecodedDocumentEnvelope& envelope) {
    std::unordered_map<std::uint64_t, FontId> byAsset;
    for (const auto& asset : envelope.assets)
        if (asset.kind == document::AssetKind::Font)
            if (auto id = fontForAsset(asset))
                byAsset.emplace(asset.id.value(), *std::move(id));

    for (auto& composition : envelope.compositions)
        for (auto& record : composition.parameters) {
            if (record.schemaKey != document::kTextFontParameterSchemaKey)
                continue;
            auto* constant = std::get_if<document::ConstantValueSource>(&record.source);
            if (constant == nullptr)
                continue;
            std::optional<FontId> converted;
            if (const auto* face = std::get_if<std::int64_t>(&constant->value)) {
                // An embedded face number of the first font-aware documents.
                converted = embeddedFont(*face >= 0 && *face < 4 ? static_cast<std::size_t>(*face) : 0);
            } else if (const auto* text = std::get_if<std::string>(&constant->value)) {
                if (document::decodeFontId(*text))
                    continue;
                std::uint64_t raw = 0;
                const auto parsed = std::from_chars(text->data(), text->data() + text->size(), raw);
                if (parsed.ec == std::errc{} && parsed.ptr == text->data() + text->size()) {
                    if (const auto found = byAsset.find(raw); found != byAsset.end())
                        converted = found->second;
                } else {
                    converted = fromLegacyReference(*text);
                }
                // Anything else drew in the built-in face before, and still does.
                if (!converted)
                    converted = embeddedFont(0);
            }
            if (converted)
                constant->value = document::encodeFontId(*converted);
        }

    std::erase_if(envelope.assets, [](const document::AssetRecord& asset) {
        return asset.kind == document::AssetKind::Font;
    });
    std::erase_if(envelope.dataBlocks, [&](const document::DataBlockRecord& block) {
        return block.kind == document::DataBlockKind::Font;
    });
}

} // namespace bloom::project
