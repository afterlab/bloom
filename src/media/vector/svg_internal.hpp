#pragma once

// Internal to bloom_media_vector: the XML tree, the style cascade and the value parsers the scene
// builder uses. Nothing here is visible outside the library.

#include <bloom/media/svg_scene.hpp>

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace bloom::media::svg {

struct XmlAttribute final {
    std::string name;
    std::string value;
};

struct XmlNode final {
    // The local name of an SVG-namespace element. Foreign elements keep their prefixed name and
    // `foreign` set; the builder skips them with their subtrees.
    std::string tag;
    bool foreign = false;
    std::vector<XmlAttribute> attributes;
    std::size_t parent = 0;
    std::vector<std::size_t> children;
    // Character data directly inside the element (style sheets, text).
    std::string text;

    [[nodiscard]] const std::string* find(std::string_view name) const noexcept;
};

struct XmlDocument final {
    // Node 0 is the document root element.
    std::vector<XmlNode> nodes;
    SvgError error = SvgError::None;
    std::string diagnostic;
};

// Bounded XML reader. Nothing is fetched. Entities declared in the document's own subset are
// expanded once (a declared value may not itself contain a reference), so a document cannot grow
// beyond its own size; external entities are refused.
[[nodiscard]] XmlDocument parseXml(std::span<const std::byte> bytes, const SvgCancel& cancel);

// ---- values ----

struct Declaration final {
    std::string name;
    std::string value;
};

struct CssRule final {
    struct Part final {
        std::string type; // empty: any
        std::string id;
        std::vector<std::string> classes;
        // Attribute conditions: name, optional value (equals), present-only when the value is empty.
        std::vector<std::pair<std::string, std::optional<std::string>>> attributes;
        bool rootOnly = false;
        // The combinator joining this part to the one before it: ' ' or '>'.
        char combinator = ' ';
    };
    std::vector<Part> parts;
    int specificity = 0;
    std::size_t order = 0;
    std::vector<Declaration> declarations;
};

struct StyleSheet final {
    std::vector<CssRule> rules;
    // Selectors and at-rules the reader does not apply, each named once.
    std::vector<std::string> unsupported;
};

// Appends the rules of one style sheet's text.
void parseStyleSheet(std::string_view text, StyleSheet& sheet);
// The declarations of a `style` attribute or a rule body.
[[nodiscard]] std::vector<Declaration> parseDeclarations(std::string_view text);

[[nodiscard]] bool ruleMatches(const CssRule& rule, const XmlDocument& document, std::size_t node);

// A colour spelling: named, #rgb, #rgba, #rrggbb, #rrggbbaa, rgb(), rgba(), hsl(), hsla(). A
// transparent result has alpha 0. `currentColor` and `inherit` are the caller's.
[[nodiscard]] std::optional<core::Color4d> parseColor(std::string_view text);

// A number at the start of `text`; `rest` receives what follows. Rejects NaN and infinities.
[[nodiscard]] std::optional<double> parseNumber(std::string_view text, std::string_view* rest);

struct Length final {
    double value = 0;
    // 0 absolute (value in pixels), 1 percent (value in percent), 2 em, 3 ex.
    int unit = 0;
};
[[nodiscard]] std::optional<Length> parseLength(std::string_view text);

[[nodiscard]] std::string_view trim(std::string_view text) noexcept;
[[nodiscard]] std::string lower(std::string_view text);

// `url(#id)` -> id.
[[nodiscard]] std::optional<std::string> parseUrlReference(std::string_view text,
                                                           std::string_view* rest = nullptr);

[[nodiscard]] std::optional<SvgAffine> parseTransform(std::string_view text);

// Path data to contours in the path's own coordinates. `error` is set for malformed data (the
// contours read up to the error are kept, as the specification asks).
struct PathParse final {
    std::vector<SvgContour> contours;
    bool errorSeen = false;
};
[[nodiscard]] PathParse parsePathData(std::string_view data);

[[nodiscard]] SvgContour ellipseContour(double cx, double cy, double rx, double ry);
[[nodiscard]] SvgContour roundedRectContour(double x, double y, double w, double h, double rx,
                                            double ry);

[[nodiscard]] SvgPoint transformPoint(const SvgAffine& m, SvgPoint p) noexcept;
void transformContour(const SvgAffine& m, SvgContour& contour) noexcept;

} // namespace bloom::media::svg
