#include <bloom/platform/font_catalog.hpp>

#include <cstdlib>
#include <vector>

#if !defined(_WIN32)
#error "font_catalog_windows.cpp requires Windows"
#endif

namespace bloom::platform::detail {

// The installed fonts are the files in the system font folder and in the per-user one that Windows
// 10 and later offer; each is read for its names and style (FontCatalogueProvider::
// enumerateDirectories). No GDI or DirectWrite handle is needed, so the scan runs on any worker.
FontCatalogue enumerateSystemFonts(const FontCatalogueProvider::Cancellation& cancelled,
                                   const bool digests) {
    std::vector<std::filesystem::path> directories;
    if (const auto* windows = std::getenv("WINDIR")) // NOLINT(concurrency-mt-unsafe)
        directories.emplace_back(std::filesystem::path(windows) / "Fonts");
    else
        directories.emplace_back("C:\\Windows\\Fonts");
    if (const auto* local = std::getenv("LOCALAPPDATA")) // NOLINT(concurrency-mt-unsafe)
        directories.emplace_back(std::filesystem::path(local) / "Microsoft" / "Windows" / "Fonts");
    return FontCatalogueProvider::enumerateDirectories(directories, cancelled, digests);
}

} // namespace bloom::platform::detail
