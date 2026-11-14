// #724 Scopes: the Qt-free scope picture (runtime/scope_picture.hpp).
//
// Asserted, over hand-made tables of a few pixels and no widget or device:
//   * a picture has the size asked for; the trace pixels inside its trace rectangle equal the trace
//     mapping's output composed over the canvas (additively for the Waveform and the Parade);
//   * a single-count cell is drawn at or above the visible floor; Histogram heights, linear and
//     logarithmic, match values computed here; Parade panes are thirds with the gap; clip markers
//     appear only for under and over counts;
//   * the graticule plan holds the facts of the specification, computed independently here: the
//     Waveform's lines at codes 0, 128, ... 1023; the Vectorscope's ticks every 10 degrees (longer
//     every 30), its six 75% targets from the luma coefficients and the skin tone line at 123
//     degrees; the CIE grid every 0.1, labels every 0.2, the Rec. 709 triangle and white point and
//     the four printed lines;
//   * the picture of all five scopes has the panel's heights and every part has content;
//   * PNG bytes decode back to the same pixels; a cancelled call, a byte limit below the picture
//     and an invalid size fail with their error and never throw.

#include <bloom/render/image_types.hpp>
#include <bloom/render/scope_table_layout.hpp>
#include <bloom/runtime/scope_measure.hpp>
#include <bloom/runtime/scope_picture.hpp>
#include <bloom/runtime/task_scheduler.hpp>

#include <zlib.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <numbers>
#include <source_location>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace render = bloom::render;
namespace runtime = bloom::runtime;
namespace color = bloom::color;
namespace scopes = bloom::runtime::scopes;
using render::Rgba32f;

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

[[nodiscard]] Rgba32f px(const float r, const float g, const float b) {
    const auto result = Rgba32f::fromPremultiplied(r, g, b, 1.0F);
    if (!result)
        throw std::logic_error("invalid fixture pixel");
    return *result.value();
}

constexpr double kKr = 0.2126;
constexpr double kKg = 0.7152;
constexpr double kKb = 0.0722;

[[nodiscard]] runtime::ScopeLuma rec709() {
    return runtime::scopeLuma(static_cast<float>(kKr), static_cast<float>(kKg),
                              static_cast<float>(kKb));
}

[[nodiscard]] Rgba32f xyzOf(const Rgba32f& rgb) {
    const float r = rgb.red();
    const float g = rgb.green();
    const float b = rgb.blue();
    return px(0.4124F * r + 0.3576F * g + 0.1805F * b, 0.2126F * r + 0.7152F * g + 0.0722F * b,
              0.0193F * r + 0.1192F * g + 0.9505F * b);
}

[[nodiscard]] runtime::ScopeTables tablesOf(const std::vector<Rgba32f>& signal,
                                            const std::uint32_t width, const std::uint32_t height,
                                            const std::uint32_t scopeBits,
                                            const std::uint32_t detail = 0) {
    std::vector<Rgba32f> xyz;
    for (const auto& pixel : signal)
        xyz.push_back(xyzOf(pixel));
    runtime::ScopeMeasureInput input;
    input.signal = signal;
    input.xyz = xyz;
    input.width = width;
    input.height = height;
    input.areaWidth = width;
    input.areaHeight = height;
    input.scopes = scopeBits;
    input.axisMaximum = 1;
    input.detail = detail;
    input.luma = rec709();
    const auto counts = runtime::measureScopesCpu(input);
    if (!counts)
        throw std::logic_error("the fixture could not be measured");
    return runtime::ScopeTables::pack(*counts, runtime::ScopeRoute::Cpu);
}

constexpr std::uint32_t kAllScopes = render::kScopeTableAll;

// A 64 x 64 frame of mid gray with one odd pixel, so the odd pixel's cells hold a single count
// among thousands.
constexpr double kOddRed = 0.9;
constexpr double kOddGreen = 0.2;
constexpr double kOddBlue = 0.2;
[[nodiscard]] runtime::ScopeTables grayWithOddPixel() {
    std::vector<Rgba32f> pixels(64 * 64, px(0.5F, 0.5F, 0.5F));
    pixels[0] = px(static_cast<float>(kOddRed), static_cast<float>(kOddGreen),
                   static_cast<float>(kOddBlue));
    return tablesOf(pixels, 64, 64, kAllScopes);
}

// Color bars: 8 bars of 8 pixels at 75%, 64 rows.
[[nodiscard]] runtime::ScopeTables colorBars() {
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
    return tablesOf(pixels, 64, 64, kAllScopes);
}

[[nodiscard]] runtime::ScopeTables clippedFrame() {
    std::vector<Rgba32f> pixels(8 * 8, px(0.5F, 0.5F, 0.5F));
    pixels[3] = px(2.0F, 2.0F, 2.0F);
    pixels[5] = px(-0.5F, -0.5F, -0.5F);
    return tablesOf(pixels, 8, 8, kAllScopes);
}

// A fixed, deterministic text measure for the plan facts: 7 logical pixels per character, 14 high.
[[nodiscard]] scopes::MeasureText fixedMeasure() {
    return [](const std::string_view text, const scopes::LabelFont) {
        return scopes::TextSize{7.0 * static_cast<double>(text.size()), 14.0};
    };
}

[[nodiscard]] color::ScopeColorimetry rec709Colorimetry() {
    color::ScopeColorimetry value;
    value.luma = {static_cast<float>(kKr), static_cast<float>(kKg), static_cast<float>(kKb)};
    value.red = {0.64, 0.33};
    value.green = {0.30, 0.60};
    value.blue = {0.15, 0.06};
    value.white = {0.3127, 0.3290};
    value.colorSpaceName = "Rec.709";
    return value;
}

[[nodiscard]] scopes::GraticuleRequest planRequest(const scopes::Kind kind, const double width,
                                                   const double height, const double dpr,
                                                   const runtime::ScopeLuma* luma,
                                                   const color::ScopeColorimetry* colorimetry) {
    scopes::GraticuleRequest request;
    request.kind = kind;
    request.width = width;
    request.height = height;
    request.devicePixelRatio = dpr;
    request.luma = luma;
    request.colorimetry = colorimetry;
    request.measure = fixedMeasure();
    return request;
}

[[nodiscard]] double snapped(const double value, const double dpr) {
    return (std::floor(value * dpr) + 0.5) / dpr;
}

[[nodiscard]] bool near(const double a, const double b, const double tolerance = 1e-6) {
    return std::abs(a - b) <= tolerance;
}

[[nodiscard]] const scopes::Label* findLabel(const scopes::ScopeGraticule& plan,
                                             const std::string& text) {
    for (const auto& label : plan.labels)
        if (label.text == text)
            return &label;
    return nullptr;
}

// A token of a scheduler task that has been cancelled while it ran.
runtime::CancellationToken makeCancelledToken() {
    using namespace runtime;
    TaskSchedulerConfig config = TaskSchedulerConfig::defaults();
    config.cpuWorkerCount = 1;
    config.blockingIoWorkerCount = 1;
    TaskScheduler scheduler(config);
    std::mutex mutex;
    std::condition_variable condition;
    bool entered = false;
    bool release = false;
    CancellationToken token;
    auto submission = scheduler.submit<void>(
        TaskRequest("scope picture cancel token",
                    {.kind = TaskOwnerKind::Composition, .id = TaskOwnerId::fromRaw(724)}),
        [&](TaskContext& context) {
            {
                std::lock_guard lock(mutex);
                token = context.cancellation();
                entered = true;
            }
            condition.notify_all();
            std::unique_lock lock(mutex);
            condition.wait(lock, [&] { return release; });
            return TaskResult<void>::succeeded();
        });
    if (!submission.accepted())
        throw std::logic_error("the cancel token task was refused");
    {
        std::unique_lock lock(mutex);
        condition.wait(lock, [&] { return entered; });
    }
    submission.handle.cancel();
    {
        std::lock_guard lock(mutex);
        release = true;
    }
    condition.notify_all();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!submission.handle.tryTakeResult().has_value() &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    scheduler.beginShutdown();
    while (!scheduler.isQuiescent() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    return token;
}

// ---------------------------------------------------------------------------------------------

void testWaveformPlan(Expectations& e) {
    for (const double dpr : {1.0, 1.5}) {
        const auto plan = scopes::buildScopeGraticule(
            planRequest(scopes::Kind::Waveform, 300, 180, dpr, nullptr, nullptr));
        // left = advance of "1023" + 2 * 4; top = ceil(14 / 2) + 2; bottom = tick 8 + small 4.
        e.expect(near(plan.plot.x, 28 + 8) && near(plan.plot.y, 9) &&
                     near(plan.plot.width, 300 - 36 - 8) && near(plan.plot.height, 180 - 9 - 12),
                 "the waveform's plot follows the label gutter and the tick margins");
        std::vector<double> lines;
        for (const auto& line : plan.lines)
            if (line.color == scopes::ColorRole::Quiet && line.from.y == line.to.y)
                lines.push_back(line.from.y);
        std::vector<double> expected;
        for (const int code : {0, 128, 256, 384, 512, 640, 768, 896, 1023})
            expected.push_back(
                snapped(plan.plot.bottom() - (code + 0.5) / 1024.0 * plan.plot.height, dpr));
        e.expect(lines == expected, "the waveform has a line at codes 0, 128, ... 1023");
        std::vector<std::string> labels;
        for (const auto& label : plan.labels)
            labels.push_back(label.text);
        e.expect(labels == std::vector<std::string>(
                               {"0", "128", "256", "384", "512", "640", "768", "896", "1023"}),
                 "the waveform labels are the codes");
        e.expect(plan.panes.size() == 1 && plan.panes[0].width == plan.plot.width,
                 "the waveform has one pane");
        // Minor ticks every 32 codes between the labelled lines: 32 steps minus 8 labelled.
        int minor = 0;
        for (const auto& line : plan.lines)
            if (line.color == scopes::ColorRole::Stronger && line.from.x != line.to.x)
                ++minor;
        e.expect(minor == 24,
                 "the waveform has minor ticks every 32 codes between the labelled lines");
    }
    // The Working signal labels are linear values of the axis maximum.
    auto request = planRequest(scopes::Kind::Waveform, 300, 180, 1.0, nullptr, nullptr);
    request.signal = runtime::ScopeSignal::Working;
    request.axisMaximum = 4;
    const auto plan = scopes::buildScopeGraticule(request);
    e.expect(findLabel(plan, "0") != nullptr && findLabel(plan, "4") != nullptr &&
                 findLabel(plan, "2.5") != nullptr && findLabel(plan, "0.5") != nullptr &&
                 findLabel(plan, "1023") == nullptr,
             "the working signal labels run 0 to the axis maximum");
}

void testParadePlan(Expectations& e) {
    const auto plan = scopes::buildScopeGraticule(
        planRequest(scopes::Kind::Parade, 300, 180, 1.0, nullptr, nullptr));
    e.expect(plan.panes.size() == 3, "the parade has three panes");
    if (plan.panes.size() != 3)
        return;
    const double pane = (plan.plot.width - 2 * scopes::kParadeGap) / 3;
    bool thirds = true;
    for (std::size_t index = 0; index < 3; ++index)
        thirds = thirds && near(plan.panes[index].width, pane) &&
                 near(plan.panes[index].x,
                      plan.plot.left() + static_cast<double>(index) * (pane + scopes::kParadeGap));
    e.expect(thirds && near(plan.panes[1].left() - plan.panes[0].right(), scopes::kParadeGap) &&
                 near(plan.panes[2].right(), plan.plot.right()),
             "the parade panes are thirds of the plot with the gap between them");
    int names = 0;
    for (const auto& label : plan.labels)
        names += label.font == scopes::LabelFont::Name ? 1 : 0;
    e.expect(names == 3, "the parade names its three panes");
    // The mapping gives the same split in pixels.
    std::vector<Rgba32f> red(4, px(1.0F, 0.0F, 0.0F));
    const auto tables = tablesOf(red, 4, 1, render::kScopeTableWaveform);
    scopes::TraceColors colors;
    colors.channel = {0xFFFF0000U, 0xFF00FF00U, 0xFF0000FFU, 0xFF808080U};
    const auto image = scopes::mapParade(tables, colors, 1.0F, {100, 64}, 4);
    bool gaps = !image.empty();
    bool panes = !image.empty();
    for (int y = 0; y < image.height && !image.empty(); ++y) {
        for (const int x : {30, 31, 32, 33, 64, 65, 66, 67, 98, 99})
            gaps = gaps && image.pixel(x, y) == 0U;
    }
    // Panes of 30 pixels at 0..29, 34..63 (30 wide: 34..63) and 68..97.
    panes = panes && image.pixel(0, 0) == 0xFFFF0000U && image.pixel(29, 0) == 0xFFFF0000U &&
            image.pixel(34, 63) == 0xFF00FF00U && image.pixel(63, 63) == 0xFF00FF00U &&
            image.pixel(68, 63) == 0xFF0000FFU && image.pixel(97, 63) == 0xFF0000FFU;
    e.expect(gaps && panes, "the parade image has three panes of 30 pixels and gaps of 4");
}

void testVectorscopePlan(Expectations& e) {
    const auto luma = rec709();
    const auto plan = scopes::buildScopeGraticule(
        planRequest(scopes::Kind::Vectorscope, 300, 320, 1.0, &luma, nullptr));
    const double side = 300 - 16;
    e.expect(near(plan.plot.width, side) && near(plan.plot.height, side) && near(plan.plot.x, 8) &&
                 near(plan.plot.y, (320 - side) / 2),
             "the vectorscope plot is a square inset by the padding");
    const auto center = plan.plot.center();
    const double radius = side / 2;
    e.expect(plan.circles.size() == 1 && near(plan.circles[0].radius, radius) &&
                 near(plan.circles[0].center.x, center.x) &&
                 near(plan.circles[0].center.y, center.y),
             "the vectorscope circle has radius 0.5 of the plot");
    // Ticks: every 10 degrees, longer every 30.
    int ticks = 0;
    int major = 0;
    int matched = 0;
    for (const auto& line : plan.lines) {
        if (!line.antialiased || line.dashed || line.color != scopes::ColorRole::Label ||
            line.layer != scopes::Layer::UnderTrace)
            continue;
        ++ticks;
        const double length = std::hypot(line.to.x - line.from.x, line.to.y - line.from.y);
        major += near(length, 8.0, 1e-9) ? 1 : 0;
        for (int degrees = 0; degrees < 360; degrees += 10) {
            const double angle = degrees * std::numbers::pi / 180.0;
            const double expectedLength = degrees % 30 == 0 ? 8.0 : 4.0;
            if (near(line.from.x, center.x + std::cos(angle) * radius, 1e-6) &&
                near(line.from.y, center.y - std::sin(angle) * radius, 1e-6) &&
                near(line.to.x, center.x + std::cos(angle) * (radius - expectedLength), 1e-6) &&
                near(line.to.y, center.y - std::sin(angle) * (radius - expectedLength), 1e-6)) {
                ++matched;
                break;
            }
        }
    }
    e.expect(ticks == 36 && major == 12 && matched == 36,
             "the vectorscope has a tick every 10 degrees and a longer one every 30");
    // The skin tone line.
    int skin = 0;
    for (const auto& line : plan.lines) {
        if (!line.dashed)
            continue;
        const double angle = 123.0 * std::numbers::pi / 180.0;
        skin += near(line.from.x, center.x) && near(line.from.y, center.y) &&
                        near(line.to.x, center.x + std::cos(angle) * radius, 1e-6) &&
                        near(line.to.y, center.y - std::sin(angle) * radius, 1e-6) &&
                        line.color == scopes::ColorRole::Faint
                    ? 1
                    : 0;
    }
    e.expect(skin == 1, "the skin tone line runs from the center at 123 degrees");
    // Targets at the 75% bars, with the same coefficients.
    struct Bar {
        const char* name;
        double r, g, b;
    };
    const std::array<Bar, 6> bars{{{"R", 0.75, 0, 0},
                                   {"Mg", 0.75, 0, 0.75},
                                   {"B", 0, 0, 0.75},
                                   {"Cy", 0, 0.75, 0.75},
                                   {"G", 0, 0.75, 0},
                                   {"Yl", 0.75, 0.75, 0}}};
    const double half = std::max(5.0, side * 0.025);
    for (const auto& bar : bars) {
        const double y = kKr * bar.r + kKg * bar.g + kKb * bar.b;
        const double cb = (bar.b - y) / (2 * (1 - kKb));
        const double cr = (bar.r - y) / (2 * (1 - kKr));
        const auto* label = findLabel(plan, bar.name);
        e.expect(label != nullptr &&
                     near(label->anchor.x, center.x + cb * side + half + 2.0, 1e-3) &&
                     near(label->anchor.y, center.y - cr * side - half - 4.0, 1e-3),
                 std::string("the vectorscope target ") + bar.name + " sits at its 75% bar");
    }
    int brackets = 0;
    for (const auto& line : plan.lines)
        brackets += line.layer == scopes::Layer::OverTrace ? 1 : 0;
    e.expect(brackets == 48, "each target is four corner brackets of two arms");
    // The targets are positions of the luma coefficients: other coefficients move them.
    const auto other = runtime::scopeLuma(0.299F, 0.587F, 0.114F);
    const auto moved = scopes::buildScopeGraticule(
        planRequest(scopes::Kind::Vectorscope, 300, 320, 1.0, &other, nullptr));
    const auto* before = findLabel(plan, "G");
    const auto* after = findLabel(moved, "G");
    e.expect(before != nullptr && after != nullptr && !near(before->anchor.x, after->anchor.x, 0.5),
             "the targets follow the luma coefficients");
    // No luma, no targets.
    const auto bare = scopes::buildScopeGraticule(
        planRequest(scopes::Kind::Vectorscope, 300, 320, 1.0, nullptr, nullptr));
    e.expect(bare.labels.empty(), "without coefficients the targets are not drawn");
}

void testChromaticityPlan(Expectations& e) {
    const auto colorimetry = rec709Colorimetry();
    for (const double dpr : {1.0, 2.0}) {
        const auto plan = scopes::buildScopeGraticule(
            planRequest(scopes::Kind::Chromaticity, 300, 300, dpr, nullptr, &colorimetry));
        const double side = plan.plot.width;
        e.expect(near(plan.plot.width, plan.plot.height) && near(plan.image.width, side * 0.8) &&
                     near(plan.image.height, side * 0.9) &&
                     near(plan.image.bottom(), plan.plot.bottom()),
                 "the chromaticity image covers x 0 to 0.8 and y 0 to 0.9 of the axes");
        // Grid every 0.1.
        std::vector<double> vertical;
        std::vector<double> horizontal;
        for (const auto& line : plan.lines) {
            if (line.layer != scopes::Layer::UnderTrace)
                continue;
            if (line.from.x == line.to.x)
                vertical.push_back(line.from.x);
            else if (line.from.y == line.to.y)
                horizontal.push_back(line.from.y);
        }
        std::vector<double> expectedX;
        std::vector<double> expectedY;
        for (int step = 0; step <= 10; ++step) {
            expectedX.push_back(snapped(plan.plot.left() + side * step / 10.0, dpr));
            expectedY.push_back(snapped(plan.plot.bottom() - side * step / 10.0, dpr));
        }
        e.expect(vertical == expectedX && horizontal == expectedY,
                 "the chromaticity grid has a line every 0.1");
        // Labels every 0.2 along the bottom and up the left.
        for (const std::string text : {"0.2", "0.4", "0.6", "0.8", "1.0"}) {
            int found = 0;
            for (const auto& label : plan.labels)
                found += label.text == text ? 1 : 0;
            e.expect(found == 2, "the label " + text + " is on both axes");
        }
        e.expect(findLabel(plan, "0.1") == nullptr && findLabel(plan, "0.3") == nullptr,
                 "only every 0.2 is labelled");
        // The axis names x and y sit between the labels of 0.8 and 1.0, never on top of one.
        const auto* xName = findLabel(plan, "x");
        const auto* yName = findLabel(plan, "y");
        e.expect(xName != nullptr && yName != nullptr &&
                     near(xName->anchor.x, plan.plot.left() + side * 0.9) &&
                     near(yName->anchor.y, plan.plot.bottom() - side * 0.9),
                 "the axis names are between the labels of 0.8 and 1.0");
        // The gamut triangle, the white point and the printed lines.
        const auto map = [&](const double x, const double y) {
            return scopes::Point{plan.plot.left() + x * side, plan.plot.bottom() - y * side};
        };
        const scopes::Polyline* triangle = nullptr;
        const scopes::Polyline* locus = nullptr;
        for (const auto& polyline : plan.polylines) {
            if (polyline.closed)
                triangle = &polyline;
            else
                locus = &polyline;
        }
        const auto corners = std::array{map(0.64, 0.33), map(0.30, 0.60), map(0.15, 0.06)};
        bool corner = triangle != nullptr && triangle->points.size() == 3;
        for (std::size_t index = 0; corner && index < 3; ++index)
            corner = near(triangle->points[index].x, corners[index].x) &&
                     near(triangle->points[index].y, corners[index].y);
        e.expect(corner, "the Rec. 709 triangle corners are at their xy");
        const auto white = map(0.3127, 0.3290);
        e.expect(plan.circles.size() == 1 && near(plan.circles[0].center.x, white.x) &&
                     near(plan.circles[0].center.y, white.y),
                 "the white point ring is at its xy");
        e.expect(locus != nullptr && locus->points.size() > 40,
                 "the spectral locus is a curve of many points");
        for (const std::string text :
             {"R 0.6400 0.3300", "G 0.3000 0.6000", "B 0.1500 0.0600", "W 0.3127 0.3290"}) {
            e.expect(findLabel(plan, text) != nullptr, "the printed line \"" + text + "\"");
        }
        e.expect(findLabel(plan, "Rec.709") != nullptr, "the space's name is printed");
    }
}

void testHistogramPlan(Expectations& e) {
    auto request = planRequest(scopes::Kind::Histogram, 300, 400, 1.0, nullptr, nullptr);
    auto plan = scopes::buildScopeGraticule(request);
    e.expect(plan.rows == 3 && near(plan.rowHeight, 56) && near(plan.plot.height, 168),
             "the histogram has three rows of 56");
    int rowLines = 0;
    int codeLines = 0;
    for (const auto& line : plan.lines) {
        if (line.color != scopes::ColorRole::Quiet)
            continue;
        rowLines += line.from.y == line.to.y ? 1 : 0;
        codeLines += line.from.x == line.to.x ? 1 : 0;
    }
    e.expect(rowLines == 4 && codeLines == 9, "row frames and a line at every labelled code");
    e.expect(plan.fills.empty(), "no clip counts, no markers");
    request.histogramChannels = scopes::HistogramChannels::RgbAndLuma;
    e.expect(scopes::buildScopeGraticule(request).rows == 4, "luma adds a row");
    // Clip markers at either end of the rows with counts.
    scopes::ClipSummary clip;
    clip.under[0] = 2;
    clip.over[2] = 1;
    request.histogramChannels = scopes::HistogramChannels::Rgb;
    request.clip = &clip;
    plan = scopes::buildScopeGraticule(request);
    e.expect(plan.fills.size() == 2 && near(plan.fills[0].rect.x, plan.plot.left()) &&
                 near(plan.fills[0].rect.y, plan.plot.top()) &&
                 near(plan.fills[0].rect.width, scopes::kClipMarkerWidth) &&
                 near(plan.fills[1].rect.right(), plan.plot.right()) &&
                 near(plan.fills[1].rect.y, plan.plot.top() + 2 * 56),
             "clip markers sit at the end of the row with under or over counts");
}

// ---------------------------------------------------------------------------------------------

void testBrightnessFloor(Expectations& e) {
    const auto tables = grayWithOddPixel();
    scopes::TraceColors colors;
    colors.channel = {0xFFFF0000U, 0xFF00FF00U, 0xFF0000FFU, 0xFFFFFFFFU};
    const auto luma = rec709();
    const std::uint32_t floorAlpha =
        static_cast<std::uint32_t>(std::lround(scopes::kTraceFloor * 255.0F));
    const auto minimumAlpha = [](const scopes::ScopeImage& image) {
        unsigned least = 256;
        int lit = 0;
        for (const auto pixel : image.pixels)
            if (scopes::alphaOf(pixel) != 0U) {
                least = std::min(least, scopes::alphaOf(pixel));
                ++lit;
            }
        return std::pair{least, lit};
    };
    const auto check = [&](const scopes::ScopeImage& image, const char* name) {
        const auto [least, lit] = minimumAlpha(image);
        e.expect(lit >= 2 && least >= floorAlpha,
                 std::string(name) + " draws a single-count cell at or above the floor");
    };
    check(scopes::mapWaveform(tables, scopes::WaveformChannels::Luma, colors, 1.0F, {64, 512}),
          "the waveform");
    check(scopes::mapVectorscope(tables, luma, 1.0F, {512, 512}), "the vectorscope");
    check(scopes::mapChromaticity(tables, 1.0F, scopes::chromaticityImageSize(512)),
          "the chromaticity");
    // The vectorscope's odd cell, by its coordinates.
    const double y = kKr * kOddRed + kKg * kOddGreen + kKb * kOddBlue;
    const int cbCell =
        static_cast<int>(std::floor(((kOddBlue - y) / (2 * (1 - kKb)) + 0.5) * 512.0));
    const int crCell =
        static_cast<int>(std::floor(((kOddRed - y) / (2 * (1 - kKr)) + 0.5) * 512.0));
    const auto scope = scopes::mapVectorscope(tables, luma, 1.0F, {512, 512});
    e.expect(!scope.empty() && scopes::alphaOf(scope.pixel(cbCell, 511 - crCell)) >= floorAlpha,
             "the odd pixel's vectorscope cell is lit");
    // The histogram never hides a bin with a count: one pixel tall at least.
    const auto histogram = scopes::mapHistogram(tables, scopes::HistogramChannels::Rgb,
                                                scopes::HistogramScale::Linear, colors, 56, 1024);
    const int oddCode = static_cast<int>(std::floor(kOddRed * 1023.0 + 0.5));
    e.expect(!histogram.empty() && scopes::alphaOf(histogram.pixel(oddCode, 55)) != 0U,
             "a histogram bin with one pixel among thousands is one pixel tall at least");
}

void testHistogramHeights(Expectations& e) {
    // 64 gray pixels: 48 at code 256, 15 at code 512 and 1 at code 767.
    std::vector<Rgba32f> pixels(48, px(0.25F, 0.25F, 0.25F));
    pixels.insert(pixels.end(), 15, px(0.5F, 0.5F, 0.5F));
    pixels.push_back(px(0.75F, 0.75F, 0.75F));
    const auto tables = tablesOf(pixels, 8, 8, render::kScopeTableHistogram);
    scopes::TraceColors colors;
    colors.channel = {0xFFFF0000U, 0xFF00FF00U, 0xFF0000FFU, 0xFF808080U};
    const auto height = [](const scopes::ScopeImage& image, const int column) {
        int count = 0;
        for (int y = 0; y < image.height; ++y)
            count += scopes::alphaOf(image.pixel(column, y)) != 0U ? 1 : 0;
        return count;
    };
    const auto linear = scopes::mapHistogram(tables, scopes::HistogramChannels::Luma,
                                             scopes::HistogramScale::Linear, colors, 56, 1024);
    const auto logarithmic =
        scopes::mapHistogram(tables, scopes::HistogramChannels::Luma,
                             scopes::HistogramScale::Logarithmic, colors, 56, 1024);
    e.expect(linear.width == 1024 && linear.height == 56, "one row of 56 pixels for luma");
    const std::array<int, 3> codes{256, 512, 767};
    const std::array<double, 3> counts{48, 15, 1};
    for (std::size_t index = 0; index < 3; ++index) {
        const int expectedLinear =
            std::clamp(static_cast<int>(std::ceil(counts[index] / 48.0 * 56.0)), 1, 56);
        const int expectedLog = std::clamp(
            static_cast<int>(std::ceil(std::log2(1.0 + counts[index]) / std::log2(49.0) * 56.0)), 1,
            56);
        e.expect(height(linear, codes[index]) == expectedLinear,
                 "linear height of code " + std::to_string(codes[index]));
        e.expect(height(logarithmic, codes[index]) == expectedLog,
                 "logarithmic height of code " + std::to_string(codes[index]));
    }
    e.expect(height(linear, 100) == 0 && height(logarithmic, 100) == 0,
             "a bin with no count has no height");
}

void testClipMarkers(Expectations& e) {
    const auto clipped = clippedFrame();
    const auto clip =
        scopes::clipSummary(clipped, scopes::Kind::Waveform, scopes::WaveformChannels::Rgb,
                            scopes::HistogramChannels::Rgb);
    scopes::PictureInput input;
    input.tables = &clipped;
    const auto luma = rec709();
    input.luma = &luma;
    const auto colorimetry = rec709Colorimetry();
    input.colorimetry = &colorimetry;
    const scopes::PictureOptions options;
    const auto picture =
        scopes::renderScopePicture(scopes::Kind::Waveform, 320, 200, input, options, {}, 1U << 24U);
    e.expect(picture.ok() && picture.image.width == 320 && picture.image.height == 200,
             "the picture has the size asked for");
    // Column 3 is over (top edge), column 5 under (bottom edge); the plan says where.
    auto request = planRequest(scopes::Kind::Waveform, 320, 200, 1.0, &luma, &colorimetry);
    request.measure = scopes::embeddedTextMeasure(1.0);
    request.clip = &clip;
    const auto plan = scopes::buildScopeGraticule(request);
    e.expect(plan.fills.size() == 2, "one over marker and one under marker");
    const auto warning = options.palette.warning;
    bool both = picture.ok() && plan.fills.size() == 2;
    for (const auto& fill : plan.fills) {
        const int x = static_cast<int>(std::floor(fill.rect.x + 0.5));
        const int y = static_cast<int>(std::floor(fill.rect.y + 0.5));
        both = both && picture.image.pixel(x, y + 1) == warning;
    }
    e.expect(both, "the markers are drawn in the warning color");
    // A frame with no under or over counts has none.
    const auto clean = grayWithOddPixel();
    input.tables = &clean;
    const auto quiet =
        scopes::renderScopePicture(scopes::Kind::Waveform, 320, 200, input, options, {}, 1U << 24U);
    int warnings = 0;
    for (const auto pixel : quiet.image.pixels)
        warnings += pixel == warning ? 1 : 0;
    e.expect(quiet.ok() && warnings == 0, "no marker without an under or over count");
    const auto cleanClip =
        scopes::clipSummary(clean, scopes::Kind::Waveform, scopes::WaveformChannels::Rgb,
                            scopes::HistogramChannels::Rgb);
    request.clip = &cleanClip;
    e.expect(scopes::buildScopeGraticule(request).fills.empty(), "the plan has no marker either");
}

// The trace pixels inside the trace rectangle equal the mapping composed over the canvas. The
// graticule roles are set to no alpha so only the trace is left; label boxes are skipped.
void testTraceComposition(Expectations& e) {
    const auto tables = colorBars();
    const auto luma = rec709();
    const auto colorimetry = rec709Colorimetry();
    scopes::PictureInput input;
    input.tables = &tables;
    input.luma = &luma;
    input.colorimetry = &colorimetry;
    scopes::PictureOptions options;
    auto& palette = options.palette;
    for (auto* role : {&palette.quiet, &palette.stronger, &palette.strongest, &palette.label,
                       &palette.faint, &palette.warning})
        *role = 0U;
    struct Case {
        scopes::Kind kind;
        int width;
        int height;
    };
    const auto measure = scopes::embeddedTextMeasure(1.0);
    for (const auto& item :
         {Case{scopes::Kind::Waveform, 320, 200}, Case{scopes::Kind::Parade, 320, 200},
          Case{scopes::Kind::Vectorscope, 300, 300}, Case{scopes::Kind::Histogram, 300, 240},
          Case{scopes::Kind::Chromaticity, 300, 300}}) {
        const auto picture = scopes::renderScopePicture(item.kind, item.width, item.height, input,
                                                        options, {}, 1U << 24U);
        e.expect(picture.ok() && picture.image.width == item.width &&
                     picture.image.height == item.height,
                 "the picture has the size asked for");
        if (!picture.ok())
            continue;
        // The same plan and mapping the picture used.
        scopes::GraticuleRequest request;
        request.kind = item.kind;
        request.width = item.width;
        request.height = item.height;
        request.luma = &luma;
        request.colorimetry = &colorimetry;
        request.measure = measure;
        const auto plan = scopes::buildScopeGraticule(request);
        scopes::TraceRequest mapping;
        mapping.kind = item.kind;
        mapping.tables = &tables;
        mapping.luma = &luma;
        mapping.colors = palette.traceColors();
        mapping.size = plan.traceRequestSize;
        mapping.gapPixels = scopes::kParadeGap;
        const auto trace = scopes::renderTrace(mapping);
        e.expect(!trace.image.empty() && trace.image.width == plan.traceImageSize.width &&
                     trace.image.height == plan.traceImageSize.height,
                 "the mapping returns the size the plan says");
        const int left = static_cast<int>(std::lround(plan.trace.x));
        const int top = static_cast<int>(std::lround(plan.trace.y));
        const bool additive =
            item.kind == scopes::Kind::Waveform || item.kind == scopes::Kind::Parade;
        // Pixels near a label are not compared.
        const auto nearLabel = [&](const int x, const int y) {
            for (const auto& label : plan.labels) {
                const auto size = measure(label.text, label.font);
                const double l =
                    label.anchor.x -
                    size.width * (label.horizontal == scopes::HorizontalAlign::Left     ? 0.0
                                  : label.horizontal == scopes::HorizontalAlign::Center ? 0.5
                                                                                        : 1.0);
                const double t =
                    label.anchor.y -
                    size.height * (label.vertical == scopes::VerticalAlign::Top ? 0.0 : 0.5);
                if (x >= l - 3 && x <= l + size.width + 3 && y >= t - 3 && y <= t + size.height + 3)
                    return true;
            }
            return false;
        };
        int compared = 0;
        int lit = 0;
        int mismatched = 0;
        for (int y = 0; y < trace.image.height; ++y)
            for (int x = 0; x < trace.image.width; ++x) {
                const int px = left + x;
                const int py = top + y;
                if (px < 0 || py < 0 || px >= item.width || py >= item.height || nearLabel(px, py))
                    continue;
                const auto source = trace.image.pixel(x, y);
                const auto canvas = palette.canvas;
                std::uint32_t expected = 0;
                const auto channel = [&](const unsigned d, const unsigned s, const unsigned a) {
                    return additive ? std::min(255U, d + s) : s + (d * (255U - a) + 127U) / 255U;
                };
                expected = scopes::packArgb(
                    channel(scopes::alphaOf(canvas), scopes::alphaOf(source),
                            scopes::alphaOf(source)),
                    channel(scopes::redOf(canvas), scopes::redOf(source), scopes::alphaOf(source)),
                    channel(scopes::greenOf(canvas), scopes::greenOf(source),
                            scopes::alphaOf(source)),
                    channel(scopes::blueOf(canvas), scopes::blueOf(source),
                            scopes::alphaOf(source)));
                ++compared;
                lit += source != 0U ? 1 : 0;
                mismatched += picture.image.pixel(px, py) == expected ? 0 : 1;
            }
        e.expect(compared > 0 && lit > 0 && mismatched == 0,
                 "the trace pixels of the picture are the mapping over the canvas (kind " +
                     std::to_string(static_cast<int>(item.kind)) + ", " +
                     std::to_string(mismatched) + " differ)");
    }
}

// A picture drawn from detail-1 (and detail-2) tables: the same size and graticule, and a trace
// that covers the same region.
//
// The rule asserted for the trace: with `size` the larger side of the trace image in pixels, a
// detail-d cell is 2^d full cells wide, so it covers at most ceil(2^d * size / 512) output pixels
// (the unit of the footprint below, plus one for rounding of the spans). Every drawn trace pixel of
// the detail-d image has a drawn pixel of the full-detail image within that footprint on each
// axis, and every drawn pixel of the full-detail image has a drawn pixel of the detail-d image
// within it. The Histogram does not depend on the detail: its image is equal.
[[nodiscard]] std::vector<Rgba32f> gradientPixels() {
    std::vector<Rgba32f> pixels;
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x)
            pixels.push_back(px(static_cast<float>(x) / 63.0F, static_cast<float>(y) / 63.0F,
                                static_cast<float>((x * 5 + y * 3) % 64) / 63.0F));
    return pixels;
}

[[nodiscard]] bool drawnNear(const scopes::ScopeImage& image, const int x, const int y,
                             const int footprint) {
    for (int dy = -footprint; dy <= footprint; ++dy)
        for (int dx = -footprint; dx <= footprint; ++dx) {
            const int px = x + dx;
            const int py = y + dy;
            if (px >= 0 && py >= 0 && px < image.width && py < image.height &&
                image.pixel(px, py) != 0U)
                return true;
        }
    return false;
}

void testDetailPicture(Expectations& e) {
    const auto pixels = gradientPixels();
    const auto full = tablesOf(pixels, 64, 64, kAllScopes, 0);
    const auto luma = rec709();
    const auto colorimetry = rec709Colorimetry();
    scopes::PictureOptions options;
    struct Case {
        scopes::Kind kind;
        int width;
        int height;
    };
    const auto measure = scopes::embeddedTextMeasure(1.0);
    for (const std::uint32_t detail : {1U, 2U}) {
        const auto coarse = tablesOf(pixels, 64, 64, kAllScopes, detail);
        e.expect(coarse.layout().detail == detail && coarse.layout().cells() == (512U >> detail),
                 "the detail's tables have the detail's layout");
        scopes::PictureInput fullInput;
        fullInput.tables = &full;
        fullInput.luma = &luma;
        fullInput.colorimetry = &colorimetry;
        auto coarseInput = fullInput;
        coarseInput.tables = &coarse;
        for (const auto& item :
             {Case{scopes::Kind::Waveform, 320, 200}, Case{scopes::Kind::Parade, 320, 200},
              Case{scopes::Kind::Vectorscope, 300, 300}, Case{scopes::Kind::Histogram, 300, 240},
              Case{scopes::Kind::Chromaticity, 300, 300}}) {
            const std::string name = "detail " + std::to_string(detail) + ", kind " +
                                     std::to_string(static_cast<int>(item.kind));
            const auto fullPicture = scopes::renderScopePicture(item.kind, item.width, item.height,
                                                                fullInput, options, {}, 1U << 24U);
            const auto coarsePicture = scopes::renderScopePicture(
                item.kind, item.width, item.height, coarseInput, options, {}, 1U << 24U);
            e.expect(fullPicture.ok() && coarsePicture.ok() &&
                         coarsePicture.image.width == fullPicture.image.width &&
                         coarsePicture.image.height == fullPicture.image.height,
                     name + ": the picture has the same size");
            if (!fullPicture.ok() || !coarsePicture.ok())
                continue;
            // The graticule: the same plan, and the same pixels wherever neither trace draws.
            scopes::GraticuleRequest request;
            request.kind = item.kind;
            request.width = item.width;
            request.height = item.height;
            request.luma = &luma;
            request.colorimetry = &colorimetry;
            request.measure = measure;
            const auto plan = scopes::buildScopeGraticule(request);
            scopes::TraceRequest mapping;
            mapping.kind = item.kind;
            mapping.luma = &luma;
            mapping.colors = options.palette.traceColors();
            mapping.size = plan.traceRequestSize;
            mapping.gapPixels = scopes::kParadeGap;
            mapping.tables = &full;
            const auto fullTrace = scopes::renderTrace(mapping);
            mapping.tables = &coarse;
            const auto coarseTrace = scopes::renderTrace(mapping);
            e.expect(!fullTrace.image.empty() && !coarseTrace.image.empty() &&
                         fullTrace.image.width == coarseTrace.image.width &&
                         fullTrace.image.height == coarseTrace.image.height,
                     name + ": the traces have the same size");
            if (fullTrace.image.empty() || coarseTrace.image.empty())
                continue;
            const int left = static_cast<int>(std::lround(plan.trace.x));
            const int top = static_cast<int>(std::lround(plan.trace.y));
            int graticuleDifferences = 0;
            for (int y = 0; y < item.height; ++y)
                for (int x = 0; x < item.width; ++x) {
                    const int tx = x - left;
                    const int ty = y - top;
                    const bool inTrace = tx >= 0 && ty >= 0 && tx < fullTrace.image.width &&
                                         ty < fullTrace.image.height;
                    if (inTrace && (fullTrace.image.pixel(tx, ty) != 0U ||
                                    coarseTrace.image.pixel(tx, ty) != 0U))
                        continue;
                    graticuleDifferences +=
                        fullPicture.image.pixel(x, y) == coarsePicture.image.pixel(x, y) ? 0 : 1;
                }
            e.expect(graticuleDifferences == 0,
                     name + ": the graticule and everything outside the trace is the same (" +
                         std::to_string(graticuleDifferences) + " differ)");
            if (item.kind == scopes::Kind::Histogram) {
                e.expect(fullTrace.image.pixels == coarseTrace.image.pixels,
                         name + ": the Histogram's trace does not depend on the detail");
                continue;
            }
            // The trace covers the same region.
            const int larger = std::max(fullTrace.image.width, fullTrace.image.height);
            const int footprint = ((1 << detail) * larger + 511) / 512 + 1;
            int drawnCoarse = 0;
            int drawnFull = 0;
            int uncovered = 0;
            int missed = 0;
            for (int y = 0; y < fullTrace.image.height; ++y)
                for (int x = 0; x < fullTrace.image.width; ++x) {
                    if (coarseTrace.image.pixel(x, y) != 0U) {
                        ++drawnCoarse;
                        uncovered += drawnNear(fullTrace.image, x, y, footprint) ? 0 : 1;
                    }
                    if (fullTrace.image.pixel(x, y) != 0U) {
                        ++drawnFull;
                        missed += drawnNear(coarseTrace.image, x, y, footprint) ? 0 : 1;
                    }
                }
            e.expect(drawnCoarse > 0 && drawnFull > 0, name + ": both traces draw something");
            e.expect(uncovered == 0 && missed == 0,
                     name + ": the traces cover the same region within " +
                         std::to_string(footprint) + " pixels (" + std::to_string(uncovered) +
                         " drawn pixels of the detail's trace have none of the full trace near, " +
                         std::to_string(missed) + " of the full trace have none of the detail's)");
        }
    }
}

void testAllScopesPicture(Expectations& e) {
    const auto tables = colorBars();
    const auto luma = rec709();
    const auto colorimetry = rec709Colorimetry();
    scopes::PictureInput input;
    input.tables = &tables;
    input.luma = &luma;
    input.colorimetry = &colorimetry;
    const scopes::PictureOptions options;
    const auto all = scopes::renderAllScopesPicture(400, input, options, {}, 1U << 26U);
    e.expect(all.ok() && all.image.width == 400, "the picture of all five scopes has the width");
    if (!all.ok())
        return;
    // Heights of the panel: Waveform and Parade width / 2 within 170..380, Vectorscope and
    // Chromaticity the width within bounds, the Histogram its rows and margins.
    const auto measure = scopes::embeddedTextMeasure(1.0);
    const double labelHeight = measure("0", scopes::LabelFont::Numeric).height;
    const int histogram = static_cast<int>(std::ceil(std::ceil(labelHeight) + 8 + 3 * 56)) + 8;
    const std::array<int, 5> expected{200, 200, 400, histogram, 400};
    int total = 0;
    bool heights = true;
    for (std::size_t index = 0; index < 5; ++index) {
        heights = heights && all.height[index] == expected[index] && all.top[index] == total;
        total += expected[index];
    }
    e.expect(heights && all.image.height == total, "each scope has the height the panel gives");
    const auto size = scopes::allScopesPictureSize(400, input, options);
    e.expect(size.width == 400 && size.height == total, "the size is known without drawing");
    for (std::size_t index = 0; index < 5; ++index) {
        int drawn = 0;
        for (int y = all.top[index]; y < all.top[index] + all.height[index]; ++y)
            for (int x = 0; x < all.image.width; ++x)
                drawn += all.image.pixel(x, y) != options.palette.canvas ? 1 : 0;
        e.expect(drawn > 0, "the part of scope " + std::to_string(index) + " is not empty");
    }
    // One scope at the same size as its part: the same pixels.
    const auto vectorscope = scopes::renderScopePicture(
        scopes::Kind::Vectorscope, 400, all.height[2], input, options, {}, 1U << 26U);
    bool same = vectorscope.ok();
    for (int y = 0; same && y < all.height[2]; ++y)
        for (int x = 0; same && x < 400; ++x)
            same = vectorscope.image.pixel(x, y) == all.image.pixel(x, all.top[2] + y);
    e.expect(same, "a scope alone is its part of the whole picture");
}

// The PNG decoded here with zlib alone: signature, chunks with their checksums, IHDR, one IDAT
// stream of filter-0 rows.
struct DecodedPng {
    bool valid = false;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    int bitDepth = 0;
    int colorType = 0;
    bool srgb = false;
    int idatChunks = 0;
    std::vector<std::uint8_t> rgba;
};

[[nodiscard]] std::uint32_t be32(const std::vector<std::uint8_t>& bytes, const std::size_t at) {
    return (static_cast<std::uint32_t>(bytes[at]) << 24U) |
           (static_cast<std::uint32_t>(bytes[at + 1]) << 16U) |
           (static_cast<std::uint32_t>(bytes[at + 2]) << 8U) | bytes[at + 3];
}

[[nodiscard]] DecodedPng decodePng(const std::vector<std::uint8_t>& bytes) {
    DecodedPng result;
    constexpr std::array<std::uint8_t, 8> signature{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (bytes.size() < 8 || !std::equal(signature.begin(), signature.end(), bytes.begin()))
        return result;
    std::size_t at = 8;
    std::vector<std::uint8_t> idat;
    bool ended = false;
    std::vector<std::string> order;
    while (at + 12 <= bytes.size()) {
        const auto length = be32(bytes, at);
        if (at + 12 + length > bytes.size())
            return result;
        const std::string type(bytes.begin() + static_cast<std::ptrdiff_t>(at) + 4,
                               bytes.begin() + static_cast<std::ptrdiff_t>(at) + 8);
        const auto crc = crc32(0L, bytes.data() + at + 4, 4 + length);
        if (be32(bytes, at + 8 + length) != static_cast<std::uint32_t>(crc))
            return result;
        order.push_back(type);
        const auto* data = bytes.data() + at + 8;
        if (type == "IHDR" && length == 13) {
            result.width = be32(bytes, at + 8);
            result.height = be32(bytes, at + 12);
            result.bitDepth = data[8];
            result.colorType = data[9];
        } else if (type == "sRGB") {
            result.srgb = true;
        } else if (type == "IDAT") {
            idat.insert(idat.end(), data, data + length);
            ++result.idatChunks;
        } else if (type == "IEND") {
            ended = true;
        }
        at += 12 + length;
    }
    if (!ended || at != bytes.size() || order.front() != "IHDR" || order.back() != "IEND")
        return result;
    const std::size_t rowBytes = 1U + static_cast<std::size_t>(result.width) * 4U;
    std::vector<std::uint8_t> raw(rowBytes * result.height);
    uLongf size = static_cast<uLongf>(raw.size());
    if (uncompress(raw.data(), &size, idat.data(), static_cast<uLong>(idat.size())) != Z_OK ||
        size != raw.size())
        return result;
    for (std::uint32_t y = 0; y < result.height; ++y) {
        if (raw[y * rowBytes] != 0)
            return result;
        result.rgba.insert(result.rgba.end(),
                           raw.begin() + static_cast<std::ptrdiff_t>(y * rowBytes + 1),
                           raw.begin() + static_cast<std::ptrdiff_t>((y + 1) * rowBytes));
    }
    result.valid = true;
    return result;
}

void testPng(Expectations& e) {
    const auto tables = colorBars();
    const auto luma = rec709();
    const auto colorimetry = rec709Colorimetry();
    scopes::PictureInput input;
    input.tables = &tables;
    input.luma = &luma;
    input.colorimetry = &colorimetry;
    const auto picture =
        scopes::renderScopePicture(scopes::Kind::Vectorscope, 200, 200, input, {}, {}, 1U << 24U);
    e.expect(picture.ok(), "a picture to encode");
    if (!picture.ok())
        return;
    const auto png = scopes::encodeScopePng(picture.image, 1U << 24U);
    e.expect(png.ok(), "the picture encodes");
    const auto decoded = decodePng(png.bytes);
    e.expect(decoded.valid && decoded.width == 200 && decoded.height == 200 &&
                 decoded.bitDepth == 8 && decoded.colorType == 6 && decoded.srgb &&
                 decoded.idatChunks >= 1,
             "the PNG is 8-bit RGBA with an sRGB chunk and the picture's size");
    bool same = decoded.valid;
    for (int y = 0; same && y < 200; ++y)
        for (int x = 0; same && x < 200; ++x) {
            const auto pixel = picture.image.pixel(x, y);
            const auto* at =
                &decoded
                     .rgba[(static_cast<std::size_t>(y) * 200 + static_cast<std::size_t>(x)) * 4];
            same = at[0] == scopes::redOf(pixel) && at[1] == scopes::greenOf(pixel) &&
                   at[2] == scopes::blueOf(pixel) && at[3] == scopes::alphaOf(pixel);
        }
    e.expect(same, "the PNG decodes to the picture's pixels");
    // Alpha is straight in the file.
    scopes::ScopeImage translucent;
    translucent.width = 2;
    translucent.height = 1;
    translucent.pixels = {scopes::packArgb(128, 64, 0, 0), scopes::packArgb(0, 0, 0, 0)};
    const auto small = decodePng(scopes::encodeScopePng(translucent, 1U << 20U).bytes);
    e.expect(small.valid && small.rgba == std::vector<std::uint8_t>({128, 0, 0, 128, 0, 0, 0, 0}),
             "premultiplied pixels are stored with straight alpha");
    // Refusals.
    e.expect(scopes::encodeScopePng(picture.image, 10).error == scopes::PngError::ByteLimitExceeded,
             "a PNG limit below the encoded size fails");
    e.expect(scopes::encodeScopePng({}, 1U << 20U).error == scopes::PngError::InvalidImage,
             "an empty image is not encoded");
}

void testRefusals(Expectations& e) {
    scopes::PictureInput input;
    const scopes::PictureOptions options;
    e.expect(
        scopes::renderScopePicture(scopes::Kind::Waveform, 0, 10, input, options, {}, 1U << 20U)
                    .error == scopes::PictureError::InvalidSize &&
            scopes::renderScopePicture(scopes::Kind::Waveform, 10, -1, input, options, {},
                                       1U << 20U)
                    .error == scopes::PictureError::InvalidSize &&
            scopes::renderAllScopesPicture(0, input, options, {}, 1U << 20U).error ==
                scopes::PictureError::InvalidSize,
        "a size that is not positive is refused");
    scopes::PictureOptions bad;
    bad.devicePixelRatio = 0.0;
    e.expect(scopes::renderScopePicture(scopes::Kind::Waveform, 100, 100, input, bad, {}, 1U << 20U)
                     .error == scopes::PictureError::InvalidSize,
             "a device pixel ratio that is not positive is refused");
    // A byte limit below the picture fails; the exact size passes.
    e.expect(scopes::renderScopePicture(scopes::Kind::Waveform, 100, 100, input, options, {},
                                        100 * 100 * 4 - 1)
                     .error == scopes::PictureError::ByteLimitExceeded,
             "a byte limit below the picture fails");
    e.expect(scopes::renderScopePicture(scopes::Kind::Waveform, 100, 100, input, options, {},
                                        100 * 100 * 4)
                 .ok(),
             "a byte limit equal to the picture passes");
    e.expect(scopes::renderAllScopesPicture(400, input, options, {}, 1000).error ==
                 scopes::PictureError::ByteLimitExceeded,
             "a byte limit below the picture of all five fails");
    // Cancellation.
    const auto token = makeCancelledToken();
    e.expect(token.isCancellationRequested(), "the fixture token is cancelled");
    e.expect(scopes::renderScopePicture(scopes::Kind::Waveform, 100, 100, input, options, token,
                                        1U << 20U)
                         .error == scopes::PictureError::Cancelled &&
                 scopes::renderAllScopesPicture(400, input, options, token, 1U << 26U).error ==
                     scopes::PictureError::Cancelled,
             "a cancelled call returns the cancelled error");
    // A scope without tables is its canvas and graticule.
    const auto bare = scopes::renderScopePicture(scopes::Kind::Histogram, 300, 240, input, options,
                                                 {}, 1U << 22U);
    e.expect(bare.ok(), "a scope without tables still draws");
    // The default palette is opaque.
    const auto palette = scopes::defaultScopePalette();
    bool opaque = scopes::alphaOf(palette.canvas) == 255U;
    for (const auto color : palette.channel)
        opaque = opaque && scopes::alphaOf(color) == 255U;
    e.expect(opaque, "the default palette is opaque");
}

} // namespace

int main() {
    try {
        Expectations e;
        testWaveformPlan(e);
        testParadePlan(e);
        testVectorscopePlan(e);
        testChromaticityPlan(e);
        testHistogramPlan(e);
        testBrightnessFloor(e);
        testHistogramHeights(e);
        testClipMarkers(e);
        testTraceComposition(e);
        testDetailPicture(e);
        testAllScopesPicture(e);
        testPng(e);
        testRefusals(e);
        if (e.failures() != 0) {
            std::cerr << e.failures() << " scope picture expectation(s) failed\n";
            return 1;
        }
        std::cout << "scope picture tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "scope picture tests threw: " << error.what() << '\n';
        return 1;
    }
}
