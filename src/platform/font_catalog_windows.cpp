#include <bloom/platform/font_catalog.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if !defined(_WIN32)
#error "font_catalog_windows.cpp requires Windows"
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <dwrite_3.h>
#include <wrl/client.h>

namespace bloom::platform::detail {
namespace {

using Microsoft::WRL::ComPtr;

constexpr std::uintmax_t kMaximumFontBytes = static_cast<std::uintmax_t>(64) * 1024U * 1024U;

// What one font file contributes: its faces as the shared sfnt reader names them, and its digest.
struct ParsedFile final {
    std::vector<SfntFaceNames> faces;
    std::optional<core::Sha256Digest> digest;
};

[[nodiscard]] ParsedFile parseFile(const std::filesystem::path& path, const bool digests) {
    ParsedFile parsed;
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size == 0 || size > kMaximumFontBytes)
        return parsed;
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    std::ifstream stream(path, std::ios::binary);
    if (!stream || !stream.read(reinterpret_cast<char*>(bytes.data()),
                                static_cast<std::streamsize>(bytes.size())))
        return parsed;
    parsed.faces = readSfntFaceNames(bytes);
    if (digests)
        parsed.digest = core::Sha256Hasher::hash(std::as_bytes(std::span(bytes)));
    else
        parsed.digest = core::Sha256Digest{};
    return parsed;
}

[[nodiscard]] std::string toUtf8(const std::wstring& wide) {
    if (wide.empty())
        return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (length <= 0)
        return {};
    std::string result(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), result.data(),
                        length, nullptr, nullptr);
    return result;
}

// The English name of a localized string list, or its first entry when it has no English one.
[[nodiscard]] std::string englishName(IDWriteLocalizedStrings* strings) {
    if (strings == nullptr || strings->GetCount() == 0)
        return {};
    UINT32 index = 0;
    BOOL exists = FALSE;
    if (FAILED(strings->FindLocaleName(L"en-us", &index, &exists)) || exists == FALSE)
        index = 0;
    UINT32 length = 0;
    if (FAILED(strings->GetStringLength(index, &length)))
        return {};
    std::wstring wide(static_cast<std::size_t>(length) + 1, L'\0');
    if (FAILED(strings->GetString(index, wide.data(), length + 1)))
        return {};
    wide.resize(length);
    return toUtf8(wide);
}

// The path of a font file on a local disk; empty for a file DirectWrite holds elsewhere.
[[nodiscard]] std::wstring localPath(IDWriteFontFile* file) {
    ComPtr<IDWriteFontFileLoader> loader;
    if (file == nullptr || FAILED(file->GetLoader(&loader)))
        return {};
    ComPtr<IDWriteLocalFontFileLoader> local;
    if (FAILED(loader.As(&local)))
        return {};
    const void* key = nullptr;
    UINT32 keySize = 0;
    UINT32 length = 0;
    if (FAILED(file->GetReferenceKey(&key, &keySize)) ||
        FAILED(local->GetFilePathLengthFromKey(key, keySize, &length)))
        return {};
    std::wstring path(static_cast<std::size_t>(length) + 1, L'\0');
    if (FAILED(local->GetFilePathFromKey(key, keySize, path.data(), length + 1)))
        return {};
    path.resize(length);
    return path;
}

// DirectWrite's nine stretch steps as the percent widths the other catalogues report.
[[nodiscard]] std::int32_t widthOf(const DWRITE_FONT_STRETCH stretch) noexcept {
    constexpr std::array<std::int32_t, 10> kWidths{100, 50, 62, 75, 87, 100, 112, 125, 150, 200};
    const auto step = static_cast<std::size_t>(stretch);
    return step < kWidths.size() ? kWidths[step] : 100;
}

} // namespace

// The installed fonts are the ones DirectWrite's system font collection lists: the system font
// folder and the fonts a user installed for themselves, without the downloadable ones. DirectWrite
// names the file, the face inside it and the face's weight, width and slant; the shared sfnt reader
// names the face (family, style and PostScript name) from that file, as it does everywhere else, so
// a face has the same names on every platform. No window or device context is needed, so the scan
// runs on any worker.
FontCatalogue enumerateSystemFonts(const FontCatalogueProvider::Cancellation& cancelled,
                                   const bool digests) {
    FontCatalogue result;
    result.status = FontCatalogueStatus::Available;

    ComPtr<IDWriteFactory3> factory;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory3),
                                   reinterpret_cast<IUnknown**>(factory.GetAddressOf()))))
        return result;
    ComPtr<IDWriteFontCollection1> collection;
    if (FAILED(factory->GetSystemFontCollection(FALSE, &collection, FALSE)))
        return result;

    std::unordered_map<std::wstring, ParsedFile> files;
    std::unordered_set<std::wstring> seen;
    const UINT32 familyCount = collection->GetFontFamilyCount();
    for (UINT32 familyIndex = 0; familyIndex < familyCount; ++familyIndex) {
        ComPtr<IDWriteFontFamily1> family;
        if (FAILED(collection->GetFontFamily(familyIndex, &family)))
            continue;
        ComPtr<IDWriteLocalizedStrings> familyNames;
        static_cast<void>(family->GetFamilyNames(&familyNames));
        const UINT32 fontCount = family->GetFontCount();
        for (UINT32 fontIndex = 0; fontIndex < fontCount; ++fontIndex) {
            if (cancelled && cancelled())
                return result;
            ComPtr<IDWriteFont3> font;
            // A simulated bold or oblique is not a face of any file.
            if (FAILED(family->GetFont(fontIndex, &font)) ||
                font->GetSimulations() != DWRITE_FONT_SIMULATIONS_NONE)
                continue;
            ComPtr<IDWriteFontFaceReference> reference;
            ComPtr<IDWriteFontFile> file;
            if (FAILED(font->GetFontFaceReference(&reference)) ||
                FAILED(reference->GetFontFile(&file)))
                continue;
            const auto path = localPath(file.Get());
            if (path.empty())
                continue;
            const UINT32 faceIndex = reference->GetFontFaceIndex();
            if (!seen.insert(path + L'\n' + std::to_wstring(faceIndex)).second)
                continue;
            auto parsed = files.find(path);
            if (parsed == files.end())
                parsed = files.emplace(path, parseFile(std::filesystem::path(path), digests)).first;
            if (!parsed->second.digest.has_value())
                continue;

            FontFace face;
            const auto named =
                std::ranges::find_if(parsed->second.faces, [faceIndex](const SfntFaceNames& names) {
                    return names.faceIndex == faceIndex;
                });
            if (named != parsed->second.faces.end()) {
                face.family = named->family;
                face.style = named->style;
                face.postscriptName = named->postscriptName;
            } else {
                // A format the sfnt reader does not name: DirectWrite's own names.
                face.family = englishName(familyNames.Get());
                ComPtr<IDWriteLocalizedStrings> faceNames;
                if (SUCCEEDED(font->GetFaceNames(&faceNames)))
                    face.style = englishName(faceNames.Get());
                ComPtr<IDWriteLocalizedStrings> postscript;
                BOOL exists = FALSE;
                if (SUCCEEDED(font->GetInformationalStrings(
                        DWRITE_INFORMATIONAL_STRING_POSTSCRIPT_NAME, &postscript, &exists)) &&
                    exists != FALSE)
                    face.postscriptName = englishName(postscript.Get());
            }
            if (face.family.empty() || face.style.empty())
                continue;
            face.weight = std::clamp(static_cast<std::int32_t>(font->GetWeight()), 100, 900);
            face.width = widthOf(font->GetStretch());
            face.italic = font->GetStyle() != DWRITE_FONT_STYLE_NORMAL;
            face.path = std::filesystem::path(path);
            face.contentDigest = *parsed->second.digest;
            face.source = FontSource::System;
            face.faceIndex = faceIndex;
            result.faces.push_back(std::move(face));
        }
    }
    return result;
}

} // namespace bloom::platform::detail
