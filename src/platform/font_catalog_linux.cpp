#include <bloom/platform/font_catalog.hpp>

#include <bloom/core/sha256.hpp>

#include <fontconfig/fontconfig.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <fstream>
#include <limits>
#include <string>
#include <unordered_set>

namespace bloom::platform::detail {
namespace {

constexpr std::uintmax_t kMaximumFontBytes = static_cast<std::uintmax_t>(64) * 1024U * 1024U;

[[nodiscard]] std::optional<core::Sha256Digest> digestFile(const std::filesystem::path& path) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size == 0 || size > kMaximumFontBytes)
        return std::nullopt;
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        return std::nullopt;
    core::Sha256Hasher hasher;
    std::array<std::byte, static_cast<std::size_t>(64) * 1024U> buffer{};
    std::uintmax_t remaining = size;
    while (remaining != 0) {
        const auto requested = static_cast<std::streamsize>(
            std::min<std::uintmax_t>(remaining, static_cast<std::uintmax_t>(buffer.size())));
        stream.read(reinterpret_cast<char*>(buffer.data()), requested);
        if (stream.gcount() != requested)
            return std::nullopt;
        if (!hasher.update(
                std::span<const std::byte>(buffer.data(), static_cast<std::size_t>(requested))))
            return std::nullopt;
        remaining -= static_cast<std::uintmax_t>(requested);
    }
    return hasher.finalize();
}

[[nodiscard]] std::string patternString(FcPattern* pattern, const char* object) {
    FcChar8* value = nullptr;
    return FcPatternGetString(pattern, object, 0, &value) == FcResultMatch && value != nullptr
               ? reinterpret_cast<const char*>(value)
               : std::string{};
}

[[nodiscard]] int patternInteger(FcPattern* pattern, const char* object, const int fallback) {
    int value = fallback;
    return FcPatternGetInteger(pattern, object, 0, &value) == FcResultMatch ? value : fallback;
}

} // namespace

FontCatalogue enumerateSystemFonts(const FontCatalogueProvider::Cancellation& cancelled,
                                   const bool digests) {
    FontCatalogue result;
    result.status = FontCatalogueStatus::Available;
    if (!FcInitLoadConfigAndFonts())
        return result;
    // An empty pattern matches every font. (The name "*" is a family called "*" in current
    // fontconfig releases and matches none.)
    FcPattern* query = FcPatternCreate();
    FcObjectSet* objects = FcObjectSetBuild(FC_FAMILY, FC_STYLE, FC_WEIGHT, FC_WIDTH, FC_SLANT,
                                            FC_FILE, FC_INDEX, FC_POSTSCRIPT_NAME, nullptr);
    if (query == nullptr || objects == nullptr) {
        if (objects != nullptr)
            FcObjectSetDestroy(objects);
        if (query != nullptr)
            FcPatternDestroy(query);
        return result;
    }
    FcFontSet* set = FcFontList(nullptr, query, objects);
    FcObjectSetDestroy(objects);
    FcPatternDestroy(query);
    if (set == nullptr)
        return result;

    for (int index = 0; index < set->nfont; ++index) {
        if (cancelled && cancelled())
            break;
        auto* pattern = set->fonts[index];
        const auto family = patternString(pattern, FC_FAMILY);
        const auto style = patternString(pattern, FC_STYLE);
        const auto file = patternString(pattern, FC_FILE);
        if (family.empty() || style.empty() || file.empty())
            continue;
        const std::filesystem::path path(file);
        std::optional<core::Sha256Digest> digest = core::Sha256Digest{};
        if (digests)
            digest = digestFile(path);
        else {
            std::error_code error;
            if (!std::filesystem::is_regular_file(path, error) || error)
                continue;
        }
        if (!digest.has_value())
            continue;
        // One file can expose multiple faces. The digest is the file identity; retain each face
        // index while avoiding duplicate pattern rows from fontconfig's aliases.
        const auto faceIndex = patternInteger(pattern, FC_INDEX, 0);
        const auto slant = patternInteger(pattern, FC_SLANT, FC_SLANT_ROMAN);
        const auto weight = patternInteger(pattern, FC_WEIGHT, FC_WEIGHT_NORMAL);
        const auto width = patternInteger(pattern, FC_WIDTH, FC_WIDTH_NORMAL);
        // Fontconfig's weight scale (regular 80, bold 200) is not the CSS/OpenType one (400, 700)
        // that the catalogue records; its own conversion keeps a face's weight inside 1..1000.
        const auto normalizedWeight = FcWeightToOpenType(weight);
        const auto normalizedWidth =
            width >= FC_WIDTH_ULTRACONDENSED && width <= FC_WIDTH_ULTRAEXPANDED
                ? 50 + (width - FC_WIDTH_ULTRACONDENSED) * 25
                : width;
        result.faces.push_back({family, style, normalizedWeight, normalizedWidth,
                                slant == FC_SLANT_ITALIC || slant == FC_SLANT_OBLIQUE, path,
                                *digest, FontSource::System,
                                static_cast<std::uint32_t>(std::max(faceIndex, 0)),
                                patternString(pattern, FC_POSTSCRIPT_NAME)});
    }
    FcFontSetDestroy(set);
    return result;
}

} // namespace bloom::platform::detail
