#include "svg_internal.hpp"

#include <bloom/core/float_chars.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <numbers>

namespace bloom::media {
SvgAffine operator*(const SvgAffine& o, const SvgAffine& i) noexcept {
    return {o.a * i.a + o.c * i.b,
            o.b * i.a + o.d * i.b,
            o.a * i.c + o.c * i.d,
            o.b * i.c + o.d * i.d,
            o.a * i.e + o.c * i.f + o.e,
            o.b * i.e + o.d * i.f + o.f};
}

}

namespace bloom::media::svg {
namespace {

bool isSpace(const char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

struct NamedColor {
    std::string_view name;
    std::uint32_t rgb;
};
// The CSS colour keywords, sorted by name.
constexpr std::array<NamedColor, 148> kNamedColors{{
    {"aliceblue", 0xF0F8FF},   {"antiquewhite", 0xFAEBD7}, {"aqua", 0x00FFFF},
    {"aquamarine", 0x7FFFD4},  {"azure", 0xF0FFFF},        {"beige", 0xF5F5DC},
    {"bisque", 0xFFE4C4},      {"black", 0x000000},        {"blanchedalmond", 0xFFEBCD},
    {"blue", 0x0000FF},        {"blueviolet", 0x8A2BE2},   {"brown", 0xA52A2A},
    {"burlywood", 0xDEB887},   {"cadetblue", 0x5F9EA0},    {"chartreuse", 0x7FFF00},
    {"chocolate", 0xD2691E},   {"coral", 0xFF7F50},        {"cornflowerblue", 0x6495ED},
    {"cornsilk", 0xFFF8DC},    {"crimson", 0xDC143C},      {"cyan", 0x00FFFF},
    {"darkblue", 0x00008B},    {"darkcyan", 0x008B8B},     {"darkgoldenrod", 0xB8860B},
    {"darkgray", 0xA9A9A9},    {"darkgreen", 0x006400},    {"darkgrey", 0xA9A9A9},
    {"darkkhaki", 0xBDB76B},   {"darkmagenta", 0x8B008B},  {"darkolivegreen", 0x556B2F},
    {"darkorange", 0xFF8C00},  {"darkorchid", 0x9932CC},   {"darkred", 0x8B0000},
    {"darksalmon", 0xE9967A},  {"darkseagreen", 0x8FBC8F}, {"darkslateblue", 0x483D8B},
    {"darkslategray", 0x2F4F4F}, {"darkslategrey", 0x2F4F4F}, {"darkturquoise", 0x00CED1},
    {"darkviolet", 0x9400D3},  {"deeppink", 0xFF1493},     {"deepskyblue", 0x00BFFF},
    {"dimgray", 0x696969},     {"dimgrey", 0x696969},      {"dodgerblue", 0x1E90FF},
    {"firebrick", 0xB22222},   {"floralwhite", 0xFFFAF0},  {"forestgreen", 0x228B22},
    {"fuchsia", 0xFF00FF},     {"gainsboro", 0xDCDCDC},    {"ghostwhite", 0xF8F8FF},
    {"gold", 0xFFD700},        {"goldenrod", 0xDAA520},    {"gray", 0x808080},
    {"green", 0x008000},       {"greenyellow", 0xADFF2F},  {"grey", 0x808080},
    {"honeydew", 0xF0FFF0},    {"hotpink", 0xFF69B4},      {"indianred", 0xCD5C5C},
    {"indigo", 0x4B0082},      {"ivory", 0xFFFFF0},        {"khaki", 0xF0E68C},
    {"lavender", 0xE6E6FA},    {"lavenderblush", 0xFFF0F5}, {"lawngreen", 0x7CFC00},
    {"lemonchiffon", 0xFFFACD}, {"lightblue", 0xADD8E6},   {"lightcoral", 0xF08080},
    {"lightcyan", 0xE0FFFF},   {"lightgoldenrodyellow", 0xFAFAD2}, {"lightgray", 0xD3D3D3},
    {"lightgreen", 0x90EE90},  {"lightgrey", 0xD3D3D3},    {"lightpink", 0xFFB6C1},
    {"lightsalmon", 0xFFA07A}, {"lightseagreen", 0x20B2AA}, {"lightskyblue", 0x87CEFA},
    {"lightslategray", 0x778899}, {"lightslategrey", 0x778899}, {"lightsteelblue", 0xB0C4DE},
    {"lightyellow", 0xFFFFE0}, {"lime", 0x00FF00},         {"limegreen", 0x32CD32},
    {"linen", 0xFAF0E6},       {"magenta", 0xFF00FF},      {"maroon", 0x800000},
    {"mediumaquamarine", 0x66CDAA}, {"mediumblue", 0x0000CD}, {"mediumorchid", 0xBA55D3},
    {"mediumpurple", 0x9370DB}, {"mediumseagreen", 0x3CB371}, {"mediumslateblue", 0x7B68EE},
    {"mediumspringgreen", 0x00FA9A}, {"mediumturquoise", 0x48D1CC}, {"mediumvioletred", 0xC71585},
    {"midnightblue", 0x191970}, {"mintcream", 0xF5FFFA},   {"mistyrose", 0xFFE4E1},
    {"moccasin", 0xFFE4B5},    {"navajowhite", 0xFFDEAD},  {"navy", 0x000080},
    {"oldlace", 0xFDF5E6},     {"olive", 0x808000},        {"olivedrab", 0x6B8E23},
    {"orange", 0xFFA500},      {"orangered", 0xFF4500},    {"orchid", 0xDA70D6},
    {"palegoldenrod", 0xEEE8AA}, {"palegreen", 0x98FB98},  {"paleturquoise", 0xAFEEEE},
    {"palevioletred", 0xDB7093}, {"papayawhip", 0xFFEFD5}, {"peachpuff", 0xFFDAB9},
    {"peru", 0xCD853F},        {"pink", 0xFFC0CB},         {"plum", 0xDDA0DD},
    {"powderblue", 0xB0E0E6},  {"purple", 0x800080},       {"rebeccapurple", 0x663399},
    {"red", 0xFF0000},         {"rosybrown", 0xBC8F8F},    {"royalblue", 0x4169E1},
    {"saddlebrown", 0x8B4513}, {"salmon", 0xFA8072},       {"sandybrown", 0xF4A460},
    {"seagreen", 0x2E8B57},    {"seashell", 0xFFF5EE},     {"sienna", 0xA0522D},
    {"silver", 0xC0C0C0},      {"skyblue", 0x87CEEB},      {"slateblue", 0x6A5ACD},
    {"slategray", 0x708090},   {"slategrey", 0x708090},    {"snow", 0xFFFAFA},
    {"springgreen", 0x00FF7F}, {"steelblue", 0x4682B4},    {"tan", 0xD2B48C},
    {"teal", 0x008080},        {"thistle", 0xD8BFD8},      {"tomato", 0xFF6347},
    {"turquoise", 0x40E0D0},   {"violet", 0xEE82EE},       {"wheat", 0xF5DEB3},
    {"white", 0xFFFFFF},       {"whitesmoke", 0xF5F5F5},   {"yellow", 0xFFFF00},
    {"yellowgreen", 0x9ACD32},
}};

core::Color4d fromRgb8(const std::uint32_t rgb, const double alpha = 1.0) {
    return {static_cast<double>((rgb >> 16) & 0xFFU) / 255.0,
            static_cast<double>((rgb >> 8) & 0xFFU) / 255.0, static_cast<double>(rgb & 0xFFU) / 255.0,
            alpha};
}

int hexDigit(const char c) noexcept {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

double clamp01(const double v) noexcept { return std::clamp(v, 0.0, 1.0); }

// hsl() to rgb.
core::Color4d hslToRgb(double h, double s, double l, double a) {
    h = std::fmod(h, 360.0);
    if (h < 0)
        h += 360.0;
    h /= 360.0;
    s = clamp01(s);
    l = clamp01(l);
    const auto q = l < 0.5 ? l * (1 + s) : l + s - l * s;
    const auto p = 2 * l - q;
    const auto channel = [&](double t) {
        if (t < 0)
            t += 1;
        if (t > 1)
            t -= 1;
        if (t < 1.0 / 6)
            return p + (q - p) * 6 * t;
        if (t < 0.5)
            return q;
        if (t < 2.0 / 3)
            return p + (q - p) * (2.0 / 3 - t) * 6;
        return p;
    };
    return {channel(h + 1.0 / 3), channel(h), channel(h - 1.0 / 3), a};
}

// Splits a function's arguments on commas, spaces and a slash.
std::vector<std::string_view> splitArguments(std::string_view text) {
    std::vector<std::string_view> out;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= text.size(); ++i) {
        if (i == text.size() || text[i] == ',' || text[i] == '/' || isSpace(text[i])) {
            if (i > start)
                out.push_back(text.substr(start, i - start));
            start = i + 1;
        }
    }
    return out;
}

std::optional<double> numberOrPercent(std::string_view token, double percentScale) {
    std::string_view rest;
    const auto value = parseNumber(token, &rest);
    if (!value)
        return std::nullopt;
    rest = trim(rest);
    if (rest == "%")
        return *value * percentScale / 100.0;
    if (!rest.empty())
        return std::nullopt;
    return *value;
}

// ---- path data ----

class PathReader final {
  public:
    explicit PathReader(std::string_view data) : data_(data) {}

    PathParse run() {
        char command = 0;
        bool first = true;
        while (true) {
            skip();
            if (pos_ >= data_.size())
                break;
            const char c = data_[pos_];
            if (std::isalpha(static_cast<unsigned char>(c)) != 0) {
                command = c;
                ++pos_;
                if (first && command != 'M' && command != 'm') {
                    result_.errorSeen = true;
                    break;
                }
            } else if (command == 0 || command == 'z' || command == 'Z') {
                result_.errorSeen = true;
                break;
            } else if (command == 'M') {
                command = 'L';
            } else if (command == 'm') {
                command = 'l';
            }
            first = false;
            if (!execute(command)) {
                result_.errorSeen = true;
                break;
            }
        }
        finishContour(false);
        return std::move(result_);
    }

  private:
    std::string_view data_;
    std::size_t pos_ = 0;
    PathParse result_;
    SvgContour current_;
    SvgPoint cursor_{}, start_{}, lastControl_{};
    char previous_ = 0;
    bool open_ = false;

    void skip() {
        while (pos_ < data_.size() && (isSpace(data_[pos_]) || data_[pos_] == ','))
            ++pos_;
    }
    bool number(double& out) {
        skip();
        std::string_view rest;
        const auto value = parseNumber(data_.substr(pos_), &rest);
        if (!value)
            return false;
        pos_ = data_.size() - rest.size();
        out = *value;
        return true;
    }
    bool flag(bool& out) {
        skip();
        if (pos_ >= data_.size() || (data_[pos_] != '0' && data_[pos_] != '1'))
            return false;
        out = data_[pos_++] == '1';
        return true;
    }
    bool point(SvgPoint& p, const bool relative) {
        double x = 0, y = 0;
        if (!number(x) || !number(y))
            return false;
        p = relative ? SvgPoint{cursor_.x + x, cursor_.y + y} : SvgPoint{x, y};
        return true;
    }

    void finishContour(const bool close) {
        if (!open_)
            return;
        if (close) {
            current_.closed = true;
            // A final anchor on the start point merges into it.
            auto& anchors = current_.anchors;
            if (anchors.size() > 1 && anchors.back().point.x == anchors.front().point.x &&
                anchors.back().point.y == anchors.front().point.y) {
                if (anchors.back().hasIn) {
                    anchors.front().in = anchors.back().in;
                    anchors.front().hasIn = true;
                }
                anchors.pop_back();
            }
        }
        if (!current_.anchors.empty())
            result_.contours.push_back(std::move(current_));
        current_ = {};
        open_ = false;
    }
    void begin(const SvgPoint p) {
        finishContour(false);
        current_ = {};
        current_.anchors.push_back({p, {}, {}, false, false});
        open_ = true;
        cursor_ = start_ = p;
    }
    void ensureOpen() {
        if (!open_)
            begin(cursor_);
    }
    void lineTo(const SvgPoint p) {
        ensureOpen();
        current_.anchors.push_back({p, {}, {}, false, false});
        cursor_ = p;
    }
    void cubicTo(const SvgPoint c1, const SvgPoint c2, const SvgPoint p) {
        ensureOpen();
        current_.anchors.back().out = c1;
        current_.anchors.back().hasOut = true;
        current_.anchors.push_back({p, c2, {}, true, false});
        lastControl_ = c2;
        cursor_ = p;
    }
    void arcTo(double rx, double ry, const double rotationDegrees, const bool large, const bool sweep,
               const SvgPoint end) {
        const auto from = cursor_;
        if (from.x == end.x && from.y == end.y)
            return;
        rx = std::fabs(rx);
        ry = std::fabs(ry);
        if (rx == 0.0 || ry == 0.0) {
            lineTo(end);
            return;
        }
        const double phi = rotationDegrees * std::numbers::pi / 180.0;
        const double cosPhi = std::cos(phi), sinPhi = std::sin(phi);
        const double dx2 = (from.x - end.x) / 2.0, dy2 = (from.y - end.y) / 2.0;
        const double x1p = cosPhi * dx2 + sinPhi * dy2;
        const double y1p = -sinPhi * dx2 + cosPhi * dy2;
        const double lambda = x1p * x1p / (rx * rx) + y1p * y1p / (ry * ry);
        if (lambda > 1.0) {
            const double scale = std::sqrt(lambda);
            rx *= scale;
            ry *= scale;
        }
        const double rx2 = rx * rx, ry2 = ry * ry;
        double numerator = rx2 * ry2 - rx2 * y1p * y1p - ry2 * x1p * x1p;
        const double denominator = rx2 * y1p * y1p + ry2 * x1p * x1p;
        double coefficient = denominator == 0.0 ? 0.0 : std::sqrt(std::max(0.0, numerator / denominator));
        if (large == sweep)
            coefficient = -coefficient;
        const double cxp = coefficient * rx * y1p / ry;
        const double cyp = -coefficient * ry * x1p / rx;
        const double cx = cosPhi * cxp - sinPhi * cyp + (from.x + end.x) / 2.0;
        const double cy = sinPhi * cxp + cosPhi * cyp + (from.y + end.y) / 2.0;
        const auto angle = [](double ux, double uy, double vx, double vy) {
            return std::atan2(ux * vy - uy * vx, ux * vx + uy * vy);
        };
        const double theta1 = angle(1, 0, (x1p - cxp) / rx, (y1p - cyp) / ry);
        double delta = angle((x1p - cxp) / rx, (y1p - cyp) / ry, (-x1p - cxp) / rx, (-y1p - cyp) / ry);
        if (!sweep && delta > 0)
            delta -= 2 * std::numbers::pi;
        else if (sweep && delta < 0)
            delta += 2 * std::numbers::pi;
        const int segments = std::max(1, static_cast<int>(std::ceil(std::fabs(delta) / (std::numbers::pi / 2) - 1e-9)));
        const double step = delta / segments;
        const double alpha = 4.0 / 3.0 * std::tan(step / 4.0);
        double t = theta1;
        const auto at = [&](const double a, const double r1, const double r2, SvgPoint& position,
                            SvgPoint& derivative) {
            const double ca = std::cos(a), sa = std::sin(a);
            position = {cx + cosPhi * r1 * ca - sinPhi * r2 * sa, cy + sinPhi * r1 * ca + cosPhi * r2 * sa};
            derivative = {-cosPhi * r1 * sa - sinPhi * r2 * ca, -sinPhi * r1 * sa + cosPhi * r2 * ca};
        };
        for (int i = 0; i < segments; ++i) {
            SvgPoint p0, d0, p1, d1;
            at(t, rx, ry, p0, d0);
            at(t + step, rx, ry, p1, d1);
            const SvgPoint c1{p0.x + alpha * d0.x, p0.y + alpha * d0.y};
            const SvgPoint c2{p1.x - alpha * d1.x, p1.y - alpha * d1.y};
            // Land the last segment exactly on the requested end point.
            cubicTo(c1, c2, i + 1 == segments ? end : p1);
            t += step;
        }
    }

    bool execute(const char command) {
        const bool relative = std::islower(static_cast<unsigned char>(command)) != 0;
        SvgPoint p, c1, c2;
        double v = 0;
        switch (std::toupper(static_cast<unsigned char>(command))) {
        case 'M':
            if (!point(p, relative))
                return false;
            begin(p);
            previous_ = 'M';
            return true;
        case 'L':
            if (!point(p, relative))
                return false;
            lineTo(p);
            break;
        case 'H':
            if (!number(v))
                return false;
            lineTo({relative ? cursor_.x + v : v, cursor_.y});
            break;
        case 'V':
            if (!number(v))
                return false;
            lineTo({cursor_.x, relative ? cursor_.y + v : v});
            break;
        case 'C':
            if (!point(c1, relative) || !point(c2, relative) || !point(p, relative))
                return false;
            cubicTo(c1, c2, p);
            previous_ = 'C';
            return true;
        case 'S': {
            if (!point(c2, relative) || !point(p, relative))
                return false;
            const bool smooth = previous_ == 'C' || previous_ == 'S';
            c1 = smooth ? SvgPoint{2 * cursor_.x - lastControl_.x, 2 * cursor_.y - lastControl_.y} : cursor_;
            cubicTo(c1, c2, p);
            previous_ = 'S';
            return true;
        }
        case 'Q': {
            if (!point(c1, relative) || !point(p, relative))
                return false;
            const auto from = cursor_;
            cubicTo({from.x + 2.0 / 3.0 * (c1.x - from.x), from.y + 2.0 / 3.0 * (c1.y - from.y)},
                    {p.x + 2.0 / 3.0 * (c1.x - p.x), p.y + 2.0 / 3.0 * (c1.y - p.y)}, p);
            lastControl_ = c1;
            previous_ = 'Q';
            return true;
        }
        case 'T': {
            if (!point(p, relative))
                return false;
            const bool smooth = previous_ == 'Q' || previous_ == 'T';
            c1 = smooth ? SvgPoint{2 * cursor_.x - lastControl_.x, 2 * cursor_.y - lastControl_.y} : cursor_;
            const auto from = cursor_;
            cubicTo({from.x + 2.0 / 3.0 * (c1.x - from.x), from.y + 2.0 / 3.0 * (c1.y - from.y)},
                    {p.x + 2.0 / 3.0 * (c1.x - p.x), p.y + 2.0 / 3.0 * (c1.y - p.y)}, p);
            lastControl_ = c1;
            previous_ = 'T';
            return true;
        }
        case 'A': {
            double rx = 0, ry = 0, rotation = 0;
            bool large = false, sweep = false;
            if (!number(rx) || !number(ry) || !number(rotation) || !flag(large) || !flag(sweep) ||
                !point(p, relative))
                return false;
            arcTo(rx, ry, rotation, large, sweep, p);
            previous_ = 'A';
            return true;
        }
        case 'Z':
            finishContour(true);
            cursor_ = start_;
            previous_ = 'Z';
            return true;
        default:
            return false;
        }
        previous_ = static_cast<char>(std::toupper(static_cast<unsigned char>(command)));
        return true;
    }
};

} // namespace

std::string lower(const std::string_view text) {
    std::string out(text);
    for (auto& c : out)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::optional<double> parseNumber(std::string_view text, std::string_view* rest) {
    while (!text.empty() && isSpace(text.front()))
        text.remove_prefix(1);
    std::size_t i = 0;
    const auto n = text.size();
    if (i < n && (text[i] == '+' || text[i] == '-'))
        ++i;
    const auto digitsStart = i;
    while (i < n && std::isdigit(static_cast<unsigned char>(text[i])) != 0)
        ++i;
    bool any = i > digitsStart;
    if (i < n && text[i] == '.') {
        ++i;
        const auto fractionStart = i;
        while (i < n && std::isdigit(static_cast<unsigned char>(text[i])) != 0)
            ++i;
        any = any || i > fractionStart;
    }
    if (!any)
        return std::nullopt;
    if (i < n && (text[i] == 'e' || text[i] == 'E')) {
        auto j = i + 1;
        if (j < n && (text[j] == '+' || text[j] == '-'))
            ++j;
        const auto exponentStart = j;
        while (j < n && std::isdigit(static_cast<unsigned char>(text[j])) != 0)
            ++j;
        if (j > exponentStart)
            i = j;
    }
    double value = 0;
    auto token = text.substr(0, i);
    if (!token.empty() && token.front() == '+')
        token.remove_prefix(1);
    const auto result = core::fromChars(token.data(), token.data() + token.size(), value);
    if (result.ec != std::errc{} && result.ec != std::errc::result_out_of_range)
        return std::nullopt;
    if (!std::isfinite(value))
        return std::nullopt;
    if (rest != nullptr)
        *rest = text.substr(i);
    return value;
}

std::optional<Length> parseLength(const std::string_view text) {
    std::string_view rest;
    const auto number = parseNumber(text, &rest);
    if (!number)
        return std::nullopt;
    const auto unit = lower(trim(rest));
    Length length{*number, 0};
    if (unit.empty() || unit == "px")
        return length;
    if (unit == "%") {
        length.unit = 1;
        return length;
    }
    if (unit == "em") {
        length.unit = 2;
        return length;
    }
    if (unit == "ex") {
        length.unit = 3;
        return length;
    }
    double scale = 0;
    if (unit == "pt")
        scale = 96.0 / 72.0;
    else if (unit == "pc")
        scale = 16.0;
    else if (unit == "mm")
        scale = 96.0 / 25.4;
    else if (unit == "cm")
        scale = 96.0 / 2.54;
    else if (unit == "in")
        scale = 96.0;
    else
        return std::nullopt;
    length.value *= scale;
    return length;
}

std::optional<core::Color4d> parseColor(std::string_view text) {
    text = trim(text);
    if (text.empty())
        return std::nullopt;
    if (text.front() == '#') {
        const auto digits = text.substr(1);
        std::array<int, 8> d{};
        if (digits.size() != 3 && digits.size() != 4 && digits.size() != 6 && digits.size() != 8)
            return std::nullopt;
        for (std::size_t i = 0; i < digits.size(); ++i) {
            d[i] = hexDigit(digits[i]);
            if (d[i] < 0)
                return std::nullopt;
        }
        if (digits.size() <= 4) {
            return core::Color4d{d[0] / 15.0, d[1] / 15.0, d[2] / 15.0, digits.size() == 4 ? d[3] / 15.0 : 1.0};
        }
        return core::Color4d{(d[0] * 16 + d[1]) / 255.0, (d[2] * 16 + d[3]) / 255.0,
                             (d[4] * 16 + d[5]) / 255.0, digits.size() == 8 ? (d[6] * 16 + d[7]) / 255.0 : 1.0};
    }
    const auto open = text.find('(');
    if (open != std::string_view::npos && text.back() == ')') {
        const auto name = lower(trim(text.substr(0, open)));
        const auto arguments = splitArguments(text.substr(open + 1, text.size() - open - 2));
        if ((name == "rgb" || name == "rgba") && arguments.size() >= 3) {
            const auto r = numberOrPercent(arguments[0], 255.0), g = numberOrPercent(arguments[1], 255.0),
                       b = numberOrPercent(arguments[2], 255.0);
            double alpha = 1.0;
            if (arguments.size() >= 4) {
                const auto a = numberOrPercent(arguments[3], 1.0);
                if (!a)
                    return std::nullopt;
                alpha = *a;
            }
            if (!r || !g || !b)
                return std::nullopt;
            return core::Color4d{clamp01(*r / 255.0), clamp01(*g / 255.0), clamp01(*b / 255.0), clamp01(alpha)};
        }
        if ((name == "hsl" || name == "hsla") && arguments.size() >= 3) {
            std::string_view rest;
            const auto h = parseNumber(arguments[0], &rest);
            const auto s = numberOrPercent(arguments[1], 1.0), l = numberOrPercent(arguments[2], 1.0);
            double alpha = 1.0;
            if (arguments.size() >= 4) {
                const auto a = numberOrPercent(arguments[3], 1.0);
                if (!a)
                    return std::nullopt;
                alpha = *a;
            }
            if (!h || !s || !l)
                return std::nullopt;
            return hslToRgb(*h, *s, *l, clamp01(alpha));
        }
        return std::nullopt;
    }
    const auto key = lower(text);
    if (key == "transparent")
        return core::Color4d{0, 0, 0, 0};
    const auto found = std::ranges::lower_bound(kNamedColors, key, {}, &NamedColor::name);
    if (found != kNamedColors.end() && found->name == key)
        return fromRgb8(found->rgb);
    return std::nullopt;
}

std::optional<std::string> parseUrlReference(std::string_view text, std::string_view* rest) {
    text = trim(text);
    if (text.size() < 5 || lower(text.substr(0, 4)) != "url(")
        return std::nullopt;
    const auto close = text.find(')');
    if (close == std::string_view::npos)
        return std::nullopt;
    auto inner = trim(text.substr(4, close - 4));
    if (!inner.empty() && (inner.front() == '"' || inner.front() == '\''))
        inner = inner.substr(1, inner.size() >= 2 ? inner.size() - 2 : 0);
    if (inner.empty() || inner.front() != '#')
        return std::nullopt; // Only a fragment of this document is ever followed.
    if (rest != nullptr)
        *rest = text.substr(close + 1);
    return std::string(inner.substr(1));
}

std::optional<SvgAffine> parseTransform(std::string_view text) {
    SvgAffine total;
    text = trim(text);
    while (!text.empty()) {
        const auto open = text.find('(');
        const auto close = text.find(')');
        if (open == std::string_view::npos || close == std::string_view::npos || close < open)
            return std::nullopt;
        const auto name = lower(trim(text.substr(0, open)));
        std::vector<double> v;
        auto args = text.substr(open + 1, close - open - 1);
        while (true) {
            while (!args.empty() && (isSpace(args.front()) || args.front() == ','))
                args.remove_prefix(1);
            if (args.empty())
                break;
            std::string_view rest;
            const auto n = parseNumber(args, &rest);
            if (!n)
                return std::nullopt;
            v.push_back(*n);
            args = rest;
        }
        SvgAffine m;
        constexpr double kDegrees = std::numbers::pi / 180.0;
        if (name == "matrix" && v.size() == 6)
            m = {v[0], v[1], v[2], v[3], v[4], v[5]};
        else if (name == "translate" && (v.size() == 1 || v.size() == 2))
            m = {1, 0, 0, 1, v[0], v.size() == 2 ? v[1] : 0.0};
        else if (name == "scale" && (v.size() == 1 || v.size() == 2))
            m = {v[0], 0, 0, v.size() == 2 ? v[1] : v[0], 0, 0};
        else if (name == "rotate" && (v.size() == 1 || v.size() == 3)) {
            const double c = std::cos(v[0] * kDegrees), s = std::sin(v[0] * kDegrees);
            m = {c, s, -s, c, 0, 0};
            if (v.size() == 3)
                m = SvgAffine{1, 0, 0, 1, v[1], v[2]} * m * SvgAffine{1, 0, 0, 1, -v[1], -v[2]};
        } else if (name == "skewx" && v.size() == 1)
            m = {1, 0, std::tan(v[0] * kDegrees), 1, 0, 0};
        else if (name == "skewy" && v.size() == 1)
            m = {1, std::tan(v[0] * kDegrees), 0, 1, 0, 0};
        else
            return std::nullopt;
        total = total * m;
        text = trim(text.substr(close + 1));
        if (!text.empty() && text.front() == ',')
            text = trim(text.substr(1));
    }
    for (const double value : {total.a, total.b, total.c, total.d, total.e, total.f})
        if (!std::isfinite(value) || std::fabs(value) > 1e12)
            return std::nullopt;
    return total;
}

PathParse parsePathData(const std::string_view data) { return PathReader(data).run(); }

SvgPoint transformPoint(const SvgAffine& m, const SvgPoint p) noexcept {
    return {m.a * p.x + m.c * p.y + m.e, m.b * p.x + m.d * p.y + m.f};
}
void transformContour(const SvgAffine& m, SvgContour& contour) noexcept {
    for (auto& anchor : contour.anchors) {
        anchor.point = transformPoint(m, anchor.point);
        if (anchor.hasIn)
            anchor.in = transformPoint(m, anchor.in);
        if (anchor.hasOut)
            anchor.out = transformPoint(m, anchor.out);
    }
}

SvgContour ellipseContour(const double cx, const double cy, const double rx, const double ry) {
    constexpr double k = 0.5522847498307936;
    SvgContour contour;
    contour.closed = true;
    const SvgPoint right{cx + rx, cy}, bottom{cx, cy + ry}, left{cx - rx, cy}, top{cx, cy - ry};
    contour.anchors = {
        {right, {right.x, right.y - k * ry}, {right.x, right.y + k * ry}, true, true},
        {bottom, {bottom.x + k * rx, bottom.y}, {bottom.x - k * rx, bottom.y}, true, true},
        {left, {left.x, left.y + k * ry}, {left.x, left.y - k * ry}, true, true},
        {top, {top.x - k * rx, top.y}, {top.x + k * rx, top.y}, true, true},
    };
    return contour;
}

SvgContour roundedRectContour(const double x, const double y, const double w, const double h,
                              double rx, double ry) {
    rx = std::clamp(rx, 0.0, w / 2.0);
    ry = std::clamp(ry, 0.0, h / 2.0);
    SvgContour contour;
    contour.closed = true;
    if (rx <= 0.0 || ry <= 0.0) {
        contour.anchors = {{{x, y}, {}, {}, false, false},
                           {{x + w, y}, {}, {}, false, false},
                           {{x + w, y + h}, {}, {}, false, false},
                           {{x, y + h}, {}, {}, false, false}};
        return contour;
    }
    constexpr double k = 0.5522847498307936;
    const double kx = k * rx, ky = k * ry;
    // Eight anchors, going clockwise from the top edge's left end.
    contour.anchors = {
        {{x + rx, y}, {}, {}, false, false},
        {{x + w - rx, y}, {}, {x + w - rx + kx, y}, false, true},
        {{x + w, y + ry}, {x + w, y + ry - ky}, {}, true, false},
        {{x + w, y + h - ry}, {}, {x + w, y + h - ry + ky}, false, true},
        {{x + w - rx, y + h}, {x + w - rx + kx, y + h}, {}, true, false},
        {{x + rx, y + h}, {}, {x + rx - kx, y + h}, false, true},
        {{x, y + h - ry}, {x, y + h - ry + ky}, {}, true, false},
        {{x, y + ry}, {}, {x, y + ry - ky}, false, true},
    };
    contour.anchors[0].in = {x + rx - kx, y};
    contour.anchors[0].hasIn = false;
    // The top-left corner arc runs from the last anchor to the first.
    contour.anchors[0].in = {x + rx - kx, y};
    contour.anchors[0].hasIn = true;
    return contour;
}

// ---- CSS ----

std::vector<Declaration> parseDeclarations(const std::string_view text) {
    std::vector<Declaration> out;
    std::size_t i = 0;
    while (i < text.size()) {
        const auto end = [&] {
            // A semicolon inside url(...) or quotes does not end the declaration.
            int depth = 0;
            char quote = 0;
            for (auto j = i; j < text.size(); ++j) {
                const char c = text[j];
                if (quote != 0) {
                    if (c == quote)
                        quote = 0;
                } else if (c == '"' || c == '\'')
                    quote = c;
                else if (c == '(')
                    ++depth;
                else if (c == ')')
                    depth = std::max(0, depth - 1);
                else if (c == ';' && depth == 0)
                    return j;
            }
            return text.size();
        }();
        const auto piece = text.substr(i, end - i);
        i = end + 1;
        const auto colon = piece.find(':');
        if (colon == std::string_view::npos)
            continue;
        auto name = lower(trim(piece.substr(0, colon)));
        auto value = std::string(trim(piece.substr(colon + 1)));
        if (const auto bang = value.find('!'); bang != std::string::npos)
            value = std::string(trim(std::string_view(value).substr(0, bang)));
        if (!name.empty())
            out.push_back({std::move(name), std::move(value)});
    }
    return out;
}

namespace {

std::string stripComments(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '/' && i + 1 < text.size() && text[i + 1] == '*') {
            const auto end = text.find("*/", i + 2);
            if (end == std::string_view::npos)
                break;
            i = end + 1;
        } else {
            out.push_back(text[i]);
        }
    }
    return out;
}

// One compound selector ("g.cluster#a[data-x]") and the combinator before it.
bool parseCompound(std::string_view text, CssRule::Part& part) {
    std::size_t i = 0;
    const auto identifier = [&] {
        const auto start = i;
        while (i < text.size() && (std::isalnum(static_cast<unsigned char>(text[i])) != 0 || text[i] == '-' ||
                                   text[i] == '_' || (static_cast<unsigned char>(text[i]) & 0x80U) != 0))
            ++i;
        return std::string(text.substr(start, i - start));
    };
    if (i < text.size() && text[i] == '*') {
        ++i;
    } else if (i < text.size() && (std::isalpha(static_cast<unsigned char>(text[i])) != 0)) {
        part.type = identifier();
    }
    while (i < text.size()) {
        if (text[i] == '.') {
            ++i;
            part.classes.push_back(identifier());
        } else if (text[i] == '#') {
            ++i;
            part.id = identifier();
        } else if (text[i] == '[') {
            const auto close = text.find(']', i);
            if (close == std::string_view::npos)
                return false;
            auto body = text.substr(i + 1, close - i - 1);
            const auto eq = body.find('=');
            if (eq == std::string_view::npos) {
                part.attributes.push_back({std::string(trim(body)), std::nullopt});
            } else {
                auto value = trim(body.substr(eq + 1));
                if (!value.empty() && (value.front() == '"' || value.front() == '\''))
                    value = value.substr(1, value.size() >= 2 ? value.size() - 2 : 0);
                auto name = trim(body.substr(0, eq));
                if (!name.empty() && std::string("~|^$*").find(name.back()) != std::string::npos)
                    return false;
                part.attributes.push_back({std::string(name), std::string(value)});
            }
            i = close + 1;
        } else if (text[i] == ':') {
            if (text.substr(i) == ":root") {
                part.rootOnly = true;
                i += 5;
            } else {
                return false;
            }
        } else {
            return false;
        }
    }
    return true;
}

bool parseSelector(std::string_view text, CssRule& rule) {
    text = trim(text);
    if (text.empty())
        return false;
    std::size_t i = 0;
    char combinator = ' ';
    int specificity = 0;
    while (i < text.size()) {
        while (i < text.size() && isSpace(text[i]))
            ++i;
        if (i >= text.size())
            break;
        if (text[i] == '>') {
            combinator = '>';
            ++i;
            continue;
        }
        if (text[i] == '+' || text[i] == '~')
            return false;
        const auto start = i;
        int bracket = 0;
        while (i < text.size() && (bracket > 0 || (!isSpace(text[i]) && text[i] != '>'))) {
            if (text[i] == '[')
                ++bracket;
            else if (text[i] == ']')
                --bracket;
            ++i;
        }
        CssRule::Part part;
        part.combinator = rule.parts.empty() ? ' ' : combinator;
        if (!parseCompound(text.substr(start, i - start), part))
            return false;
        specificity += (part.id.empty() ? 0 : 100) +
                       10 * static_cast<int>(part.classes.size() + part.attributes.size() + (part.rootOnly ? 1 : 0)) +
                       (part.type.empty() ? 0 : 1);
        rule.parts.push_back(std::move(part));
        combinator = ' ';
    }
    rule.specificity = specificity;
    return !rule.parts.empty();
}

} // namespace

void parseStyleSheet(const std::string_view raw, StyleSheet& sheet) {
    const auto text = stripComments(raw);
    std::string_view rest = text;
    const auto noteUnsupported = [&](const std::string& what) {
        if (sheet.unsupported.size() < 64 && std::ranges::find(sheet.unsupported, what) == sheet.unsupported.end())
            sheet.unsupported.push_back(what);
    };
    while (true) {
        rest = trim(rest);
        if (rest.empty())
            break;
        const auto open = rest.find('{');
        if (open == std::string_view::npos)
            break;
        const auto prelude = trim(rest.substr(0, open));
        // Find the matching brace (at-rules nest).
        int depth = 0;
        std::size_t close = std::string_view::npos;
        for (std::size_t i = open; i < rest.size(); ++i) {
            if (rest[i] == '{')
                ++depth;
            else if (rest[i] == '}' && --depth == 0) {
                close = i;
                break;
            }
        }
        if (close == std::string_view::npos)
            break;
        const auto body = rest.substr(open + 1, close - open - 1);
        rest = rest.substr(close + 1);
        if (!prelude.empty() && prelude.front() == '@') {
            noteUnsupported(std::string(prelude.substr(0, prelude.find_first_of(" \t\n({"))));
            continue;
        }
        const auto declarations = parseDeclarations(body);
        std::size_t start = 0;
        for (std::size_t i = 0; i <= prelude.size(); ++i) {
            if (i == prelude.size() || prelude[i] == ',') {
                const auto selectorText = prelude.substr(start, i - start);
                start = i + 1;
                CssRule rule;
                if (!parseSelector(selectorText, rule)) {
                    noteUnsupported(std::string(trim(selectorText)));
                    continue;
                }
                rule.order = sheet.rules.size();
                rule.declarations = declarations;
                sheet.rules.push_back(std::move(rule));
            }
        }
    }
}

namespace {

bool partMatches(const CssRule::Part& part, const XmlDocument& document, const std::size_t node) {
    const auto& n = document.nodes[node];
    if (!part.type.empty() && part.type != n.tag)
        return false;
    if (part.rootOnly && node != 0)
        return false;
    if (!part.id.empty()) {
        const auto* id = n.find("id");
        if (id == nullptr || *id != part.id)
            return false;
    }
    if (!part.classes.empty()) {
        const auto* value = n.find("class");
        if (value == nullptr)
            return false;
        for (const auto& wanted : part.classes) {
            bool found = false;
            std::string_view rest = *value;
            while (!rest.empty()) {
                rest = trim(rest);
                const auto end = rest.find_first_of(" \t\r\n");
                const auto token = rest.substr(0, end);
                if (token == wanted)
                    found = true;
                rest = end == std::string_view::npos ? std::string_view{} : rest.substr(end);
            }
            if (!found)
                return false;
        }
    }
    for (const auto& [name, value] : part.attributes) {
        const auto* actual = n.find(name);
        if (actual == nullptr || (value && *actual != *value))
            return false;
    }
    return true;
}

bool matchFrom(const CssRule& rule, const XmlDocument& document, std::size_t node, std::size_t partIndex) {
    if (!partMatches(rule.parts[partIndex], document, node))
        return false;
    if (partIndex == 0)
        return true;
    const auto combinator = rule.parts[partIndex].combinator;
    if (node == 0)
        return false;
    if (combinator == '>')
        return matchFrom(rule, document, document.nodes[node].parent, partIndex - 1);
    for (auto ancestor = document.nodes[node].parent;; ancestor = document.nodes[ancestor].parent) {
        if (matchFrom(rule, document, ancestor, partIndex - 1))
            return true;
        if (ancestor == 0)
            return false;
    }
}

} // namespace

bool ruleMatches(const CssRule& rule, const XmlDocument& document, const std::size_t node) {
    return matchFrom(rule, document, node, rule.parts.size() - 1);
}

} // namespace bloom::media::svg
