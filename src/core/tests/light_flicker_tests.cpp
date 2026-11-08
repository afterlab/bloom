#include <bloom/core/light_flicker.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <source_location>
#include <string_view>

namespace {

using namespace bloom::core::light_flicker;

class Expectations final {
  public:
    void expect(const bool condition, const std::string_view message,
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

void testPureAndSeeded(Expectations& e) {
    Settings a;
    a.seed = 5;
    Settings b = a;
    b.seed = 6;
    bool differs = false;
    for (const double t : {0.0, 0.31, 1.7, 9.2, -4.4}) {
        e.expect(evaluate(a, t) == evaluate(a, t), "the same settings and time repeat");
        differs = differs || evaluate(a, t) != evaluate(b, t);
    }
    e.expect(differs, "another seed gives another flicker");
    // Order of evaluation does not matter: there is no state.
    const double forward = evaluate(a, 3.3);
    (void)evaluate(a, 8.0);
    e.expect(evaluate(a, 3.3) == forward, "a later query does not change an earlier time");
}

void testRange(Expectations& e) {
    Settings s;
    s.depth = 0.6;
    s.dropoutRate = 3.0;
    s.dropoutLevel = 0.2;
    for (int i = 0; i < 4000; ++i) {
        const double g = evaluate(s, i * 0.0137);
        e.expect(g >= 0.0 && g <= 1.0, "the gain stays in [0, 1]");
        e.expect(g >= (1.0 - 0.6) * 0.2 - 1.0e-12, "the gain never falls below depth and dropout floor");
    }
}

void testNoDepthNoDropoutIsSteady(Expectations& e) {
    Settings s;
    s.depth = 0.0;
    s.dropoutRate = 0.0;
    for (const double t : {0.0, 1.1, 77.7})
        e.expect(evaluate(s, t) == 1.0, "no depth and no dropouts is a steady light");
}

void testDropoutsHappenAndReachBelowFloor(Expectations& e) {
    Settings s;
    s.depth = 0.0;
    s.dropoutRate = 4.0;
    s.dropoutLength = 0.2;
    s.dropoutLevel = 0.0;
    s.softness = 0.0;
    double lowest = 1.0;
    int dark = 0;
    for (int i = 0; i < 20000; ++i) {
        const double g = evaluate(s, i * 0.001);
        lowest = std::min(lowest, g);
        dark += g == 0.0 ? 1 : 0;
    }
    e.expect(lowest == 0.0, "a hard dropout to level 0 is black");
    // About 4 per second, each lasting 0.15 s on average, over 20 s: roughly 12 s dark of 20.
    // Only a loose bound: it must happen, and must not be all of the time.
    e.expect(dark > 500 && dark < 19000, "dropouts cover part of the time");
}

void testSoftEdges(Expectations& e) {
    Settings s;
    s.depth = 0.0;
    s.dropoutRate = 2.0;
    s.dropoutLength = 0.4;
    s.dropoutLevel = 0.0;
    s.softness = 1.0;
    bool intermediate = false;
    for (int i = 0; i < 20000 && !intermediate; ++i) {
        const double g = evaluate(s, i * 0.001);
        intermediate = g > 0.05 && g < 0.95;
    }
    e.expect(intermediate, "soft dropouts pass through intermediate levels");
}

void testLongDropoutsSurviveSlotBoundaries(Expectations& e) {
    // A dropout of up to two seconds with a rate of ten a second spans many slots.
    Settings s;
    s.depth = 0.0;
    s.dropoutRate = 10.0;
    s.dropoutLength = 2.0;
    s.dropoutLevel = 0.0;
    s.softness = 0.0;
    int dark = 0;
    for (int i = 0; i < 5000; ++i)
        dark += evaluate(s, i * 0.002) == 0.0 ? 1 : 0;
    e.expect(dark > 2500, "long overlapping dropouts keep the light dark");
}

void testNonFiniteAndOutOfDomain(Expectations& e) {
    Settings s;
    s.rate = std::numeric_limits<double>::quiet_NaN();
    s.depth = 7.0;
    s.dropoutRate = -3.0;
    const double g = evaluate(s, std::numeric_limits<double>::infinity());
    e.expect(std::isfinite(g) && g >= 0.0 && g <= 1.0, "bad settings and time still answer a gain");
}

void testStops(Expectations& e) {
    e.expect(stopsOf(1.0) == 0.0, "a steady light is zero stops");
    e.expect(std::abs(stopsOf(0.5) + 1.0) < 1.0e-12, "half is minus one stop");
    e.expect(stopsOf(0.0) == kLowestStops, "black is limited to the lowest stops");
    e.expect(stopsOf(-1.0) == kLowestStops, "a negative gain is limited");
}

} // namespace

int main() {
    Expectations e;
    testPureAndSeeded(e);
    testRange(e);
    testNoDepthNoDropoutIsSteady(e);
    testDropoutsHappenAndReachBelowFloor(e);
    testSoftEdges(e);
    testLongDropoutsSurviveSlotBoundaries(e);
    testNonFiniteAndOutOfDomain(e);
    testStops(e);
    return e.failures() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
