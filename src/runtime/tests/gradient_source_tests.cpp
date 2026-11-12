#include "gradient_source.hpp"

#include <bloom/document/graph.hpp>
#include <bloom/document/node_definition_registry.hpp>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <vector>

namespace {
using bloom::runtime::detail::gradientTintAt;
using bloom::runtime::detail::ResolvedGradientStop;
using bloom::runtime::detail::resolveGradientStops;

bool near(const float actual, const float expected) {
    return std::fabs(actual - expected) < 0.00001F;
}

std::vector<ResolvedGradientStop> pair(const bloom::core::Color4d first,
                                       const bloom::core::Color4d second) {
    return resolveGradientStops(bloom::document::GradientValue{{{0.0, first}, {1.0, second}}});
}
} // namespace

int main() {
    using bloom::runtime::detail::ResolvedGradient;
    using bloom::runtime::detail::gradientPixel;
    using namespace bloom::document;
    const auto& definitions = builtInNodeDefinitions();
    const auto* linear = definitions.find(kLinearGradientNodeType, kGradientNodeSchemaVersion);
    const auto* radial = definitions.find(kRadialGradientNodeType, kGradientNodeSchemaVersion);
    if (!linear || !radial || linear->lowering != NodeLoweringKind::Gradient ||
        radial->lowering != NodeLoweringKind::Gradient || linear->parameters.size() != 6 ||
        radial->parameters.size() != 6) {
        std::cerr << "gradient node schemas are not registered\n";
        return 1;
    }
    ResolvedGradient gradient{pair({-1, 0, 2, 1}, {3, 0, -2, 0}), {0, 0}, {4, 0}, 0, false, 1};
    const auto first = gradientPixel(gradient, 0.5, 0.5);
    const auto last = gradientPixel(gradient, 3.5, 0.5);
    if (!first || !last || !near(first.value()->red(), -0.4375F) ||
        !near(first.value()->alpha(), 0.875F) || !near(last.value()->red(), 0.3125F) ||
        !near(last.value()->alpha(), 0.125F)) {
        std::cerr << "scene-linear RGB and alpha interpolation changed\n";
        return 1;
    }
    gradient.stops = pair({0, 0, 0, 1}, {1, 1, 1, 1});
    gradient.repeat = 1;
    const auto repeated = gradientPixel(gradient, 5.0, 0);
    gradient.repeat = 2;
    const auto mirrored = gradientPixel(gradient, 5.0, 0);
    if (!repeated || !mirrored || !near(repeated.value()->red(), 0.25F) ||
        !near(mirrored.value()->red(), 0.75F)) {
        std::cerr << "gradient repeat modes changed\n";
        return 1;
    }
    gradient.repeat = 0;
    gradient.radial = true;
    gradient.aspect = 2;
    const auto aspect = gradientPixel(gradient, 0, 4);
    if (!aspect || !near(aspect.value()->red(), 0.5F)) {
        std::cerr << "radial pixel-aspect geometry changed\n";
        return 1;
    }
    gradient.end = gradient.start;
    const auto degenerate = gradientPixel(gradient, 100, 100);
    if (!degenerate || !near(degenerate.value()->red(), 0)) {
        std::cerr << "degenerate gradient must choose first color\n";
        return 1;
    }
    gradient.aspect = 1;
    for (const bool radialMode : {false, true}) {
        gradient.radial = radialMode;
        gradient.start = {-1e200, 0};
        gradient.end = {1e200, 0};
        const auto large = gradientPixel(gradient, 0.5, 0.5);
        gradient.start = {0, 0};
        gradient.end = {1e-200, 0};
        const auto tiny = gradientPixel(gradient, 0.5, 0.5);
        if (!large || !tiny || !near(large.value()->red(), 0.5F) ||
            !near(tiny.value()->red(), 1.0F)) {
            std::cerr << "nonzero extreme gradient spans must not become degenerate\n";
            return 1;
        }
    }
    // Authoring colours are Float64. Only the sampled, premultiplied process pixel
    // must fit Float32; neither unused endpoint is itself a process pixel.
    gradient = {pair({1e39, -1e39, 0, 1}, {-1e39, 1e39, 0, 1}), {0, 0}, {1, 0}, 0, false, 1};
    const auto hdrMidpoint = gradientPixel(gradient, 0.5, 0.5);
    if (!hdrMidpoint || !near(hdrMidpoint.value()->red(), 0.0F) ||
        !near(hdrMidpoint.value()->green(), 0.0F) ||
        !near(hdrMidpoint.value()->alpha(), 1.0F)) {
        std::cerr << "finite HDR gradient samples must survive oversized endpoints\n";
        return 1;
    }
    gradient = {pair({0, 0, 0, 1}, {1, 1, 1, 1}), {100000000, 0}, {100000008, 0}, 1, false, 1};
    const auto distantRepeat = gradientPixel(gradient, 0.5, 0.5);
    gradient.repeat = 2;
    const auto distantMirror = gradientPixel(gradient, 0.5, 0.5);
    if (!distantRepeat || !distantMirror ||
        !near(distantRepeat.value()->red(), 0.0625F) ||
        !near(distantMirror.value()->red(), 0.0625F)) {
        std::cerr << "distant repeat and mirror must preserve fractional pixel phase\n";
        return 1;
    }
    // #541: a multi-stop table. Stops sort by position, the colour holds before the first and from
    // the last, and two stops at one position make a hard edge.
    const bloom::document::GradientValue stops{{{0.75, {0, 0, 1, 1}},
                                         {0.25, {1, 0, 0, 1}},
                                         {0.5, {0, 1, 0, 0}},
                                         {0.5, {0, 0, 0, 1}}}};
    gradient = {resolveGradientStops(stops), {0, 0}, {8, 0}, 0, false, 1};
    const auto at = [&](const double x) { return gradientPixel(gradient, x, 0.5); };
    const auto before = at(0.5);     // t = 1/16, before the first stop
    const auto between = at(2.5);    // t = 5/16, a quarter from red to green
    const auto hardEdge = at(4.5);   // t = 9/16, past the edge, from black towards blue
    const auto after = at(7.5);      // t = 15/16, after the last stop
    if (!before || !between || !hardEdge || !after || !near(before.value()->red(), 1.0F) ||
        !near(between.value()->alpha(), 0.75F) ||
        !near(between.value()->red(), 0.75F * 0.75F) ||
        !near(between.value()->green(), 0.25F * 0.75F) ||
        !near(hardEdge.value()->alpha(), 1.0F) || !near(hardEdge.value()->blue(), 0.25F) ||
        !near(hardEdge.value()->green(), 0.0F) || !near(after.value()->blue(), 1.0F)) {
        std::cerr << "multi-stop gradients sort, hold and make hard edges\n";
        return 1;
    }
    // A two-stop table at 0 and 1 gives schema 1's start/end pixels bit for bit: (1 - t) * from +
    // t * to over the same Float32 phase, in native GradientV1's Float32 sequence (#676: two rounded
    // products and one sum per component, then one rounded product with alpha).
    {
        const bloom::core::Color4d from{-0.25, 2.0, 0.5, 0.75};
        const bloom::core::Color4d to{1.0, -0.5, 0.25, 0.25};
        ResolvedGradient two{pair(from, to), {0, 0}, {7, 0}, 0, false, 1};
        bool same = true;
        for (int column = -1; column <= 8; ++column) {
            const double x = column + 0.5;
            const auto phase = static_cast<float>(std::clamp(x / 7.0, 0.0, 1.0));
            const float remaining = 1.0F - phase;
            const float fromAlpha = static_cast<float>(from.alpha) * remaining;
            const float toAlpha = static_cast<float>(to.alpha) * phase;
            const float alpha = fromAlpha + toAlpha;
            const auto channel = [&](const double a, const double b) {
                const float first = static_cast<float>(a) * remaining;
                const float second = static_cast<float>(b) * phase;
                const float straight = first + second;
                return straight * alpha;
            };
            const auto old = bloom::render::Rgba32f::fromPremultiplied(
                channel(from.red, to.red), channel(from.green, to.green),
                channel(from.blue, to.blue), alpha);
            const auto now = gradientPixel(two, x, 0.5);
            same = same && old && now && old.value()->red() == now.value()->red() &&
                   old.value()->green() == now.value()->green() &&
                   old.value()->blue() == now.value()->blue() &&
                   old.value()->alpha() == now.value()->alpha();
        }
        if (!same) {
            std::cerr << "a two-stop gradient no longer matches the schema 1 colour pair\n";
            return 1;
        }
    }
    // Glow's octave tint: a two-stop gradient at 0 and 1 is exactly tint + (outer - tint) * t.
    const std::vector<bloom::document::GradientStop> tints{{0.0, {1.0, 1.0, 1.0, 1.0}},
                                                    {1.0, {1.0, 0.5, 0.25, 1.0}}};
    for (const double t : {0.0, 0.25, 1.0 / 3.0, 0.75, 1.0}) {
        const auto tint = gradientTintAt(tints, t);
        if (tint.green != std::fma(0.5 - 1.0, t, 1.0) || tint.blue != std::fma(0.25 - 1.0, t, 1.0)) {
            std::cerr << "the octave tint is not the schema 1 tint ramp\n";
            return 1;
        }
    }
    return 0;
}
