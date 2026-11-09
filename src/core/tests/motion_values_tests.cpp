#include <bloom/core/motion_values.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <source_location>
#include <string_view>

namespace {

using namespace bloom::core::motion_values;

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

bool near(const double a, const double b, const double tolerance = 1e-12) {
    return std::abs(a - b) <= tolerance;
}

void testOscillator(Expectations& e) {
    OscillatorSettings s;
    s.frequency = 2.0;
    s.amplitude = 3.0;
    s.offset = 10.0;
    // Sine at 2 Hz: a quarter cycle is 0.125 s.
    e.expect(near(oscillate(s, 0.0), 10.0), "a sine starts at its offset");
    e.expect(near(oscillate(s, 0.125), 13.0), "a sine peaks at a quarter cycle");
    e.expect(near(oscillate(s, 0.375), 7.0), "and troughs at three quarters");
    e.expect(near(oscillate(s, 0.5), 10.0, 1e-9), "and returns to the offset after a cycle");
    s.phase = 90.0;
    e.expect(near(oscillate(s, 0.0), 13.0), "a phase of 90 degrees starts at the peak");
    s.phase = 0.0;
    s.wave = Wave::Triangle;
    e.expect(near(oscillate(s, 0.125), 13.0) && near(oscillate(s, 0.25), 10.0) &&
                 near(oscillate(s, 0.375), 7.0) && near(oscillate(s, 0.0625), 11.5),
             "a triangle is 0, 1, 0, -1 at the quarter points and straight between");
    s.wave = Wave::Square;
    e.expect(near(oscillate(s, 0.1), 13.0) && near(oscillate(s, 0.3), 7.0),
             "a square holds the top then the bottom");
    s.wave = Wave::Sawtooth;
    e.expect(near(oscillate(s, 0.0), 7.0) && near(oscillate(s, 0.25), 10.0) &&
                 near(oscillate(s, 0.4999), 13.0, 2e-3),
             "a sawtooth rises from the bottom to the top");
    s.wave = Wave::Sine;
    e.expect(near(oscillate(s, -0.125), 7.0), "a negative time is the same function");
    s.frequency = -2.0;
    e.expect(near(oscillate(s, 0.125), 7.0), "a negative frequency runs backwards");
    s.frequency = std::numeric_limits<double>::quiet_NaN();
    e.expect(std::isfinite(oscillate(s, 0.3)), "a non-finite setting takes its default");
    e.expect(std::isfinite(oscillate(OscillatorSettings{}, std::numeric_limits<double>::infinity())),
             "a non-finite time is zero");
    // A large time stays accurate: whole cycles drop out before the sine.
    OscillatorSettings big;
    big.frequency = 1.0;
    e.expect(near(oscillate(big, 1.0e9 + 0.25), 1.0, 1e-6), "a billion seconds in is still a quarter cycle");
}

void testWiggle(Expectations& e) {
    WiggleSettings a;
    a.seed = 5;
    a.amplitude = 2.0;
    a.offset = 1.0;
    WiggleSettings b = a;
    b.seed = 6;
    bool differs = false;
    double low = 1e9;
    double high = -1e9;
    for (int i = 0; i < 2000; ++i) {
        const double t = i * 0.0137 - 5.0;
        const double v = wiggle(a, t);
        e.expect(v == wiggle(a, t), "the same settings and time repeat");
        low = std::min(low, v);
        high = std::max(high, v);
        differs = differs || v != wiggle(b, t);
    }
    e.expect(differs, "another seed gives another wiggle");
    e.expect(low >= -1.0 - 1e-12 && high <= 3.0 + 1e-12, "the wiggle stays within offset +- amplitude");
    e.expect(low < 0.0 && high > 2.0, "and uses most of that range");
    const double forward = wiggle(a, 3.3);
    (void)wiggle(a, 90.0);
    e.expect(wiggle(a, 3.3) == forward, "a later query does not change an earlier time");
    // Smooth: a tiny step in time moves the value a tiny amount, including across a lattice point.
    double largest = 0.0;
    for (int i = 0; i < 4000; ++i)
        largest = std::max(largest, std::abs(wiggle(a, i * 0.001) - wiggle(a, i * 0.001 + 1e-6)));
    e.expect(largest < 1e-4, "the wiggle is continuous");
    // The lattice value itself: at an integer lattice time the value is the lattice level, so two
    // evaluations one lattice period apart are independent but each is repeatable.
    a.octaves = 4;
    a.roughness = 0.5;
    double lowFractal = 1e9;
    double highFractal = -1e9;
    for (int i = 0; i < 4000; ++i) {
        const double v = wiggle(a, i * 0.0173);
        lowFractal = std::min(lowFractal, v);
        highFractal = std::max(highFractal, v);
    }
    e.expect(lowFractal >= -1.0 - 1e-12 && highFractal <= 3.0 + 1e-12,
             "octaves are normalized into the same range");
    a.amplitude = 0.0;
    e.expect(near(wiggle(a, 1.234), 1.0), "amplitude zero is the offset");
    a.octaves = 100;
    e.expect(std::isfinite(wiggle(a, 1.0)), "octaves are limited");
    e.expect(kNoiseVersion == 1, "the noise definition is version 1");
}

void testLoop(Expectations& e) {
    LoopSettings s;
    s.start = 10.0;
    s.end = 20.0;
    s.duration = 2.0;
    e.expect(near(loop(s, 0.0).value, 10.0) && loop(s, 0.0).cycle == 0, "a loop starts at its start");
    e.expect(near(loop(s, 1.0).value, 15.0), "and is halfway halfway through");
    e.expect(near(loop(s, 2.0).value, 10.0) && loop(s, 2.0).cycle == 1, "and repeats");
    e.expect(near(loop(s, 5.0).value, 15.0) && loop(s, 5.0).cycle == 2, "forever");
    e.expect(near(loop(s, -0.5).value, 17.5) && loop(s, -0.5).cycle == -1, "and backwards");
    s.mode = LoopMode::PingPong;
    e.expect(near(loop(s, 1.0).value, 15.0), "a ping-pong goes up");
    e.expect(near(loop(s, 2.5).value, 17.5) && loop(s, 2.5).cycle == 1, "then comes back");
    e.expect(near(loop(s, 3.0).value, 15.0), "through the middle");
    e.expect(near(loop(s, 4.0).value, 10.0) && loop(s, 4.0).cycle == 2, "to the start");
    s.start = 20.0;
    s.end = 10.0;
    s.mode = LoopMode::Repeat;
    e.expect(near(loop(s, 0.5).value, 17.5), "an end below the start runs downward");
    s.duration = 0.0;
    e.expect(std::isfinite(loop(s, 1.0).value), "a duration of zero is limited");
}

} // namespace

int main() {
    Expectations e;
    testOscillator(e);
    testWiggle(e);
    testLoop(e);
    if (e.failures() != 0)
        return EXIT_FAILURE;
    std::cout << "motion values tests passed\n";
    return EXIT_SUCCESS;
}
