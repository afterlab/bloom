#pragma once

#include <bloom/runtime/font_fallback.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bloom::runtime::fonts {

struct GoogleFontStyle final {
    std::int32_t weight = 400;
    bool italic = false;

    friend bool operator==(const GoogleFontStyle&, const GoogleFontStyle&) = default;
};

struct GoogleFontAxis final {
    std::string tag;
    double minimum = 0.0;
    double maximum = 0.0;

    friend bool operator==(const GoogleFontAxis&, const GoogleFontAxis&) = default;
};

// One family of the baked index: no font file, only what browsing needs.
struct GoogleFamily final {
    std::string family;
    FontCategory category = FontCategory::Sans;
    // The family version at the index date (for example "v51"); empty when unknown.
    std::string version;
    std::vector<GoogleFontStyle> styles;
    // Variable axes, including `wght` and `wdth` when the family has them.
    std::vector<GoogleFontAxis> axes;

    [[nodiscard]] const GoogleFontAxis* axis(std::string_view tag) const noexcept;
    [[nodiscard]] bool hasItalic() const noexcept;
};

// Where the index came from: the day it was generated and the revision of the public font
// repository it was read from. Both are recorded in the release it ships with.
struct GoogleCatalogueInfo final {
    std::string indexDate;
    std::string sourceRevision;
};

// The Google Fonts index baked into this build (src/runtime/google_catalogue_data.cpp, regenerated
// by tools/fonts/generate_google_catalogue.py before a release). Bloom never fetches the family
// list at run time, so browsing works offline and needs no key or account.
class GoogleCatalogue final {
  public:
    GoogleCatalogue() = default;
    // Parses the index text: `#key=value` header lines, then one tab-separated line per family
    // (family, category, version, styles such as `400,400i,700`, axes such as `wght:100:900`).
    [[nodiscard]] static GoogleCatalogue parse(std::string_view text);

    [[nodiscard]] std::span<const GoogleFamily> families() const noexcept { return families_; }
    // Case-insensitive on the family name.
    [[nodiscard]] const GoogleFamily* find(std::string_view family) const noexcept;
    [[nodiscard]] const GoogleCatalogueInfo& info() const noexcept { return info_; }

  private:
    std::vector<GoogleFamily> families_;
    GoogleCatalogueInfo info_;
};

[[nodiscard]] const GoogleCatalogue& googleCatalogue();

namespace detail {
// The generated data: the index text in pieces, each short enough for every compiler's string
// literal limit.
[[nodiscard]] std::span<const std::string_view> googleCatalogueChunks() noexcept;
} // namespace detail

} // namespace bloom::runtime::fonts
