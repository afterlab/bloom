// #504 Colorize on the CPU reference: every mode (Fill, Tint, Tritone, Gradient Map with each
// source and cycle, Black & White) and a partial amount against an independent double-precision
// formula over an HDR, signed, partial-alpha image with transparent pixels; the promises each mode
// makes (alpha kept, Fill flat, Tint's end colours, Tritone's three stops, a repeating and a
// mirrored map, the classic grey weights); identity, domains, the one fixed program whose values
// are data, the cache identity, the pixel budget and row cancellation.

#include "color_grade_image_effect.hpp"
#include "image_effect.hpp"
#include <bloom/document/colorize_nodes.hpp>
#include <bloom/runtime/task_scheduler.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace bloom;
using runtime::ColorizeCycle;
using runtime::ColorizeKernel;
using runtime::ColorizeMode;
using runtime::ColorizeSource;

int failures = 0;
void expect(const bool value, const std::string& message) {
    if (!value) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

// Premultiplied pixels: grey ramp values, saturated primaries and secondaries, HDR, signed colour,
// partial alpha and transparent black.
const std::vector<std::array<float, 4>>& pixels() {
    static const std::vector<std::array<float, 4>> values{
        {0.0F, 0.0F, 0.0F, 1.0F},  {0.18F, 0.18F, 0.18F, 1.0F}, {1.0F, 1.0F, 1.0F, 1.0F},
        {1.0F, 0.0F, 0.0F, 1.0F},  {1.0F, 1.0F, 0.0F, 1.0F},    {0.0F, 1.0F, 0.0F, 1.0F},
        {0.0F, 1.0F, 1.0F, 1.0F},  {0.0F, 0.0F, 1.0F, 1.0F},    {1.0F, 0.0F, 1.0F, 1.0F},
        {1.0F, 0.5F, 0.25F, 1.0F}, {4.0F, 2.5F, 6.0F, 1.0F},    {-0.25F, 0.5F, 0.125F, 0.75F},
        {0.3F, 0.15F, 0.6F, 0.6F}, {0.0F, 0.0F, 0.0F, 0.0F},    {0.05F, 0.02F, 0.01F, 0.1F},
        {0.4F, 0.4F, 0.1F, 0.5F},  {0.7F, 0.2F, 0.7F, 1.0F}};
    return values;
}

render::Rgba32fImage image() {
    const auto count = static_cast<std::int64_t>(pixels().size());
    const auto data = render::ImageWindow::create(-3, 2, static_cast<std::uint32_t>(count), 1);
    const auto display = render::ImageWindow::create(0, 0, 32, 8);
    auto descriptor = render::Rgba32fImageDescriptor::create(*data.value(), *display.value(),
                                                             core::PixelAspectRatio::square());
    auto builder = render::Rgba32fImageBuilder::create(*descriptor.value(), 1U << 20U);
    for (std::int64_t x = 0; x < count; ++x) {
        const auto& p = pixels()[static_cast<std::size_t>(x)];
        const auto pixel = render::Rgba32f::fromPremultiplied(p[0], p[1], p[2], p[3]);
        if (!pixel || builder.value()->write(x - 3, 2, *pixel.value()))
            throw std::logic_error("fixture pixel writes");
    }
    auto frozen = std::move(*builder.value()).freeze();
    return std::move(*frozen.value());
}

using Color = core::Color4d;

ColorizeKernel kernel(const ColorizeMode mode) {
    ColorizeKernel item;
    item.mode = mode;
    item.color = {document::ParameterId::fromRaw(2), Color{0.9, 0.3, 1.5, 1.0}};
    item.shadows = {document::ParameterId::fromRaw(3), Color{0.05, 0.0, 0.2, 1.0}};
    item.midtones = {document::ParameterId::fromRaw(4), Color{0.6, 0.2, -0.1, 1.0}};
    item.highlights = {document::ParameterId::fromRaw(5), Color{1.0, 0.9, 0.4, 1.0}};
    return item;
}

double scalar(const runtime::CompiledScalarParameter& parameter) {
    return std::get<double>(parameter.source);
}

std::vector<double> operands(const ColorizeKernel& item) {
    std::vector<double> values{scalar(item.amount)};
    for (const auto* colour : {&item.color, &item.shadows, &item.midtones, &item.highlights}) {
        const auto& c = std::get<Color>(colour->source);
        values.insert(values.end(), {c.red, c.green, c.blue, c.alpha});
    }
    values.push_back(scalar(item.midpoint));
    values.push_back(scalar(item.repetitions));
    values.push_back(scalar(item.phase));
    for (const auto& weight : item.weights)
        values.push_back(scalar(weight));
    // #541: a bound gradient trails the fixed operands as its stop count, then each stop.
    if (item.gradient) {
        const auto& gradient = std::get<document::GradientValue>(item.gradient->source);
        values.push_back(static_cast<double>(gradient.stops.size()));
        for (const auto& stop : gradient.stops)
            values.insert(values.end(), {stop.position, stop.color.red, stop.color.green,
                                         stop.color.blue, stop.color.alpha});
        // #546: then the blend's interpolation, colour mode and hue direction.
        values.insert(values.end(), {static_cast<double>(gradient.modes.interpolation),
                                     static_cast<double>(gradient.modes.colorMode),
                                     static_cast<double>(gradient.modes.hueDirection)});
    }
    return values;
}

// A Gradient Map bound to `stops`, as the compiler lowers a node with its gradient.
ColorizeKernel gradientMap(std::vector<document::GradientStop> stops) {
    auto item = kernel(ColorizeMode::GradientMap);
    item.gradient = runtime::CompiledGradientParameter{document::ParameterId::fromRaw(30),
                                                       document::GradientValue{std::move(stops)}};
    return item;
}

std::optional<runtime::detail::PreparedColorGrade> prepare(const ColorizeKernel& item) {
    return runtime::detail::prepareColorGrade(item, operands(item));
}

// The specification in double precision (docs/architecture/image-filters.md, "Colorize"),
// independent of the binary32 table.
std::array<double, 4> reference(const ColorizeKernel& item, const std::array<float, 4>& pixel) {
    const std::array<double, 4> in{static_cast<double>(pixel[0]), static_cast<double>(pixel[1]),
                                   static_cast<double>(pixel[2]), static_cast<double>(pixel[3])};
    const double a = in[3];
    if (a == 0.0)
        return in;
    const std::array<double, 3> s{in[0] / a, in[1] / a, in[2] / a};
    const auto rgb = [](const runtime::CompiledColorParameter& p) {
        const auto& c = std::get<Color>(p.source);
        return std::array<double, 3>{c.red, c.green, c.blue};
    };
    const double luma = 0.2126 * s[0] + 0.7152 * s[1] + 0.0722 * s[2];
    std::array<double, 3> c{};
    const auto mix = [](const std::array<double, 3>& from, const std::array<double, 3>& to,
                        const double u) {
        return std::array<double, 3>{from[0] + u * (to[0] - from[0]),
                                     from[1] + u * (to[1] - from[1]),
                                     from[2] + u * (to[2] - from[2])};
    };
    switch (item.mode) {
    case ColorizeMode::Fill:
        c = rgb(item.color);
        break;
    case ColorizeMode::Tint:
        c = mix(rgb(item.shadows), rgb(item.highlights), luma);
        break;
    case ColorizeMode::Tritone:
    case ColorizeMode::GradientMap: {
        const bool map = item.mode == ColorizeMode::GradientMap;
        const auto source = map ? item.source : ColorizeSource::Luma;
        double x = source == ColorizeSource::Luma    ? luma
                   : source == ColorizeSource::Alpha ? a
                                                     : s[static_cast<std::size_t>(source) - 1];
        double t = map ? x * scalar(item.repetitions) + scalar(item.phase) : x;
        const auto cycle = map ? item.cycle : ColorizeCycle::Clamp;
        if (cycle == ColorizeCycle::Clamp)
            t = std::clamp(t, 0.0, 1.0);
        else if (cycle == ColorizeCycle::Repeat)
            t -= std::floor(t);
        else {
            const double u = t - 2.0 * std::floor(t / 2.0);
            t = u > 1.0 ? 2.0 - u : u;
        }
        if (map && item.gradient) {
            // The stops by position, ties in stored order; the last stop at or before t blends
            // towards the next, and the ends hold.
            const auto& value = std::get<document::GradientValue>(item.gradient->source);
            if (!value.modes.isLinearRgb()) {
                // #546: the other blends are the document's own evaluator.
                const auto blended = document::gradientColorAt(value, t);
                c = {blended.red, blended.green, blended.blue};
                break;
            }
            auto stops = value.stops;
            std::ranges::stable_sort(stops, {}, &document::GradientStop::position);
            const auto colour = [](const document::GradientStop& stop) {
                return std::array<double, 3>{stop.color.red, stop.color.green, stop.color.blue};
            };
            if (t <= stops.front().position) {
                c = colour(stops.front());
            } else if (t >= stops.back().position) {
                c = colour(stops.back());
            } else {
                std::size_t i = 0;
                while (i + 2 < stops.size() && stops[i + 1].position <= t)
                    ++i;
                const double span = stops[i + 1].position - stops[i].position;
                c = mix(colour(stops[i]), colour(stops[i + 1]),
                        span > 0.0 ? (t - stops[i].position) / span : 0.0);
            }
            break;
        }
        const double m = scalar(item.midpoint);
        c = t <= m ? mix(rgb(item.shadows), rgb(item.midtones), t / m)
                   : mix(rgb(item.midtones), rgb(item.highlights), (t - m) / (1.0 - m));
        break;
    }
    case ColorizeMode::BlackAndWhite: {
        // Weights by the largest channel (red, green, blue) and the smallest (cyan when red is
        // smallest, magenta when green is, yellow when blue is).
        const std::array<double, 3> primary{scalar(item.weights[0]), scalar(item.weights[2]),
                                            scalar(item.weights[4])};
        const std::array<double, 3> secondary{scalar(item.weights[3]), scalar(item.weights[5]),
                                              scalar(item.weights[1])};
        const std::size_t largest = s[0] >= s[1] && s[0] >= s[2] ? 0 : s[1] >= s[2] ? 1 : 2;
        const std::size_t smallest = s[0] <= s[1] && s[0] <= s[2] ? 0 : s[1] <= s[2] ? 1 : 2;
        std::array<double, 3> sorted = s;
        std::ranges::sort(sorted);
        const double grey = sorted[0] + (sorted[1] - sorted[0]) * secondary[smallest] +
                            (sorted[2] - sorted[1]) * primary[largest];
        c = {grey, grey, grey};
        break;
    }
    }
    const double amount = scalar(item.amount);
    for (std::size_t i = 0; i < 3; ++i)
        c[i] = s[i] + amount * (c[i] - s[i]);
    return {c[0] * a, c[1] * a, c[2] * a, a};
}

std::vector<std::array<float, 4>> apply(const ColorizeKernel& item) {
    const auto grade = prepare(item);
    expect(grade.has_value(), "the colorize prepares");
    if (!grade)
        return {};
    const auto input = image();
    const auto result = runtime::detail::applyColorGrade(*grade, input, 1U << 22U, {},
                                                         runtime::OperationIndex::fromRaw(0), {});
    expect(result.image &&
               result.image->descriptor()->dataWindow() == input.descriptor()->dataWindow() &&
               result.image->descriptor()->displayWindow() == input.descriptor()->displayWindow(),
           "the colorize keeps the data and display windows");
    std::vector<std::array<float, 4>> out;
    if (!result.image)
        return out;
    for (std::int64_t x = 0; x < static_cast<std::int64_t>(pixels().size()); ++x) {
        const auto pixel = result.image->read(x - 3, 2);
        std::array<float, 4> value{};
        if (pixel)
            std::ranges::copy(pixel.value()->components(), value.begin());
        out.push_back(value);
    }
    return out;
}

template <typename Actual, typename Expected>
bool near(const Actual actualValue, const Expected expectedValue) {
    const auto actual = static_cast<double>(actualValue);
    const auto expected = static_cast<double>(expectedValue);
    return std::abs(actual - expected) <= 2e-5 * std::max(1.0, std::abs(expected));
}

std::vector<std::pair<ColorizeKernel, std::string>> cases() {
    std::vector<std::pair<ColorizeKernel, std::string>> list{
        {kernel(ColorizeMode::Fill), "Fill"},
        {kernel(ColorizeMode::Tint), "Tint"},
        {kernel(ColorizeMode::Tritone), "Tritone"},
        {kernel(ColorizeMode::BlackAndWhite), "Black & White"}};
    auto map = kernel(ColorizeMode::GradientMap);
    map.repetitions.source = 1.5;
    list.emplace_back(map, "Gradient Map");
    for (const auto source : {ColorizeSource::Red, ColorizeSource::Green, ColorizeSource::Blue,
                              ColorizeSource::Alpha}) {
        auto item = kernel(ColorizeMode::GradientMap);
        item.source = source;
        list.emplace_back(item,
                          "Gradient Map from channel " + std::to_string(static_cast<int>(source)));
    }
    for (const auto cycle : {ColorizeCycle::Repeat, ColorizeCycle::Mirror}) {
        auto item = kernel(ColorizeMode::GradientMap);
        item.cycle = cycle;
        item.repetitions.source = 2.5;
        item.phase.source = -0.3;
        item.midpoint.source = 0.4;
        list.emplace_back(item, cycle == ColorizeCycle::Repeat ? "repeating Gradient Map"
                                                               : "mirrored Gradient Map");
    }
    // #541: a bound gradient, its stops unsorted, with a hard edge at 0.5 and an HDR stop.
    const std::vector<document::GradientStop> stops{{0.8, Color{1.2, 0.9, 0.1, 1.0}},
                                                    {0.0, Color{0.0, 0.05, 0.3, 1.0}},
                                                    {0.5, Color{0.1, 0.6, 0.2, 1.0}},
                                                    {0.5, Color{0.9, 0.1, -0.2, 1.0}},
                                                    {1.0, Color{3.0, 2.0, 1.5, 1.0}}};
    list.emplace_back(gradientMap(stops), "Gradient Map through five stops");
    auto cycling = gradientMap(stops);
    cycling.cycle = ColorizeCycle::Repeat;
    cycling.repetitions.source = 1.7;
    cycling.phase.source = 0.25;
    cycling.source = ColorizeSource::Green;
    list.emplace_back(cycling, "repeating Gradient Map through five stops");
    list.emplace_back(gradientMap({{0.3, Color{0.4, 0.5, 0.6, 1.0}}}),
                      "Gradient Map through one stop");
    // #546: every interpolation, and HSV and HSL in each hue direction.
    const std::vector<document::GradientStop> blendStops{{0.1, Color{1.0, 0.1, 0.0, 1.0}},
                                                         {0.55, Color{0.2, 0.9, 0.3, 1.0}},
                                                         {0.95, Color{0.1, 0.2, 1.4, 1.0}}};
    for (const auto interpolation : document::kGradientInterpolations) {
        auto item = gradientMap(blendStops);
        std::get<document::GradientValue>(item.gradient->source).modes.interpolation =
            interpolation;
        list.emplace_back(item,
                          "Gradient Map with " +
                              std::string(document::gradientInterpolationLabel(interpolation)) +
                              " interpolation");
    }
    for (const auto mode : {document::GradientColorMode::Hsv, document::GradientColorMode::Hsl})
        for (const auto hue : document::kGradientHueDirections) {
            auto item = gradientMap(blendStops);
            auto& modes = std::get<document::GradientValue>(item.gradient->source).modes;
            modes.colorMode = mode;
            modes.hueDirection = hue;
            list.emplace_back(item,
                              "Gradient Map in " +
                                  std::string(document::gradientColorModeLabel(mode)) + " with " +
                                  std::string(document::gradientHueDirectionLabel(hue)) + " hue");
        }
    auto partial = kernel(ColorizeMode::Tritone);
    partial.amount.source = 0.35;
    list.emplace_back(partial, "Tritone at 35%");
    auto weights = kernel(ColorizeMode::BlackAndWhite);
    const std::array<double, 6> custom{-1.5, 2.5, 0.1, 3.0, -2.0, 1.2};
    for (std::size_t i = 0; i < 6; ++i)
        weights.weights[i].source = custom[i];
    weights.amount.source = 0.6;
    list.emplace_back(weights, "Black & White with custom weights at 60%");
    return list;
}

void testModesAgainstReference() {
    for (const auto& [item, name] : cases()) {
        const auto out = apply(item);
        if (out.size() != pixels().size())
            continue;
        bool matches = true;
        bool alpha = true;
        for (std::size_t i = 0; i < out.size(); ++i) {
            const auto expected = reference(item, pixels()[i]);
            for (std::size_t c = 0; c < 3; ++c)
                matches = matches && near(out[i][c], expected[c]);
            alpha = alpha && out[i][3] == pixels()[i][3];
        }
        expect(matches, name + " matches the double-precision reference");
        expect(alpha, name + " keeps every alpha exactly");
    }
}

void testModePromises() {
    // Fill: every visible pixel is the colour times its alpha; transparent stays transparent.
    const auto fill = apply(kernel(ColorizeMode::Fill));
    bool flat = fill.size() == pixels().size();
    for (std::size_t i = 0; flat && i < fill.size(); ++i) {
        const auto a = static_cast<double>(pixels()[i][3]);
        flat = near(fill[i][0], 0.9 * a) && near(fill[i][1], 0.3 * a) && near(fill[i][2], 1.5 * a);
    }
    expect(flat, "Fill is the flat colour over the kept alpha, HDR included");
    // Tint: black to shadows, white to highlights, HDR luma extends the line.
    const auto tint = apply(kernel(ColorizeMode::Tint));
    expect(tint.size() > 2 && near(tint[0][0], 0.05) && near(tint[0][2], 0.2) &&
               near(tint[2][0], 1.0) && near(tint[2][1], 0.9) && near(tint[2][2], 0.4),
           "Tint maps black to the shadows colour and white to the highlights colour");
    // Tritone: mid grey (luma 0.18, the default midpoint) is the midtones colour.
    const auto tritone = apply(kernel(ColorizeMode::Tritone));
    expect(tritone.size() > 2 && near(tritone[1][0], 0.6) && near(tritone[1][1], 0.2) &&
               near(tritone[1][2], -0.1) && near(tritone[0][0], 0.05) && near(tritone[2][1], 0.9),
           "Tritone hits its three stops at black, the midpoint and white");
    // Black & White with the classic weights: red 40%, yellow 60%, white 100%, all channels grey.
    const auto grey = apply(kernel(ColorizeMode::BlackAndWhite));
    expect(grey.size() > 8 && near(grey[3][0], 0.4) && near(grey[4][0], 0.6) &&
               near(grey[2][0], 1.0) && near(grey[5][0], 0.4) && near(grey[6][0], 0.6) &&
               near(grey[7][0], 0.2) && near(grey[8][0], 0.8) && grey[9][0] == grey[9][1] &&
               grey[9][1] == grey[9][2],
           "Black & White gives the classic grey for each primary and secondary");
    // A repeating map wraps: a channel one cycle apart lands on the same colour.
    auto repeat = kernel(ColorizeMode::GradientMap);
    repeat.cycle = ColorizeCycle::Repeat;
    repeat.source = ColorizeSource::Red;
    repeat.repetitions.source = 1.0;
    const auto r = prepare(repeat);
    std::array<float, 4> low{0.25F, 0.0F, 0.0F, 1.0F};
    std::array<float, 4> high{1.25F, 0.0F, 0.0F, 1.0F};
    expect(r && runtime::detail::colorGradePixel(*r, low) &&
               runtime::detail::colorGradePixel(*r, high) && near(low[0], high[0]) &&
               near(low[1], high[1]) && near(low[2], high[2]),
           "a repeating Gradient Map is periodic in its input");
    auto mirror = repeat;
    mirror.cycle = ColorizeCycle::Mirror;
    const auto m = prepare(mirror);
    std::array<float, 4> before{0.75F, 0.0F, 0.0F, 1.0F};
    std::array<float, 4> after{1.25F, 0.0F, 0.0F, 1.0F};
    expect(m && runtime::detail::colorGradePixel(*m, before) &&
               runtime::detail::colorGradePixel(*m, after) && near(before[0], after[0]) &&
               near(before[2], after[2]),
           "a mirrored Gradient Map reflects at the ramp's end");
}

void testDomainsProgramAndIdentity() {
    auto none = kernel(ColorizeMode::Tint);
    none.amount.source = 0.0;
    expect(prepare(none)->identity, "an amount of 0 is the exact identity");
    for (const auto mode : {ColorizeMode::Fill, ColorizeMode::Tint, ColorizeMode::Tritone,
                            ColorizeMode::GradientMap, ColorizeMode::BlackAndWhite})
        expect(!prepare(kernel(mode))->identity, "every mode at full amount does work");
    // A value the mode does not read never changes the table.
    auto other = kernel(ColorizeMode::Fill);
    other.shadows.source = Color{0.7, 0.7, 0.7, 1.0};
    other.weights[0].source = 2.0;
    other.phase.source = 0.5;
    expect(prepare(other)->data == prepare(kernel(ColorizeMode::Fill))->data,
           "an unused value does not change the prepared table");
    auto tritone = kernel(ColorizeMode::Tritone);
    tritone.source = ColorizeSource::Blue;
    tritone.cycle = ColorizeCycle::Mirror;
    tritone.repetitions.source = 3.0;
    expect(prepare(tritone)->data == prepare(kernel(ColorizeMode::Tritone))->data,
           "Tritone ignores the Gradient Map's source, cycle and repetitions");
    // A Gradient Map from luma, clamped, once, is the Tritone: the same table and identity.
    expect(prepare(kernel(ColorizeMode::GradientMap))->data ==
               prepare(kernel(ColorizeMode::Tritone))->data,
           "a default Gradient Map shares the Tritone's table");
    const auto refused = [](ColorizeKernel item) { return !prepare(item).has_value(); };
    auto amount = kernel(ColorizeMode::Tint);
    amount.amount.source = 1.5;
    auto midpoint = kernel(ColorizeMode::Tritone);
    midpoint.midpoint.source = 1.0;
    auto colour = kernel(ColorizeMode::Fill);
    colour.color.source = Color{2e6, 0.0, 0.0, 1.0};
    auto infinite = kernel(ColorizeMode::Fill);
    infinite.highlights.source = Color{std::numeric_limits<double>::infinity(), 0.0, 0.0, 1.0};
    auto weight = kernel(ColorizeMode::BlackAndWhite);
    weight.weights[3].source = -2.5;
    auto mode = kernel(ColorizeMode::Fill);
    mode.mode = static_cast<ColorizeMode>(5);
    expect(refused(amount) && refused(midpoint) && refused(colour) && refused(infinite) &&
               refused(weight) && refused(mode) &&
               !runtime::detail::prepareColorGrade(kernel(ColorizeMode::Fill),
                                                   std::vector<double>{1.0, 0.0}),
           "an out-of-domain amount, midpoint, colour, weight, mode or operand count is refused");
    std::vector<std::string> texts;
    std::vector<std::string> identities;
    for (const auto& [item, name] : cases()) {
        const auto grade = prepare(item);
        const auto program = runtime::detail::buildColorGradeProgram(grade->kind, grade->data);
        expect(program.succeeded() &&
                   render::validateOcioGpuProgram(*program.program(), {}) ==
                       render::OcioGpuProgramError::None &&
                   program.program()->semanticsId == "bloom.render.colorize.v1",
               name + " builds the valid colorize program");
        if (program.succeeded())
            texts.push_back(program.program()->shaderText);
        identities.push_back(runtime::detail::colorGradeIdentity(*grade));
    }
    expect(!texts.empty() &&
               std::ranges::all_of(texts, [&](const auto& text) { return text == texts.front(); }),
           "every mode runs one fixed program: the values are data");
    std::ranges::sort(identities);
    expect(std::ranges::adjacent_find(identities) == identities.end(),
           "each case has its own cache identity");
    const auto grade = prepare(kernel(ColorizeMode::Tritone));
    auto badMode = grade->data;
    badMode[0] = 7.0F;
    auto badMidpoint = grade->data;
    badMidpoint[6] = 1.0F;
    expect(!runtime::detail::buildColorGradeProgram(grade->kind, {0.0F, 0.0F}).succeeded() &&
               !runtime::detail::buildColorGradeProgram(grade->kind, badMode).succeeded() &&
               !runtime::detail::buildColorGradeProgram(grade->kind, badMidpoint).succeeded(),
           "a malformed colorize table is refused");
    // #541: a bound gradient's table: the stops in position order, at most 64, and a malformed one
    // is refused by the program builder.
    auto many = std::vector<document::GradientStop>(document::kMaximumGradientStops,
                                                    {0.5, Color{0.5, 0.5, 0.5, 1.0}});
    expect(prepare(gradientMap(many)).has_value(), "a 64-stop gradient prepares");
    auto tooMany = gradientMap(many);
    std::get<document::GradientValue>(tooMany.gradient->source)
        .stops.push_back({0.5, Color{0.5, 0.5, 0.5, 1.0}});
    auto outside = gradientMap({{1.5, Color{0.5, 0.5, 0.5, 1.0}}});
    expect(refused(tooMany) && refused(outside) && refused(gradientMap({})),
           "a gradient with no stops, more than 64 or a stop outside [0, 1] is refused");
    const auto stopped = prepare(gradientMap({{0.0, Color{0.0, 0.0, 0.0, 1.0}},
                                              {1.0, Color{1.0, 1.0, 1.0, 1.0}}}));
    expect(stopped && stopped->data[0] == 4.0F && stopped->data[7] == 2.0F &&
               runtime::detail::buildColorGradeProgram(stopped->kind, stopped->data).succeeded(),
           "a bound gradient is table mode 4 with its stop count");
    if (stopped) {
        auto shortTable = stopped->data;
        shortTable.pop_back();
        auto countMismatch = stopped->data;
        countMismatch[7] = 3.0F;
        expect(!runtime::detail::buildColorGradeProgram(stopped->kind, shortTable).succeeded() &&
                   !runtime::detail::buildColorGradeProgram(stopped->kind, countMismatch)
                        .succeeded(),
               "a gradient table whose size and stop count disagree is refused");
    }
    expect(prepare(gradientMap({{0.0, Color{0.05, 0.0, 0.2, 1.0}},
                                {0.18, Color{0.6, 0.2, -0.1, 1.0}},
                                {1.0, Color{1.0, 0.9, 0.4, 1.0}}}))
                   ->data != prepare(kernel(ColorizeMode::GradientMap))->data,
           "a bound gradient has its own table and cache identity");
    // A position beyond binary32 is refused rather than published.
    auto huge = kernel(ColorizeMode::GradientMap);
    huge.source = ColorizeSource::Red;
    huge.repetitions.source = 100.0;
    const auto overflow = prepare(huge);
    std::array<float, 4> pixel{3e37F, 0.0F, 0.0F, 1.0F};
    expect(overflow && !runtime::detail::colorGradePixel(*overflow, pixel),
           "a non-finite ramp position is reported rather than published");
}

void testBudgetAndCancellation() {
    const auto input = image();
    const auto grade = prepare(kernel(ColorizeMode::GradientMap));
    const auto refused = runtime::detail::applyColorGrade(*grade, input, 64, {},
                                                          runtime::OperationIndex::fromRaw(0), {});
    expect(!refused.image && refused.diagnostic &&
               refused.diagnostic->code ==
                   runtime::EvaluationDiagnosticCode::PixelStorageBudgetExceeded,
           "a colorize beyond the pixel budget is refused");
    runtime::TaskScheduler scheduler(runtime::TaskSchedulerConfig::defaults());
    const runtime::TaskOwner owner{.kind = runtime::TaskOwnerKind::Composition,
                                   .id = runtime::TaskOwnerId::fromRaw(504)};
    bool cancelled = false;
    auto submission = scheduler.submit<void>(
        runtime::TaskRequest("Colorize cancellation", owner), [&](runtime::TaskContext& context) {
            static_cast<void>(scheduler.cancelOwner(owner));
            const auto result =
                runtime::detail::applyColorGrade(*grade, input, 1U << 22U, context.cancellation(),
                                                 runtime::OperationIndex::fromRaw(0), {});
            cancelled = result.cancelled && !result.image;
            return runtime::TaskResult<void>::cancelled();
        });
    expect(submission.accepted(), "the cancellation task is admitted");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    std::optional<runtime::TaskResult<void>> result;
    while (!result && std::chrono::steady_clock::now() < deadline) {
        result = submission.handle.tryTakeResult();
        std::this_thread::yield();
    }
    expect(result && result->state() == runtime::TaskState::Cancelled && cancelled,
           "a cancelled colorize publishes no image");
    scheduler.beginShutdown();
    while (!scheduler.isQuiescent() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    expect(scheduler.isQuiescent(), "the cancelled colorize shuts down safely");
}

} // namespace

int main() {
    try {
        testModesAgainstReference();
        testModePromises();
        testDomainsProgramAndIdentity();
        testBudgetAndCancellation();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    if (failures != 0) {
        std::cerr << failures << " colorize expectation(s) failed\n";
        return 1;
    }
    std::cout << "PASS: colorize modes, promises, domains, program, budget and cancellation\n";
    return 0;
}
