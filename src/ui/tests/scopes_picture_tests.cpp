// #724 Scopes: the panel and the headless scope picture draw from one definition.
//
// Asserted:
//   * the default palette of the headless picture equals the interface tokens the panel uses, value
//     by value, and so do the default metrics; the panel's own palette and metrics are the
//     defaults; the plan's hairline is the token's snapped hairline at every device pixel ratio;
//   * at device pixel ratio 1, for each scope, an offscreen grab of the panel's view and the
//     headless picture drawn with the panel's palette and text measure agree on the pixels of the
//     trace rectangle (away from lines and labels, which two vector and raster text renderers
//     draw their own way), and the plan's hairline positions are where the grab has graticule
//     pixels.
// No device and no Viewer.

#include <bloom/color/ocio_builtin_registry.hpp>
#include <bloom/render/image_types.hpp>
#include <bloom/render/scope_table_layout.hpp>
#include <bloom/runtime/scope_frame_measure.hpp>
#include <bloom/runtime/scope_measure.hpp>
#include <bloom/runtime/scope_picture.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/kit/tokens.hpp>
#include <bloom/ui/preview_frame_cache.hpp>
#include <bloom/ui/scopes_trace_renderer.hpp>
#include <bloom/ui/scopes_view.hpp>
#include <bloom/ui/task_ui_bridge.hpp>

#include <QApplication>
#include <QElapsedTimer>
#include <QImage>
#include <QPixmap>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <source_location>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace kit = bloom::ui::kit;
namespace render = bloom::render;
namespace runtime = bloom::runtime;
namespace scopes = bloom::runtime::scopes;
namespace ui = bloom::ui;
using render::Rgba32f;
using namespace std::chrono_literals;

class Expectations final {
  public:
    void expect(const bool condition, const std::string& message,
                const std::source_location location = std::source_location::current()) {
        if (condition)
            return;
        ++failures_;
        std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
    }
    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

template <typename Predicate> [[nodiscard]] bool waitUntil(Predicate predicate) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < 60000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    return predicate();
}

[[nodiscard]] Rgba32f px(const float r, const float g, const float b) {
    const auto result = Rgba32f::fromPremultiplied(r, g, b, 1.0F);
    if (!result)
        throw std::logic_error("invalid fixture pixel");
    return *result.value();
}

[[nodiscard]] runtime::ScopeTables measure(const std::vector<Rgba32f>& pixels,
                                           const runtime::ScopeLuma& luma,
                                           const std::uint32_t detail = 0) {
    std::vector<Rgba32f> xyz;
    for (const auto& pixel : pixels) {
        const float r = pixel.red();
        const float g = pixel.green();
        const float b = pixel.blue();
        xyz.push_back(px(0.4124F * r + 0.3576F * g + 0.1805F * b,
                         0.2126F * r + 0.7152F * g + 0.0722F * b,
                         0.0193F * r + 0.1192F * g + 0.9505F * b));
    }
    runtime::ScopeMeasureInput input;
    input.signal = pixels;
    input.xyz = xyz;
    input.width = 64;
    input.height = 64;
    input.areaWidth = 64;
    input.areaHeight = 64;
    input.scopes = render::kScopeTableAll;
    input.axisMaximum = 1;
    input.detail = detail;
    input.luma = luma;
    const auto counts = runtime::measureScopesCpu(input);
    if (!counts)
        throw std::logic_error("the fixture could not be measured");
    return runtime::ScopeTables::pack(*counts, runtime::ScopeRoute::Cpu);
}

// 75% color bars, 64 x 64, with one odd pixel so the traces have dense and single-count cells.
[[nodiscard]] runtime::ScopeTables colorBars(const runtime::ScopeLuma& luma,
                                             const std::uint32_t detail = 0) {
    constexpr std::array<std::array<float, 3>, 8> bars{{{0.75F, 0.75F, 0.75F},
                                                        {0.75F, 0.75F, 0.0F},
                                                        {0.0F, 0.75F, 0.75F},
                                                        {0.0F, 0.75F, 0.0F},
                                                        {0.75F, 0.0F, 0.75F},
                                                        {0.75F, 0.0F, 0.0F},
                                                        {0.0F, 0.0F, 0.75F},
                                                        {0.0F, 0.0F, 0.0F}}};
    std::vector<Rgba32f> pixels;
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x) {
            const auto& bar = bars[static_cast<std::size_t>(x / 8)];
            pixels.push_back(px(bar[0], bar[1], bar[2]));
        }
    pixels[0] = px(0.3F, 0.6F, 0.1F);
    return measure(pixels, luma, detail);
}

// Every pixel of a 64 x 64 frame a different gray in the lower half of the range: a flat Histogram
// with a full bar in each bin of its left half and a clear right half.
[[nodiscard]] runtime::ScopeTables grayRamp(const runtime::ScopeLuma& luma) {
    std::vector<Rgba32f> pixels;
    for (int index = 0; index < 64 * 64; ++index) {
        const float value = static_cast<float>(index) / 4095.0F * 0.5F;
        pixels.push_back(px(value, value, value));
    }
    return measure(pixels, luma);
}

void testPaletteIsTheTokens(Expectations& e) {
    const auto defaults = scopes::defaultScopePalette();
    const auto token = [](const kit::Color color) { return kit::color(color).rgb(); };
    e.expect(defaults.canvas == token(kit::Color::Canvas), "canvas is kit::Color::Canvas");
    e.expect(defaults.quiet == token(kit::Color::Border), "quiet is kit::Color::Border");
    e.expect(defaults.stronger == token(kit::Color::BorderHover),
             "stronger is kit::Color::BorderHover");
    e.expect(defaults.strongest == token(kit::Color::Foreground),
             "strongest is kit::Color::Foreground");
    e.expect(defaults.label == token(kit::Color::Muted), "label is kit::Color::Muted");
    e.expect(defaults.faint == token(kit::Color::Faint), "faint is kit::Color::Faint");
    e.expect(defaults.warning == token(kit::Color::Warn), "warning is kit::Color::Warn");
    e.expect(defaults.channel[0] == token(kit::Color::ComponentX), "red is kit::Color::ComponentX");
    e.expect(defaults.channel[1] == token(kit::Color::ComponentY),
             "green is kit::Color::ComponentY");
    e.expect(defaults.channel[2] == token(kit::Color::ComponentZ),
             "blue is kit::Color::ComponentZ");
    e.expect(defaults.channel[3] == token(kit::Color::Foreground),
             "luma is kit::Color::Foreground");
    e.expect(defaults == ui::scopePanelPalette(), "the panel draws with the default palette");

    const scopes::ScopeMetrics metrics;
    e.expect(metrics.padding == kit::px(kit::Spacing::S), "padding is kit::Spacing::S");
    e.expect(metrics.small == kit::px(kit::Spacing::XS), "small is kit::Spacing::XS");
    e.expect(metrics.tiny == kit::px(kit::Spacing::XXS), "tiny is kit::Spacing::XXS");
    e.expect(metrics.majorTick == kit::px(kit::Size::MajorTick),
             "majorTick is kit::Size::MajorTick");
    e.expect(metrics.minorTick == kit::px(kit::Size::MinorTick),
             "minorTick is kit::Size::MinorTick");
    const auto panel = ui::scopePanelMetrics();
    e.expect(panel.padding == metrics.padding && panel.small == metrics.small &&
                 panel.tiny == metrics.tiny && panel.majorTick == metrics.majorTick &&
                 panel.minorTick == metrics.minorTick,
             "the panel draws with the default metrics");

    // The plan's hairline is the token's snapped hairline at every ratio.
    for (const double dpr : {1.0, 1.25, 1.5, 2.0, 3.0}) {
        scopes::GraticuleRequest request;
        request.kind = scopes::Kind::Waveform;
        request.width = 300;
        request.height = 200;
        request.devicePixelRatio = dpr;
        request.measure = [](const std::string_view text, const scopes::LabelFont) {
            return scopes::TextSize{7.0 * static_cast<double>(text.size()), 14.0};
        };
        e.expect(scopes::buildScopeGraticule(request).hairlineWidth ==
                     kit::snappedHairlineWidth(dpr),
                 "the plan's hairline is the snapped hairline at ratio " + std::to_string(dpr));
    }
}

struct Ink final {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> marked;

    Ink(const int w, const int h)
        : width(w), height(h),
          marked(static_cast<std::size_t>(w) * static_cast<std::size_t>(h), 0) {}
    void mark(const int x, const int y) {
        if (x >= 0 && y >= 0 && x < width && y < height)
            marked[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                   static_cast<std::size_t>(x)] = 1;
    }
    void dot(const double x, const double y, const int reach) {
        for (int dy = -reach; dy <= reach; ++dy)
            for (int dx = -reach; dx <= reach; ++dx)
                mark(static_cast<int>(std::floor(x)) + dx, static_cast<int>(std::floor(y)) + dy);
    }
    void segment(const scopes::Point a, const scopes::Point b, const int reach) {
        const double length = std::hypot(b.x - a.x, b.y - a.y);
        const int steps = std::max(1, static_cast<int>(std::ceil(length * 2)));
        for (int step = 0; step <= steps; ++step) {
            const double t = static_cast<double>(step) / steps;
            dot(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, reach);
        }
    }
    [[nodiscard]] bool at(const int x, const int y) const {
        return marked[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                      static_cast<std::size_t>(x)] != 0;
    }
};

// Where the plan puts ink that two renderers draw their own way: antialiased strokes, fills and
// text. The trace is compared everywhere else.
[[nodiscard]] Ink inkOf(const scopes::ScopeGraticule& plan, const int width, const int height,
                        const scopes::MeasureText& measure) {
    Ink ink(width, height);
    for (const auto& line : plan.lines)
        ink.segment(line.from, line.to, 2);
    for (const auto& polyline : plan.polylines) {
        for (std::size_t index = 1; index < polyline.points.size(); ++index)
            ink.segment(polyline.points[index - 1], polyline.points[index], 2);
        if (polyline.closed && polyline.points.size() > 2)
            ink.segment(polyline.points.back(), polyline.points.front(), 2);
    }
    for (const auto& circle : plan.circles)
        for (int step = 0; step < 720; ++step) {
            const double angle = 2 * 3.14159265358979 * step / 720;
            ink.dot(circle.center.x + std::cos(angle) * circle.radius,
                    circle.center.y + std::sin(angle) * circle.radius, 2);
        }
    for (const auto& fill : plan.fills)
        for (double y = fill.rect.top(); y <= fill.rect.bottom(); y += 1.0)
            ink.segment({fill.rect.left(), y}, {fill.rect.right(), y}, 2);
    for (const auto& label : plan.labels) {
        const auto size = measure(label.text, label.font);
        const double left =
            label.anchor.x - size.width * (label.horizontal == scopes::HorizontalAlign::Left ? 0.0
                                           : label.horizontal == scopes::HorizontalAlign::Center
                                               ? 0.5
                                               : 1.0);
        const double top = label.anchor.y -
                           size.height * (label.vertical == scopes::VerticalAlign::Top ? 0.0 : 0.5);
        for (double y = top - 3; y <= top + size.height + 3; y += 1.0)
            ink.segment({left - 3, y}, {left + size.width + 3, y}, 0);
    }
    return ink;
}

struct Rig final {
    std::unique_ptr<runtime::TaskScheduler> scheduler;
    std::unique_ptr<ui::TaskUiBridge> bridge;
    std::unique_ptr<ui::ScopesTraceRenderer> renderer;
};

[[nodiscard]] Rig makeRig() {
    Rig rig;
    rig.scheduler = std::make_unique<runtime::TaskScheduler>(
        runtime::TaskSchedulerConfig{.cpuWorkerCount = 2,
                                     .blockingIoWorkerCount = 1,
                                     .cpuQueueCapacity = 32,
                                     .blockingIoQueueCapacity = 4,
                                     .terminalHistoryCapacity = 64,
                                     .diagnosticsPerTask = 8,
                                     .groupRegistryCapacity = 8});
    rig.bridge = std::make_unique<ui::TaskUiBridge>(*rig.scheduler, nullptr, 1ms);
    rig.renderer = std::make_unique<ui::ScopesTraceRenderer>(*rig.scheduler, *rig.bridge);
    return rig;
}

[[nodiscard]] bool near(const QRgb a, const QRgb b, const int tolerance) {
    return std::abs(qRed(a) - qRed(b)) <= tolerance &&
           std::abs(qGreen(a) - qGreen(b)) <= tolerance &&
           std::abs(qBlue(a) - qBlue(b)) <= tolerance;
}

void testViewAgreesWithPicture(Expectations& e,
                               const std::shared_ptr<const runtime::ScopeSignalColor>& color) {
    auto rig = makeRig();
    const auto palette = ui::scopePanelPalette();
    struct Case {
        scopes::Kind kind;
        const char* name;
        int width;
    };
    for (const auto& item :
         {Case{scopes::Kind::Waveform, "Waveform", 360}, Case{scopes::Kind::Parade, "Parade", 360},
          Case{scopes::Kind::Vectorscope, "Vectorscope", 340},
          Case{scopes::Kind::Histogram, "Histogram", 360},
          Case{scopes::Kind::Chromaticity, "Chromaticity", 340}}) {
        const std::string name = item.name;
        auto built = std::make_shared<ui::PreviewScopeSidecar>();
        built->tables = item.kind == scopes::Kind::Histogram ? grayRamp(color->luma())
                                                             : colorBars(color->luma());
        built->area = {0, 0, 64, 64};
        const ui::PreviewScopeSidecarHandle sidecar = built;
        const ui::PreviewScopeSidecarHandle& handle = sidecar;
        ui::ScopeView view(item.kind, rig.renderer.get());
        ui::ScopeViewData data;
        data.scopes = handle;
        data.color = color;
        view.setData(data);
        view.resize(item.width, view.heightForWidth(item.width));
        view.show();
        e.expect(waitUntil([&] {
                     return !view.traceImage().isNull() && !view.showsBehind() &&
                            view.devicePixelRatioF() == 1.0;
                 }),
                 name + ": the trace is mapped on a worker at device pixel ratio 1");
        if (view.traceImage().isNull())
            continue;
        const auto plan = view.graticule();
        const QImage grab =
            view.grab().toImage().convertToFormat(QImage::Format_ARGB32_Premultiplied);
        e.expect(grab.width() == view.width() && grab.height() == view.height(),
                 name + ": the grab has the view's size");

        // The same scope, drawn headless with the panel's palette and the panel's text measure.
        scopes::PictureInput input;
        input.tables = &sidecar->tables;
        input.luma = &color->luma();
        input.colorimetry = &color->colorimetry();
        input.signal = data.signal;
        input.axisMaximum = data.axisMaximum;
        scopes::PictureOptions options;
        options.palette = palette;
        options.metrics = ui::scopePanelMetrics();
        options.measure = ui::ScopeView::textMeasure();
        const auto picture = scopes::renderScopePicture(item.kind, view.width(), view.height(),
                                                        input, options, {}, std::size_t{1} << 26U);
        e.expect(picture.ok(), name + ": the picture renders");
        if (!picture.ok())
            continue;

        // The plan of the picture is the plan of the panel.
        scopes::GraticuleRequest request;
        request.kind = item.kind;
        request.width = view.width();
        request.height = view.height();
        request.luma = input.luma;
        request.colorimetry = input.colorimetry;
        request.metrics = options.metrics;
        request.measure = options.measure;
        const auto clip =
            scopes::clipSummary(sidecar->tables, item.kind, scopes::WaveformChannels::Rgb,
                                scopes::HistogramChannels::Rgb);
        request.clip = &clip;
        const auto headlessPlan = scopes::buildScopeGraticule(request);
        e.expect(headlessPlan.plot.x == plan.plot.x && headlessPlan.plot.y == plan.plot.y &&
                     headlessPlan.plot.width == plan.plot.width &&
                     headlessPlan.plot.height == plan.plot.height &&
                     headlessPlan.trace.x == plan.trace.x && headlessPlan.trace.y == plan.trace.y &&
                     headlessPlan.lines.size() == plan.lines.size() &&
                     headlessPlan.labels.size() == plan.labels.size(),
                 name + ": the panel and the picture build the same plan");

        // The trace rectangle's pixels agree away from lines and labels.
        const auto ink = inkOf(plan, view.width(), view.height(), options.measure);
        const int left = static_cast<int>(std::lround(plan.trace.x));
        const int top = static_cast<int>(std::lround(plan.trace.y));
        int compared = 0;
        int lit = 0;
        int different = 0;
        for (int y = 0; y < plan.traceImageSize.height; ++y)
            for (int x = 0; x < plan.traceImageSize.width; ++x) {
                const int px = left + x;
                const int py = top + y;
                if (px < 0 || py < 0 || px >= view.width() || py >= view.height() || ink.at(px, py))
                    continue;
                ++compared;
                const QRgb seen = grab.pixel(px, py);
                const auto headless = picture.image.pixel(px, py);
                lit += headless != palette.canvas ? 1 : 0;
                different += near(seen, headless, 1) ? 0 : 1;
            }
        e.expect(compared > 500 && lit > 0 && different == 0,
                 name + ": the grab and the picture agree on the trace rectangle (" +
                     std::to_string(different) + " of " + std::to_string(compared) +
                     " pixels differ, " + std::to_string(lit) + " drawn)");

        // Named hairlines: the plan's position holds a graticule pixel in the grab.
        const auto expectLine = [&](const scopes::Line& line, const std::string& what) {
            const bool horizontal = line.from.y == line.to.y;
            const auto expected = palette.color(line.color);
            bool found = false;
            // The first of nine samples along the line (off the grid steps) with no trace on it and
            // nothing beside it.
            for (int sample = 1; sample < 10 && !found; ++sample) {
                const double t = (sample + 0.4) / 10.0;
                const double x = line.from.x + (line.to.x - line.from.x) * t;
                const double y = line.from.y + (line.to.y - line.from.y) * t;
                const int px = static_cast<int>(std::floor(x));
                const int py = static_cast<int>(std::floor(y));
                const int tx = px - static_cast<int>(std::lround(plan.trace.x));
                const int ty = py - static_cast<int>(std::lround(plan.trace.y));
                const bool traced = tx >= 0 && ty >= 0 && tx < view.traceImage().width() &&
                                    ty < view.traceImage().height() &&
                                    qAlpha(view.traceImage().pixel(tx, ty)) != 0;
                const int sx = horizontal ? 0 : 3;
                const int sy = horizontal ? 3 : 0;
                if (traced || px - sx < 0 || py - sy < 0 || px + sx >= view.width() ||
                    py + sy >= view.height())
                    continue;
                if (grab.pixel(px - sx, py - sy) != palette.canvas ||
                    grab.pixel(px + sx, py + sy) != palette.canvas)
                    continue;
                found = true;
                e.expect(grab.pixel(px, py) == expected,
                         name + ": " + what + " has its graticule pixel where the plan puts it");
            }
            e.expect(found, name + ": " + what + " has a clear place to sample");
        };
        const auto nearest = [&](const bool horizontal, const double at,
                                 const scopes::ColorRole role) {
            const scopes::Line* best = nullptr;
            for (const auto& line : plan.lines) {
                if (line.color != role || line.layer != scopes::Layer::UnderTrace ||
                    line.antialiased || (line.from.y == line.to.y) != horizontal)
                    continue;
                const double position = horizontal ? line.from.y : line.from.x;
                const double length = std::hypot(line.to.x - line.from.x, line.to.y - line.from.y);
                if (length < 20)
                    continue;
                if (best == nullptr ||
                    std::abs(position - at) <
                        std::abs((horizontal ? best->from.y : best->from.x) - at))
                    best = &line;
            }
            return best;
        };
        switch (item.kind) {
        case scopes::Kind::Waveform:
        case scopes::Kind::Parade:
            if (const auto* line = nearest(true, plan.plot.bottom() - 0.5 * plan.plot.height,
                                           scopes::ColorRole::Quiet))
                expectLine(*line, "the code 512 line");
            else
                e.expect(false, name + ": a code 512 line exists");
            if (const auto* line = nearest(true, plan.plot.bottom() - 0.1 * plan.plot.height,
                                           scopes::ColorRole::Quiet))
                expectLine(*line, "the code 128 line");
            break;
        case scopes::Kind::Vectorscope:
            if (const auto* line = nearest(true, plan.plot.center().y, scopes::ColorRole::Quiet))
                expectLine(*line, "the horizontal cross hair");
            else
                e.expect(false, name + ": a cross hair exists");
            if (const auto* line = nearest(false, plan.plot.center().x, scopes::ColorRole::Quiet))
                expectLine(*line, "the vertical cross hair");
            break;
        case scopes::Kind::Histogram:
            if (const auto* line =
                    nearest(true, plan.plot.top() + plan.rowHeight, scopes::ColorRole::Quiet))
                expectLine(*line, "the first row frame");
            else
                e.expect(false, name + ": a row frame exists");
            if (const auto* line = nearest(false, plan.plot.left() + 0.5 * plan.plot.width,
                                           scopes::ColorRole::Quiet))
                expectLine(*line, "the code 512 line");
            break;
        case scopes::Kind::Chromaticity:
            if (const auto* line = nearest(false, plan.plot.left() + 0.5 * plan.plot.width,
                                           scopes::ColorRole::Quiet))
                expectLine(*line, "the x = 0.5 grid line");
            else
                e.expect(false, name + ": a grid line exists");
            if (const auto* line = nearest(true, plan.plot.bottom() - 0.3 * plan.plot.width,
                                           scopes::ColorRole::Quiet))
                expectLine(*line, "the y = 0.3 grid line");
            break;
        }
    }
}

// The count in the text of a readout (its last "Count N" or "Count R n . G n" part).
[[nodiscard]] QString countPart(const QString& readout) {
    const auto at = readout.indexOf(QStringLiteral("Count "));
    return at < 0 ? QString() : readout.mid(at + 6);
}

// Hover readouts at detail 1: the value under the pointer is the same, and the count is the count
// of the detail's cell, which is the full detail's cells of that block summed.
void testDetailReadouts(Expectations& e,
                        const std::shared_ptr<const runtime::ScopeSignalColor>& color) {
    auto rig = makeRig();
    const auto fullHandle = std::make_shared<ui::PreviewScopeSidecar>();
    fullHandle->tables = colorBars(color->luma(), 0);
    fullHandle->area = {0, 0, 64, 64};
    const auto coarseHandle = std::make_shared<ui::PreviewScopeSidecar>();
    coarseHandle->tables = colorBars(color->luma(), 1);
    coarseHandle->area = {0, 0, 64, 64};
    const auto& full = fullHandle->tables;
    const auto& coarse = coarseHandle->tables;
    e.expect(coarse.layout().detail == 1U && coarse.layout().cells() == 256U,
             "the detail-1 sidecar has 256-cell tables");

    const auto show = [&](const scopes::Kind kind,
                          const std::shared_ptr<ui::PreviewScopeSidecar>& sidecar) {
        auto view = std::make_unique<ui::ScopeView>(kind, rig.renderer.get());
        ui::ScopeViewData data;
        data.scopes = sidecar;
        data.color = color;
        view->setData(data);
        view->resize(340, view->heightForWidth(340));
        view->show();
        return view;
    };
    // Vectorscope: the center is Cb = Cr = 0, which the gray and black bars fill; its full cell is
    // 256, and the detail's cell 128 holds the 2 x 2 block 256..257.
    const auto fullScope = show(scopes::Kind::Vectorscope, fullHandle);
    const auto coarseScope = show(scopes::Kind::Vectorscope, coarseHandle);
    const auto centerFull = fullScope->readoutAt(fullScope->plotRect().center());
    const auto centerCoarse = coarseScope->readoutAt(coarseScope->plotRect().center());
    const std::uint32_t fullCenter = full.vectorscope(256, 256);
    const std::uint32_t block = full.vectorscope(256, 256) + full.vectorscope(257, 256) +
                                full.vectorscope(256, 257) + full.vectorscope(257, 257);
    e.expect(fullCenter != 0U && coarse.vectorscope(128, 128) == block,
             "the detail's center cell is the block of full cells");
    e.expect(countPart(centerFull) == QString::number(fullCenter),
             "the full readout reports the full cell's count: " + centerFull.toStdString());
    e.expect(countPart(centerCoarse) == QString::number(block),
             "the detail-1 readout reports the detail cell's count: " + centerCoarse.toStdString());
    e.expect(centerFull.left(centerFull.indexOf(QStringLiteral("Count "))) ==
                 centerCoarse.left(centerCoarse.indexOf(QStringLiteral("Count "))),
             "the value under the pointer is the same at both details");

    // Chromaticity: the readout's count is the detail's cell under the pointer; scan the plot for
    // a cell that holds a count in the full table and compare the pooled count.
    const auto fullChroma = show(scopes::Kind::Chromaticity, fullHandle);
    const auto coarseChroma = show(scopes::Kind::Chromaticity, coarseHandle);
    const auto plot = fullChroma->plotRect();
    int compared = 0;
    int differing = 0;
    for (int step = 0; step < 400; ++step) {
        const QPointF point(plot.left() + plot.width() * (step % 20 + 0.5) / 20.0,
                            plot.top() + plot.height() * (step / 20 + 0.5) / 20.0);
        const auto fullText = fullChroma->readoutAt(point);
        const auto coarseText = coarseChroma->readoutAt(point);
        if (fullText.isEmpty() || coarseText.isEmpty())
            continue;
        ++compared;
        // The detail's cell holds at least the full cell under the pointer.
        differing += countPart(coarseText).toUInt() >= countPart(fullText).toUInt() ? 0 : 1;
    }
    e.expect(compared > 100 && differing == 0,
             "a detail cell's count is at least the full cell's under the same pointer");

    // Waveform: the same column and code, and the detail's count holds the full count.
    const auto fullWave = show(scopes::Kind::Waveform, fullHandle);
    const auto coarseWave = show(scopes::Kind::Waveform, coarseHandle);
    const auto wavePlot = fullWave->plotRect();
    int waveCompared = 0;
    int waveDiffering = 0;
    for (int step = 0; step < 100; ++step) {
        const QPointF point(wavePlot.left() + wavePlot.width() * (step % 10 + 0.5) / 10.0,
                            wavePlot.top() + wavePlot.height() * (step / 10 + 0.5) / 10.0);
        const auto fullText = fullWave->readoutAt(point);
        const auto coarseText = coarseWave->readoutAt(point);
        if (fullText.isEmpty() || coarseText.isEmpty())
            continue;
        ++waveCompared;
        const auto prefix = [](const QString& text) {
            return text.left(text.indexOf(QStringLiteral("Count ")));
        };
        waveDiffering += prefix(fullText) == prefix(coarseText) ? 0 : 1;
        // "R n · G n · B n": every channel's detail count is at least the full one.
        const auto fullCounts = countPart(fullText).split(QStringLiteral(" · "));
        const auto coarseCounts = countPart(coarseText).split(QStringLiteral(" · "));
        if (fullCounts.size() != coarseCounts.size()) {
            ++waveDiffering;
            continue;
        }
        for (qsizetype index = 0; index < fullCounts.size(); ++index)
            waveDiffering += coarseCounts[index].split(' ').last().toUInt() >=
                                     fullCounts[index].split(' ').last().toUInt()
                                 ? 0
                                 : 1;
    }
    e.expect(waveCompared > 50 && waveDiffering == 0,
             "the waveform readout names the same column and code, with the detail's counts");

    // The Histogram does not depend on the detail.
    const auto fullHistogram = show(scopes::Kind::Histogram, fullHandle);
    const auto coarseHistogram = show(scopes::Kind::Histogram, coarseHandle);
    const auto histogramPlot = fullHistogram->plotRect();
    const QPointF bin(histogramPlot.left() + 0.75 * histogramPlot.width(),
                      histogramPlot.top() + 10);
    e.expect(!fullHistogram->readoutAt(bin).isEmpty() &&
                 fullHistogram->readoutAt(bin) == coarseHistogram->readoutAt(bin),
             "the Histogram readout does not depend on the detail");
}

} // namespace

int main(int argc, char** argv) {
    try {
        QApplication application(argc, argv);
        kit::installAfterlabTheme(application);
        Expectations e;
        testPaletteIsTheTokens(e);

        auto neutral = bloom::color::resolveOcioBuiltIn(
                           bloom::color::OcioConfigLocatorKind::BloomBuiltIn,
                           bloom::color::kBloomNeutralV1ConfigUri,
                           bloom::color::ocioBuiltInContentRevision(
                               bloom::color::OcioConfigLocatorKind::BloomBuiltIn,
                               bloom::color::kBloomNeutralV1ConfigUri)
                               .value_or(bloom::color::kBloomNeutralV1ConfigDigest),
                           "lin_rec709_scene")
                           .takeResolved();
        e.expect(neutral.has_value(), "Bloom Neutral resolves");
        if (neutral) {
            auto prepared = runtime::ScopeSignalColor::prepare(
                {.config = std::make_shared<const bloom::color::ResolvedBloomNeutralConfig>(
                     std::move(*neutral)),
                 .signal = runtime::ScopeSignal::Display,
                 .display = {},
                 .view = {}});
            e.expect(static_cast<bool>(prepared), "the signal's color objects prepare");
            if (prepared) {
                testViewAgreesWithPicture(e, prepared.color);
                testDetailReadouts(e, prepared.color);
            }
        }
        if (e.failures() != 0) {
            std::cerr << e.failures() << " scope picture expectation(s) failed\n";
            return 1;
        }
        std::cout << "scope picture panel tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "scope picture panel tests threw: " << error.what() << '\n';
        return 1;
    }
}
