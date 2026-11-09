// #513 point tracking against known answers: a smooth texture shifted by known fractions of a pixel
// is followed to a small fraction of a pixel, a feature that leaves the picture, a blank pattern, a
// frame that cannot be read and a texture that changes all end the track with a reason, and a
// cancelled job stops without a result. The texture is analytic, so the position of every feature at
// every frame is known without reading it back from a picture.

#include <bloom/runtime/point_tracker.hpp>
#include <bloom/runtime/task_scheduler.hpp>

#include <chrono>
#include <cmath>
#include <iostream>
#include <numbers>
#include <optional>
#include <string>
#include <thread>

namespace {
using namespace bloom;
using runtime::PointTrackEnd;

int failures = 0;
void expect(const bool value, const std::string& message) {
    if (!value) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

constexpr std::uint32_t kWidth = 160;
constexpr std::uint32_t kHeight = 120;

// A band-limited texture with no repetition inside the grid.
double texture(const double x, const double y) {
    constexpr double tau = 2.0 * std::numbers::pi;
    constexpr double table[8][4] = {
        {0.071, 0.043, 0.0, 0.20}, {0.113, -0.097, 1.0, 0.15}, {0.031, 0.157, 2.0, 0.12},
        {0.190, 0.050, 0.5, 0.10}, {-0.137, 0.121, 3.1, 0.12}, {0.057, -0.211, 4.2, 0.10},
        {0.239, 0.173, 5.3, 0.07}, {-0.181, -0.067, 0.7, 0.09}};
    double value = 0.5;
    for (const auto& t : table)
        value += t[3] * std::sin(tau * (x * t[0] + y * t[1]) + t[2]);
    return value;
}

// The grid of frame `k`: the texture moved by (dx(k), dy(k)), sampled at pixel centres.
template <typename Move>
runtime::TrackLuma frameOf(const std::int64_t k, const Move& move) {
    runtime::TrackLuma grid;
    grid.width = kWidth;
    grid.height = kHeight;
    grid.values.resize(static_cast<std::size_t>(kWidth) * kHeight);
    const auto [dx, dy] = move(k);
    for (std::uint32_t j = 0; j < kHeight; ++j)
        for (std::uint32_t i = 0; i < kWidth; ++i)
            grid.values[static_cast<std::size_t>(j) * kWidth + i] = static_cast<float>(
                texture(static_cast<double>(i) + 0.5 - dx, static_cast<double>(j) + 0.5 - dy));
    return grid;
}

void testKnownMotion() {
    const auto move = [](const std::int64_t k) {
        const auto t = static_cast<double>(k);
        return std::pair{2.3 * t, -1.45 * t + 0.1 * t * t};
    };
    runtime::PointTrackSettings settings;
    settings.startX = 50.5;
    settings.startY = 60.5;
    settings.firstFrame = 3;
    settings.lastFrame = 13;
    settings.searchRadius = 8;
    std::uint64_t reported = 0;
    std::uint64_t reportedTotal = 0;
    const auto result = runtime::trackPoint(
        settings, [&](const std::int64_t f) { return std::optional{frameOf(f, move)}; }, {},
        [&](const std::uint64_t done, const std::uint64_t total) {
            reported = done;
            reportedTotal = total;
        });
    expect(result.end == PointTrackEnd::Finished && result.samples.size() == 11,
           "a smooth move is tracked to the last frame");
    expect(reported == 11 && reportedTotal == 11, "progress reaches the total");
    double worst = 0.0;
    for (const auto& sample : result.samples) {
        // The feature chosen on frame 3 is at (50.5, 60.5) there, and moved with the texture since.
        const auto [x3, y3] = move(3);
        const auto [xk, yk] = move(sample.frame);
        worst = std::max({worst, std::abs(sample.x - (50.5 + xk - x3)),
                          std::abs(sample.y - (60.5 + yk - y3))});
    }
    expect(worst < 0.1, "the tracked position is within a tenth of a pixel of the known one");
    expect(!result.samples.empty() && result.samples.front().score == 1.0 &&
               result.samples.front().x == 50.5,
           "the first frame is the feature as chosen");
}

void testEnds() {
    runtime::PointTrackSettings settings;
    settings.startX = 80.5;
    settings.startY = 60.5;
    settings.firstFrame = 0;
    settings.lastFrame = 30;
    const auto still = [](const std::int64_t k) { return std::pair{0.0 * static_cast<double>(k), 0.0}; };

    // Out of the picture: the pattern reaches the edge.
    {
        const auto fast = [](const std::int64_t k) { return std::pair{3.0 * static_cast<double>(k), 0.0}; };
        auto s = settings;
        s.searchRadius = 8;
        s.minimumScore = 0.85;
        const auto result = runtime::trackPoint(
            s, [&](const std::int64_t f) { return std::optional{frameOf(f, fast)}; }, {});
        expect(result.end == PointTrackEnd::Lost && result.lostFrame > 0 && !result.detail.empty() &&
                   result.samples.size() == static_cast<std::size_t>(result.lostFrame),
               "a feature that leaves the picture is lost, with the frame and the reason");
    }
    // A flat picture has nothing to follow.
    {
        const auto result = runtime::trackPoint(
            settings,
            [&](const std::int64_t) {
                runtime::TrackLuma flat{kWidth, kHeight,
                                        std::vector<float>(static_cast<std::size_t>(kWidth) * kHeight, 0.5F)};
                return std::optional{flat};
            },
            {});
        expect(result.end == PointTrackEnd::Lost && result.lostFrame == 0,
               "a pattern with no detail ends the track at the first frame");
    }
    // An unreadable frame.
    {
        const auto result = runtime::trackPoint(
            settings,
            [&](const std::int64_t f) -> std::optional<runtime::TrackLuma> {
                if (f == 4)
                    return std::nullopt;
                return frameOf(f, still);
            },
            {});
        expect(result.end == PointTrackEnd::Lost && result.lostFrame == 4 && result.samples.size() == 4,
               "a frame that cannot be read ends the track there");
    }
    // The texture changes completely: the correlation falls below the minimum.
    {
        const auto result = runtime::trackPoint(
            settings,
            [&](const std::int64_t f) {
                auto grid = frameOf(0, still);
                if (f >= 5)
                    for (std::size_t i = 0; i < grid.values.size(); ++i)
                        grid.values[i] = static_cast<float>(
                            0.5 + 0.4 * std::sin(static_cast<double>(i * i % 977)));
                return std::optional{grid};
            },
            {});
        expect(result.end == PointTrackEnd::Lost && result.lostFrame == 5,
               "a picture that stops resembling the feature loses it");
    }
    // Invalid settings.
    {
        auto s = settings;
        s.lastFrame = -1;
        const auto result = runtime::trackPoint(
            s, [&](const std::int64_t f) { return std::optional{frameOf(f, still)}; }, {});
        expect(result.end == PointTrackEnd::InvalidSettings && result.samples.empty(),
               "an empty range is refused");
    }
    // A still picture stays where it is, exactly.
    {
        const auto result = runtime::trackPoint(
            settings, [&](const std::int64_t f) { return std::optional{frameOf(f, still)}; }, {});
        double worst = 0.0;
        for (const auto& sample : result.samples)
            worst = std::max({worst, std::abs(sample.x - 80.5), std::abs(sample.y - 60.5)});
        expect(result.end == PointTrackEnd::Finished && worst < 1.0e-6, "a still picture does not drift");
    }
}

void testCancellation() {
    runtime::TaskScheduler scheduler(runtime::TaskSchedulerConfig::defaults());
    const runtime::TaskOwner owner{.kind = runtime::TaskOwnerKind::Composition,
                                   .id = runtime::TaskOwnerId::fromRaw(513)};
    const auto still = [](const std::int64_t k) { return std::pair{0.0 * static_cast<double>(k), 0.0}; };
    runtime::PointTrackSettings settings;
    settings.startX = 80.5;
    settings.startY = 60.5;
    settings.lastFrame = 1000;
    std::size_t read = 0;
    runtime::PointTrackEnd end = runtime::PointTrackEnd::Finished;
    auto submission = scheduler.submit<void>(
        runtime::TaskRequest("Track cancellation", owner), [&](runtime::TaskContext& context) {
            const auto result = runtime::trackPoint(
                settings,
                [&](const std::int64_t f) {
                    if (++read == 3)
                        static_cast<void>(scheduler.cancelOwner(owner));
                    return std::optional{frameOf(f, still)};
                },
                context.cancellation());
            end = result.end;
            return runtime::TaskResult<void>::cancelled();
        });
    expect(submission.accepted(), "the track task is admitted");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    std::optional<runtime::TaskResult<void>> result;
    while (!result && std::chrono::steady_clock::now() < deadline) {
        result = submission.handle.tryTakeResult();
        std::this_thread::yield();
    }
    expect(result && end == PointTrackEnd::Cancelled && read < 10,
           "a cancelled track stops within a frame or two");
    scheduler.beginShutdown();
    while (!scheduler.isQuiescent() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    expect(scheduler.isQuiescent(), "the cancelled track shuts down safely");
}

} // namespace

int main() {
    testKnownMotion();
    testEnds();
    testCancellation();
    if (failures != 0) {
        std::cerr << failures << " point tracker expectation(s) failed\n";
        return 1;
    }
    return 0;
}
