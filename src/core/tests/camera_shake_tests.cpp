#include <bloom/core/camera_shake.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <source_location>
#include <string_view>

namespace {

using namespace bloom::core::camera_shake;

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

bool close(const Sample& a, const Sample& b) {
    return std::abs(a.x - b.x) < 1.0e-9 && std::abs(a.y - b.y) < 1.0e-9 &&
           std::abs(a.roll - b.roll) < 1.0e-9;
}

constexpr Mode kModes[] = {Mode::Handheld, Mode::Impact, Mode::GateWeave};

void testSameSeedSameMotion(Expectations& e) {
    for (const auto mode : kModes) {
        const Settings a{mode, 7, 2.0, 0.0, 2, 0.35, 0.0};
        for (const double t : {0.0, 0.37, 1.0, 12.5, -3.2})
            e.expect(evaluate(a, t, 24.0) == evaluate(a, t, 24.0),
                     "the same settings and time give the same sample");
        const Settings b{mode, 8, 2.0, 0.0, 2, 0.35, 0.0};
        e.expect(!(evaluate(a, 1.3, 24.0) == evaluate(b, 1.3, 24.0)),
                 "another seed gives another shake");
    }
}

void testChannelsAreIndependent(Expectations& e) {
    const auto s = evaluate({Mode::Handheld, 3, 2.0, 0.0, 2, 0.35, 0.0}, 0.9, 24.0);
    e.expect(s.x != s.y && s.y != s.roll,
             "each channel has its own stream");
}

void testLoopClosesExactly(Expectations& e) {
    for (const auto mode : kModes) {
        for (const double loop : {2.0, 4.0, 1.5}) {
            const Settings settings{mode, 11, 2.0, loop, 2, 0.35, 0.0};
            for (const double offset : {0.0, 0.25, 0.7}) {
                const auto first = evaluate(settings, offset, 24.0);
                const auto again = evaluate(settings, offset + loop, 24.0);
                const auto twice = evaluate(settings, offset + 2.0 * loop, 24.0);
                e.expect(close(first, again), "the motion repeats after one loop length");
                e.expect(close(first, twice), "the motion repeats after two loop lengths");
                if (offset == 0.0)
                    e.expect(first == again, "frame zero equals the frame one loop length on");
            }
        }
    }
}

void testLoopedMotionIsContinuous(Expectations& e) {
    // Smooth modes have no jump at the loop point.
    for (const auto mode : {Mode::Handheld, Mode::Impact}) {
        const Settings settings{mode, 5, 2.0, 3.0, 2, 0.35, 0.0};
        const auto before = evaluate(settings, 3.0 - 1.0e-5, 24.0);
        const auto after = evaluate(settings, 3.0, 24.0);
        e.expect(std::abs(before.x - after.x) < 1.0e-3 && std::abs(before.y - after.y) < 1.0e-3,
                 "a looping shake is continuous at the loop point");
    }
}

void testHandheldIsSmoothAndBounded(Expectations& e) {
    const Settings settings{Mode::Handheld, 2, 2.0, 0.0, 2, 0.35, 0.0};
    double largest = 0.0;
    double previous = evaluate(settings, 0.0, 24.0).x;
    for (int step = 1; step <= 4800; ++step) {
        const double value = evaluate(settings, step * 0.01, 24.0).x;
        largest = std::max(largest, std::abs(value));
        e.expect(std::abs(value - previous) < 0.25, "handheld motion has no jump");
        previous = value;
    }
    e.expect(largest <= 1.0 + 1.0e-12, "handheld motion stays within the unit range");
    e.expect(largest > 0.4, "handheld motion uses the range");
}

void testImpactRingsOut(Expectations& e) {
    // A hit starts from rest, peaks near its amount, and has died away before the next.
    double largest = 0.0;
    for (std::int64_t seed = 1; seed <= 8; ++seed) {
        const Settings settings{Mode::Impact, seed, 1.0, 0.0, 2, 0.35, 0.0};
        e.expect(std::abs(evaluate(settings, 0.0, 24.0).x) < 0.01, "a hit starts from rest");
        e.expect(std::abs(evaluate(settings, 0.999, 24.0).x) < 0.02,
                 "a hit has died away before the next");
        for (int step = 0; step <= 100; ++step)
            largest = std::max(largest, std::abs(evaluate(settings, step * 0.01, 24.0).x));
    }
    e.expect(largest > 0.5 && largest <= 1.0, "a hit moves the image by about its amount");
}

void testGateWeaveChangesEveryFrame(Expectations& e) {
    const Settings settings{Mode::GateWeave, 6, 0.5, 0.0, 2, 0.35, 0.0};
    int different = 0;
    for (int frame = 0; frame < 24; ++frame)
        different += evaluate(settings, frame / 24.0, 24.0).y !=
                     evaluate(settings, (frame + 1) / 24.0, 24.0).y;
    e.expect(different == 24, "gate weave registers differently on every frame");
    // Within a frame only the slow drift moves.
    e.expect(std::abs(evaluate(settings, 3.0 / 24.0, 24.0).y -
                      evaluate(settings, 3.0 / 24.0 + 0.01, 24.0).y) < 0.01,
             "gate weave holds within a frame");
}

void testOctavesAndRoughness(Expectations& e) {
    Settings one{Mode::Handheld, 9, 2.0, 0.0, 1, 0.35, 0.0};
    Settings three = one;
    three.octaves = 3;
    Settings rough = three;
    rough.roughness = 0.9;
    e.expect(!(evaluate(one, 0.77, 24.0) == evaluate(three, 0.77, 24.0)), "octaves add detail");
    e.expect(!(evaluate(three, 0.77, 24.0) == evaluate(rough, 0.77, 24.0)),
             "roughness weighs the finer octaves");
    // Rougher noise changes faster.
    double smoothTravel = 0.0;
    double roughTravel = 0.0;
    for (int step = 0; step < 2000; ++step) {
        const double t = step * 0.005;
        smoothTravel += std::abs(evaluate(one, t + 0.005, 24.0).x - evaluate(one, t, 24.0).x);
        roughTravel += std::abs(evaluate(rough, t + 0.005, 24.0).x - evaluate(rough, t, 24.0).x);
    }
    e.expect(roughTravel > smoothTravel, "a rougher shake travels further");
    for (int step = 0; step < 4000; ++step)
        e.expect(std::abs(evaluate(rough, step * 0.0123, 24.0).x) <= 1.0 + 1.0e-12,
                 "fractal noise stays within the unit range");
}

void testSmoothingCalmsTheMotion(Expectations& e) {
    Settings sharp{Mode::GateWeave, 3, 2.0, 0.0, 2, 0.35, 0.0};
    Settings smooth = sharp;
    smooth.smoothing = 1.0;
    double sharpTravel = 0.0;
    double smoothTravel = 0.0;
    for (int frame = 0; frame < 480; ++frame) {
        sharpTravel += std::abs(evaluate(sharp, (frame + 1) / 24.0, 24.0).y -
                                evaluate(sharp, frame / 24.0, 24.0).y);
        smoothTravel += std::abs(evaluate(smooth, (frame + 1) / 24.0, 24.0).y -
                                 evaluate(smooth, frame / 24.0, 24.0).y);
    }
    e.expect(smoothTravel < sharpTravel, "smoothing reduces the frame-to-frame travel");
    smooth.loopSeconds = 2.0;
    e.expect(close(evaluate(smooth, 0.3, 24.0), evaluate(smooth, 2.3, 24.0)),
             "a smoothed loop still closes exactly");
}

// Whether the image (the frame scaled by `scale`, rolled by `roll` degrees clockwise and moved by
// (tx, ty)) covers every pixel centre of a width by height frame.
bool covers(const double scale, const double roll, const double tx, const double ty,
            const double width, const double height) {
    const double radians = roll * 3.14159265358979323846 / 180.0;
    const double c = std::cos(radians);
    const double s = std::sin(radians);
    for (double py = 0.5; py < height; py += (height - 1.0) / 8.0)
        for (double px = 0.5; px < width; px += (width - 1.0) / 8.0) {
            const double dx = px - width * 0.5 - tx;
            const double dy = py - height * 0.5 - ty;
            const double ux = (dx * c + dy * s) / scale;
            const double uy = (-dx * s + dy * c) / scale;
            if (std::abs(ux) > width * 0.5 || std::abs(uy) > height * 0.5)
                return false;
        }
    return true;
}

void testFillScale(Expectations& e) {
    e.expect(fillScale(0.0, 0.0, 0.0, 1920.0, 1080.0) == 1.0, "no motion needs no fill");
    const double scale = fillScale(10.0, 4.0, 0.5, 1920.0, 1080.0);
    e.expect(scale > 1.0 && scale < 1.1, "the fill scale grows with the amounts");
    e.expect(fillScale(20.0, 4.0, 0.5, 1920.0, 1080.0) > scale, "more X needs more fill");
    e.expect(fillScale(10.0, 14.0, 0.5, 1920.0, 1080.0) > scale, "more Y needs more fill");
    e.expect(fillScale(10.0, 4.0, 1.5, 1920.0, 1080.0) > scale, "more roll needs more fill");
    // At every extreme of the amounts the frame stays covered; and the scale is not wasteful: a
    // little less leaves an edge at some extreme.
    for (const double ax : {0.0, 12.0, 80.0})
        for (const double ay : {0.0, 7.0, 40.0})
            for (const double roll : {0.0, 0.5, 4.0, 20.0}) {
                const double need = fillScale(ax, ay, roll, 640.0, 360.0);
                bool leaks = false;
                bool tight = false;
                for (const double sx : {-1.0, 1.0})
                    for (const double sy : {-1.0, 1.0})
                        for (int step = 0; step <= 64; ++step) {
                            const double r = roll * (2.0 * step / 64.0 - 1.0);
                            leaks = leaks ||
                                    !covers(need, r, sx * ax, sy * ay, 640.0, 360.0);
                            tight = tight ||
                                    !covers(need * 0.97, r, sx * ax, sy * ay, 640.0, 360.0);
                        }
                e.expect(!leaks, "the fill scale covers the frame at the maximum shake");
                if (need > 1.05)
                    e.expect(tight, "the fill scale is no larger than needed");
            }
}

} // namespace

int main() {
    Expectations expectations;
    testSameSeedSameMotion(expectations);
    testChannelsAreIndependent(expectations);
    testLoopClosesExactly(expectations);
    testLoopedMotionIsContinuous(expectations);
    testHandheldIsSmoothAndBounded(expectations);
    testImpactRingsOut(expectations);
    testGateWeaveChangesEveryFrame(expectations);
    testOctavesAndRoughness(expectations);
    testSmoothingCalmsTheMotion(expectations);
    testFillScale(expectations);
    return expectations.failures() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
