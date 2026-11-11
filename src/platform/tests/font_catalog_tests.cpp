#include <bloom/platform/font_catalog.hpp>

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
    setenv("FONTCONFIG_FILE", config.c_str(), 1);
    const auto catalogue = bloom::platform::FontCatalogueProvider{}.enumerate();
    if (previous == nullptr)
        unsetenv("FONTCONFIG_FILE");
    else
        setenv("FONTCONFIG_FILE", previousValue.c_str(), 1);
    std::filesystem::remove_all(root, error);

    expectations.expect(catalogue.available(), "the installed-font catalogue is available");
    expectations.expect(catalogue.faces.size() >= 4, "embedded faces are always present");
#if defined(__APPLE__)
    // The macOS catalogue lists what Core Text reports, not a fontconfig database, so the fixture
    // folder above does not reach it. Helvetica is part of every macOS installation.
    constexpr const char* kInstalledFamily = "Helvetica";
    constexpr std::size_t kInstalledFaces = 1;
    constexpr const char* kListed = "installed fonts are listed from the system's font database";
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

    // The directory reader (the Windows catalogue) names the Inter fixtures from their sfnt tables.
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
