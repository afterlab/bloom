#include <bloom/core/environment.hpp>
#include <bloom/platform/font_catalog.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

struct Expectations final {
    int failures = 0;
    void expect(const bool condition, const char* message) {
        if (!condition) {
            ++failures;
            std::fprintf(stderr, "FAIL: %s\n", message);
        }
    }
};

} // namespace

int main() {
    Expectations expectations;
    const auto root = std::filesystem::temp_directory_path() / "bloom-font-catalog-test";
    std::error_code error;
    std::filesystem::create_directories(root, error);
    const auto config = root / "fonts.conf";
    {
        std::ofstream output(config);
        output << "<?xml version=\"1.0\"?><fontconfig><reset-dirs/><dir>"
               << std::filesystem::path(BLOOM_FONT_FIXTURE_DIRECTORY).generic_string()
               << "</dir></fontconfig>";
    }
    const auto* previous = std::getenv("FONTCONFIG_FILE");
    const std::string previousValue = previous == nullptr ? std::string{} : previous;
    static_cast<void>(
        bloom::core::setEnvironmentVariable("FONTCONFIG_FILE", config.string().c_str()));
    const auto catalogue = bloom::platform::FontCatalogueProvider{}.enumerate();
    if (previous == nullptr)
        static_cast<void>(bloom::core::unsetEnvironmentVariable("FONTCONFIG_FILE"));
    else
        static_cast<void>(
            bloom::core::setEnvironmentVariable("FONTCONFIG_FILE", previousValue.c_str()));
    std::filesystem::remove_all(root, error);

    expectations.expect(catalogue.available(), "the installed-font catalogue is available");
    expectations.expect(catalogue.faces.size() >= 4, "embedded faces are always present");
#if defined(__APPLE__)
    // The macOS catalogue lists what Core Text reports, not a fontconfig database, so the fixture
    // folder above does not reach it. Helvetica is part of every macOS installation.
    constexpr const char* kInstalledFamily = "Helvetica";
    constexpr std::size_t kInstalledFaces = 1;
    constexpr const char* kListed = "installed fonts are listed from the system's font database";
#elif defined(_WIN32)
    // The Windows catalogue lists the system font collection, not a fontconfig database. Arial is
    // part of every Windows installation.
    constexpr const char* kInstalledFamily = "Arial";
    constexpr std::size_t kInstalledFaces = 1;
    constexpr const char* kListed = "installed fonts are listed from the system's font collection";
#else
    // The installed-font listing returns the fonts fontconfig knows (here, the fixture folder), not
    // only the embedded ones.
    constexpr const char* kInstalledFamily = "Inter";
    constexpr std::size_t kInstalledFaces = 3;
    constexpr const char* kListed = "installed fonts are listed from the fontconfig database";
#endif
    std::size_t systemFaces = 0;
    for (const auto& face : catalogue.faces)
        if (face.source == bloom::platform::FontSource::System &&
            face.family.starts_with(kInstalledFamily))
            ++systemFaces;
    expectations.expect(systemFaces >= kInstalledFaces, kListed);
    // Weights are the CSS/OpenType ones (a fontconfig regular is 400, not its own 80).
    bool weightsInRange = systemFaces > 0;
    for (const auto& face : catalogue.faces)
        if (face.source == bloom::platform::FontSource::System &&
            face.family.starts_with(kInstalledFamily) && (face.weight < 100 || face.weight > 900))
            weightsInRange = false;
    expectations.expect(weightsInRange,
                        "installed faces report OpenType weights between 100 and 900");
    if (catalogue.faces.size() >= 4) {
        expectations.expect(catalogue.faces[0].source == bloom::platform::FontSource::Embedded,
                            "embedded faces sort before system faces");
        expectations.expect(catalogue.faces[0].family == "DejaVu Sans",
                            "DejaVu Sans is the first embedded face");
    }

#if defined(_WIN32)
    // A font a user installed for themselves is in the system collection too. The test installs
    // nothing: where the per-user font folder holds a font, the catalogue must list a face from it.
    if (const auto* local = std::getenv("LOCALAPPDATA")) { // NOLINT(concurrency-mt-unsafe)
        const auto lower = [](std::string text) {
            for (auto& c : text)
                c = static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
            return text;
        };
        const auto folder = std::filesystem::path(local) / "Microsoft" / "Windows" / "Fonts";
        std::vector<std::string> installed;
        std::error_code scanError;
        for (std::filesystem::directory_iterator entry(folder, scanError), end;
             !scanError && entry != end; entry.increment(scanError)) {
            const auto extension = lower(entry->path().extension().string());
            if (extension == ".ttf" || extension == ".otf" || extension == ".ttc")
                installed.push_back(lower(entry->path().generic_string()));
        }
        if (installed.empty()) {
            std::puts("SKIP: no per-user font is installed for this account");
        } else {
            bool listed = false;
            for (const auto& face : catalogue.faces)
                if (face.source == bloom::platform::FontSource::System &&
                    std::find(installed.begin(), installed.end(),
                              lower(face.path.generic_string())) != installed.end())
                    listed = true;
            expectations.expect(listed, "a font installed for the user alone is listed");
        }
    }
    // Every system face names an existing file and a width in the catalogue's range.
    bool filesExist = true;
    for (const auto& face : catalogue.faces)
        if (face.source == bloom::platform::FontSource::System &&
            (!std::filesystem::is_regular_file(face.path) || face.width < 50 || face.width > 200))
            filesExist = false;
    expectations.expect(filesExist, "every system face names its font file");
#endif

    // The directory reader names the Inter fixtures from their sfnt tables; the Windows catalogue
    // uses the same reader on the files DirectWrite lists.
    const std::array<std::filesystem::path, 1> fixtures{
        std::filesystem::path(BLOOM_FONT_FIXTURE_DIRECTORY)};
    const auto direct = bloom::platform::FontCatalogueProvider::enumerateDirectories(fixtures);
    bool medium = false;
    bool semiBold = false;
    for (const auto& face : direct.faces) {
        if (face.family == "Inter" && face.style == "Medium" && face.weight == 500 &&
            face.postscriptName == "Inter-Medium" && !face.italic)
            medium = true;
        if (face.family == "Inter" && face.style == "SemiBold" && face.weight == 600)
            semiBold = true;
    }
    expectations.expect(direct.available() && medium && semiBold,
                        "the directory reader names Inter Medium and SemiBold from their tables");
    const auto fast = bloom::platform::FontCatalogueProvider::enumerateDirectories(
        fixtures, {}, false);
    expectations.expect(!fast.faces.empty() &&
                            fast.faces.front().contentDigest == bloom::core::Sha256Digest{},
                        "a scan without digests leaves them empty");
    expectations.expect(bloom::platform::FontCatalogueProvider::digestFile(
                            fixtures[0] / "Inter-Medium.ttf")
                            .has_value(),
                        "a font file has a content digest");
    const auto bytes = std::vector<std::uint8_t>(16, 0);
    expectations.expect(bloom::platform::readSfntFaceNames(bytes).empty(),
                        "bytes that are not a font name no face");
    return expectations.failures;
}
