// #724, #730 Scopes: the Qt-free scope picture (runtime/scope_picture.hpp).
//
// Asserted, over hand-made tables of a few pixels and no widget or device:
//   * a picture has the size asked for; the trace pixels inside its trace rectangle equal the trace
//     mapping's output composed over the canvas (additively for the Waveform, the Parade and the
//     Vectorscope);
//   * the drawing of the reference grading application (docs/architecture/scopes.md): a Waveform
//     column is as bright as its own pixels make it, whatever else the picture holds, so columns
//     of one and two pixels draw alike (no stripes at any measured width); a 10-bit ramp is one
//     continuous trace and a flat Histogram, and an 8-bit ramp draws no comb; values outside the
//     axis are drawn on the 0 and 1023 lines and in the end bins; a single pixel among thousands
//     is visible; Histogram heights, linear and logarithmic, match values computed here; the CIE
//     trace marks presence in full color; Parade panes are thirds with the gap;
//   * the graticule plan holds the facts of the specification, computed independently here: the
//     Waveform's lines at codes 0, 128, ... 1023 and its bottom ticks; the Vectorscope's ring at
//     0.60 of the +-0.625 plot with 72 ticks, cross hairs, the six 75% targets from the luma
//     coefficients and the skin tone line at 123 degrees; 75% and 100% bars inside the ring; the
//     CIE grid every 0.1, labels every 0.2, the gamut triangle, white point, the spectral and
//     Planckian loci, the gamut's name and the printed lines;
//   * the picture of all five scopes has the panel's heights and every part has content;
//   * PNG bytes decode back to the same pixels; a cancelled call, a byte limit below the picture
//     and an invalid size fail with their error and never throw.

#include "await.hpp"
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
                                            const render::ScopeTableReduction reduction = {}) {
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
    input.luma = rec709();
    input.reduction = reduction;
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
    (void)bloom::test_support::awaitCondition(
        "the cancelled task ends", [&] { return submission.handle.tryTakeResult().has_value(); });
    scheduler.beginShutdown();
    (void)bloom::test_support::awaitCondition("the scheduler drains after shutdown",
                                              [&] { return scheduler.isQuiescent(); });
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
                 "the waveform labels are the codes, and nothing else is written");
        e.expect(plan.panes.size() == 1 && plan.panes[0].width == plan.plot.width,
                 "the waveform has one pane");
        // Minor ticks every 32 codes between the labelled lines, down the left edge; 48 steps
        // along the bottom, every fourth longer.
        int left = 0;
        int bottom = 0;
        int longer = 0;
        for (const auto& line : plan.lines) {
            if (line.color != scopes::ColorRole::Stronger)
                continue;
            if (line.from.y == line.to.y) {
                ++left;
            } else if (line.from.x == line.to.x && near(line.from.y, plan.plot.bottom())) {
                ++bottom;
                longer += near(line.to.y - line.from.y, 8.0) ? 1 : 0;
            }
        }
        e.expect(left == 24, "the waveform has minor ticks every 32 codes between the lines");
        e.expect(bottom == 49 && longer == 13,
                 "the waveform has 48 steps of ticks along the bottom");
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
    int bottom = 0;
    for (const auto& line : plan.lines)
        bottom += line.color == scopes::ColorRole::Stronger && line.from.x == line.to.x ? 1 : 0;
    e.expect(bottom == 3 * 17, "each parade pane has 16 steps of ticks along the bottom");
    // The mapping gives the same split in pixels: red at the top of the first pane, green and
    // blue at the bottom of the others.
    std::vector<Rgba32f> red(4, px(1.0F, 0.0F, 0.0F));
    const auto tables = tablesOf(red, 4, 1, render::kScopeTableWaveform);
    const auto image = scopes::mapParade(tables, {100, 64}, 4);
    bool gaps = !image.empty();
    for (int y = 0; y < image.height && !image.empty(); ++y) {
        for (const int x : {30, 31, 32, 33, 64, 65, 66, 67, 98, 99})
            gaps = gaps && image.pixel(x, y) == 0U;
    }
    // Panes of 30 pixels at 0..29, 34..63 and 68..97.
    const bool panes = !image.empty() && image.pixel(0, 0) != 0U && image.pixel(29, 0) != 0U &&
                       image.pixel(0, 63) == 0U && image.pixel(34, 63) != 0U &&
                       image.pixel(63, 63) != 0U && image.pixel(68, 63) != 0U &&
                       image.pixel(97, 63) != 0U && image.pixel(34, 0) == 0U;
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
    // The plot spans -0.625..0.625 of Cb and Cr; the ring is at 0.60.
    const double scale = side / 1.25;
    const double radius = 0.60 * scale;
    e.expect(plan.circles.size() == 1 && near(plan.circles[0].radius, radius) &&
                 near(plan.circles[0].center.x, center.x) &&
                 near(plan.circles[0].center.y, center.y),
             "the vectorscope ring is at 0.60 of Cb and Cr, inside the 0.625 plot");
    // 72 equal ticks inward from the ring, every 5 degrees, 5% of its radius.
    int ticks = 0;
    int matched = 0;
    for (const auto& line : plan.lines) {
        if (!line.antialiased || line.color != scopes::ColorRole::Stronger ||
            line.layer != scopes::Layer::UnderTrace)
            continue;
        ++ticks;
        for (int degrees = 0; degrees < 360; degrees += 5) {
            const double angle = degrees * std::numbers::pi / 180.0;
            if (near(line.from.x, center.x + std::cos(angle) * radius, 1e-6) &&
                near(line.from.y, center.y - std::sin(angle) * radius, 1e-6) &&
                near(line.to.x, center.x + std::cos(angle) * radius * 0.95, 1e-6) &&
                near(line.to.y, center.y - std::sin(angle) * radius * 0.95, 1e-6)) {
                ++matched;
                break;
            }
        }
    }
    e.expect(ticks == 72 && matched == 72, "the vectorscope ring has 72 equal ticks");
    // Full cross hairs.
    int hairs = 0;
    for (const auto& line : plan.lines)
        hairs +=
            line.color == scopes::ColorRole::Quiet &&
                    ((near(line.from.x, plan.plot.left()) && near(line.to.x, plan.plot.right())) ||
                     (near(line.from.y, plan.plot.top()) && near(line.to.y, plan.plot.bottom())))
                ? 1
                : 0;
    e.expect(hairs == 2, "the vectorscope has full cross hairs");
    // The skin tone line: solid and faint, from the center to the ring at 123 degrees.
    int skin = 0;
    for (const auto& line : plan.lines) {
        const double angle = 123.0 * std::numbers::pi / 180.0;
        skin += !line.dashed && line.color == scopes::ColorRole::Faint &&
                        near(line.from.x, center.x) && near(line.from.y, center.y) &&
                        near(line.to.x, center.x + std::cos(angle) * radius, 1e-6) &&
                        near(line.to.y, center.y - std::sin(angle) * radius, 1e-6)
                    ? 1
                    : 0;
    }
    e.expect(skin == 1, "the skin tone line runs solid from the center at 123 degrees");
    // Targets at the 75% bars, with the same coefficients, the letter at the lower right.
    struct Bar {
        const char* name;
        double r, g, b;
    };
    const std::array<Bar, 6> bars{{{"R", 0.75, 0, 0},
                                   {"M", 0.75, 0, 0.75},
                                   {"B", 0, 0, 0.75},
                                   {"C", 0, 0.75, 0.75},
                                   {"G", 0, 0.75, 0},
                                   {"Y", 0.75, 0.75, 0}}};
    const double half = std::max(5.0, side * 0.024);
    for (const auto& bar : bars) {
        const double y = kKr * bar.r + kKg * bar.g + kKb * bar.b;
        const double cb = (bar.b - y) / (2 * (1 - kKb));
        const double cr = (bar.r - y) / (2 * (1 - kKr));
        const auto* label = findLabel(plan, bar.name);
        e.expect(label != nullptr &&
                     near(label->anchor.x, center.x + cb * scale + half + 2.0, 1.0) &&
                     near(label->anchor.y, center.y - cr * scale + half, 1.0),
                 std::string("the vectorscope target ") + bar.name + " sits at its 75% bar");
        // The 75% and the 100% bars are both inside the ring.
        const double length = std::hypot(cb, cr);
        e.expect(length < 0.60 && length / 0.75 < 0.60,
                 std::string("the 75% and 100% ") + bar.name + " bars are inside the ring");
    }
    int brackets = 0;
    for (const auto& line : plan.lines)
        brackets += line.layer == scopes::Layer::OverTrace ? 1 : 0;
    e.expect(brackets == 48, "each target is four corner brackets of two arms");
    e.expect(plan.labels.size() == 6, "the six target letters are the only text");
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
                     near(plan.image.bottom(), plan.plot.bottom()) &&
                     near(plan.image.left(), plan.plot.left()),
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
        // The axis names X and Y sit between the labels of 0.8 and 1.0, never on top of one.
        const auto* xName = findLabel(plan, "X");
        const auto* yName = findLabel(plan, "Y");
        e.expect(xName != nullptr && yName != nullptr &&
                     near(xName->anchor.x, plan.plot.left() + side * 0.9) &&
                     near(yName->anchor.y, plan.plot.bottom() - side * 0.9),
                 "the axis names are between the labels of 0.8 and 1.0");
        // The gamut triangle, the white point, the two loci and the printed lines.
        const auto map = [&](const double x, const double y) {
            return scopes::Point{plan.plot.left() + x * side, plan.plot.bottom() - y * side};
        };
        const scopes::Polyline* triangle = nullptr;
        const scopes::Polyline* spectral = nullptr;
        const scopes::Polyline* planckian = nullptr;
        for (const auto& polyline : plan.polylines) {
            if (polyline.closed)
                triangle = &polyline;
            else if (polyline.color == scopes::ColorRole::Strongest)
                spectral = &polyline;
            else if (polyline.color == scopes::ColorRole::Faint)
                planckian = &polyline;
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
        e.expect(spectral != nullptr && spectral->points.size() > 40,
                 "the spectral locus is a curve of many points");
        // The Planckian locus passes D65 within 0.01 of xy.
        bool nearD65 = false;
        if (planckian != nullptr)
            for (const auto& point : planckian->points)
                nearD65 =
                    nearD65 || std::hypot(point.x - white.x, point.y - white.y) <= 0.01 * side;
        e.expect(planckian != nullptr && planckian->points.size() > 20 && nearD65,
                 "the Planckian locus is drawn and passes near D65");
        for (const std::string text : {"Rec.709 (D65)", "R: 0.6400, 0.3300", "G: 0.3000, 0.6000",
                                       "B: 0.1500, 0.0600", "W: 0.3127, 0.3290"})
            e.expect(findLabel(plan, text) != nullptr, "the printed line \"" + text + "\"");
        e.expect(findLabel(plan, "Rec.709") != nullptr, "the gamut is named at its green corner");
    }
    // The gamut and white names come from the primaries, not the config's name.
    auto p3 = rec709Colorimetry();
    p3.colorSpaceName = "Some display";
    p3.red = {0.680, 0.320};
    p3.green = {0.265, 0.690};
    p3.blue = {0.150, 0.060};
    const auto p3Name = scopes::chromaticityGamutName(p3);
    e.expect(p3Name.gamut == "P3" && p3Name.white == "D65", "P3 primaries with D65 are named");
    auto ap1 = rec709Colorimetry();
    ap1.red = {0.713, 0.293};
    ap1.green = {0.165, 0.830};
    ap1.blue = {0.128, 0.044};
    ap1.white = {0.32168, 0.33767};
    const auto ap1Name = scopes::chromaticityGamutName(ap1);
    e.expect(ap1Name.gamut == "ACES AP1" && ap1Name.white == "D60", "ACES AP1 with D60 is named");
    auto unknown = rec709Colorimetry();
    unknown.colorSpaceName = "custom";
    unknown.red = {0.6, 0.35};
    unknown.white = {0.30, 0.30};
    const auto unknownName = scopes::chromaticityGamutName(unknown);
    e.expect(unknownName.gamut == "custom" && unknownName.white.empty(),
             "other primaries keep the space's own name");
}

void testHistogramPlan(Expectations& e) {
    auto request = planRequest(scopes::Kind::Histogram, 300, 400, 1.0, nullptr, nullptr);
    const auto plan = scopes::buildScopeGraticule(request);
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
    int names = 0;
    for (const auto& label : plan.labels)
        names += label.font == scopes::LabelFont::Name ? 1 : 0;
    e.expect(names == 3 && findLabel(plan, "R") != nullptr && findLabel(plan, "B") != nullptr,
             "each row is named by its channel's letter");
    request.histogramChannels = scopes::HistogramChannels::RgbAndLuma;
    e.expect(scopes::buildScopeGraticule(request).rows == 4, "luma adds a row");
}

// ---------------------------------------------------------------------------------------------

// The lit rows of one column of an image, top to bottom.
[[nodiscard]] std::vector<int> litRows(const scopes::ScopeImage& image, const int x) {
    std::vector<int> rows;
    for (int y = 0; y < image.height; ++y)
        if (image.pixel(x, y) != 0U)
            rows.push_back(y);
    return rows;
}

[[nodiscard]] std::vector<std::uint32_t> column(const scopes::ScopeImage& image, const int x) {
    std::vector<std::uint32_t> pixels;
    for (int y = 0; y < image.height; ++y)
        pixels.push_back(image.pixel(x, y));
    return pixels;
}

void testWaveformNoStripes(Expectations& e) {
    // A flat gray at measured widths where the 512 Waveform columns hold one or two pixels (640),
    // one to two (1000) and three to four (1920): every screen column draws the same.
    for (const std::uint32_t width : {640U, 1000U, 1920U}) {
        const std::vector<Rgba32f> pixels(width, px(0.4F, 0.5F, 0.6F));
        const auto tables = tablesOf(pixels, width, 1, render::kScopeTableWaveform);
        e.expect(tables.layout().columns == 512U, "a wide picture has 512 Waveform columns");
        for (const int screen : {512, 300, 700}) {
            for (const auto channels :
                 {scopes::WaveformChannels::Rgb, scopes::WaveformChannels::Luma}) {
                const auto image = scopes::mapWaveform(tables, channels, {screen, 128});
                bool same = !image.empty();
                const auto first = image.empty() ? std::vector<std::uint32_t>{} : column(image, 0);
                bool lit = false;
                for (const auto pixel : first)
                    lit = lit || pixel != 0U;
                for (int x = 1; same && x < image.width; ++x)
                    same = column(image, x) == first;
                e.expect(same && lit, "no column stripes: measured " + std::to_string(width) +
                                          " wide, drawn " + std::to_string(screen) + " wide");
            }
        }
    }
}

void testWaveformRamp(Expectations& e) {
    // A 10-bit ramp, one pixel per code: the Waveform is one continuous rising line and the
    // Histogram is flat.
    std::vector<Rgba32f> pixels;
    for (int code = 0; code < 1024; ++code) {
        const float v = static_cast<float>(code) / 1023.0F;
        pixels.push_back(px(v, v, v));
    }
    const auto tables = tablesOf(pixels, 1024, 1, kAllScopes);
    const auto waveform = scopes::mapWaveform(tables, scopes::WaveformChannels::Rgb, {512, 256});
    bool continuous = !waveform.empty();
    int previousTop = waveform.height;
    for (int x = 0; continuous && x < waveform.width; ++x) {
        const auto rows = litRows(waveform, x);
        continuous = !rows.empty() && rows.back() - rows.front() + 1 ==
                                          static_cast<int>(rows.size()); // no hole in a column
        if (!continuous)
            break;
        // Rising, and touching or overlapping the previous column: no gap between neighbors.
        continuous = rows.front() <= previousTop && (x == 0 || rows.back() >= previousTop - 1);
        previousTop = rows.front();
    }
    e.expect(continuous, "a 10-bit ramp draws one continuous rising Waveform trace");
    const auto histogram = scopes::mapHistogram(tables, scopes::HistogramChannels::Rgb,
                                                scopes::HistogramScale::Linear, 56, 1024);
    bool flat = !histogram.empty();
    const auto height = histogram.empty() ? 0U : litRows(histogram, 0).size();
    for (int x = 1; flat && x < histogram.width; ++x)
        flat = litRows(histogram, x).size() == height;
    e.expect(flat && height == 3U * 50U,
             "a 10-bit ramp draws a flat Histogram, 90% of each channel's row");
    // Under a flat curve: the channel's full-color outline on top, a translucent fill below.
    if (!histogram.empty()) {
        const auto top = histogram.pixel(500, 56 - 50);
        const auto inside = histogram.pixel(500, 55);
        e.expect(scopes::alphaOf(top) > 200U &&
                     scopes::alphaOf(inside) ==
                         static_cast<unsigned>(std::lround(scopes::kHistogramFillShare * 255.0F)),
                 "the curve is a full-color outline over a translucent fill");
    }
}

void testEightBitNoComb(Expectations& e) {
    // An 8-bit gray ramp, 8 pixels of each of the 256 values in one column: its codes are 4.01
    // apart, yet the Waveform is one unbroken vertical line at every height (no comb of empty rows)
    // and the Histogram has no empty columns between its first and last value.
    std::vector<Rgba32f> pixels;
    for (int value = 0; value < 256; ++value)
        for (int copy = 0; copy < 8; ++copy) {
            const float v = static_cast<float>(value) / 255.0F;
            pixels.push_back(px(v, v, v));
        }
    const auto tables = tablesOf(pixels, 1, 2048, kAllScopes);
    for (const int height : {256, 333, 700, 1024, 1500}) {
        const auto waveform =
            scopes::mapWaveform(tables, scopes::WaveformChannels::Luma, {4, height});
        const auto rows = waveform.empty() ? std::vector<int>{} : litRows(waveform, 0);
        e.expect(static_cast<int>(rows.size()) == height,
                 "an 8-bit ramp draws an unbroken Waveform line " + std::to_string(height) +
                     " pixels tall");
    }
    for (const int width : {300, 816, 1024, 2000}) {
        const auto histogram = scopes::mapHistogram(tables, scopes::HistogramChannels::Luma,
                                                    scopes::HistogramScale::Linear, 56, width);
        bool full = !histogram.empty();
        for (int x = 0; full && x < histogram.width; ++x)
            full = !litRows(histogram, x).empty();
        e.expect(full, "an 8-bit ramp draws a Histogram with no empty column at width " +
                           std::to_string(width));
    }
}

void testWaveformIndependence(Expectations& e) {
    // A column is as bright as its own pixels make it: other columns' content changes nothing.
    std::vector<Rgba32f> plain(64 * 8, px(0.5F, 0.5F, 0.5F));
    std::vector<Rgba32f> busy = plain;
    for (int y = 0; y < 8; ++y)
        for (int x = 32; x < 64; ++x)
            busy[static_cast<std::size_t>(y * 64 + x)] =
                px(static_cast<float>((x * 7 + y * 3) % 17) / 16.0F, 0.9F,
                   static_cast<float>(y) / 7.0F);
    const auto plainImage = scopes::mapWaveform(tablesOf(plain, 64, 8, kAllScopes),
                                                scopes::WaveformChannels::Rgb, {64, 128});
    const auto busyImage = scopes::mapWaveform(tablesOf(busy, 64, 8, kAllScopes),
                                               scopes::WaveformChannels::Rgb, {64, 128});
    bool same = !plainImage.empty() && !busyImage.empty();
    for (int x = 0; same && x < 32; ++x)
        same = column(plainImage, x) == column(busyImage, x);
    e.expect(same, "a Waveform column's brightness does not depend on the other columns");
}

void testOutsideTheAxis(Expectations& e) {
    // Pixel 3 is over the axis and pixel 5 under it (in every row): drawn on the 1023 and 0 lines
    // of their columns and counted in the Histogram's end bins.
    const auto tables = clippedFrame();
    const auto waveform = scopes::mapWaveform(tables, scopes::WaveformChannels::Luma, {8, 64});
    e.expect(!waveform.empty() && waveform.pixel(3, 0) != 0U && waveform.pixel(3, 63) == 0U &&
                 waveform.pixel(5, 63) != 0U && waveform.pixel(5, 0) == 0U,
             "over is drawn on the 1023 line and under on the 0 line");
    const auto histogram = scopes::mapHistogram(tables, scopes::HistogramChannels::Luma,
                                                scopes::HistogramScale::Linear, 56, 1024);
    e.expect(!histogram.empty() && !litRows(histogram, 0).empty() &&
                 !litRows(histogram, 1023).empty(),
             "under and over are drawn in the Histogram's end bins");
}

// #733: the size a panel's panes ask their tables at.
void testPaneTableSize(Expectations& e) {
    using scopes::Kind;
    const auto waveform = scopes::scopePaneCells(Kind::Waveform, {300, 256}, 4);
    const auto parade = scopes::scopePaneCells(Kind::Parade, {608, 200}, 4);
    const auto vectorscope = scopes::scopePaneCells(Kind::Vectorscope, {300, 280}, 4);
    const auto histogram = scopes::scopePaneCells(Kind::Histogram, {400, 56}, 4);
    const auto chromaticity = scopes::scopePaneCells(Kind::Chromaticity, {400, 400}, 4);
    e.expect(waveform.waveformColumns == 300U && waveform.waveformLevels == 256U &&
                 parade.waveformColumns == 200U && parade.waveformLevels == 200U &&
                 vectorscope.vectorscope == 300U && histogram.histogram &&
                 chromaticity.chromaticity == 360U,
             "a pane draws its trace's device pixels along its table's axes");
    e.expect(scopes::scopeTablesFor(waveform) == render::kScopeTableWaveform &&
                 scopes::scopeTablesFor(parade) == render::kScopeTableWaveform &&
                 scopes::scopeTablesFor(vectorscope) == render::kScopeTableVectorscope &&
                 scopes::scopeTablesFor(histogram) == render::kScopeTableHistogram &&
                 scopes::scopeTablesFor(chromaticity) == render::kScopeTableChromaticity &&
                 scopes::scopeTablesFor({}) == 0U &&
                 scopes::scopeTablesFor(scopes::scopePaneCells(Kind::Waveform, {0, 0}, 4)) == 0U,
             "a pane asks for its own table, and no open pane for none");
    auto all = scopes::mergeScopePaneCells(waveform, parade);
    all = scopes::mergeScopePaneCells(all, vectorscope);
    all = scopes::mergeScopePaneCells(all, histogram);
    all = scopes::mergeScopePaneCells(all, chromaticity);
    e.expect(all.waveformColumns == 300U && all.waveformLevels == 256U &&
                 scopes::scopeTablesFor(all) == render::kScopeTableAll,
             "panes together draw the larger size along every axis and every table");
    // Along every axis the smallest table not below the pane: 512 columns (256 is below 300),
    // 256 levels, 320 Vectorscope cells (160 is below 300), 512 CIE cells (256 is below 360).
    const auto reduction = scopes::scopeTableReductionFor(all);
    e.expect(reduction == render::ScopeTableReduction{0, 2, 1, 0},
             "the tables are asked at the smallest size not below the panes");
    const auto small = scopes::scopeTableReductionFor(
        scopes::mergeScopePaneCells(scopes::scopePaneCells(Kind::Waveform, {100, 60}, 4),
                                    scopes::scopePaneCells(Kind::Vectorscope, {90, 90}, 4)));
    e.expect(small == render::ScopeTableReduction{2, 4, 2, 0} &&
                 render::validScopeTableReduction(small),
             "small panes ask for small tables; an axis no pane draws stays full");
    e.expect(scopes::scopeTableReductionFor({}) == render::ScopeTableReduction{},
             "no open pane names the full size");
    // The Waveform's columns are asked against the widest table; a narrower picture is counted
    // with the shift that still gives as many columns, or every column it has.
    const render::ScopeTableReduction asked{2, 0, 0, 0}; // at least 128 columns
    e.expect(render::scopeReductionForWidth(asked, 1920).waveformColumnShift == 2U &&
                 render::scopeReductionForWidth(asked, 512).waveformColumnShift == 2U &&
                 render::scopeReductionForWidth(asked, 300).waveformColumnShift == 1U &&
                 render::scopeReductionForWidth(asked, 128).waveformColumnShift == 0U &&
                 render::scopeReductionForWidth(asked, 64).waveformColumnShift == 0U &&
                 render::scopeReductionForWidth({2, 3, 4, 5}, 300) ==
                     render::ScopeTableReduction{1, 3, 4, 5},
             "a narrower picture keeps the columns the pane draws");
    for (const std::uint32_t width : {64U, 128U, 300U, 512U, 1920U}) {
        const auto counted = render::scopeReductionForWidth(asked, width);
        const auto layout = render::scopeTableLayout(render::kScopeTableWaveform, width, counted);
        e.expect(layout.columns >= std::min<std::uint32_t>(128U, layout.fullColumns),
                 "the counted table has the columns asked for at width " + std::to_string(width));
    }
}

// The tables a pane of `size` asks for, of a picture `width` wide.
[[nodiscard]] render::ScopeTableReduction paneReduction(const scopes::Kind kind,
                                                        const scopes::PixelSize size,
                                                        const std::uint32_t width) {
    return render::scopeReductionForWidth(
        scopes::scopeTableReductionFor(scopes::scopePaneCells(kind, size, 4)), width);
}

// #733: the trace facts hold for tables counted at the size the pane draws.
void testTracesAtPaneSize(Expectations& e) {
    // No column stripes.
    for (const std::uint32_t width : {640U, 1000U, 1920U}) {
        const std::vector<Rgba32f> pixels(width, px(0.4F, 0.5F, 0.6F));
        for (const int screen : {512, 300, 100}) {
            const auto reduction = paneReduction(scopes::Kind::Waveform, {screen, 128}, width);
            const auto tables = tablesOf(pixels, width, 1, render::kScopeTableWaveform, reduction);
            e.expect(tables.layout().columns >= static_cast<std::uint32_t>(std::min(screen, 512)) &&
                         tables.layout().waveformLevels() >= 128U &&
                         tables.layout().waveformLevels() < 256U,
                     "the table is the smallest not below the pane");
            const auto image =
                scopes::mapWaveform(tables, scopes::WaveformChannels::Rgb, {screen, 128});
            bool same = !image.empty();
            const auto first = image.empty() ? std::vector<std::uint32_t>{} : column(image, 0);
            bool lit = false;
            for (const auto pixel : first)
                lit = lit || pixel != 0U;
            for (int x = 1; same && x < image.width; ++x)
                same = column(image, x) == first;
            e.expect(same && lit, "pane size: no column stripes, measured " +
                                      std::to_string(width) + " wide, drawn " +
                                      std::to_string(screen) + " wide");
        }
    }
    // A 10-bit ramp is one continuous rising line.
    {
        std::vector<Rgba32f> pixels;
        for (int code = 0; code < 1024; ++code) {
            const float v = static_cast<float>(code) / 1023.0F;
            pixels.push_back(px(v, v, v));
        }
        for (const scopes::PixelSize size : {scopes::PixelSize{512, 256}, {256, 128}, {200, 90}}) {
            const auto tables = tablesOf(pixels, 1024, 1, render::kScopeTableWaveform,
                                         paneReduction(scopes::Kind::Waveform, size, 1024));
            const auto waveform = scopes::mapWaveform(tables, scopes::WaveformChannels::Rgb, size);
            bool continuous = !waveform.empty();
            int previousTop = waveform.height;
            for (int x = 0; continuous && x < waveform.width; ++x) {
                const auto rows = litRows(waveform, x);
                continuous = !rows.empty() && rows.back() - rows.front() + 1 ==
                                                  static_cast<int>(rows.size());
                if (!continuous)
                    break;
                continuous =
                    rows.front() <= previousTop && (x == 0 || rows.back() >= previousTop - 1);
                previousTop = rows.front();
            }
            e.expect(continuous, "pane size: a 10-bit ramp draws one continuous rising trace " +
                                     std::to_string(size.height) + " pixels tall");
        }
    }
    // Every code equally often in one column: the column is evenly bright from end to end, at
    // every table size (the spread adds up to 1 and stays flat at the ends of the axis).
    {
        std::vector<Rgba32f> pixels;
        for (int code = 0; code < 1024; ++code) {
            const float v = static_cast<float>(code) / 1023.0F;
            pixels.push_back(px(v, v, v));
        }
        for (const std::uint8_t shift : {std::uint8_t{0}, std::uint8_t{1}, std::uint8_t{2},
                                         std::uint8_t{3}, std::uint8_t{6}}) {
            const auto tables = tablesOf(pixels, 1, 1024, render::kScopeTableWaveform,
                                         render::ScopeTableReduction{0, shift, 0, 0});
            const int levels = static_cast<int>(tables.layout().waveformLevels());
            const auto waveform =
                scopes::mapWaveform(tables, scopes::WaveformChannels::Luma, {1, levels});
            const auto pixelsDrawn =
                waveform.empty() ? std::vector<std::uint32_t>{} : column(waveform, 0);
            bool even = !pixelsDrawn.empty() && pixelsDrawn.front() != 0U;
            for (const auto pixel : pixelsDrawn)
                even = even && pixel == pixelsDrawn.front();
            e.expect(even, "a flat spread stays flat to its ends at " + std::to_string(levels) +
                               " levels");
        }
    }
    // An 8-bit ramp draws no comb.
    {
        std::vector<Rgba32f> pixels;
        for (int value = 0; value < 256; ++value)
            for (int copy = 0; copy < 8; ++copy) {
                const float v = static_cast<float>(value) / 255.0F;
                pixels.push_back(px(v, v, v));
            }
        for (const int height : {64, 256, 333, 700, 1024, 1500}) {
            const auto tables = tablesOf(pixels, 1, 2048, render::kScopeTableWaveform,
                                         paneReduction(scopes::Kind::Waveform, {4, height}, 1));
            const auto waveform =
                scopes::mapWaveform(tables, scopes::WaveformChannels::Luma, {4, height});
            const auto rows = waveform.empty() ? std::vector<int>{} : litRows(waveform, 0);
            e.expect(static_cast<int>(rows.size()) == height,
                     "pane size: an 8-bit ramp draws an unbroken Waveform line " +
                         std::to_string(height) + " pixels tall");
        }
    }
    // Values outside the axis stay on the 0 and 1023 lines.
    {
        std::vector<Rgba32f> pixels(8 * 8, px(0.5F, 0.5F, 0.5F));
        pixels[3] = px(2.0F, 2.0F, 2.0F);
        pixels[5] = px(-0.5F, -0.5F, -0.5F);
        const auto tables = tablesOf(pixels, 8, 8, render::kScopeTableWaveform,
                                     paneReduction(scopes::Kind::Waveform, {8, 64}, 8));
        const auto waveform = scopes::mapWaveform(tables, scopes::WaveformChannels::Luma, {8, 64});
        e.expect(tables.layout().waveformLevels() == 64U && !waveform.empty() &&
                     waveform.pixel(3, 0) != 0U && waveform.pixel(3, 63) == 0U &&
                     waveform.pixel(5, 63) != 0U && waveform.pixel(5, 0) == 0U,
                 "pane size: over is drawn on the 1023 line and under on the 0 line");
    }
    // The Vectorscope and the Chromaticity: a table of exactly the pane's cells draws the very
    // image the full table draws there (a smaller cell is the exact sum of the full cells).
    {
        std::vector<Rgba32f> pixels;
        for (int y = 0; y < 64; ++y)
            for (int x = 0; x < 64; ++x)
                pixels.push_back(px(static_cast<float>(x) / 63.0F, static_cast<float>(y) / 63.0F,
                                    static_cast<float>((x * 5 + y * 3) % 64) / 63.0F));
        const auto full = tablesOf(pixels, 64, 64, kAllScopes);
        const auto luma = rec709();
        for (const int side : {320, 160}) {
            const auto reduction = paneReduction(scopes::Kind::Vectorscope, {side, side}, 64);
            const auto reduced = tablesOf(pixels, 64, 64, kAllScopes, reduction);
            const auto fromFull = scopes::mapVectorscope(full, luma, {side, side});
            const auto fromReduced = scopes::mapVectorscope(reduced, luma, {side, side});
            e.expect(reduced.layout().vectorscopeCells() == static_cast<std::uint32_t>(side) &&
                         !fromFull.empty() && fromFull.pixels == fromReduced.pixels,
                     "pane size: the Vectorscope of " + std::to_string(side) +
                         " cells draws the full table's image");
        }
        {
            const render::ScopeTableReduction reduction{0, 0, 0, 1};
            const auto reduced = tablesOf(pixels, 64, 64, kAllScopes, reduction);
            const auto fromFull = scopes::mapChromaticity(full, {256, 256});
            const auto fromReduced = scopes::mapChromaticity(reduced, {256, 256});
            e.expect(reduced.layout().chromaticityCells() == 256U && !fromFull.empty() &&
                         fromFull.pixels == fromReduced.pixels,
                     "pane size: the Chromaticity of 256 cells draws the full table's image");
        }
        // One flat color at pane size is still one dot.
        const auto barsSmall =
            tablesOf(std::vector<Rgba32f>(64 * 64, px(0.75F, 0.0F, 0.75F)), 64, 64, kAllScopes,
                     paneReduction(scopes::Kind::Vectorscope, {200, 200}, 64));
        const auto dot = scopes::mapVectorscope(barsSmall, luma, {200, 200});
        int lit = 0;
        for (const auto pixel : dot.pixels)
            lit += pixel != 0U ? 1 : 0;
        e.expect(!dot.empty() && lit >= 1 && lit <= 4,
                 "pane size: one flat color is one Vectorscope dot");
    }
}

void testSinglePixelVisible(Expectations& e) {
    const auto tables = grayWithOddPixel();
    const auto luma = rec709();
    // The vectorscope's odd cell, by its coordinates, on a 640-pixel image (one cell a pixel).
    const double y = kKr * kOddRed + kKg * kOddGreen + kKb * kOddBlue;
    const int cbCell =
        static_cast<int>(std::floor(((kOddBlue - y) / (2 * (1 - kKb)) + 0.625) * 512.0));
    const int crCell =
        static_cast<int>(std::floor(((kOddRed - y) / (2 * (1 - kKr)) + 0.625) * 512.0));
    const auto scope = scopes::mapVectorscope(tables, luma, {640, 640});
    e.expect(!scope.empty() && scopes::alphaOf(scope.pixel(cbCell, 639 - crCell)) >= 64U,
             "a single pixel among thousands is a visible Vectorscope dot");
    const auto waveform = scopes::mapWaveform(tables, scopes::WaveformChannels::Rgb, {64, 256});
    e.expect(!waveform.empty() && litRows(waveform, 0).size() >= 2U,
             "the odd pixel's Waveform column draws it beside the gray");
    // The histogram never hides a bin with a count: one pixel tall at least.
    const auto histogram = scopes::mapHistogram(tables, scopes::HistogramChannels::Rgb,
                                                scopes::HistogramScale::Linear, 56, 1024);
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
    const auto height = [](const scopes::ScopeImage& image, const int x) {
        return static_cast<int>(litRows(image, x).size());
    };
    const auto linear = scopes::mapHistogram(tables, scopes::HistogramChannels::Luma,
                                             scopes::HistogramScale::Linear, 56, 1024);
    const auto logarithmic = scopes::mapHistogram(tables, scopes::HistogramChannels::Luma,
                                                  scopes::HistogramScale::Logarithmic, 56, 1024);
    e.expect(linear.width == 1024 && linear.height == 56, "one row of 56 pixels for luma");
    // The tallest bin reaches 90% of the row. Each isolated code is drawn spread by the tent, its
    // own bin holding a quarter of its count.
    const std::array<int, 3> codes{256, 512, 767};
    const std::array<double, 3> counts{48, 15, 1};
    for (std::size_t index = 0; index < 3; ++index) {
        const int expectedLinear =
            std::clamp(static_cast<int>(std::lround(counts[index] / 48.0 * 0.9 * 56.0)), 1, 56);
        const int expectedLog =
            std::clamp(static_cast<int>(std::lround(std::log2(1.0 + counts[index] / 4.0) /
                                                    std::log2(1.0 + 48.0 / 4.0) * 0.9 * 56.0)),
                       1, 56);
        e.expect(height(linear, codes[index]) == expectedLinear,
                 "linear height of code " + std::to_string(codes[index]));
        e.expect(height(logarithmic, codes[index]) == expectedLog,
                 "logarithmic height of code " + std::to_string(codes[index]));
    }
    e.expect(height(linear, 100) == 0 && height(logarithmic, 100) == 0,
             "a bin with no count has no height");
}

void testChromaticityPresence(Expectations& e) {
    // One pixel or a thousand at the same chromaticity draw the same: presence in full color.
    std::vector<Rgba32f> one(64 * 16, px(0.0F, 0.0F, 0.0F));
    one[0] = px(0.2F, 0.6F, 0.3F);
    std::vector<Rgba32f> many(64 * 16, px(0.2F, 0.6F, 0.3F));
    const auto size = scopes::chromaticityImageSize(400);
    const auto oneImage = scopes::mapChromaticity(tablesOf(one, 64, 16, kAllScopes), size);
    const auto manyImage = scopes::mapChromaticity(tablesOf(many, 64, 16, kAllScopes), size);
    int lit = 0;
    bool same = !oneImage.empty() && !manyImage.empty();
    for (int y = 0; same && y < manyImage.height; ++y)
        for (int x = 0; x < manyImage.width; ++x) {
            const auto pixel = manyImage.pixel(x, y);
            if (pixel == 0U)
                continue;
            ++lit;
            same = same && oneImage.pixel(x, y) == pixel && scopes::alphaOf(pixel) == 255U;
        }
    e.expect(same && lit > 0, "the CIE trace marks where pixels are, in full color");
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
    for (auto* role :
         {&palette.quiet, &palette.stronger, &palette.strongest, &palette.label, &palette.faint})
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
        mapping.size = plan.traceRequestSize;
        mapping.gapPixels = scopes::kParadeGap;
        const auto trace = scopes::renderTrace(mapping);
        e.expect(!trace.empty() && trace.width == plan.traceImageSize.width &&
                     trace.height == plan.traceImageSize.height,
                 "the mapping returns the size the plan says");
        const int left = static_cast<int>(std::lround(plan.trace.x));
        const int top = static_cast<int>(std::lround(plan.trace.y));
        const bool additive = item.kind == scopes::Kind::Waveform ||
                              item.kind == scopes::Kind::Parade ||
                              item.kind == scopes::Kind::Vectorscope;
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
        for (int y = 0; y < trace.height; ++y)
            for (int x = 0; x < trace.width; ++x) {
                const int px = left + x;
                const int py = top + y;
                if (px < 0 || py < 0 || px >= item.width || py >= item.height || nearLabel(px, py))
                    continue;
                const auto source = trace.pixel(x, y);
                const auto canvas = palette.canvas;
                const auto channel = [&](const unsigned d, const unsigned s, const unsigned a) {
                    return additive ? std::min(255U, d + s) : s + (d * (255U - a) + 127U) / 255U;
                };
                const auto expected = scopes::packArgb(
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
        testWaveformNoStripes(e);
        testWaveformRamp(e);
        testEightBitNoComb(e);
        testWaveformIndependence(e);
        testOutsideTheAxis(e);
        testPaneTableSize(e);
        testTracesAtPaneSize(e);
        testSinglePixelVisible(e);
        testHistogramHeights(e);
        testChromaticityPresence(e);
        testTraceComposition(e);
        testAllScopesPicture(e);
        testPng(e);
        testRefusals(e);
        if (e.failures() + bloom::test_support::awaitFailureCount() != 0) {
            std::cerr << e.failures() + bloom::test_support::awaitFailureCount()
                      << " scope picture expectation(s) failed\n";
            return 1;
        }
        std::cout << "scope picture tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "scope picture tests threw: " << error.what() << '\n';
        return 1;
    }
}
