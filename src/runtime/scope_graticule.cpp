#include <bloom/runtime/scope_picture.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <numbers>

namespace bloom::runtime::scopes {
namespace {

constexpr std::array<int, 9> kLabelCodes{0, 128, 256, 384, 512, 640, 768, 896, 1023};

// The position of a code on an axis: a code covers 1/1024 of it, so its center is (code + 0.5).
[[nodiscard]] double codeFraction(const int code) {
    return (static_cast<double>(code) + 0.5) / 1024.0;
}

// A line position on the center of a device pixel.
[[nodiscard]] double snap(const double value, const double dpr) {
    return (std::floor(value * dpr) + 0.5) / dpr;
}

[[nodiscard]] ColorRole channelRole(const std::size_t channel) {
    constexpr std::array<ColorRole, 4> roles{ColorRole::Red, ColorRole::Green, ColorRole::Blue,
                                             ColorRole::Luma};
    return roles[channel];
}
constexpr std::array<const char*, 4> kChannelLetters{"R", "G", "B", "Y"};

struct Target final {
    const char* name;
    float red;
    float green;
    float blue;
};
// The 75% color bars.
constexpr std::array<Target, 6> kTargets{{{"R", 0.75F, 0.0F, 0.0F},
                                          {"Mg", 0.75F, 0.0F, 0.75F},
                                          {"B", 0.0F, 0.0F, 0.75F},
                                          {"Cy", 0.0F, 0.75F, 0.75F},
                                          {"G", 0.0F, 0.75F, 0.0F},
                                          {"Yl", 0.75F, 0.75F, 0.0F}}};

[[nodiscard]] Point vectorscopePoint(const ScopeLuma& luma, const Target& target) {
    const float y = (luma.red * target.red + luma.green * target.green) + luma.blue * target.blue;
    const float cb = (target.blue - y) / luma.cbDenominator;
    const float cr = (target.red - y) / luma.crDenominator;
    return {static_cast<double>(cb), static_cast<double>(cr)};
}

[[nodiscard]] int pixelsOf(const double logical, const double dpr) {
    return std::max(1, static_cast<int>(std::lround(logical * dpr)));
}

class Builder final {
  public:
    explicit Builder(const GraticuleRequest& request) : request_(request) {}

    [[nodiscard]] ScopeGraticule build() {
        plan_.kind = request_.kind;
        plan_.width = request_.width;
        plan_.height = request_.height;
        plan_.devicePixelRatio = dpr();
        plan_.hairlineWidth = std::max(1.0, std::round(dpr())) / dpr();
        geometry(request_.width, request_.height);
        sizes();
        switch (request_.kind) {
        case Kind::Waveform:
        case Kind::Parade:
            waveform();
            break;
        case Kind::Vectorscope:
            vectorscope();
            break;
        case Kind::Histogram:
            histogram();
            break;
        case Kind::Chromaticity:
            chromaticity();
            break;
        }
        return std::move(plan_);
    }

    // The plot of the geometry alone, for heightForWidth.
    [[nodiscard]] ScopeGraticule geometryOnly(const double width, const double height) {
        plan_.kind = request_.kind;
        geometry(width, height);
        return std::move(plan_);
    }

  private:
    [[nodiscard]] double dpr() const {
        return request_.devicePixelRatio > 0.0 ? request_.devicePixelRatio : 1.0;
    }
    [[nodiscard]] TextSize measure(const std::string_view text, const LabelFont font) const {
        return request_.measure ? request_.measure(text, font) : TextSize{};
    }

    // The geometry of ScopeView: the plot of every scope and where its trace image goes.
    void geometry(const double width, const double height) {
        const auto& m = request_.metrics;
        const double labelHeight = measure("0", LabelFont::Numeric).height;
        const double pad = m.padding;
        const double small = m.small;
        switch (request_.kind) {
        case Kind::Waveform:
        case Kind::Parade: {
            const double left = measure("1023", LabelFont::Numeric).width + small * 2;
            const double top = std::ceil(labelHeight / 2) + m.tiny;
            plan_.plot = Rect{left, top, std::max(1.0, width - left - pad),
                              std::max(1.0, height - top - m.majorTick - small)};
            plan_.image = plan_.plot;
            break;
        }
        case Kind::Histogram: {
            plan_.rows = static_cast<int>(histogramChannelList(request_.histogramChannels).size());
            plan_.rowHeight = kHistogramRowHeight;
            const double top = std::ceil(labelHeight) + small * 2;
            plan_.plot =
                Rect{pad, top, std::max(1.0, width - pad * 2), plan_.rowHeight * plan_.rows};
            plan_.image = plan_.plot;
            break;
        }
        case Kind::Vectorscope: {
            const double side = std::max(1.0, std::min(width, height) - pad * 2);
            plan_.plot = Rect{(width - side) / 2, (height - side) / 2, side, side};
            plan_.image = plan_.plot;
            break;
        }
        case Kind::Chromaticity: {
            const double left = measure("1.0", LabelFont::Numeric).width + small * 2;
            const double bottom = std::ceil(labelHeight) + small * 2;
            const double side = std::max(1.0, std::min(width - left - pad, height - bottom - pad));
            plan_.plot = Rect{left + (width - left - pad - side) / 2, pad, side, side};
            plan_.image =
                Rect{plan_.plot.left(), plan_.plot.bottom() - side * kChromaticityYMaximum,
                     side * kChromaticityXMaximum, side * kChromaticityYMaximum};
            break;
        }
        }
    }

    // The size to ask the mapping for, the size of the image it returns, and where that image is
    // drawn: on whole device pixels, so the trace is copied and never resampled.
    void sizes() {
        const double ratio = dpr();
        const auto& plot = plan_.plot;
        switch (request_.kind) {
        case Kind::Histogram:
            plan_.traceRequestSize = {pixelsOf(plot.width, ratio),
                                      pixelsOf(plan_.rowHeight, ratio)};
            plan_.traceImageSize = {plan_.traceRequestSize.width,
                                    plan_.traceRequestSize.height * std::max(plan_.rows, 0)};
            break;
        case Kind::Chromaticity:
            plan_.traceRequestSize = {pixelsOf(plot.width, ratio), pixelsOf(plot.height, ratio)};
            plan_.traceImageSize = chromaticityImageSize(plan_.traceRequestSize.width);
            break;
        case Kind::Waveform:
        case Kind::Parade:
        case Kind::Vectorscope:
            plan_.traceRequestSize = {pixelsOf(plot.width, ratio), pixelsOf(plot.height, ratio)};
            plan_.traceImageSize = plan_.traceRequestSize;
            break;
        }
        plan_.trace = Rect{std::round(plan_.image.left() * ratio) / ratio,
                           std::round(plan_.image.top() * ratio) / ratio,
                           plan_.traceImageSize.width / ratio, plan_.traceImageSize.height / ratio};
    }

    void line(const Point from, const Point to, const ColorRole color, const Layer layer,
              const bool antialiased = false, const bool dashed = false) {
        plan_.lines.push_back({from, to, color, layer, antialiased, dashed});
    }
    void label(std::string text, const Point anchor, const HorizontalAlign horizontal,
               const VerticalAlign vertical, const LabelFont font, const ColorRole color) {
        plan_.labels.push_back({std::move(text), anchor, horizontal, vertical, font, color});
    }
    [[nodiscard]] std::string axisLabel(const int code) const {
        return scopeAxisLabel(code, request_.signal, request_.axisMaximum);
    }

    void waveform() {
        const double ratio = dpr();
        const auto& m = request_.metrics;
        const auto& plot = plan_.plot;
        const bool parade = request_.kind == Kind::Parade;
        const double gap = kParadeGap;
        const double paneWidth = parade ? (plot.width - 2 * gap) / 3 : plot.width;
        const int paneCount = parade ? 3 : 1;
        for (int pane = 0; pane < paneCount; ++pane)
            plan_.panes.push_back(
                Rect{plot.left() + pane * (paneWidth + gap), plot.top(), paneWidth, plot.height});

        // Graticule under the trace: a quiet line at every labelled code.
        for (const int code : kLabelCodes) {
            const double y = snap(plot.bottom() - codeFraction(code) * plot.height, ratio);
            for (const auto& pane : plan_.panes)
                line({pane.left(), y}, {pane.right(), y}, ColorRole::Quiet, Layer::UnderTrace);
        }
        // Minor ticks between the labelled lines, down the left edge.
        for (int code = 0; code <= 1023; code += 32) {
            if (code % 128 == 0)
                continue;
            const double y = snap(plot.bottom() - codeFraction(code) * plot.height, ratio);
            line({plot.left() - m.minorTick, y}, {plot.left(), y}, ColorRole::Stronger,
                 Layer::UnderTrace);
        }
        // And along the bottom, sixteen steps of each pane.
        for (const auto& pane : plan_.panes)
            for (int step = 0; step <= 16; ++step) {
                const double x = snap(pane.left() + pane.width * step / 16.0, ratio);
                const double length = step % 4 == 0 ? m.majorTick : m.minorTick;
                line({x, plot.bottom()}, {x, plot.bottom() + length}, ColorRole::Stronger,
                     Layer::UnderTrace);
            }

        // Clip ticks: a column with an under or over count marks the bottom or top edge.
        if (request_.clip != nullptr && !request_.clip->columns.empty()) {
            const auto& columns = request_.clip->columns;
            const auto count = static_cast<double>(columns.size());
            for (const auto& pane : plan_.panes) {
                const double width = std::max(1.0 / ratio, pane.width / count);
                for (std::size_t column = 0; column < columns.size(); ++column) {
                    const double x = pane.left() + pane.width * static_cast<double>(column) / count;
                    if ((columns[column] & 1U) != 0)
                        plan_.fills.push_back(
                            {Rect{x, pane.bottom() - kClipMarkerWidth, width, kClipMarkerWidth},
                             ColorRole::Warning});
                    if ((columns[column] & 2U) != 0)
                        plan_.fills.push_back(
                            {Rect{x, pane.top(), width, kClipMarkerWidth}, ColorRole::Warning});
                }
            }
        }

        // Labels never overlap the trace area: they sit in the left gutter.
        for (const int code : kLabelCodes) {
            const double y = plot.bottom() - codeFraction(code) * plot.height;
            label(axisLabel(code), {plot.left() - m.small, y}, HorizontalAlign::Right,
                  VerticalAlign::Middle, LabelFont::Numeric, ColorRole::Label);
        }
        if (parade)
            for (std::size_t pane = 0; pane < 3; ++pane)
                label(std::string(kChannelLetters[pane]) + "\xE2\x80\x89",
                      {plan_.panes[pane].right(), plan_.panes[pane].top() + m.small},
                      HorizontalAlign::Right, VerticalAlign::Top, LabelFont::Name,
                      channelRole(pane));
    }

    void vectorscope() {
        const double ratio = dpr();
        const auto& m = request_.metrics;
        const auto& plot = plan_.plot;
        const Point center = plot.center();
        const double radius = plot.width / 2;
        // Cross hairs.
        line({plot.left(), snap(center.y, ratio)}, {plot.right(), snap(center.y, ratio)},
             ColorRole::Quiet, Layer::UnderTrace);
        line({snap(center.x, ratio), plot.top()}, {snap(center.x, ratio), plot.bottom()},
             ColorRole::Quiet, Layer::UnderTrace);
        // The circle of radius 0.5 and the tick marks around it, every 10 degrees and longer every
        // 30.
        plan_.circles.push_back({center, radius, ColorRole::Stronger, Layer::UnderTrace});
        for (int degrees = 0; degrees < 360; degrees += 10) {
            const double angle = degrees * std::numbers::pi / 180.0;
            const double length = degrees % 30 == 0 ? m.majorTick : m.minorTick;
            const Point direction{std::cos(angle), -std::sin(angle)};
            line({center.x + direction.x * radius, center.y + direction.y * radius},
                 {center.x + direction.x * (radius - length),
                  center.y + direction.y * (radius - length)},
                 ColorRole::Label, Layer::UnderTrace, true);
        }
        // The skin tone line, thin, from the center at 123 degrees counter-clockwise from +Cb.
        {
            const double angle = kSkinToneDegrees * std::numbers::pi / 180.0;
            line(center, {center.x + std::cos(angle) * radius, center.y - std::sin(angle) * radius},
                 ColorRole::Faint, Layer::UnderTrace, true, true);
        }
        // The targets: four corner brackets each, with the letter beside, at the 75% bars.
        if (request_.luma != nullptr) {
            const auto& luma = *request_.luma;
            const double half = std::max(5.0, plot.width * 0.025);
            const double arm = half * 0.8;
            for (const auto& target : kTargets) {
                const auto position = vectorscopePoint(luma, target);
                const Point at{snap(center.x + position.x * plot.width, ratio),
                               snap(center.y - position.y * plot.height, ratio)};
                for (const int sx : {-1, 1})
                    for (const int sy : {-1, 1}) {
                        const Point corner{at.x + sx * half, at.y + sy * half};
                        line(corner, {corner.x - sx * arm, corner.y}, ColorRole::Label,
                             Layer::OverTrace);
                        line(corner, {corner.x, corner.y - sy * arm}, ColorRole::Label,
                             Layer::OverTrace);
                    }
            }
            for (const auto& target : kTargets) {
                const auto position = vectorscopePoint(luma, target);
                const Point at{center.x + position.x * plot.width,
                               center.y - position.y * plot.height};
                label(target.name, {at.x + half + m.tiny, at.y - half - m.small},
                      HorizontalAlign::Left, VerticalAlign::Top, LabelFont::Name, ColorRole::Label);
            }
        }
    }

    void histogram() {
        const double ratio = dpr();
        const auto& m = request_.metrics;
        const auto& plot = plan_.plot;
        const auto channels = histogramChannelList(request_.histogramChannels);
        // Row frames, then the vertical graticule through every row.
        for (int row = 0; row <= plan_.rows; ++row) {
            const double y = snap(plot.top() + row * plan_.rowHeight, ratio);
            line({plot.left(), y}, {plot.right(), y}, ColorRole::Quiet, Layer::UnderTrace);
        }
        for (const int code : kLabelCodes) {
            const double x = snap(plot.left() + codeFraction(code) * plot.width, ratio);
            line({x, plot.top()}, {x, plot.bottom()}, ColorRole::Quiet, Layer::UnderTrace);
        }
        // Minor ticks between the labelled lines along the top.
        for (int code = 0; code <= 1023; code += 32) {
            if (code % 128 == 0)
                continue;
            const double x = snap(plot.left() + codeFraction(code) * plot.width, ratio);
            line({x, plot.top() - m.minorTick}, {x, plot.top()}, ColorRole::Stronger,
                 Layer::UnderTrace);
        }
        // Clip markers at either end of a row whose channel has an under or over count.
        if (request_.clip != nullptr)
            for (std::size_t row = 0; row < channels.size(); ++row) {
                const auto channel = channels[row];
                const double top = plot.top() + static_cast<double>(row) * plan_.rowHeight;
                if (request_.clip->under[channel] != 0)
                    plan_.fills.push_back(
                        {Rect{plot.left(), top, kClipMarkerWidth, plan_.rowHeight},
                         ColorRole::Warning});
                if (request_.clip->over[channel] != 0)
                    plan_.fills.push_back({Rect{plot.right() - kClipMarkerWidth, top,
                                                kClipMarkerWidth, plan_.rowHeight},
                                           ColorRole::Warning});
            }
        // Code labels across the top, in the gutter above the rows.
        const double labelHeight = measure("0", LabelFont::Numeric).height;
        const double labelTop = plot.top() - m.minorTick - labelHeight;
        for (const int code : kLabelCodes) {
            const double x = plot.left() + codeFraction(code) * plot.width;
            auto text = axisLabel(code);
            const double width = measure(text, LabelFont::Numeric).width + 2;
            const double left =
                std::clamp(x - width / 2, 0.0, std::max(0.0, request_.width - width));
            label(std::move(text), {left + width / 2, labelTop + labelHeight / 2},
                  HorizontalAlign::Center, VerticalAlign::Middle, LabelFont::Numeric,
                  ColorRole::Label);
        }
        for (std::size_t row = 0; row < channels.size(); ++row)
            label(kChannelLetters[channels[row]],
                  {plot.left() + m.small,
                   plot.top() + static_cast<double>(row) * plan_.rowHeight + m.tiny},
                  HorizontalAlign::Left, VerticalAlign::Top, LabelFont::Name,
                  channelRole(channels[row]));
    }

    void chromaticity() {
        const double ratio = dpr();
        const auto& m = request_.metrics;
        const auto& plot = plan_.plot;
        const double side = plot.width;
        const auto map = [&plot, side](const double x, const double y) {
            return Point{plot.left() + x * side, plot.bottom() - y * side};
        };
        // Grid every 0.1, labelled every 0.2.
        for (int step = 0; step <= 10; ++step) {
            const auto role = step == 0 || step == 10 ? ColorRole::Stronger : ColorRole::Quiet;
            const double x = snap(plot.left() + side * step / 10.0, ratio);
            const double y = snap(plot.bottom() - side * step / 10.0, ratio);
            line({x, plot.top()}, {x, plot.bottom()}, role, Layer::UnderTrace);
            line({plot.left(), y}, {plot.right(), y}, role, Layer::UnderTrace);
        }
        // The spectral locus: a thin curve in the strongest role with a tick every 10 nm.
        static const auto locus = color::cie1931SpectralLocus();
        if (!locus.empty()) {
            Polyline curve;
            curve.color = ColorRole::Strongest;
            for (const auto& point : locus)
                curve.points.push_back(map(point.x, point.y));
            plan_.polylines.push_back(std::move(curve));
            // The line of purples closes the shape, quieter than the locus itself.
            line(map(locus.back().x, locus.back().y), map(locus.front().x, locus.front().y),
                 ColorRole::Label, Layer::OverTrace, true);
            for (std::size_t index = 1; index + 1 < locus.size(); ++index) {
                if (locus[index].wavelengthNm % 10 != 0 || locus[index].wavelengthNm < 460 ||
                    locus[index].wavelengthNm > 620)
                    continue;
                const auto before = map(locus[index - 1].x, locus[index - 1].y);
                const auto after = map(locus[index + 1].x, locus[index + 1].y);
                const Point along{after.x - before.x, after.y - before.y};
                const double length = std::hypot(along.x, along.y);
                if (length <= 0)
                    continue;
                // The normal points away from the inside of the horseshoe.
                Point normal{-along.y / length, along.x / length};
                const auto point = map(locus[index].x, locus[index].y);
                const auto center = plot.center();
                if ((point.x - center.x) * normal.x + (point.y - center.y) * normal.y < 0) {
                    normal.x = -normal.x;
                    normal.y = -normal.y;
                }
                line(point, {point.x + normal.x * m.minorTick, point.y + normal.y * m.minorTick},
                     ColorRole::Strongest, Layer::OverTrace, true);
            }
        }
        if (request_.colorimetry != nullptr) {
            const auto& colorimetry = *request_.colorimetry;
            Polyline triangle;
            triangle.closed = true;
            triangle.color = ColorRole::Label;
            triangle.points = {map(colorimetry.red.x, colorimetry.red.y),
                               map(colorimetry.green.x, colorimetry.green.y),
                               map(colorimetry.blue.x, colorimetry.blue.y)};
            plan_.polylines.push_back(std::move(triangle));
            plan_.circles.push_back({map(colorimetry.white.x, colorimetry.white.y), 4.0,
                                     ColorRole::Strongest, Layer::OverTrace});
            const auto greenCorner = map(colorimetry.green.x, colorimetry.green.y);
            label(colorimetry.colorSpaceName,
                  {greenCorner.x + m.small, greenCorner.y - m.padding + 8.0}, HorizontalAlign::Left,
                  VerticalAlign::Middle, LabelFont::Name, ColorRole::Label);
            // The space's name and R, G, B and W to four decimals in the top-right corner.
            const auto line4 = [](const char* letter, const color::ScopeChromaticity value) {
                return std::string(letter) + ' ' + color::formatChromaticityXy(value);
            };
            const std::array<std::string, 5> lines{
                colorimetry.colorSpaceName, line4("R", colorimetry.red),
                line4("G", colorimetry.green), line4("B", colorimetry.blue),
                line4("W", colorimetry.white)};
            const double lineHeight = measure("0", LabelFont::Numeric).height;
            for (std::size_t index = 0; index < lines.size(); ++index)
                label(lines[index],
                      {plot.left() + side - m.padding, plot.top() + m.small +
                                                           static_cast<double>(index) * lineHeight +
                                                           lineHeight / 2},
                      HorizontalAlign::Right, VerticalAlign::Middle, LabelFont::Numeric,
                      ColorRole::Label);
        }
        // Axis labels every 0.2, with the axis names at the ends.
        const double labelHeight = measure("0", LabelFont::Numeric).height;
        for (int step = 0; step <= 10; step += 2) {
            std::string text;
            if (step == 0)
                text = "0";
            else if (step == 10)
                text = "1.0";
            else {
                std::array<char, 16> buffer{};
                std::snprintf(buffer.data(), buffer.size(), "%.1f", step / 10.0);
                text = buffer.data();
            }
            const double x = plot.left() + side * step / 10.0;
            const double y = plot.bottom() - side * step / 10.0;
            label(text, {x, plot.bottom() + m.tiny + labelHeight / 2}, HorizontalAlign::Center,
                  VerticalAlign::Middle, LabelFont::Numeric, ColorRole::Label);
            if (step != 0)
                label(text, {plot.left() - m.small, y}, HorizontalAlign::Right,
                      VerticalAlign::Middle, LabelFont::Numeric, ColorRole::Label);
        }
        label("x", {plot.right(), plot.bottom() + m.tiny + labelHeight / 2}, HorizontalAlign::Right,
              VerticalAlign::Middle, LabelFont::Name, ColorRole::Label);
        label("y", {plot.left() - m.small, plot.top() - m.tiny}, HorizontalAlign::Right,
              VerticalAlign::Top, LabelFont::Name, ColorRole::Label);
    }

    const GraticuleRequest& request_;
    ScopeGraticule plan_;
};

} // namespace

std::string scopeAxisLabel(const int code, const ScopeSignal signal,
                           const std::uint32_t axisMaximum) {
    if (signal == ScopeSignal::Display)
        return std::to_string(code);
    std::array<char, 32> buffer{};
    std::snprintf(buffer.data(), buffer.size(), "%.3g",
                  static_cast<double>(code) / 1023.0 * static_cast<double>(axisMaximum));
    return buffer.data();
}

std::array<VectorscopeTarget, 6> vectorscopeTargets(const ScopeLuma& luma) {
    std::array<VectorscopeTarget, 6> result{};
    for (std::size_t index = 0; index < kTargets.size(); ++index) {
        const auto point = vectorscopePoint(luma, kTargets[index]);
        result[index] = {kTargets[index].name, point.x, point.y};
    }
    return result;
}

ScopeGraticule buildScopeGraticule(const GraticuleRequest& request) {
    if (!(request.width > 0.0) || !(request.height > 0.0) || !std::isfinite(request.width) ||
        !std::isfinite(request.height) || !request.measure)
        return {};
    return Builder(request).build();
}

int scopeHeightForWidth(const GraticuleRequest& request, const int width) {
    switch (request.kind) {
    case Kind::Waveform:
    case Kind::Parade:
        return std::clamp(width / 2, 170, 380);
    case Kind::Vectorscope:
        return std::clamp(width, 240, 480);
    case Kind::Chromaticity:
        return std::clamp(width, 280, 480);
    case Kind::Histogram: {
        if (!request.measure)
            return 200;
        const auto plan = Builder(request).geometryOnly(width, 10000);
        return static_cast<int>(std::ceil(plan.plot.bottom())) +
               static_cast<int>(request.metrics.padding);
    }
    }
    return 200;
}

} // namespace bloom::runtime::scopes
