// #518 Tone Map on the CPU reference: known answers read from real pixels for the roll-off, the hard
// clip, the brightest-channel method, shadow recovery and gamut compression, the properties the
// formulas promise (identity below the knee, continuity and unit slope at it, monotonic, bounded by
// the ceiling), premultiplication and alpha, domain refusal, the table's cache identity, the pixel
// budget and row cancellation.

#include "color_grade_image_effect.hpp"
#include "color_tool_image_effect.hpp"
#include "color_tool_test_support.hpp"
#include "image_effect.hpp"

#include <bloom/runtime/task_scheduler.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace bloom;
namespace detail = runtime::detail;
using color_tool_test::ColorMatrixSettings;
using color_tool_test::ToneMapSettings;

int failures = 0;
void expect(const bool value, const std::string& message) {
    if (!value) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

using Pixel = std::array<float, 4>;

bool near(const float a, const float b, const float tolerance = 1e-5F) {
    return std::fabs(a - b) <= tolerance * std::max(1.0F, std::fabs(b));
}
bool same(const Pixel& a, const Pixel& b, const float tolerance = 1e-5F) {
    return near(a[0], b[0], tolerance) && near(a[1], b[1], tolerance) &&
           near(a[2], b[2], tolerance) && near(a[3], b[3], tolerance);
}

std::optional<detail::PreparedColorGrade> prepare(const runtime::ColorToolKernel& kernel) {
    std::vector<double> values;
    for (std::size_t index = 0; index < runtime::colorToolScalarCount(kernel.operation); ++index)
        values.push_back(std::get<double>(kernel.values[index].source));
    return detail::prepareColorToolGrade(kernel, values);
}

// A premultiplied pixel from straight color and alpha.
Pixel premultiplied(const float r, const float g, const float b, const float a) {
    return {r * a, g * a, b * a, a};
}

Pixel run(const ToneMapSettings& settings, Pixel pixel) {
    const auto grade = prepare(color_tool_test::toneMap(1, settings));
    if (!grade)
        throw std::logic_error("the kernel prepares");
    const bool ok = detail::colorToolPixel(grade->data, pixel);
    expect(ok, "a finite pixel always succeeds");
    return pixel;
}

// A settings value with the roll-off alone, the shadows and gamut off.
ToneMapSettings rollOffOnly(const double knee, const double ceiling, const std::int64_t method = 0) {
    ToneMapSettings settings;
    settings.method = method;
    settings.knee = knee;
    settings.ceiling = ceiling;
    return settings;
}

// The roll-off of the specification in double precision.
double reference(const double x, const double knee, const double ceiling) {
    const double room = ceiling - knee;
    if (room <= 0.0)
        return std::min(x, ceiling);
    if (x <= knee)
        return x;
    const double d = x - knee;
    return knee + room * d / (d + room);
}

void testRollOff() {
    // Knee 0.8, ceiling 1.0: room 0.2. At 1.0 the distance is 0.2, so 0.8 + 0.2 * 0.2 / 0.4 = 0.9;
    // at 1.8 it is 0.8 + 0.2 * 1.0 / 1.2.
    const auto got = run(rollOffOnly(0.8, 1.0), {0.5F, 1.0F, 1.8F, 1.0F});
    expect(same(got, {0.5F, 0.9F, 0.8F + 0.2F / 1.2F, 1.0F}), "the roll-off at known values");
    expect(same(run(rollOffOnly(0.8, 1.0), {0.8F, 0.0F, -3.0F, 1.0F}), {0.8F, 0.0F, -3.0F, 1.0F}),
           "values at or below the knee, negative ones included, are untouched");
    // Far above the knee the value approaches, and never reaches, the ceiling.
    const auto far = run(rollOffOnly(0.8, 1.0), {1000.0F, 100000.0F, 0.0F, 1.0F});
    expect(far[0] < 1.0F && far[0] > 0.999F && far[1] <= 1.0F && far[1] > 0.99999F,
           "huge values approach the ceiling");
    // A ceiling at or below the knee is a hard clip at the ceiling.
    expect(same(run(rollOffOnly(0.8, 0.5), {2.0F, 0.3F, 0.6F, 1.0F}), {0.5F, 0.3F, 0.5F, 1.0F}),
           "a ceiling below the knee clips hard");
    expect(same(run(rollOffOnly(0.5, 0.5), {2.0F, 0.3F, 0.5F, 1.0F}), {0.5F, 0.3F, 0.5F, 1.0F}),
           "a ceiling at the knee clips hard");
    // HDR ceiling: a knee of 4 and a ceiling of 8 leave a scene-linear 3 alone and bend 12.
    expect(same(run(rollOffOnly(4.0, 8.0), {3.0F, 12.0F, 4.0F, 1.0F}),
                {3.0F, static_cast<float>(reference(12.0, 4.0, 8.0)), 4.0F, 1.0F}),
           "a high knee and ceiling work in HDR");
}

void testProperties() {
    // Monotonic, below the ceiling, continuous and with unit slope at the knee, and equal to the
    // double-precision formula across a sweep that crosses the knee.
    float previous = -1.0e9F;
    bool monotonic = true;
    bool bounded = true;
    bool matches = true;
    for (int step = 0; step <= 4000; ++step) {
        const float x = -1.0F + 0.005F * static_cast<float>(step);
        const auto got = run(rollOffOnly(0.8, 1.0), {x, 0.0F, 0.0F, 1.0F});
        monotonic = monotonic && got[0] >= previous;
        previous = got[0];
        bounded = bounded && got[0] < 1.0F;
        matches = matches &&
                  std::fabs(static_cast<double>(got[0]) - reference(static_cast<double>(x), 0.8, 1.0)) <=
                      1e-6;
    }
    expect(monotonic, "the roll-off never decreases");
    expect(bounded, "the roll-off stays below the ceiling");
    expect(matches, "the roll-off matches the double-precision formula over a sweep");
    const float epsilon = 1.0e-3F;
    const auto above = run(rollOffOnly(0.8, 1.0), {0.8F + epsilon, 0.0F, 0.0F, 1.0F});
    expect(std::fabs((above[0] - 0.8F) / epsilon - 1.0F) < 0.01F,
           "the slope at the knee is one, so the curve leaves it without a corner");
}

void testBrightestChannel() {
    // The brightest channel rolls off and the others follow by the same factor: hue holds.
    const auto got = run(rollOffOnly(0.8, 1.0, 1), {2.0F, 1.0F, 0.5F, 1.0F});
    const float rolled = static_cast<float>(reference(2.0, 0.8, 1.0));
    const float factor = rolled / 2.0F;
    expect(same(got, {rolled, 1.0F * factor, 0.5F * factor, 1.0F}),
           "the brightest channel rolls off and the others follow");
    expect(near(got[1] / got[0], 0.5F) && near(got[2] / got[0], 0.25F),
           "channel ratios (hue and saturation) are kept");
    // Each channel on its own bends the ratios instead.
    const auto each = run(rollOffOnly(0.8, 1.0, 0), {2.0F, 1.0F, 0.5F, 1.0F});
    expect(!near(each[1] / each[0], 0.5F), "rolling each channel on its own changes the ratios");
    expect(same(run(rollOffOnly(0.8, 1.0, 1), {0.5F, 0.25F, 0.1F, 1.0F}), {0.5F, 0.25F, 0.1F, 1.0F}),
           "a pixel whose brightest channel is below the knee is untouched");
    expect(same(run(rollOffOnly(0.8, 1.0, 1), {-0.5F, -0.25F, -0.1F, 1.0F}),
                {-0.5F, -0.25F, -0.1F, 1.0F}),
           "an all-negative pixel is untouched");
}

void testShadows() {
    // The gain is 1 + amount * range / (range + x): twice near black and 1.5 at the range itself.
    ToneMapSettings settings;
    settings.knee = 100.0;
    settings.ceiling = 200.0;
    settings.shadows = 1.0;
    expect(same(run(settings, {0.0F, 0.18F, 1.0F, 1.0F}),
                {0.0F, 0.18F * 1.5F, 1.0F * (1.0F + 0.18F / 1.18F), 1.0F}),
           "shadow recovery gains dark values most and keeps black black");
    settings.shadows = -1.0;
    expect(same(run(settings, {0.0F, 0.18F, 100.0F, 1.0F}),
                {0.0F, 0.09F, 100.0F * (1.0F - 0.18F / 100.18F), 1.0F}),
           "a negative amount darkens the shadows and keeps highlights");
    settings.shadows = 0.0;
    expect(same(run(settings, {0.02F, 0.18F, 1.0F, 1.0F}), {0.02F, 0.18F, 1.0F, 1.0F}),
           "no shadow recovery changes nothing");
    // A narrow range reaches only the darkest values.
    settings.shadows = 4.0;
    settings.shadowRange = 0.01;
    const auto narrow = run(settings, {0.001F, 0.5F, 0.0F, 1.0F});
    expect(narrow[0] > 0.001F * 4.0F && near(narrow[1] / 0.5F, 1.0F + 4.0F * 0.01F / 0.51F),
           "the shadow range sets how far up the recovery reaches");
}

void testGamut() {
    // A distance equal to the limit lands on the edge: (1.0, 0.5, -0.2) has the blue 1.2 below the
    // brightest channel, a distance of 1.2 with threshold 0.8 and limit 1.2, so blue becomes 0.
    ToneMapSettings settings;
    settings.knee = 100.0;
    settings.ceiling = 200.0;
    settings.gamut = true;
    const auto got = run(settings, {1.0F, 0.5F, -0.2F, 1.0F});
    expect(same(got, {1.0F, 0.5F, 0.0F, 1.0F}), "a distance equal to the limit reaches the edge");
    // A distance beyond the limit is compressed, but still inside the gamut's far side.
    const auto beyond = run(settings, {1.0F, 0.5F, -0.6F, 1.0F});
    expect(beyond[2] > -0.6F && beyond[2] < 0.0F && beyond[0] == 1.0F && beyond[1] == 0.5F,
           "a larger distance is pulled in but not onto the edge");
    // Below the threshold nothing moves, exactly.
    const auto inside = run(settings, {1.0F, 0.5F, 0.3F, 1.0F});
    expect(inside[0] == 1.0F && inside[1] == 0.5F && inside[2] == 0.3F,
           "colors inside the threshold are not touched");
    // Off by default: the same pixel keeps its negative channel.
    settings.gamut = false;
    expect(same(run(settings, {1.0F, 0.5F, -0.2F, 1.0F}), {1.0F, 0.5F, -0.2F, 1.0F}),
           "gamut compression is off unless asked for");
    // The compression is monotonic in the distance.
    settings.gamut = true;
    float last = 2.0F;
    bool monotonic = true;
    for (int step = 0; step <= 300; ++step) {
        const float blue = -0.01F * static_cast<float>(step);
        const auto out = run(settings, {1.0F, 0.5F, blue, 1.0F});
        monotonic = monotonic && out[2] <= last + 1e-6F;
        last = out[2];
    }
    expect(monotonic, "a farther color never ends nearer to the edge");
}

void testAlpha() {
    // The straight color rolls off and is premultiplied again; alpha is untouched.
    const auto half = premultiplied(1.0F, 1.0F, 1.0F, 0.5F);
    expect(same(run(rollOffOnly(0.8, 1.0), half), premultiplied(0.9F, 0.9F, 0.9F, 0.5F)),
           "a half-covered pixel rolls off its straight color");
    expect(same(run(rollOffOnly(0.8, 1.0), {0.0F, 0.0F, 0.0F, 0.0F}), {0.0F, 0.0F, 0.0F, 0.0F}),
           "alpha 0 stays as it is");
    // Premultiplied light with no alpha has no straight color to roll off and is left as stored.
    expect(same(run(rollOffOnly(0.8, 1.0), {3.0F, 2.0F, 1.0F, 0.0F}), {3.0F, 2.0F, 1.0F, 0.0F}),
           "stored light at alpha 0 is not changed");
    ToneMapSettings settings;
    settings.knee = 100.0;
    settings.ceiling = 200.0;
    const auto nan = [&] {
        Pixel pixel{std::numeric_limits<float>::quiet_NaN(), 0.0F, 0.0F, 1.0F};
        const auto grade = prepare(color_tool_test::toneMap(1, settings));
        return grade && !detail::colorToolPixel(grade->data, pixel);
    }();
    expect(nan, "a non-finite input is refused, as every grade refuses it");
}

void testDomainAndIdentity() {
    using color_tool_test::toneMap;
    ToneMapSettings bad;
    bad.knee = -0.1;
    expect(!prepare(toneMap(1, bad)), "a negative knee is refused");
    bad = {};
    bad.shadows = -1.5;
    expect(!prepare(toneMap(1, bad)), "shadow recovery below -1 is refused");
    bad = {};
    bad.shadowRange = 0.0;
    expect(!prepare(toneMap(1, bad)), "a zero shadow range is refused");
    bad = {};
    bad.gamutLimit = 1.0;
    expect(!prepare(toneMap(1, bad)), "a gamut limit at one is refused");
    bad = {};
    bad.gamutThreshold = 1.0;
    expect(!prepare(toneMap(1, bad)), "a gamut threshold at one is refused");
    bad = {};
    bad.ceiling = std::numeric_limits<double>::quiet_NaN();
    expect(!prepare(toneMap(1, bad)), "a NaN ceiling is refused");
    bad = {};
    bad.method = 2;
    expect(!prepare(toneMap(1, bad)), "an unknown method is refused");

    const auto a = prepare(toneMap(1, {}));
    const auto b = prepare(toneMap(1, {}));
    ToneMapSettings other;
    other.ceiling = 2.0;
    const auto c = prepare(toneMap(1, other));
    ToneMapSettings gamut;
    gamut.gamut = true;
    const auto d = prepare(toneMap(1, gamut));
    expect(a && b && c && d, "the tables prepare");
    expect(a->data == b->data && detail::colorGradeIdentity(*a) == detail::colorGradeIdentity(*b),
           "equal settings give an equal table and cache identity");
    expect(a->data != c->data && a->data != d->data &&
               detail::colorGradeIdentity(*a) != detail::colorGradeIdentity(*c) &&
               detail::colorGradeIdentity(*a) != detail::colorGradeIdentity(*d),
           "a changed value or switch changes the table and the cache identity");
    expect(!a->identity, "the pass is never an identity alias");
    expect(detail::validColorToolProgramData(a->data), "the table passes the program's check");
    auto broken = a->data;
    broken.pop_back();
    expect(!detail::validColorToolProgramData(broken) &&
               !detail::buildColorGradeProgram(detail::ColorGradeKind::ColorTool, broken)
                    .succeeded(),
           "a table of the wrong size builds no program");
    const auto program = detail::buildColorGradeProgram(detail::ColorGradeKind::ColorTool, a->data);
    expect(program.succeeded() && program.program()->functionName == "bloom_color_tool",
           "the one fixed program builds from the table");
}


// --- Color Matrix ---------------------------------------------------------------------------------

Pixel runMatrix(const ColorMatrixSettings& settings, Pixel pixel) {
    const auto grade = prepare(color_tool_test::colorMatrix(1, settings));
    if (!grade)
        throw std::logic_error("the matrix kernel prepares");
    if (grade->identity)
        return pixel;
    expect(detail::colorToolPixel(grade->data, pixel), "a finite pixel always succeeds");
    return pixel;
}

ColorMatrixSettings modelPair(const std::int64_t input, const std::int64_t output) {
    ColorMatrixSettings settings;
    settings.inputModel = input;
    settings.outputModel = output;
    return settings;
}

void testMatrixKnownAnswers() {
    using document::kColorMatrixSize4;
    ColorMatrixSettings gray;
    gray.preset = 2;
    expect(same(runMatrix(gray, {1.0F, 0.0F, 0.0F, 1.0F}), {0.2126F, 0.2126F, 0.2126F, 1.0F}),
           "the gray preset gives red its luminance weight");
    ColorMatrixSettings swap;
    swap.preset = 3;
    expect(same(runMatrix(swap, {0.1F, 0.2F, 0.3F, 1.0F}), {0.3F, 0.2F, 0.1F, 1.0F}),
           "the swap preset exchanges red and blue");
    swap.preset = 4;
    expect(same(runMatrix(swap, {0.1F, 0.2F, 0.3F, 1.0F}), {0.2F, 0.1F, 0.3F, 1.0F}),
           "the swap preset exchanges red and green");
    swap.preset = 5;
    expect(same(runMatrix(swap, {0.1F, 0.2F, 0.3F, 1.0F}), {0.1F, 0.3F, 0.2F, 1.0F}),
           "the swap preset exchanges green and blue");
    ColorMatrixSettings sepia;
    sepia.preset = 6;
    expect(same(runMatrix(sepia, {1.0F, 1.0F, 1.0F, 1.0F}), {1.351F, 1.203F, 0.937F, 1.0F}),
           "the sepia preset sums its rows over white");
    ColorMatrixSettings custom;
    custom.matrix = {0.5, 0.5, 0, 0, 0, 1, 0, 0, 2, 0, -1, 0, 0, 0, 0, 1};
    expect(same(runMatrix(custom, {0.2F, 0.4F, 0.6F, 1.0F}), {0.3F, 0.4F, -0.2F, 1.0F}),
           "a custom 3 x 3 matrix mixes the straight channels, HDR and negative results kept");
    expect(same(runMatrix(custom, premultiplied(0.2F, 0.4F, 0.6F, 0.5F)),
                premultiplied(0.3F, 0.4F, -0.2F, 0.5F)),
           "a 3 x 3 matrix mixes the straight color of a partly covered pixel");
    // The alpha row and column count only at 4 x 4.
    ColorMatrixSettings alpha;
    alpha.matrix = {1, 0, 0, 0.5, 0, 1, 0, 0, 0, 0, 1, 0, 0.2126, 0.7152, 0.0722, 0};
    expect(same(runMatrix(alpha, premultiplied(0.2F, 0.4F, 0.6F, 0.5F)),
                premultiplied(0.2F, 0.4F, 0.6F, 0.5F)),
           "at 3 x 3 the alpha row and column are ignored");
    alpha.size = kColorMatrixSize4;
    const float luma = 0.2126F * 0.2F + 0.7152F * 0.4F + 0.0722F * 0.6F;
    expect(same(runMatrix(alpha, premultiplied(0.2F, 0.4F, 0.6F, 0.5F)),
                premultiplied(0.2F + 0.25F, 0.4F, 0.6F, luma)),
           "at 4 x 4 alpha is made from color, color from alpha, and alpha stays in range");
    alpha.matrix = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 5};
    expect(near(runMatrix(alpha, {0.2F, 0.2F, 0.2F, 0.5F})[3], 1.0F),
           "a result alpha above one is limited to one");
    alpha.matrix = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -1};
    expect(same(runMatrix(alpha, {0.2F, 0.2F, 0.2F, 0.5F}), {0.0F, 0.0F, 0.0F, 0.0F}),
           "a negative alpha is zero and transparent black");
    expect(same(runMatrix(sepia, {3.0F, 2.0F, 1.0F, 0.0F}), {3.0F, 2.0F, 1.0F, 0.0F}),
           "a pixel with alpha 0 is left as stored");
}

void testMatrixModels() {
    // Known answers of each model for a few colors.
    expect(same(runMatrix(modelPair(0, 2), {1.0F, 0.0F, 0.0F, 1.0F}), {0.0F, 1.0F, 1.0F, 1.0F}),
           "HSV of red");
    expect(same(runMatrix(modelPair(0, 2), {0.0F, 1.0F, 0.0F, 1.0F}), {1.0F / 3.0F, 1.0F, 1.0F, 1.0F}),
           "HSV of green: a third of a turn");
    expect(same(runMatrix(modelPair(0, 2), {0.5F, 0.25F, 0.25F, 1.0F}), {0.0F, 0.5F, 0.5F, 1.0F}),
           "HSV of a muted red");
    expect(same(runMatrix(modelPair(0, 2), {8.0F, 4.0F, 0.0F, 1.0F}),
                {1.0F / 12.0F, 1.0F, 8.0F, 1.0F}),
           "HSV keeps an HDR value");
    expect(same(runMatrix(modelPair(0, 1), {1.0F, 0.0F, 0.0F, 1.0F}), {0.0F, 1.0F, 0.5F, 1.0F}),
           "HSL of red");
    expect(same(runMatrix(modelPair(0, 1), {0.5F, 0.25F, 0.25F, 1.0F}),
                {0.0F, 1.0F / 3.0F, 0.375F, 1.0F}),
           "HSL of a muted red");
    expect(same(runMatrix(modelPair(0, 3), {1.0F, 0.0F, 0.0F, 1.0F}),
                {0.2126F, -0.2126F / 1.8556F, 0.5F, 1.0F}),
           "YCbCr of red: its Rec. 709 luma and Cr of one half");
    expect(same(runMatrix(modelPair(0, 3), {1.0F, 1.0F, 1.0F, 1.0F}), {1.0F, 0.0F, 0.0F, 1.0F}, 3e-5F),
           "YCbCr of white has no chroma");
    // Lab, with L, a and b divided by 100: white is 1, red is L* 53.24, a* 80.09, b* 67.20.
    expect(same(runMatrix(modelPair(0, 4), {1.0F, 1.0F, 1.0F, 1.0F}), {1.0F, 0.0F, 0.0F, 1.0F}, 3e-4F),
           "Lab of white");
    expect(same(runMatrix(modelPair(0, 4), {1.0F, 0.0F, 0.0F, 1.0F}),
                {0.5324F, 0.8009F, 0.6720F, 1.0F}, 6e-4F),
           "Lab of red");
    expect(near(runMatrix(modelPair(0, 4), {0.18F, 0.18F, 0.18F, 1.0F})[0], 0.49496F, 3e-4F),
           "Lab lightness of 18 percent gray");
    // Each model back to RGB is the inverse, over a set that includes HDR for the models that
    // extend (HSL limits to 0 to 1).
    const std::vector<Pixel> colors{{0.5F, 0.25F, 0.125F, 1.0F}, {0.9F, 0.1F, 0.4F, 1.0F},
                                    {0.2F, 0.7F, 0.3F, 1.0F},   {0.0F, 0.3F, 0.9F, 1.0F},
                                    {0.3F, 0.3F, 0.3F, 1.0F},   {0.05F, 0.02F, 0.01F, 1.0F}};
    const std::vector<Pixel> hdr{{4.0F, 2.0F, 0.5F, 1.0F}, {0.2F, 6.0F, 1.5F, 1.0F}};
    for (const std::int64_t model : {1, 2, 3, 4}) {
        bool back = true;
        for (const auto& color : colors) {
            const auto out = runMatrix(modelPair(0, model), color);
            const auto round = runMatrix(modelPair(model, 0), out);
            back = back && same(round, color, 2e-4F);
        }
        if (model != 1)
            for (const auto& color : hdr) {
                const auto out = runMatrix(modelPair(0, model), color);
                const auto round = runMatrix(modelPair(model, 0), out);
                back = back && same(round, color, 2e-4F);
            }
        expect(back, "converting to a model and back reproduces the color (model " +
                         std::to_string(model) + ")");
    }
    // A hue is a turn: 1.25 is 0.25, so a hue outside the turn wraps.
    expect(same(runMatrix(modelPair(2, 0), {1.25F, 1.0F, 1.0F, 1.0F}),
                runMatrix(modelPair(2, 0), {0.25F, 1.0F, 1.0F, 1.0F})),
           "a hue wraps around the turn");
}

void testMatrixDomainAndIdentity() {
    using color_tool_test::colorMatrix;
    const auto identity = prepare(colorMatrix(1, {}));
    expect(identity && identity->identity, "an identity matrix between RGB and RGB aliases");
    ColorMatrixSettings model;
    model.outputModel = 2;
    const auto notIdentity = prepare(colorMatrix(1, model));
    expect(notIdentity && !notIdentity->identity, "a color model change is a pass");
    ColorMatrixSettings preset;
    preset.preset = 1;
    preset.matrix[0] = 5.0;
    const auto presetGrade = prepare(colorMatrix(1, preset));
    expect(presetGrade && presetGrade->identity, "a preset replaces the matrix elements");
    ColorMatrixSettings bad;
    bad.matrix[3] = 101.0;
    expect(!prepare(colorMatrix(1, bad)), "an element beyond the domain is refused");
    bad = {};
    bad.matrix[3] = std::numeric_limits<double>::quiet_NaN();
    expect(!prepare(colorMatrix(1, bad)), "a NaN element is refused");
    bad = {};
    bad.preset = 7;
    expect(!prepare(colorMatrix(1, bad)), "an unknown preset is refused");
    bad = {};
    bad.inputModel = 5;
    expect(!prepare(colorMatrix(1, bad)), "an unknown model is refused");
    ColorMatrixSettings a;
    a.matrix[1] = 0.25;
    ColorMatrixSettings b = a;
    b.size = 1;
    const auto ga = prepare(colorMatrix(1, a));
    const auto gb = prepare(colorMatrix(1, b));
    expect(ga && gb && detail::colorGradeIdentity(*ga) != detail::colorGradeIdentity(*gb) &&
               detail::validColorToolProgramData(ga->data) &&
               detail::buildColorGradeProgram(detail::ColorGradeKind::ColorTool, ga->data)
                   .succeeded(),
           "a changed size changes the cache identity and the table builds the program");
}

bloom::render::Rgba32fImage image(const std::uint32_t width, const std::uint32_t height,
                                  const Pixel& fill) {
    const auto window = render::ImageWindow::create(0, 0, width, height);
    auto descriptor = render::Rgba32fImageDescriptor::create(*window.value(), *window.value(),
                                                             core::PixelAspectRatio::square());
    auto builder = render::Rgba32fImageBuilder::create(*descriptor.value(), 1U << 26U);
    for (std::int64_t y = 0; y < static_cast<std::int64_t>(height); ++y)
        for (std::int64_t x = 0; x < static_cast<std::int64_t>(width); ++x) {
            const auto stored =
                render::Rgba32f::fromPremultiplied(fill[0], fill[1], fill[2], fill[3]);
            if (!stored || builder.value()->write(x, y, *stored.value()))
                throw std::logic_error("fixture pixel writes");
        }
    auto frozen = std::move(*builder.value()).freeze();
    return std::move(*frozen.value());
}

Pixel runQualifier(const color_tool_test::HslQualifierSettings& settings, Pixel pixel) {
    const auto grade = prepare(color_tool_test::hslQualifier(1, settings));
    if (!grade)
        throw std::logic_error("the qualifier kernel prepares");
    expect(detail::colorToolPixel(grade->data, pixel), "a finite pixel always succeeds");
    return pixel;
}

void testQualifier() {
    using color_tool_test::HslQualifierSettings;
    const auto coverage = [](const float c) { return Pixel{c, c, c, c}; };
    HslQualifierSettings hue;
    hue.hueWidth = 60.0;
    hue.hueSoftness = 0.0;
    expect(same(runQualifier(hue, {1.0F, 0.0F, 0.0F, 1.0F}), coverage(1.0F)),
           "red is inside a hue window centered on red");
    expect(same(runQualifier(hue, {0.0F, 1.0F, 0.0F, 1.0F}), coverage(0.0F)),
           "green is outside a hard window centered on red");
    hue.hueCenter = 120.0;
    expect(same(runQualifier(hue, {0.0F, 1.0F, 0.0F, 1.0F}), coverage(1.0F)),
           "green is inside a window centered on 120 degrees");
    hue.hueCenter = 350.0;
    expect(same(runQualifier(hue, {1.0F, 0.0F, 0.0F, 1.0F}), coverage(1.0F)),
           "a hue window wraps past 360 degrees: red is 10 degrees from 350");
    HslQualifierSettings soft;
    soft.hueWidth = 40.0;
    soft.hueSoftness = 20.0;
    // Yellow is 60 degrees from red, 40 past the window's half width of 20: out; orange at 30
    // degrees is 10 past, half way up the 20 degree ramp.
    expect(same(runQualifier(soft, {1.0F, 0.5F, 0.0F, 1.0F}), coverage(0.5F)),
           "orange, 30 degrees from red, is half selected by a 40 wide window with 20 softness");
    expect(same(runQualifier(soft, {1.0F, 1.0F, 0.0F, 1.0F}), coverage(0.0F)),
           "yellow, beyond the softness, is not selected");
    HslQualifierSettings sat;
    sat.satLow = 0.5;
    sat.satSoftness = 1.0;
    expect(same(runQualifier(sat, {0.5F, 0.5F, 0.5F, 1.0F}), coverage(0.5F)),
           "gray is half selected by a saturation range from 0.5 with softness 1");
    sat.satSoftness = 0.0;
    expect(same(runQualifier(sat, {0.5F, 0.5F, 0.5F, 1.0F}), coverage(0.0F)),
           "a hard saturation range excludes gray");
    expect(same(runQualifier(sat, {1.0F, 0.0F, 0.0F, 1.0F}), coverage(1.0F)),
           "a saturated color is inside the range");
    HslQualifierSettings luma;
    luma.lumaHigh = 0.5;
    luma.lumaSoftness = 1.0;
    expect(same(runQualifier(luma, {1.0F, 1.0F, 1.0F, 1.0F}), coverage(0.5F)),
           "white is half selected by a luma range to 0.5 with softness 1");
    luma.lumaHigh = 4.0;
    expect(same(runQualifier(luma, {3.0F, 3.0F, 3.0F, 1.0F}), coverage(1.0F)),
           "an HDR value is inside a luma range that reaches it");
    HslQualifierSettings invert = hue;
    invert.invert = true;
    expect(same(runQualifier(invert, {1.0F, 0.0F, 0.0F, 1.0F}), coverage(0.0F)),
           "invert swaps the selection");
    expect(same(runQualifier(invert, {0.0F, 1.0F, 0.0F, 1.0F}), coverage(1.0F)),
           "invert selects what was outside");
    expect(same(runQualifier(HslQualifierSettings{}, premultiplied(0.3F, 0.6F, 0.2F, 0.5F)),
                coverage(0.5F)),
           "the default selects every color, weighted by the pixel's alpha");
    expect(same(runQualifier(HslQualifierSettings{}, {2.0F, 3.0F, 1.0F, 0.0F}), coverage(0.0F)),
           "a pixel with alpha 0 has no coverage");
    // Preparation refuses a value outside the document domain.
    auto bad = color_tool_test::hslQualifier(1, HslQualifierSettings{});
    bad.values[0].source = 400.0;
    expect(!prepare(bad), "a hue center beyond 360 degrees is refused");
    auto good = color_tool_test::hslQualifier(1, HslQualifierSettings{});
    expect(prepare(good) && !prepare(good)->identity,
           "the qualifier is never an identity: it changes the pixel into coverage");
}

void testImageBudgetAndCancellation() {
    const auto grade = prepare(color_tool_test::toneMap(1, {}));
    const auto input = image(8, 4, {2.0F, 1.0F, 0.5F, 1.0F});
    const auto result = detail::applyColorGrade(*grade, input, 1U << 22U, {},
                                                runtime::OperationIndex::fromRaw(0), {});
    expect(result.image != nullptr, "the pass applies to an image");
    if (result.image) {
        Pixel value{};
        if (const auto pixel = result.image->read(3, 2))
            std::ranges::copy(pixel.value()->components(), value.begin());
        expect(same(value, {static_cast<float>(reference(2.0, 0.8, 1.0)),
                            static_cast<float>(reference(1.0, 0.8, 1.0)), 0.5F, 1.0F}),
               "every pixel of the image is rolled off");
        expect(result.image->descriptor()->dataWindow() == input.descriptor()->dataWindow(),
               "the data window is kept");
    }
    const auto refused = detail::applyColorGrade(*grade, input, 4, {},
                                                 runtime::OperationIndex::fromRaw(0), {});
    expect(!refused.image && refused.diagnostic.has_value(),
           "a refused budget is an explicit error");

    const auto large = image(64, 64, {0.5F, 0.5F, 0.5F, 1.0F});
    runtime::TaskScheduler scheduler(runtime::TaskSchedulerConfig::defaults());
    const runtime::TaskOwner owner{.kind = runtime::TaskOwnerKind::Composition,
                                   .id = runtime::TaskOwnerId::fromRaw(518)};
    bool cancelled = false;
    auto submission = scheduler.submit<void>(
        runtime::TaskRequest("Tone Map cancellation", owner), [&](runtime::TaskContext& context) {
            static_cast<void>(scheduler.cancelOwner(owner));
            const auto cancelledResult =
                detail::applyColorGrade(*grade, large, 1U << 26U, context.cancellation(),
                                        runtime::OperationIndex::fromRaw(0), {});
            cancelled = cancelledResult.cancelled && !cancelledResult.image;
            return runtime::TaskResult<void>::cancelled();
        });
    expect(submission.accepted(), "the cancellation task is admitted");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    std::optional<runtime::TaskResult<void>> taken;
    while (!taken && std::chrono::steady_clock::now() < deadline) {
        taken = submission.handle.tryTakeResult();
        std::this_thread::yield();
    }
    expect(taken && taken->state() == runtime::TaskState::Cancelled && cancelled,
           "a cancelled pass publishes no image");
    scheduler.beginShutdown();
    while (!scheduler.isQuiescent() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
}

// BLOOM_COLOR_TOOL_ARTIFACT_DIR: a sheet of an HDR ramp (0 to 8 across 256 columns) and what Tone Map
// makes of it, one row each (display-clamped sRGB), and the values read at three columns.
void writeCapture() {
    const char* directory = std::getenv("BLOOM_COLOR_TOOL_ARTIFACT_DIR");
    if (directory == nullptr || *directory == '\0')
        return;
    constexpr std::uint32_t width = 256;
    const auto window = render::ImageWindow::create(0, 0, width, 1);
    auto descriptor = render::Rgba32fImageDescriptor::create(*window.value(), *window.value(),
                                                             core::PixelAspectRatio::square());
    auto builder = render::Rgba32fImageBuilder::create(*descriptor.value(), 1U << 20U);
    for (std::int64_t x = 0; x < width; ++x) {
        const float v = 8.0F * static_cast<float>(x) / 255.0F;
        const auto stored = render::Rgba32f::fromPremultiplied(v, v * 0.6F, v * 0.3F, 1.0F);
        if (!stored || builder.value()->write(x, 0, *stored.value()))
            throw std::logic_error("capture pixel writes");
    }
    auto frozen = std::move(*builder.value()).freeze();
    const render::Rgba32fImage ramp = std::move(*frozen.value());
    const auto apply = [&](const ToneMapSettings& settings) {
        const auto grade = prepare(color_tool_test::toneMap(1, settings));
        const auto result = detail::applyColorGrade(*grade, ramp, 1U << 20U, {},
                                                    runtime::OperationIndex::fromRaw(0), {});
        if (!result.image)
            throw std::logic_error("the capture pass applies");
        return result.image;
    };
    ToneMapSettings eachChannel;
    ToneMapSettings brightest;
    brightest.method = 1;
    ToneMapSettings shadows;
    shadows.knee = 100.0;
    shadows.ceiling = 200.0;
    shadows.shadows = 2.0;
    const auto first = apply(eachChannel);
    const auto second = apply(brightest);
    const auto third = apply(shadows);
    const std::array<std::pair<const char*, const render::Rgba32fImage*>, 4> rows{
        {{"HDR ramp 0..8 (raw)", &ramp},
         {"Tone Map, each channel", first.get()},
         {"Tone Map, brightest channel", second.get()},
         {"Shadow recovery 2, no roll-off", third.get()}}};
    const auto encode = [](const double linear) {
        const double v = std::clamp(linear, 0.0, 1.0);
        const double e = v <= 0.0031308 ? 12.92 * v : 1.055 * std::pow(v, 1.0 / 2.4) - 0.055;
        return static_cast<unsigned char>(std::lround(e * 255.0));
    };
    constexpr int rowHeight = 24;
    std::vector<unsigned char> rgb(static_cast<std::size_t>(width) * rowHeight * rows.size() * 3);
    for (std::size_t row = 0; row < rows.size(); ++row) {
        for (std::uint32_t x = 0; x < width; ++x) {
            Pixel p{};
            if (const auto pixel = rows[row].second->read(x, 0))
                std::ranges::copy(pixel.value()->components(), p.begin());
            for (int y = 0; y < rowHeight; ++y) {
                const auto offset =
                    ((row * rowHeight + static_cast<std::size_t>(y)) * width + x) * 3;
                for (std::size_t c = 0; c < 3; ++c)
                    rgb[offset + c] = encode(static_cast<double>(p[c]));
            }
        }
        const auto value = [&](const std::int64_t x) {
            Pixel p{};
            if (const auto pixel = rows[row].second->read(x, 0))
                std::ranges::copy(pixel.value()->components(), p.begin());
            return p;
        };
        const auto a = value(0);
        const auto b = value(128);
        const auto c = value(255);
        std::cout << rows[row].first << ": x0=" << a[0] << "," << a[1] << "," << a[2]
                  << " x128=" << b[0] << "," << b[1] << "," << b[2] << " x255=" << c[0] << ","
                  << c[1] << "," << c[2] << '\n';
    }
    std::ofstream out(std::string(directory) + "/tone-map-ramp.ppm", std::ios::binary);
    out << "P6\n" << width << ' ' << rowHeight * rows.size() << "\n255\n";
    out.write(reinterpret_cast<const char*>(rgb.data()), static_cast<std::streamsize>(rgb.size()));
}

} // namespace

int main() {
    try {
        testRollOff();
        testProperties();
        testBrightestChannel();
        testShadows();
        testGamut();
        testAlpha();
        testDomainAndIdentity();
        testMatrixKnownAnswers();
        testMatrixModels();
        testMatrixDomainAndIdentity();
        testQualifier();
        testImageBudgetAndCancellation();
        writeCapture();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    return failures == 0 ? 0 : 1;
}
