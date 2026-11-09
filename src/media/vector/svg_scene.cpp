#include <bloom/media/svg_scene.hpp>
#include <bloom/core/sha256.hpp>

#include "svg_internal.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <set>

namespace bloom::media {
namespace svg {
namespace {

enum class PaintKind : std::uint8_t { None, Color, Url, CurrentColor };

struct PaintSpec final {
    PaintKind kind = PaintKind::None;
    core::Color4d color{0, 0, 0, 1};
    std::string url;
    // What follows url(...): used when the reference cannot be drawn.
    PaintKind fallbackKind = PaintKind::None;
    core::Color4d fallbackColor{0, 0, 0, 1};
};

struct Style final {
    // Inherited.
    PaintSpec fill{PaintKind::Color, {0, 0, 0, 1}, {}, PaintKind::None, {}};
    PaintSpec stroke{};
    double fillOpacity = 1.0;
    double strokeOpacity = 1.0;
    bool fillEvenOdd = false;
    bool clipEvenOdd = false;
    double strokeWidth = 1.0;
    SvgCap cap = SvgCap::Butt;
    SvgJoin join = SvgJoin::Miter;
    double miterLimit = 4.0;
    std::vector<double> dashes;
    double dashOffset = 0.0;
    core::Color4d currentColor{0, 0, 0, 1};
    bool visible = true;
    double fontSize = 16.0;
    int textAnchor = 0; // 0 start, 1 middle, 2 end
    std::string fontFamily;
    bool fontBold = false;
    int fontWeight = 400;
    bool fontItalic = false;
    bool markers = false;
    // Not inherited.
    bool displayNone = false;
    double opacity = 1.0;
    std::string clipPath;
    std::string mask;
    bool filter = false;
    std::string filterValue;
    core::Color4d stopColor{0, 0, 0, 1};
    double stopOpacity = 1.0;
    bool overflowVisible = false;
};

bool isMetadataTag(const std::string& tag) {
    static const std::set<std::string> tags{"title", "desc", "metadata", "style", "stop", "script",
                                            "namedview", "defs", "linearGradient", "radialGradient",
                                            "clipPath", "mask", "marker", "pattern", "symbol",
                                            "filter", "font", "font-face", "glyph", "cursor",
                                            "color-profile", "view", "animate", "set", "animateTransform",
                                            "animateMotion", "mpath", "tspan", "textPath", "tref"};
    return tags.contains(tag);
}

class Builder final {
  public:
    Builder(const XmlDocument& document, SvgScene& scene, const SvgCancel& cancel)
        : doc_(document), scene_(scene), cancel_(cancel) {}

    SvgReadResult run() {
        SvgReadResult result;
        indexDocument();
        const auto& root = doc_.nodes[0];
        // The viewport.
        double viewBox[4]{};
        const bool hasViewBox = parseViewBox(root, viewBox);
        auto lengthAttr = [&](const char* name, double fallback) {
            if (const auto* value = root.find(name)) {
                if (const auto length = parseLength(*value); length && length->unit == 0)
                    return length->value;
                if (const auto length = parseLength(*value); length && length->unit == 2)
                    return length->value * 16.0;
            }
            return fallback;
        };
        double width = lengthAttr("width", hasViewBox ? viewBox[2] : 300.0);
        double height = lengthAttr("height", hasViewBox ? viewBox[3] : 150.0);
        if (hasViewBox && root.find("width") == nullptr && root.find("height") != nullptr && viewBox[3] > 0)
            width = height * viewBox[2] / viewBox[3];
        if (hasViewBox && root.find("height") == nullptr && root.find("width") != nullptr && viewBox[2] > 0)
            height = width * viewBox[3] / viewBox[2];
        if (!(width > 0) || !(height > 0) || width > kMaxSvgExtent || height > kMaxSvgExtent) {
            result.error = SvgError::TooComplex;
            result.diagnostic = "The SVG's size is zero or beyond the supported range.";
            return result;
        }
        scene_.width = width;
        scene_.height = height;
        viewWidth_ = hasViewBox ? viewBox[2] : width;
        viewHeight_ = hasViewBox ? viewBox[3] : height;
        SvgAffine ctm;
        if (hasViewBox)
            ctm = viewBoxTransform(viewBox, width, height, root.find("preserveAspectRatio"));
        collectStyleSheets(0);
        scene_.root.kind = SvgElement::Kind::Group;
        scene_.root.tag = "svg";
        scene_.root.name = label(root, "svg");
        Style rootStyle = computeStyle(0, Style{});
        scene_.root.opacity = rootStyle.opacity;
        try {
            buildChildren(0, rootStyle, ctm, scene_.root, 0);
        } catch (const Refusal& refusal) {
            result.error = refusal.error;
            result.diagnostic = refusal.message;
            return result;
        }
        for (const auto& note : sheet_.unsupported)
            skip("style sheet", "CSS not applied: " + note);
        std::size_t counted = 0;
        for (std::size_t i = 1; i < doc_.nodes.size(); ++i) {
            const auto& node = doc_.nodes[i];
            if (!node.foreign && !isMetadataTag(node.tag) && node.tag != "svg")
                ++counted;
            else if (!node.foreign && (node.tag == "tspan" || node.tag == "svg"))
                ++counted;
        }
        scene_.elementCount = counted + 1;
        if (scene_.shapeCount == 0) {
            result.error = SvgError::Empty;
            result.diagnostic = "The SVG draws nothing that can be imported as a shape.";
            return result;
        }
        return result;
    }

  private:
    struct Refusal final {
        SvgError error;
        std::string message;
    };

    const XmlDocument& doc_;
    SvgScene& scene_;
    const SvgCancel& cancel_;
    StyleSheet sheet_;
    std::unordered_map<std::string, std::size_t> byId_;
    std::vector<std::optional<Style>> memo_;
    double viewWidth_ = 0, viewHeight_ = 0;
    std::size_t visited_ = 0;
    std::size_t useDepth_ = 0;
    std::vector<std::size_t> useStack_;
    std::map<std::string, std::size_t> skippedPerReason_;

    // ---- bookkeeping ----

    void checkCancel() {
        if (++visited_ % 256 != 0 || !cancel_)
            return;
        bool cancelled = false;
        try {
            cancelled = cancel_();
        } catch (...) {
            cancelled = true;
        }
        if (cancelled)
            throw Refusal{SvgError::Cancelled, "The import was cancelled."};
    }
    void skip(std::string name, std::string reason) {
        if (scene_.skipped.size() >= kMaxSvgSkippedNames) {
            ++skippedPerReason_[reason];
            return;
        }
        scene_.skipped.push_back({std::move(name), std::move(reason)});
    }
    void indexDocument() {
        memo_.assign(doc_.nodes.size(), std::nullopt);
        for (std::size_t i = 0; i < doc_.nodes.size(); ++i)
            if (const auto* id = doc_.nodes[i].find("id"); id != nullptr && !id->empty())
                byId_.emplace(*id, i);
    }
    void collectStyleSheets(const std::size_t node) {
        // Iterative over document order.
        for (std::size_t i = 0; i < doc_.nodes.size(); ++i) {
            const auto& n = doc_.nodes[i];
            if (n.foreign || n.tag != "style")
                continue;
            if (const auto* type = n.find("type"); type != nullptr && !type->empty() && *type != "text/css")
                continue;
            parseStyleSheet(n.text, sheet_);
        }
        (void)node;
        std::ranges::stable_sort(sheet_.rules, [](const CssRule& a, const CssRule& b) {
            return a.specificity != b.specificity ? a.specificity < b.specificity : a.order < b.order;
        });
    }
    std::optional<std::size_t> lookup(const std::string& id) const {
        const auto found = byId_.find(id);
        return found == byId_.end() ? std::nullopt : std::optional(found->second);
    }
    // The name an artist can search the file for: the element's id or label when it has one, and
    // otherwise where it sits, as "path 3 in g 2 in 'layer1'" (its number among its parent's
    // children, then the same for the parent, up to the nearest named ancestor or the root).
    std::string located(const XmlNode& node) const {
        if (const auto name = label(node, std::string{}); !name.empty())
            return name;
        const auto index = static_cast<std::size_t>(&node - doc_.nodes.data());
        const auto step = [&](const std::size_t at) {
            std::size_t ordinal = 1;
            if (at != 0) {
                const auto& siblings = doc_.nodes[doc_.nodes[at].parent].children;
                const auto found = std::ranges::find(siblings, at);
                ordinal = found == siblings.end() ? 1 : static_cast<std::size_t>(found - siblings.begin()) + 1;
            }
            return doc_.nodes[at].tag + " " + std::to_string(ordinal);
        };
        std::string text = step(index);
        auto at = index;
        for (int levels = 0; at != 0 && levels < 6; ++levels) {
            const auto parent = doc_.nodes[at].parent;
            if (const auto name = label(doc_.nodes[parent], std::string{}); !name.empty()) {
                text += " in '" + name + "'";
                return text;
            }
            if (parent == 0) {
                text += " in the file's root";
                return text;
            }
            text += " in " + step(parent);
            at = parent;
        }
        return text;
    }
    static const std::string& describe(const SvgElement& element) {
        return element.where.empty() ? element.name : element.where;
    }
    std::string label(const XmlNode& node, const std::string& fallback) const {
        for (const char* key : {"inkscape:label", "data-name", "id"})
            if (const auto* value = node.find(key); value != nullptr && !trim(*value).empty())
                return std::string(trim(*value));
        return fallback;
    }

    // ---- style ----

    static void setPaint(PaintSpec& target, const std::string& value) {
        const auto text = trim(value);
        if (text.empty())
            return;
        if (lower(text) == "none") {
            target = {};
            return;
        }
        if (lower(text) == "currentcolor") {
            target = {};
            target.kind = PaintKind::CurrentColor;
            return;
        }
        std::string_view rest;
        if (const auto url = parseUrlReference(text, &rest)) {
            const auto fallback = trim(rest);
            target = {};
            target.kind = PaintKind::Url;
            target.url = *url;
            if (!fallback.empty()) {
                if (lower(fallback) == "currentcolor")
                    target.fallbackKind = PaintKind::CurrentColor;
                else if (const auto color = parseColor(fallback)) {
                    target.fallbackKind = PaintKind::Color;
                    target.fallbackColor = *color;
                }
            }
            return;
        }
        if (text.size() >= 4 && lower(text.substr(0, 4)) == "url(") {
            // A reference outside this document is never followed.
            target = {};
            return;
        }
        if (const auto color = parseColor(text)) {
            target = {};
            target.kind = PaintKind::Color;
            target.color = *color;
        }
    }

    double lengthPx(const std::string& value, const double percentBase, const double fontSize,
                    const double fallback) const {
        const auto length = parseLength(value);
        if (!length)
            return fallback;
        switch (length->unit) {
        case 1:
            return length->value * percentBase / 100.0;
        case 2:
            return length->value * fontSize;
        case 3:
            return length->value * fontSize / 2.0;
        default:
            return length->value;
        }
    }

    void applyProperty(Style& s, const std::string& name, const std::string& rawValue, const Style& parent) {
        const auto value = std::string(trim(rawValue));
        const bool inherit = lower(value) == "inherit";
        const auto number01 = [&](double& target) {
            std::string_view rest;
            if (const auto n = parseNumber(value, &rest)) {
                target = trim(rest) == "%" ? std::clamp(*n / 100.0, 0.0, 1.0) : std::clamp(*n, 0.0, 1.0);
            }
        };
        if (name == "fill") {
            if (inherit)
                s.fill = parent.fill;
            else
                setPaint(s.fill, value);
        } else if (name == "stroke") {
            if (inherit)
                s.stroke = parent.stroke;
            else
                setPaint(s.stroke, value);
        } else if (name == "fill-opacity")
            number01(s.fillOpacity);
        else if (name == "stroke-opacity")
            number01(s.strokeOpacity);
        else if (name == "opacity") {
            if (inherit)
                s.opacity = parent.opacity;
            else
                number01(s.opacity);
        } else if (name == "fill-rule")
            s.fillEvenOdd = lower(value) == "evenodd";
        else if (name == "clip-rule")
            s.clipEvenOdd = lower(value) == "evenodd";
        else if (name == "stroke-width") {
            const auto width = lengthPx(value, std::sqrt((viewWidth_ * viewWidth_ + viewHeight_ * viewHeight_) / 2.0),
                                        s.fontSize, s.strokeWidth);
            s.strokeWidth = std::max(0.0, width);
        } else if (name == "stroke-linecap") {
            const auto v = lower(value);
            s.cap = v == "round" ? SvgCap::Round : v == "square" ? SvgCap::Square : SvgCap::Butt;
        } else if (name == "stroke-linejoin") {
            const auto v = lower(value);
            s.join = v == "round" ? SvgJoin::Round : (v == "bevel" ? SvgJoin::Bevel : SvgJoin::Miter);
        } else if (name == "stroke-miterlimit") {
            std::string_view rest;
            if (const auto n = parseNumber(value, &rest))
                s.miterLimit = std::max(1.0, *n);
        } else if (name == "stroke-dasharray") {
            s.dashes.clear();
            if (lower(value) != "none") {
                bool ok = true;
                std::vector<double> dashes;
                std::size_t at = 0;
                while (at < value.size() && ok) {
                    while (at < value.size() && (value[at] == ' ' || value[at] == ',' || value[at] == '\t' || value[at] == '\n'))
                        ++at;
                    if (at >= value.size())
                        break;
                    const auto end = value.find_first_of(" ,\t\n", at);
                    const auto token = value.substr(at, end == std::string::npos ? std::string::npos : end - at);
                    at = end == std::string::npos ? value.size() : end;
                    const auto length = parseLength(token);
                    if (!length) {
                        ok = false;
                        break;
                    }
                    double px = length->value;
                    if (length->unit == 1)
                        px = length->value * std::sqrt((viewWidth_ * viewWidth_ + viewHeight_ * viewHeight_) / 2.0) / 100.0;
                    else if (length->unit == 2)
                        px = length->value * s.fontSize;
                    if (px < 0) {
                        ok = false;
                        break;
                    }
                    dashes.push_back(px);
                }
                double sum = 0;
                for (const auto d : dashes)
                    sum += d;
                if (ok && !dashes.empty() && sum > 0) {
                    if (dashes.size() % 2 == 1) {
                        auto copy = dashes;
                        dashes.insert(dashes.end(), copy.begin(), copy.end());
                    }
                    s.dashes = std::move(dashes);
                }
            }
        } else if (name == "stroke-dashoffset") {
            s.dashOffset = lengthPx(value, 0.0, s.fontSize, 0.0);
        } else if (name == "color") {
            if (const auto color = parseColor(value))
                s.currentColor = *color;
        } else if (name == "display") {
            s.displayNone = lower(value) == "none";
        } else if (name == "visibility") {
            const auto v = lower(value);
            if (v == "hidden" || v == "collapse")
                s.visible = false;
            else if (v == "visible")
                s.visible = true;
        } else if (name == "clip-path") {
            s.clipPath = lower(value) == "none" ? std::string{} : value;
        } else if (name == "mask") {
            s.mask = lower(value) == "none" ? std::string{} : value;
        } else if (name == "filter") {
            s.filter = lower(value) != "none" && !value.empty();
            s.filterValue = s.filter ? value : std::string{};
        } else if (name == "stop-color") {
            if (lower(value) == "currentcolor")
                s.stopColor = s.currentColor;
            else if (const auto color = parseColor(value))
                s.stopColor = *color;
        } else if (name == "stop-opacity")
            number01(s.stopOpacity);
        else if (name == "text-anchor") {
            const auto v = lower(value);
            s.textAnchor = v == "middle" ? 1 : v == "end" ? 2 : 0;
        } else if (name == "font-family") {
            // The first family, without its quotes.
            auto family = value.substr(0, value.find(','));
            while (!family.empty() && (family.front() == '\'' || family.front() == '"' || family.front() == ' '))
                family.erase(family.begin());
            while (!family.empty() && (family.back() == '\'' || family.back() == '"' || family.back() == ' '))
                family.pop_back();
            s.fontFamily = family;
        } else if (name == "font-weight") {
            const auto v = lower(value);
            s.fontBold = v == "bold" || v == "bolder" || (!v.empty() && std::isdigit(static_cast<unsigned char>(v[0])) && std::atoi(v.c_str()) >= 600);
            if (v == "normal")
                s.fontWeight = 400;
            else if (v == "bold" || v == "bolder")
                s.fontWeight = 700;
            else if (v == "lighter")
                s.fontWeight = 300;
            else if (!v.empty() && std::isdigit(static_cast<unsigned char>(v[0])))
                s.fontWeight = std::clamp(std::atoi(v.c_str()), 1, 1000);
        } else if (name == "font-style") {
            const auto v = lower(value);
            s.fontItalic = v == "italic" || v.rfind("oblique", 0) == 0;
        } else if (name == "font-size") {
            s.fontSize = std::max(0.0, lengthPx(value, parent.fontSize, parent.fontSize, s.fontSize));
        } else if (name == "overflow") {
            const auto v = lower(value);
            s.overflowVisible = v == "visible" || v == "auto";
        } else if (name == "marker-start" || name == "marker-mid" || name == "marker-end") {
            if (lower(value) != "none" && !value.empty())
                s.markers = true;
        } else if (name == "marker") {
            if (lower(value) != "none" && !value.empty())
                s.markers = true;
        }
    }

    static bool presentationAttribute(const std::string& name) {
        static const std::set<std::string> names{
            "fill", "stroke", "fill-opacity", "stroke-opacity", "opacity", "fill-rule", "clip-rule",
            "stroke-width", "stroke-linecap", "stroke-linejoin", "stroke-miterlimit", "stroke-dasharray",
            "stroke-dashoffset", "color", "display", "visibility", "clip-path", "mask", "filter",
            "stop-color", "stop-opacity", "font-size", "text-anchor", "font-family", "font-weight", "font-style", "overflow", "marker-start", "marker-mid",
            "marker-end", "marker"};
        return names.contains(name);
    }

    Style computeStyle(const std::size_t node, const Style& parent) {
        Style s = parent;
        s.displayNone = false;
        s.opacity = 1.0;
        s.clipPath.clear();
        s.mask.clear();
        s.filter = false;
        s.filterValue.clear();
        s.stopColor = {0, 0, 0, 1};
        s.stopOpacity = 1.0;
        s.overflowVisible = false;
        s.markers = false;
        const auto& n = doc_.nodes[node];
        std::vector<Declaration> declarations;
        for (const auto& attribute : n.attributes)
            if (presentationAttribute(attribute.name))
                declarations.push_back({attribute.name, attribute.value});
        for (const auto& rule : sheet_.rules)
            if (ruleMatches(rule, doc_, node))
                declarations.insert(declarations.end(), rule.declarations.begin(), rule.declarations.end());
        if (const auto* style = n.find("style"))
            for (auto& declaration : parseDeclarations(*style))
                declarations.push_back(std::move(declaration));
        // `color` and `font-size` first, so keywords and em units in later properties resolve.
        for (const auto& d : declarations)
            if (d.name == "color" || d.name == "font-size")
                applyProperty(s, d.name, d.value, parent);
        for (const auto& d : declarations)
            if (d.name != "color" && d.name != "font-size")
                applyProperty(s, d.name, d.value, parent);
        return s;
    }

    // The style a definition (gradient stop, clip child, mask child) has through its own ancestors.
    Style styleThroughAncestors(const std::size_t node) {
        if (memo_[node])
            return *memo_[node];
        std::vector<std::size_t> chain;
        for (auto at = node;; at = doc_.nodes[at].parent) {
            chain.push_back(at);
            if (at == 0)
                break;
        }
        Style s;
        for (auto it = chain.rbegin(); it != chain.rend(); ++it)
            s = computeStyle(*it, s);
        memo_[node] = s;
        return s;
    }

    // ---- geometry helpers ----

    bool parseViewBox(const XmlNode& node, double (&out)[4]) const {
        const auto* value = node.find("viewBox");
        if (value == nullptr)
            return false;
        std::string_view rest = *value;
        for (int i = 0; i < 4; ++i) {
            rest = trim(rest);
            if (!rest.empty() && rest.front() == ',')
                rest = trim(rest.substr(1));
            const auto n = parseNumber(rest, &rest);
            if (!n)
                return false;
            out[i] = *n;
        }
        return out[2] > 0 && out[3] > 0;
    }

    static SvgAffine viewBoxTransform(const double (&vb)[4], const double width, const double height,
                                      const std::string* preserve) {
        std::string align = "xmidymid";
        bool slice = false;
        if (preserve != nullptr) {
            const auto text = lower(trim(*preserve));
            std::size_t pos = 0;
            std::string token;
            std::vector<std::string> tokens;
            while (pos <= text.size()) {
                const auto end = text.find(' ', pos);
                token = text.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
                if (!token.empty())
                    tokens.push_back(token);
                if (end == std::string::npos)
                    break;
                pos = end + 1;
            }
            if (!tokens.empty() && tokens[0] == "defer")
                tokens.erase(tokens.begin());
            if (!tokens.empty())
                align = tokens[0];
            if (tokens.size() > 1)
                slice = tokens[1] == "slice";
        }
        double sx = width / vb[2], sy = height / vb[3];
        if (align == "none")
            return {sx, 0, 0, sy, -vb[0] * sx, -vb[1] * sy};
        const double s = slice ? std::max(sx, sy) : std::min(sx, sy);
        const double extraX = width - vb[2] * s, extraY = height - vb[3] * s;
        double tx = 0, ty = 0;
        if (align.find("xmid") != std::string::npos)
            tx = extraX / 2;
        else if (align.find("xmax") != std::string::npos)
            tx = extraX;
        if (align.find("ymid") != std::string::npos)
            ty = extraY / 2;
        else if (align.find("ymax") != std::string::npos)
            ty = extraY;
        return {s, 0, 0, s, tx - vb[0] * s, ty - vb[1] * s};
    }

    double attrLength(const XmlNode& node, const char* name, const double percentBase, const Style& style,
                      const double fallback) const {
        if (const auto* value = node.find(name))
            return lengthPx(*value, percentBase, style.fontSize, fallback);
        return fallback;
    }

    static std::array<double, 4> bounds(const std::vector<SvgContour>& contours) {
        double minX = 1e300, minY = 1e300, maxX = -1e300, maxY = -1e300;
        const auto add = [&](const SvgPoint p) {
            minX = std::min(minX, p.x);
            minY = std::min(minY, p.y);
            maxX = std::max(maxX, p.x);
            maxY = std::max(maxY, p.y);
        };
        for (const auto& contour : contours) {
            const auto n = contour.anchors.size();
            for (std::size_t i = 0; i < n; ++i) {
                const auto& a = contour.anchors[i];
                add(a.point);
                const bool hasNext = i + 1 < n || contour.closed;
                if (!hasNext)
                    continue;
                const auto& b = contour.anchors[(i + 1) % n];
                if (!a.hasOut && !b.hasIn)
                    continue;
                const SvgPoint p0 = a.point, p1 = a.hasOut ? a.out : a.point, p2 = b.hasIn ? b.in : b.point,
                               p3 = b.point;
                for (int k = 1; k < 8; ++k) {
                    const double t = k / 8.0, u = 1 - t;
                    add({u * u * u * p0.x + 3 * u * u * t * p1.x + 3 * u * t * t * p2.x + t * t * t * p3.x,
                         u * u * u * p0.y + 3 * u * u * t * p1.y + 3 * u * t * t * p2.y + t * t * t * p3.y});
                }
            }
        }
        return {minX, minY, maxX, maxY};
    }

    // ---- gradients ----

    struct GradientAttributes {
        std::map<std::string, std::string> values;
        std::optional<std::size_t> stopsNode;
    };

    GradientAttributes gradientChain(const std::size_t node, const bool radial) {
        GradientAttributes out;
        std::set<std::size_t> seen;
        auto at = std::optional(node);
        while (at && seen.insert(*at).second && seen.size() < 16) {
            const auto& n = doc_.nodes[*at];
            if (n.foreign || (n.tag != "linearGradient" && n.tag != "radialGradient"))
                break;
            const bool sameKind = (n.tag == "radialGradient") == radial;
            for (const auto& attribute : n.attributes) {
                const bool geometric = attribute.name == "x1" || attribute.name == "y1" || attribute.name == "x2" ||
                                       attribute.name == "y2" || attribute.name == "cx" || attribute.name == "cy" ||
                                       attribute.name == "r" || attribute.name == "fx" || attribute.name == "fy";
                if (geometric && !sameKind)
                    continue;
                if (attribute.name == "gradientUnits" || attribute.name == "gradientTransform" ||
                    attribute.name == "spreadMethod" || geometric)
                    out.values.emplace(attribute.name, attribute.value);
            }
            if (!out.stopsNode)
                for (const auto child : n.children)
                    if (!doc_.nodes[child].foreign && doc_.nodes[child].tag == "stop") {
                        out.stopsNode = *at;
                        break;
                    }
            const auto* href = n.find("href");
            if (href == nullptr || href->empty() || (*href)[0] != '#')
                break;
            at = lookup(href->substr(1));
        }
        return out;
    }

    // Returns the paint for a gradient reference, or nullopt (with a note) when it cannot be drawn.
    std::optional<SvgPaint> resolveGradient(const PaintSpec& spec, const double opacity, const SvgAffine& world,
                                            const std::array<double, 4>& box, const Style& style,
                                            std::vector<std::string>& notes, const std::string& elementName,
                                            const bool isStroke) {
        const auto target = lookup(spec.url);
        const auto fallback = [&]() -> std::optional<SvgPaint> {
            if (spec.fallbackKind == PaintKind::None)
                return std::nullopt;
            SvgPaint paint;
            paint.kind = SvgPaint::Kind::Color;
            paint.color = spec.fallbackKind == PaintKind::CurrentColor ? style.currentColor : spec.fallbackColor;
            paint.color.alpha *= opacity;
            return paint;
        };
        if (!target) {
            notes.push_back("paint reference #" + spec.url + " is not in the file" + (isStroke ? " (stroke)" : " (fill)"));
            return fallback();
        }
        const auto& node = doc_.nodes[*target];
        if (node.foreign || (node.tag != "linearGradient" && node.tag != "radialGradient")) {
            if (node.tag == "pattern")
                notes.push_back(std::string("pattern ") + (isStroke ? "stroke" : "fill") + " #" + spec.url +
                                " is not drawn");
            else
                notes.push_back("paint reference #" + spec.url + " is not a gradient");
            return fallback();
        }
        const bool radial = node.tag == "radialGradient";
        auto chain = gradientChain(*target, radial);
        if (!chain.stopsNode) {
            return std::nullopt; // Per the specification, a gradient without stops paints nothing.
        }
        std::vector<SvgGradientStop> stops;
        double previous = 0;
        for (const auto child : doc_.nodes[*chain.stopsNode].children) {
            const auto& stop = doc_.nodes[child];
            if (stop.foreign || stop.tag != "stop")
                continue;
            const auto stopStyle = styleThroughAncestors(child);
            double offset = 0;
            if (const auto* value = stop.find("offset")) {
                std::string_view rest;
                if (const auto n = parseNumber(*value, &rest))
                    offset = trim(rest) == "%" ? *n / 100.0 : *n;
            }
            offset = std::clamp(offset, 0.0, 1.0);
            offset = std::max(offset, previous);
            previous = offset;
            auto color = stopStyle.stopColor;
            color.alpha *= stopStyle.stopOpacity;
            stops.push_back({offset, color});
        }
        if (stops.empty())
            return std::nullopt;
        if (stops.size() == 1) {
            SvgPaint paint;
            paint.kind = SvgPaint::Kind::Color;
            paint.color = stops[0].color;
            paint.color.alpha *= opacity;
            return paint;
        }
        for (auto& stop : stops)
            stop.color.alpha *= opacity;
        const auto get = [&](const char* name) -> const std::string* {
            const auto found = chain.values.find(name);
            return found == chain.values.end() ? nullptr : &found->second;
        };
        const bool boundingBox = !(get("gradientUnits") != nullptr && lower(trim(*get("gradientUnits"))) == "userspaceonuse");
        SvgAffine gradientTransform;
        if (const auto* text = get("gradientTransform")) {
            if (const auto parsed = parseTransform(*text))
                gradientTransform = *parsed;
        }
        SvgAffine matrix = world;
        const double boxW = box[2] - box[0], boxH = box[3] - box[1];
        if (boundingBox) {
            if (!(boxW > 0) || !(boxH > 0)) {
                notes.push_back("a gradient on a zero-width or zero-height shape is not drawn (#" + spec.url + ")");
                return std::nullopt;
            }
            matrix = matrix * SvgAffine{boxW, 0, 0, boxH, box[0], box[1]};
        }
        matrix = matrix * gradientTransform;
        // A coordinate: a number or a percentage (of the bounding box: a fraction; of the
        // viewport: of its width, height or the normalised diagonal).
        const auto coordinate = [&](const char* name, const double fallbackFraction, const int axis) {
            const auto* text = get(name);
            if (text == nullptr)
                return boundingBox ? fallbackFraction
                                   : fallbackFraction * (axis == 0 ? viewWidth_ : axis == 1 ? viewHeight_
                                                                                            : std::sqrt((viewWidth_ * viewWidth_ + viewHeight_ * viewHeight_) / 2.0));
            const auto length = parseLength(*text);
            if (!length)
                return boundingBox ? fallbackFraction : 0.0;
            if (length->unit == 1) {
                const double fraction = length->value / 100.0;
                return boundingBox ? fraction
                                   : fraction * (axis == 0 ? viewWidth_ : axis == 1 ? viewHeight_
                                                                                    : std::sqrt((viewWidth_ * viewWidth_ + viewHeight_ * viewHeight_) / 2.0));
            }
            return length->value * (length->unit == 2 ? style.fontSize : 1.0);
        };
        SvgGradient gradient;
        gradient.radial = radial;
        gradient.stops = std::move(stops);
        if (const auto* spread = get("spreadMethod")) {
            const auto v = lower(trim(*spread));
            gradient.spread = v == "reflect" ? SvgSpread::Reflect : v == "repeat" ? SvgSpread::Repeat : SvgSpread::Pad;
        }
        const double det = matrix.determinant();
        if (!(std::fabs(det) > 1e-18) || !std::isfinite(det)) {
            return std::nullopt;
        }
        if (!radial) {
            const double x1 = coordinate("x1", 0.0, 0), y1 = coordinate("y1", 0.0, 1);
            const double x2 = coordinate("x2", 1.0, 0), y2 = coordinate("y2", 0.0, 1);
            const double vx = x2 - x1, vy = y2 - y1;
            const double lengthSquared = vx * vx + vy * vy;
            if (!(lengthSquared > 0)) {
                SvgPaint paint;
                paint.kind = SvgPaint::Kind::Color;
                paint.color = gradient.stops.back().color;
                return paint;
            }
            // t(world) = n . world + c with n the gradient of t in world space.
            const double nx = (matrix.d * vx - matrix.b * vy) / det / lengthSquared;
            const double ny = (-matrix.c * vx + matrix.a * vy) / det / lengthSquared;
            const double nn = nx * nx + ny * ny;
            const auto origin = matrix.apply(x1, y1);
            gradient.p0 = {origin[0], origin[1]};
            gradient.p1 = {origin[0] + nx / nn, origin[1] + ny / nn};
        } else {
            const double cx = coordinate("cx", 0.5, 0), cy = coordinate("cy", 0.5, 1);
            const double r = coordinate("r", 0.5, 2);
            const auto centre = matrix.apply(cx, cy);
            gradient.center = {centre[0], centre[1]};
            const double meanScale = std::sqrt(std::fabs(det));
            gradient.radius = r * meanScale;
            gradient.userRadius = r;
            gradient.linearA = matrix.a;
            gradient.linearB = matrix.b;
            gradient.linearC = matrix.c;
            gradient.linearD = matrix.d;
            if (const auto* fx = get("fx"); fx != nullptr || get("fy") != nullptr) {
                const double focalX = coordinate("fx", cx, 0), focalY = coordinate("fy", cy, 1);
                if (std::fabs(focalX - cx) > 1e-9 || std::fabs(focalY - cy) > 1e-9) {
                    gradient.approximate = true;
                    gradient.approximation = gradient.approximation.empty()
                                                 ? "a radial gradient's focal point is ignored"
                                                 : gradient.approximation + "; its focal point is ignored";
                }
            }
            if (!(gradient.radius > 0)) {
                SvgPaint paint;
                paint.kind = SvgPaint::Kind::Color;
                paint.color = gradient.stops.back().color;
                return paint;
            }
        }
        if (gradient.approximate)
            notes.push_back("gradient #" + spec.url + ": " + gradient.approximation);
        (void)elementName;
        scene_.gradients.push_back(std::move(gradient));
        SvgPaint paint;
        paint.kind = SvgPaint::Kind::Gradient;
        paint.gradient = scene_.gradients.size() - 1;
        return paint;
    }

    // ---- shapes ----

    std::vector<SvgContour> shapeContours(const XmlNode& n, const Style& style, std::vector<std::string>& notes) {
        std::vector<SvgContour> contours;
        const auto w = viewWidth_, h = viewHeight_;
        const double diagonal = std::sqrt((w * w + h * h) / 2.0);
        if (n.tag == "path") {
            const auto* d = n.find("d");
            if (d == nullptr)
                return contours;
            auto parsed = parsePathData(*d);
            if (parsed.errorSeen)
                notes.push_back("the path data has an error; the part before it is drawn");
            return std::move(parsed.contours);
        }
        if (n.tag == "rect") {
            const double x = attrLength(n, "x", w, style, 0), y = attrLength(n, "y", h, style, 0);
            const double width = attrLength(n, "width", w, style, 0), height = attrLength(n, "height", h, style, 0);
            if (!(width > 0) || !(height > 0))
                return contours;
            double rx = n.find("rx") ? attrLength(n, "rx", w, style, -1) : -1.0;
            double ry = n.find("ry") ? attrLength(n, "ry", h, style, -1) : -1.0;
            if (rx < 0 && ry < 0)
                rx = ry = 0;
            else if (rx < 0)
                rx = ry;
            else if (ry < 0)
                ry = rx;
            contours.push_back(roundedRectContour(x, y, width, height, rx, ry));
        } else if (n.tag == "circle") {
            const double r = attrLength(n, "r", diagonal, style, 0);
            if (r > 0)
                contours.push_back(ellipseContour(attrLength(n, "cx", w, style, 0), attrLength(n, "cy", h, style, 0), r, r));
        } else if (n.tag == "ellipse") {
            const double rx = attrLength(n, "rx", w, style, 0), ry = attrLength(n, "ry", h, style, 0);
            if (rx > 0 && ry > 0)
                contours.push_back(ellipseContour(attrLength(n, "cx", w, style, 0), attrLength(n, "cy", h, style, 0), rx, ry));
        } else if (n.tag == "line") {
            SvgContour c;
            c.anchors.push_back({{attrLength(n, "x1", w, style, 0), attrLength(n, "y1", h, style, 0)}, {}, {}, false, false});
            c.anchors.push_back({{attrLength(n, "x2", w, style, 0), attrLength(n, "y2", h, style, 0)}, {}, {}, false, false});
            contours.push_back(std::move(c));
        } else if (n.tag == "polyline" || n.tag == "polygon") {
            const auto* points = n.find("points");
            if (points == nullptr)
                return contours;
            SvgContour c;
            std::string_view rest = *points;
            std::vector<double> numbers;
            while (true) {
                rest = trim(rest);
                while (!rest.empty() && rest.front() == ',')
                    rest = trim(rest.substr(1));
                if (rest.empty())
                    break;
                const auto number = parseNumber(rest, &rest);
                if (!number) {
                    notes.push_back("the points list has an error; the part before it is drawn");
                    break;
                }
                numbers.push_back(*number);
            }
            for (std::size_t i = 0; i + 1 < numbers.size(); i += 2)
                c.anchors.push_back({{numbers[i], numbers[i + 1]}, {}, {}, false, false});
            c.closed = n.tag == "polygon";
            if (c.anchors.size() >= 2)
                contours.push_back(std::move(c));
        }
        return contours;
    }

    static bool isShapeTag(const std::string& tag) {
        return tag == "path" || tag == "rect" || tag == "circle" || tag == "ellipse" || tag == "line" ||
               tag == "polyline" || tag == "polygon";
    }

    std::optional<SvgAffine> elementTransform(const XmlNode& n, std::vector<std::string>& notes) {
        const auto* text = n.find("transform");
        if (text == nullptr || trim(*text).empty())
            return SvgAffine{};
        auto parsed = parseTransform(*text);
        if (!parsed)
            notes.push_back("the transform attribute could not be read and is ignored");
        return parsed.value_or(SvgAffine{});
    }

    void countAnchors(const std::size_t n) {
        scene_.anchorCount += n;
        if (scene_.anchorCount > kMaxSvgAnchors)
            throw Refusal{SvgError::TooComplex, "The SVG has more than 3,000,000 path points, beyond what is imported."};
    }

    // Builds a shape element for `index` under `ctm` (which already includes everything above
    // the element). Returns false when nothing visible remains.
    bool buildShape(const std::size_t index, const Style& style, const SvgAffine& ctm, SvgElement& out,
                    const bool forClip) {
        const auto& n = doc_.nodes[index];
        SvgElement shape;
        shape.kind = SvgElement::Kind::Shape;
        shape.tag = n.tag;
        shape.name = label(n, n.tag);
        shape.where = located(n);
        auto contours = shapeContours(n, style, shape.approximations);
        if (contours.empty()) {
            if (!forClip)
                skip(describe(shape), "no geometry (" + n.tag + ")");
            return false;
        }
        std::vector<std::string> notes;
        const auto own = elementTransform(n, notes);
        const SvgAffine local = own ? *own : SvgAffine{};
        for (auto& note : notes)
            shape.approximations.push_back(std::move(note));
        const SvgAffine world = ctm * local;
        const auto box = bounds(contours);
        std::size_t anchors = 0;
        for (const auto& contour : contours) {
            anchors += contour.anchors.size();
            if (contour.anchors.size() > kMaxSvgContourAnchors) {
                skip(describe(shape), "a contour has " + std::to_string(contour.anchors.size()) +
                                     " points, beyond Bloom's limit of 4096 per contour");
                return false;
            }
        }
        countAnchors(anchors);
        for (auto& contour : contours)
            transformContour(world, contour);
        shape.contours = std::move(contours);
        shape.opacity = style.opacity;
        shape.evenOdd = forClip ? style.clipEvenOdd : style.fillEvenOdd;
        if (forClip) {
            shape.fill.kind = SvgPaint::Kind::Color;
            shape.fill.color = {1, 1, 1, 1};
            out = std::move(shape);
            ++scene_.shapeCount;
            return true;
        }
        const double scale = std::sqrt(std::fabs(world.determinant()));
        // Fill.
        const bool closedAny = std::ranges::any_of(shape.contours, [](const auto& c) { return c.closed || c.anchors.size() > 2; });
        (void)closedAny;
        const auto resolvePaint = [&](const PaintSpec& spec, const double opacity, const bool isStroke) -> SvgPaint {
            SvgPaint paint;
            switch (spec.kind) {
            case PaintKind::None:
                return paint;
            case PaintKind::Color:
            case PaintKind::CurrentColor: {
                paint.kind = SvgPaint::Kind::Color;
                paint.color = spec.kind == PaintKind::CurrentColor ? style.currentColor : spec.color;
                paint.color.alpha *= opacity;
                return paint;
            }
            case PaintKind::Url:
                return resolveGradient(spec, opacity, world, box, style, shape.approximations, describe(shape), isStroke)
                    .value_or(SvgPaint{});
            }
            return paint;
        };
        shape.fill = resolvePaint(style.fill, style.fillOpacity, false);
        shape.strokeWidth = style.strokeWidth * scale;
        if (style.strokeWidth > 0)
            shape.stroke = resolvePaint(style.stroke, style.strokeOpacity, true);
        shape.cap = style.cap;
        shape.join = style.join;
        shape.miterLimit = style.miterLimit;
        for (const auto dash : style.dashes)
            shape.dashes.push_back(dash * scale);
        shape.dashOffset = style.dashOffset * scale;
        if (shape.stroke.kind != SvgPaint::Kind::None && !(shape.strokeWidth > 0))
            shape.stroke = {};
        if (shape.stroke.kind != SvgPaint::Kind::None) {
            const double sx = std::hypot(world.a, world.b), sy = std::hypot(world.c, world.d);
            if (std::fabs(sx - sy) > 0.02 * std::max(sx, sy))
                shape.approximations.push_back("the stroke keeps one width where the transform scales x and y differently");
            if (shape.stroke.kind == SvgPaint::Kind::Gradient)
                shape.approximations.push_back("a gradient stroke is drawn through a stroke-shaped matte");
        }
        if (style.filter)
            applyFilter(style, world, shape);
        if (style.markers)
            shape.approximations.push_back("markers on this element are not drawn");
        if (!style.visible || (shape.fill.kind == SvgPaint::Kind::None && shape.stroke.kind == SvgPaint::Kind::None)) {
            std::string reason = style.visible ? "no visible fill or stroke" : "hidden (visibility)";
            for (const auto& approximation : shape.approximations)
                reason += "; " + approximation;
            skip(describe(shape), std::move(reason));
            return false;
        }
        attachClipAndMask(index, style, world, box, shape);
        out = std::move(shape);
        ++scene_.shapeCount;
        if (scene_.shapeCount > kMaxSvgDrawnElements)
            throw Refusal{SvgError::TooComplex,
                          "The SVG draws more than 20,000 elements, beyond what is imported."};
        return true;
    }


    // ---- filters ----

    // The final-coordinate bounds of an element's control points (a group: its children's).
    static void accumulateBounds(const SvgElement& element, std::array<double, 4>& box) {
        for (const auto& contour : element.contours)
            for (const auto& anchor : contour.anchors) {
                const auto add = [&box](const SvgPoint p) {
                    box[0] = std::min(box[0], p.x);
                    box[1] = std::min(box[1], p.y);
                    box[2] = std::max(box[2], p.x);
                    box[3] = std::max(box[3], p.y);
                };
                add(anchor.point);
                if (anchor.hasIn)
                    add(anchor.in);
                if (anchor.hasOut)
                    add(anchor.out);
            }
        for (const auto& child : element.children)
            accumulateBounds(child, box);
    }

    // `filter` on an element. A filter that is one Gaussian blur of the source graphic (the
    // `feGaussianBlur` of a `<filter>`, or the CSS function `blur()`) becomes the native blur of the
    // element: its standard deviation in final coordinates goes to blurX and blurY. Every other
    // primitive is listed, and a filter of several primitives is not applied at all (a blur beside
    // an offset and a merge is a shadow, not a blur).
    void applyFilter(const Style& style, const SvgAffine& world, SvgElement& element) {
        const auto text = std::string(trim(style.filterValue));
        if (text.empty() || lower(text) == "none")
            return;
        const double sx = std::hypot(world.a, world.b), sy = std::hypot(world.c, world.d);
        const auto unsupported = [&](const std::string& what) {
            element.approximations.push_back("the filter " + what + " is not applied");
        };
        // Tokens: url(#id) and CSS functions.
        std::vector<std::string> tokens;
        for (std::size_t at = 0; at < text.size();) {
            while (at < text.size() && std::isspace(static_cast<unsigned char>(text[at])))
                ++at;
            if (at >= text.size())
                break;
            const auto close = text.find(')', at);
            if (close == std::string::npos) {
                tokens.push_back(text.substr(at));
                break;
            }
            tokens.push_back(text.substr(at, close + 1 - at));
            at = close + 1;
        }
        std::optional<std::pair<double, double>> blur; // final-coordinate sigma on x and y
        std::vector<std::string> others;
        for (const auto& token : tokens) {
            const auto open = token.find('(');
            const auto name = lower(std::string(trim(token.substr(0, open))));
            const auto args = open == std::string::npos ? std::string{}
                                                        : std::string(trim(token.substr(open + 1, token.size() - open - 2)));
            if (name == "blur") {
                const auto radius = args.empty() ? 0.0 : lengthPx(args, 0.0, style.fontSize, 0.0);
                if (radius > 0 && !blur && tokens.size() == 1)
                    blur = std::pair{radius * sx, radius * sy};
                else if (radius > 0)
                    others.push_back("blur() beside other filter functions");
            } else if (name == "url") {
                const auto id = parseUrlReference(token);
                if (!id) {
                    others.push_back(token);
                    continue;
                }
                const auto target = lookup(*id);
                if (!target || doc_.nodes[*target].foreign || doc_.nodes[*target].tag != "filter") {
                    unsupported("#" + *id + " (it is not a filter in this file)");
                    continue;
                }
                const auto& filter = doc_.nodes[*target];
                std::vector<std::size_t> primitives;
                for (const auto child : filter.children)
                    if (!doc_.nodes[child].foreign && doc_.nodes[child].tag.rfind("fe", 0) == 0)
                        primitives.push_back(child);
                if (primitives.empty()) {
                    unsupported("#" + *id + " (it has no primitives, so the element is drawn unfiltered)");
                    continue;
                }
                const auto& first = doc_.nodes[primitives.front()];
                const auto* input = first.find("in");
                const bool singleBlur = primitives.size() == 1 && first.tag == "feGaussianBlur" && tokens.size() == 1 &&
                                        (input == nullptr || trim(*input).empty() || trim(*input) == "SourceGraphic");
                if (!singleBlur) {
                    std::set<std::string> names;
                    for (const auto primitive : primitives)
                        names.insert(doc_.nodes[primitive].tag);
                    std::string list;
                    for (const auto& n : names)
                        list += (list.empty() ? "" : ", ") + n;
                    others.push_back("#" + *id + " (" + list + ")");
                    continue;
                }
                double deviationX = 0, deviationY = 0;
                if (const auto* value = first.find("stdDeviation")) {
                    std::string_view rest = trim(*value);
                    if (const auto a = parseNumber(rest, &rest)) {
                        deviationX = deviationY = *a;
                        rest = trim(rest);
                        if (!rest.empty() && rest.front() == ',')
                            rest = trim(rest.substr(1));
                        if (const auto b = parseNumber(rest, &rest))
                            deviationY = *b;
                    }
                }
                if (!(deviationX > 0) && !(deviationY > 0))
                    continue; // zero: the primitive passes its input through
                double scaleX = sx, scaleY = sy;
                std::array<double, 4> box{1e300, 1e300, -1e300, -1e300};
                accumulateBounds(element, box);
                const bool haveBox = box[2] > box[0] || box[3] > box[1];
                if (const auto* units = filter.find("primitiveUnits");
                    units != nullptr && lower(trim(*units)) == "objectboundingbox") {
                    if (!haveBox) {
                        unsupported("#" + *id + " (its blur is sized by the element's box, which is empty)");
                        continue;
                    }
                    scaleX = box[2] - box[0];
                    scaleY = box[3] - box[1];
                }
                blur = std::pair{deviationX * scaleX, deviationY * scaleY};
                // The filter region clips the blur; Bloom's blur is not clipped to it.
                if (haveBox) {
                    const auto fraction = [&](const char* key, const double fallback) {
                        const auto* value = filter.find(key);
                        if (value == nullptr)
                            return fallback;
                        std::string_view rest;
                        return parseNumber(trim(*value), &rest).value_or(fallback);
                    };
                    const auto* units = filter.find("filterUnits");
                    if (units == nullptr || lower(trim(*units)) != "userspaceonuse") {
                        const double w = box[2] - box[0], h = box[3] - box[1];
                        const double fx = fraction("x", -0.1), fy = fraction("y", -0.1);
                        const double fw = fraction("width", 1.2), fh = fraction("height", 1.2);
                        const double left = -fx * w, right = (fx + fw - 1.0) * w;
                        const double top = -fy * h, bottom = (fy + fh - 1.0) * h;
                        const double reachX = 3.0 * blur->first, reachY = 3.0 * blur->second;
                        if (reachX > std::min(left, right) + 0.5 || reachY > std::min(top, bottom) + 0.5)
                            element.approximations.push_back(
                                "the filter region of #" + *id + " would clip the blur at the element's box plus its margin; "
                                "the native blur is not clipped to it");
                    }
                }
            } else {
                others.push_back(token);
            }
        }
        if (!others.empty()) {
            std::string list;
            for (const auto& other : others)
                list += (list.empty() ? "" : "; ") + other;
            unsupported(list);
        }
        if (blur && others.empty()) {
            element.blurX = blur->first;
            element.blurY = blur->second;
        }
    }

    // ---- clip paths and masks ----

    std::shared_ptr<const SvgElement> buildClip(const std::string& id, const SvgAffine& world,
                                                const std::optional<std::array<double, 4>>& box,
                                                const std::string& owner) {
        const auto target = lookup(id);
        if (!target || doc_.nodes[*target].foreign || doc_.nodes[*target].tag != "clipPath") {
            skip(owner, "clip-path #" + id + " is not a clipPath in this file");
            return nullptr;
        }
        const auto& clipNode = doc_.nodes[*target];
        SvgAffine matrix = world;
        if (const auto* units = clipNode.find("clipPathUnits"); units != nullptr && lower(trim(*units)) == "objectboundingbox") {
            if (!box || !((*box)[2] > (*box)[0]) || !((*box)[3] > (*box)[1])) {
                skip(owner, "clip-path #" + id + " uses bounding box units on a group, which is not applied");
                return nullptr;
            }
            matrix = matrix * SvgAffine{(*box)[2] - (*box)[0], 0, 0, (*box)[3] - (*box)[1], (*box)[0], (*box)[1]};
        }
        std::vector<std::string> notes;
        if (const auto own = elementTransform(clipNode, notes))
            matrix = matrix * *own;
        auto clip = std::make_shared<SvgElement>();
        clip->kind = SvgElement::Kind::Group;
        clip->name = "Clip " + id;
        clip->tag = "clipPath";
        const auto base = styleThroughAncestors(*target);
        std::set<std::size_t> active;
        collectClipShapes(*target, base, matrix, *clip, owner, 0);
        if (clip->children.empty()) {
            // An empty clip hides the element entirely.
            clip->approximations.push_back("the clip has no shapes, so the element is fully clipped");
        }
        return clip;
    }

    void collectClipShapes(const std::size_t node, const Style& parentStyle, const SvgAffine& ctm, SvgElement& out,
                           const std::string& owner, const std::size_t depth) {
        if (depth > 8)
            return;
        for (const auto child : doc_.nodes[node].children) {
            const auto& n = doc_.nodes[child];
            if (n.foreign)
                continue;
            auto style = computeStyle(child, parentStyle);
            if (style.displayNone)
                continue;
            if (isShapeTag(n.tag)) {
                SvgElement shape;
                if (buildShape(child, style, ctm, shape, true))
                    out.children.push_back(std::move(shape));
            } else if (n.tag == "use") {
                const auto* href = n.find("href");
                if (href == nullptr || href->empty() || (*href)[0] != '#')
                    continue;
                const auto target = lookup(href->substr(1));
                if (!target || !isShapeTag(doc_.nodes[*target].tag))
                    continue;
                SvgAffine at = ctm;
                std::vector<std::string> notes;
                if (const auto own = elementTransform(n, notes))
                    at = at * *own;
                at = at * SvgAffine{1, 0, 0, 1, attrLength(n, "x", viewWidth_, style, 0), attrLength(n, "y", viewHeight_, style, 0)};
                auto targetStyle = computeStyle(*target, style);
                SvgElement shape;
                if (buildShape(*target, targetStyle, at, shape, true))
                    out.children.push_back(std::move(shape));
            } else if (n.tag == "text") {
                skip(owner, "clip-path text is not part of the clip");
            }
        }
    }

    std::shared_ptr<const SvgElement> buildMask(const std::string& id, const SvgAffine& world, const std::string& owner) {
        const auto target = lookup(id);
        if (!target || doc_.nodes[*target].foreign || doc_.nodes[*target].tag != "mask") {
            skip(owner, "mask #" + id + " is not a mask in this file");
            return nullptr;
        }
        const auto& maskNode = doc_.nodes[*target];
        if (const auto* units = maskNode.find("maskContentUnits"); units != nullptr && lower(trim(*units)) == "objectboundingbox") {
            skip(owner, "mask #" + id + " uses bounding box content units, which are not applied");
            return nullptr;
        }
        for (const auto& seen : useStack_)
            if (seen == *target)
                return nullptr; // A mask that contains itself.
        useStack_.push_back(*target);
        auto mask = std::make_shared<SvgElement>();
        mask->kind = SvgElement::Kind::Group;
        mask->name = "Mask " + id;
        mask->tag = "mask";
        const auto base = styleThroughAncestors(*target);
        buildChildren(*target, base, world, *mask, 0);
        useStack_.pop_back();
        return mask;
    }

    // ---- structure ----

    void buildChildren(const std::size_t node, const Style& style, const SvgAffine& ctm, SvgElement& out,
                       const std::size_t depth) {
        for (const auto child : doc_.nodes[node].children)
            buildElement(child, style, ctm, out, depth + 1);
    }

    SvgElement makeGroup(const XmlNode& n, const std::string& fallbackName) const {
        SvgElement group;
        group.kind = SvgElement::Kind::Group;
        group.tag = n.tag;
        group.name = label(n, fallbackName);
        group.where = located(n);
        return group;
    }


    // ---- text ----

    // Collapses runs of white space as the default xml:space does.
    static std::string collapseSpace(std::string_view text) {
        std::string out;
        bool pending = false;
        for (const char c : text) {
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                pending = !out.empty();
                continue;
            }
            if (pending)
                out.push_back(' ');
            pending = false;
            out.push_back(c);
        }
        return out;
    }

    static std::optional<double> firstNumber(const XmlNode& n, const char* key) {
        const auto* value = n.find(key);
        if (value == nullptr)
            return std::nullopt;
        std::string_view rest;
        return parseNumber(trim(*value), &rest);
    }

    // A <text> becomes native text elements: one per run that starts at its own position (a <text>
    // or a <tspan> with x or y), carrying the characters, size, colour, anchor and turn. The
    // glyphs are Bloom's text layer's, in the font Bloom resolves, not the file's outlines.
    void buildText(const std::size_t index, const Style& style, const SvgAffine& ctm, SvgElement& out,
                   const std::size_t depth) {
        const auto& n = doc_.nodes[index];
        std::vector<std::string> notes;
        const auto own = elementTransform(n, notes);
        const SvgAffine world = ctm * (own ? *own : SvgAffine{});
        const std::string name = label(n, "text");
        double penX = firstNumber(n, "x").value_or(0), penY = firstNumber(n, "y").value_or(0);
        struct Run {
            std::string text;
            double x, y;
            Style style;
            std::string name;
            bool mixedStyle = false;
            std::string where;
        };
        std::vector<Run> runs;
        const auto begin = [&](const Style& st, const std::string& runName, const std::string& where) {
            runs.push_back({{}, penX, penY, st, runName, false, where});
        };
        begin(style, name, located(n));
        std::size_t visited = 0;
        const auto walk = [&](auto&& self, const std::size_t node, const Style& st, const std::size_t level) -> void {
            if (++visited > 4096 || level > 32)
                return;
            checkCancel();
            const auto& e = doc_.nodes[node];
            runs.back().text += e.text;
            for (const auto child : e.children) {
                const auto& c = doc_.nodes[child];
                if (c.foreign || c.tag != "tspan")
                    continue;
                const auto cs = computeStyle(child, st);
                if (cs.displayNone)
                    continue;
                const auto x = firstNumber(c, "x"), y = firstNumber(c, "y");
                if (x || y) {
                    penX = x.value_or(penX);
                    penY = y.value_or(penY);
                    begin(cs, label(c, "tspan"), located(c));
                } else if (cs.fontSize != runs.back().style.fontSize || cs.fill.kind != runs.back().style.fill.kind ||
                           cs.fill.color.red != runs.back().style.fill.color.red ||
                           cs.fill.color.green != runs.back().style.fill.color.green ||
                           cs.fill.color.blue != runs.back().style.fill.color.blue) {
                    runs.back().mixedStyle = true;
                }
                if (c.find("dx") != nullptr || c.find("dy") != nullptr)
                    runs.back().mixedStyle = true;
                self(self, child, cs, level + 1);
            }
        };
        walk(walk, index, style, 0);
        const double scale = std::sqrt(std::fabs(world.determinant()));
        const double turn = std::atan2(world.b, world.a) * 180.0 / 3.14159265358979323846;
        for (auto& run : runs) {
            run.text = collapseSpace(run.text);
            if (run.text.empty())
                continue;
            if (!run.style.visible || run.style.fill.kind == PaintKind::None) {
                skip(run.where, "text without a visible fill");
                continue;
            }
            SvgElement text;
            text.kind = SvgElement::Kind::Text;
            text.tag = "text";
            text.name = run.name;
            text.where = run.where;
            text.text = run.text;
            const auto at = world.apply(run.x, run.y);
            text.textX = at[0];
            text.textY = at[1];
            text.fontSize = run.style.fontSize * scale;
            text.textAnchor = run.style.textAnchor;
            text.textRotation = turn;
            text.fontFamily = run.style.fontFamily;
            text.fontBold = run.style.fontBold;
            text.fontWeight = run.style.fontBold ? std::max(run.style.fontWeight, 600) : run.style.fontWeight;
            text.fontItalic = run.style.fontItalic;
            text.opacity = run.style.opacity;
            text.fill.kind = SvgPaint::Kind::Color;
            text.fill.color = run.style.fill.kind == PaintKind::CurrentColor ? run.style.currentColor : run.style.fill.color;
            text.fill.color.alpha *= run.style.fillOpacity;
            for (const auto& note : notes)
                text.approximations.push_back(note);
            if (run.style.fill.kind == PaintKind::Url)
                text.approximations.push_back("a gradient text fill is drawn as solid black");
            if (run.style.stroke.kind != PaintKind::None && run.style.strokeWidth > 0)
                text.approximations.push_back("the stroke of this text is not drawn");
            if (run.mixedStyle)
                text.approximations.push_back("spans inside one run keep the first span's size and colour, and dx and dy are not applied");
            if (run.style.filter)
                applyFilter(run.style, world, text);
            ++scene_.shapeCount;
            ++scene_.textCount;
            out.children.push_back(std::move(text));
        }
        (void)depth;
    }

    void buildElement(const std::size_t index, const Style& parentStyle, const SvgAffine& ctm, SvgElement& out,
                      const std::size_t depth) {
        checkCancel();
        if (depth > kMaxSvgNesting)
            throw Refusal{SvgError::TooComplex, "The elements are nested more than 128 levels deep."};
        const auto& n = doc_.nodes[index];
        if (n.foreign)
            return;
        const auto& tag = n.tag;
        if (tag == "script") {
            skip(located(n), "scripts are never run");
            return;
        }
        if (tag == "image") {
            skip(located(n), "embedded or linked images are not imported");
            return;
        }
        if (tag == "foreignObject") {
            skip(located(n), "foreign content is not imported");
            return;
        }
        if (isMetadataTag(tag))
            return;
        const auto style = computeStyle(index, parentStyle);
        if (style.displayNone) {
            skip(located(n), "hidden (display none)");
            return;
        }
        if (tag == "text") {
            buildText(index, style, ctm, out, depth);
            return;
        }
        if (isShapeTag(tag)) {
            SvgElement shape;
            if (buildShape(index, style, ctm, shape, false))
                out.children.push_back(std::move(shape));
            return;
        }
        std::vector<std::string> notes;
        const auto own = elementTransform(n, notes);
        const SvgAffine world = ctm * (own ? *own : SvgAffine{});
        if (tag == "g" || tag == "a" || tag == "switch" || tag == "svg" || tag == "use") {
            SvgElement group = makeGroup(n, tag == "use" ? "use" : tag);
            for (auto& note : notes)
                group.approximations.push_back(std::move(note));
            ++scene_.groupCount;
            if (tag == "switch") {
                for (const auto child : n.children) {
                    const auto& c = doc_.nodes[child];
                    if (c.foreign || isMetadataTag(c.tag))
                        continue;
                    if (const auto* languages = c.find("systemLanguage");
                        languages != nullptr && lower(trim(*languages)).rfind("en", 0) != 0)
                        continue;
                    if (c.find("requiredExtensions") != nullptr)
                        continue;
                    buildElement(child, style, world, group, depth + 1);
                    break;
                }
            } else if (tag == "svg") {
                buildNestedSvg(index, style, world, group, depth, std::nullopt, std::nullopt);
            } else if (tag == "use") {
                buildUse(index, style, world, group, depth);
            } else {
                buildChildren(index, style, world, group, depth);
            }
            group.opacity = style.opacity;
            if (style.filter)
                applyFilter(style, world, group);
            attachClipAndMask(index, style, world, std::nullopt, group);
            if (!group.children.empty() || group.clip || group.mask)
                out.children.push_back(std::move(group));
            else
                --scene_.groupCount;
            return;
        }
        if (tag == "symbol")
            return;
        skip(located(n), "element <" + tag + "> is not imported");
    }

    void attachClipAndMask(const std::size_t index, const Style& style, const SvgAffine& world,
                           const std::optional<std::array<double, 4>>& box, SvgElement& element) {
        if (!style.clipPath.empty()) {
            if (const auto id = parseUrlReference(style.clipPath)) {
                if (auto clip = buildClip(*id, world, box, describe(element)))
                    element.clip = std::move(clip);
            } else {
                skip(describe(element), "clip-path does not name a clip in this file");
            }
        }
        if (!style.mask.empty()) {
            if (const auto id = parseUrlReference(style.mask)) {
                if (auto mask = buildMask(*id, world, describe(element)))
                    element.mask = std::move(mask);
            } else {
                skip(describe(element), "mask does not name a mask in this file");
            }
        }
        (void)index;
    }

    void buildNestedSvg(const std::size_t index, const Style& style, const SvgAffine& world, SvgElement& group,
                        const std::size_t depth, const std::optional<double> widthOverride,
                        const std::optional<double> heightOverride) {
        const auto& n = doc_.nodes[index];
        const double x = attrLength(n, "x", viewWidth_, style, 0), y = attrLength(n, "y", viewHeight_, style, 0);
        const double width = widthOverride.value_or(attrLength(n, "width", viewWidth_, style, viewWidth_));
        const double height = heightOverride.value_or(attrLength(n, "height", viewHeight_, style, viewHeight_));
        if (!(width > 0) || !(height > 0))
            return;
        double vb[4]{};
        SvgAffine inner = world * SvgAffine{1, 0, 0, 1, x, y};
        const auto savedW = viewWidth_, savedH = viewHeight_;
        if (parseViewBox(n, vb)) {
            inner = inner * viewBoxTransform(vb, width, height, n.find("preserveAspectRatio"));
            viewWidth_ = vb[2];
            viewHeight_ = vb[3];
        } else {
            viewWidth_ = width;
            viewHeight_ = height;
        }
        // The viewport clips its content unless overflow is visible.
        if (!style.overflowVisible && index != 0) {
            auto clip = std::make_shared<SvgElement>();
            clip->kind = SvgElement::Kind::Group;
            clip->name = "Viewport clip";
            clip->tag = "clipPath";
            SvgElement rect;
            rect.kind = SvgElement::Kind::Shape;
            rect.tag = "rect";
            rect.name = "Viewport";
            auto contour = roundedRectContour(0, 0, width, height, 0, 0);
            const auto frame = world * SvgAffine{1, 0, 0, 1, x, y};
            transformContour(frame, contour);
            rect.contours.push_back(std::move(contour));
            rect.fill.kind = SvgPaint::Kind::Color;
            rect.fill.color = {1, 1, 1, 1};
            clip->children.push_back(std::move(rect));
            group.clip = std::move(clip);
        }
        buildChildren(index, style, inner, group, depth);
        viewWidth_ = savedW;
        viewHeight_ = savedH;
    }

    void buildUse(const std::size_t index, const Style& style, const SvgAffine& world, SvgElement& group,
                  const std::size_t depth) {
        const auto& n = doc_.nodes[index];
        const auto* href = n.find("href");
        if (href == nullptr || href->empty()) {
            skip(describe(group), "use element without a reference");
            return;
        }
        if ((*href)[0] != '#') {
            skip(describe(group), "use of an external reference is never followed");
            return;
        }
        const auto target = lookup(href->substr(1));
        if (!target) {
            skip(describe(group), "use references #" + href->substr(1) + ", which is not in the file");
            return;
        }
        if (useDepth_ >= kMaxSvgUseDepth || std::ranges::find(useStack_, *target) != useStack_.end()) {
            skip(describe(group), "use references itself or is nested too deeply");
            return;
        }
        // An element that is its own ancestor makes a cycle too.
        for (auto at = index;; at = doc_.nodes[at].parent) {
            if (at == *target) {
                skip(describe(group), "use references one of its own ancestors");
                return;
            }
            if (at == 0)
                break;
        }
        if (group.name == "use")
            group.name = "use " + href->substr(1);
        const double x = attrLength(n, "x", viewWidth_, style, 0), y = attrLength(n, "y", viewHeight_, style, 0);
        const SvgAffine at = world * SvgAffine{1, 0, 0, 1, x, y};
        useStack_.push_back(*target);
        ++useDepth_;
        const auto& t = doc_.nodes[*target];
        if (!t.foreign && t.tag == "symbol") {
            SvgElement inner = makeGroup(t, "symbol");
            const auto symbolStyle = computeStyle(*target, style);
            const double width = n.find("width") ? attrLength(n, "width", viewWidth_, style, viewWidth_)
                                                 : attrLength(t, "width", viewWidth_, symbolStyle, viewWidth_);
            const double height = n.find("height") ? attrLength(n, "height", viewHeight_, style, viewHeight_)
                                                   : attrLength(t, "height", viewHeight_, symbolStyle, viewHeight_);
            double vb[4]{};
            SvgAffine inside = at;
            const auto savedW = viewWidth_, savedH = viewHeight_;
            if (parseViewBox(t, vb) && width > 0 && height > 0) {
                inside = at * viewBoxTransform(vb, width, height, t.find("preserveAspectRatio"));
                viewWidth_ = vb[2];
                viewHeight_ = vb[3];
            }
            buildChildren(*target, symbolStyle, inside, inner, depth);
            viewWidth_ = savedW;
            viewHeight_ = savedH;
            ++scene_.groupCount;
            if (!inner.children.empty())
                group.children.push_back(std::move(inner));
            else
                --scene_.groupCount;
        } else if (!t.foreign && t.tag == "svg") {
            std::optional<double> w, h;
            if (n.find("width"))
                w = attrLength(n, "width", viewWidth_, style, viewWidth_);
            if (n.find("height"))
                h = attrLength(n, "height", viewHeight_, style, viewHeight_);
            SvgElement inner = makeGroup(t, "svg");
            ++scene_.groupCount;
            buildNestedSvg(*target, computeStyle(*target, style), at, inner, depth, w, h);
            if (!inner.children.empty())
                group.children.push_back(std::move(inner));
            else
                --scene_.groupCount;
        } else {
            buildElement(*target, style, at, group, depth + 1);
        }
        --useDepth_;
        useStack_.pop_back();
    }
};

} // namespace
} // namespace svg

SvgReadResult parseSvgScene(const std::span<const std::byte> bytes, const SvgCancel& cancel) {
    SvgReadResult result;
    if (bytes.size() > kMaxSvgFileBytes) {
        result.error = SvgError::TooLarge;
        result.diagnostic = "The file is larger than 32 MiB, the most an SVG import reads.";
        return result;
    }
    auto document = svg::parseXml(bytes, cancel);
    if (document.error != SvgError::None) {
        result.error = document.error;
        result.diagnostic = std::move(document.diagnostic);
        return result;
    }
    auto scene = std::make_shared<SvgScene>();
    svg::Builder builder(document, *scene, cancel);
    auto built = builder.run();
    if (built.error != SvgError::None)
        return built;
    result.scene = std::move(scene);
    return result;
}

SvgReadResult readSvgSceneFile(const std::filesystem::path& path, const SvgCancel& cancel) {
    SvgReadResult result;
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        result.error = SvgError::ReadFailed;
        result.diagnostic = "The file could not be opened.";
        return result;
    }
    file.seekg(0, std::ios::end);
    const auto size = file.tellg();
    if (size < 0) {
        result.error = SvgError::ReadFailed;
        result.diagnostic = "The file could not be read.";
        return result;
    }
    if (static_cast<std::uint64_t>(size) > kMaxSvgFileBytes) {
        result.error = SvgError::TooLarge;
        result.diagnostic = "The file is larger than 32 MiB, the most an SVG import reads.";
        return result;
    }
    file.seekg(0);
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!file && !file.eof()) {
        result.error = SvgError::ReadFailed;
        result.diagnostic = "The file could not be read.";
        return result;
    }
    auto parsed = parseSvgScene(bytes, cancel);
    if (parsed.scene) {
        // The scene is ours alone until it is returned: record which file it came from and what
        // that file held, so the imported artwork can name its source.
        const auto scene = std::const_pointer_cast<SvgScene>(parsed.scene);
        core::Sha256Hasher hasher;
        if (hasher.update(bytes)) {
            scene->sourcePath = path;
            scene->sourceDigest = hasher.finalize();
            scene->hasSourceDigest = true;
        }
    }
    return parsed;
}

} // namespace bloom::media
