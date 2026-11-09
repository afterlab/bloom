#pragma once

#include <bloom/document/font_id.hpp>
#include <bloom/runtime/google_catalogue.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bloom::runtime::fonts {

// The keyless stylesheet endpoint. A request without an API key serves TrueType files from the
// font file host, one stylesheet block per requested style, and the file's address carries the
// family version (`/v51/`).
inline constexpr std::string_view kGoogleFontsStylesheetEndpoint =
    "https://fonts.googleapis.com/css2";

// The stylesheet address of one face. `indexed` is the family's index entry when the baked
// catalogue lists it; without one the request names weight and slope only. `subsetText` non-empty
// asks for a file holding just those characters (the picker's name-only preview).
[[nodiscard]] std::string googleStylesheetUrl(const document::FontId& id,
                                              const GoogleFamily* indexed,
                                              std::string_view subsetText = {},
                                              std::string_view endpoint =
                                                  kGoogleFontsStylesheetEndpoint);
// The least specific stylesheet for a family (all that is needed to learn that it exists and which
// version it is at); used when the precise request is refused.
[[nodiscard]] std::string googleFamilyStylesheetUrl(std::string_view family,
                                                    std::string_view endpoint =
                                                        kGoogleFontsStylesheetEndpoint);

// One block of a stylesheet reply.
struct GoogleStylesheetFace final {
    std::string family;
    bool italic = false;
    // A single weight, or the lower bound of a range.
    std::int32_t weight = 400;
    std::int32_t weightMaximum = 400;
    std::string url;
    // The family version read from the address (`v51`), or empty.
    std::string version;
};

[[nodiscard]] std::vector<GoogleStylesheetFace> parseGoogleStylesheet(std::string_view css);

// The block that serves the wanted weight and slope: an exact weight (or a range containing it)
// first, then the nearest weight of the same slope, then any block.
[[nodiscard]] const GoogleStylesheetFace*
pickGoogleStylesheetFace(const std::vector<GoogleStylesheetFace>& faces, std::int32_t weight,
                         bool italic);

// The family version in a font file address: the `vNN` path segment, or the `v=vNN` query value.
[[nodiscard]] std::string googleVersionFromUrl(std::string_view url);

// Whether `url` is an address a stylesheet may point a download at: https on the font file host.
// Anything else in a stylesheet is ignored, so a rewritten reply cannot send Bloom elsewhere.
[[nodiscard]] bool isGoogleFontFileUrl(std::string_view url);

} // namespace bloom::runtime::fonts
