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

    expectations.expect(catalogue.available(), "fontconfig catalogue is available");
    expectations.expect(catalogue.faces.size() >= 4, "embedded faces are always present");
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
