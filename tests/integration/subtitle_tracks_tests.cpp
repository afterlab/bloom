// #752: the export track pass (runtime::collectSubtitleTracks) against the picture.
//
// The fixture (subtitle_tracks_fixture.hpp) reaches the output through an offset layer, a trimmed
// layer with Time Remap, and a nested composition at twice the speed. Over an export range that
// does not start at frame zero, the pass's tracks must:
//
// - hold exactly the cues worked out by hand, on the output's timeline (frame - range start);
// - agree with the CPU-rendered picture on every frame: each node draws in its own band (Top,
//   Center, Bottom), and a band holds pixels on exactly the frames its track has a cue active, with
//   the same pixels whenever the same cue is active and different pixels for different cues;
// - leave out a node with Export as track off, honor an explicit node list, note a named node that
//   never reaches the output, report progress for every frame, and stop when cancelled.

#include "subtitle_tracks_fixture.hpp"

#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/subtitle_tracks.hpp>
#include <bloom/runtime/task_scheduler.hpp>

#include <array>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <thread>

namespace {

using namespace bloom;
using namespace bloom::test::subtitle_tracks;

int failures = 0;
void expect(const bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

constexpr std::uint64_t kFirst = 6;
constexpr std::uint64_t kLast = 90;

std::shared_ptr<const runtime::CompiledCompositionPlan> compile(const Fixture& fixture) {
    runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
    const auto compiled = compiler.compile({fixture.document.snapshot(), fixture.main}, {});
    if (!compiled.plan)
        throw std::runtime_error(
            "the fixture does not compile: " +
            runtime::describeCompileFailure(compiled, fixture.document.snapshot().project(),
                                            fixture.main));
    return compiled.plan;
}

runtime::SubtitleTracks collect(const Fixture& fixture,
                                std::optional<std::vector<document::NodeId>> nodes = std::nullopt,
                                std::uint64_t* reported = nullptr) {
    const auto outcome = runtime::collectSubtitleTracks(
        {compile(fixture), kFirst, kLast, std::move(nodes)}, fixture.document.snapshot().project(),
        runtime::CancellationToken{},
        [reported](const std::uint64_t completed, const std::uint64_t total) {
            if (reported != nullptr && total == kLast - kFirst + 1)
                *reported = completed;
        });
    if (const auto* failed = std::get_if<runtime::SubtitleTrackFailure>(&outcome))
        throw std::runtime_error("the track pass failed: " + failed->message);
    return std::get<runtime::SubtitleTracks>(outcome);
}

const runtime::SubtitleTrack* trackOf(const runtime::SubtitleTracks& tracks,
                                      const document::NodeId node) {
    for (const auto& track : tracks.tracks)
        if (track.node == node)
            return &track;
    return nullptr;
}

// The track's cues in composition frames, with their plain text.
std::vector<ExpectedCue> framesOf(const runtime::SubtitleTrack& track) {
    std::vector<ExpectedCue> out;
    for (std::size_t index = 0; index < track.cues.cues.size(); ++index) {
        const auto& cue = *track.cues.cues[index];
        const auto frame = [](const core::RationalTime time) {
            // Output time is (frame - first) / 24, exactly.
            const auto frames = time.numerator() * kRate;
            if (time.numerator() < 0 || frames % time.denominator() != 0)
                throw std::runtime_error("a track cue edge is not a whole frame");
            return static_cast<std::uint64_t>(frames / time.denominator()) + kFirst;
        };
        out.push_back({frame(cue.start), frame(cue.end), track.plainText[index]});
    }
    return out;
}

std::string describe(const std::vector<ExpectedCue>& cues) {
    std::string out;
    for (const auto& cue : cues)
        out += " [" + std::to_string(cue.first) + "," + std::to_string(cue.end) + ") " + cue.text;
    return out;
}

bool same(const std::vector<ExpectedCue>& a, const std::vector<ExpectedCue>& b) {
    if (a.size() != b.size())
        return false;
    for (std::size_t index = 0; index < a.size(); ++index)
        if (a[index].first != b[index].first || a[index].end != b[index].end ||
            a[index].text != b[index].text)
            return false;
    return true;
}

// The alpha the CPU picture holds in rows [top, bottom) and the middle columns, as a signature.
double bandSignature(const runtime::ProcessFrame& frame, const std::int64_t top,
                     const std::int64_t bottom) {
    const auto& image = frame.processImage();
    const auto window = image.descriptor()->dataWindow();
    const auto width = static_cast<std::int64_t>(window.extent().width());
    double sum = 0.0;
    for (std::int64_t y = top; y < bottom; ++y)
        for (std::int64_t x = 64; x < 192; ++x) {
            if (x < window.originX() || y < window.originY() || x >= window.maxXExclusive() ||
                y >= window.maxYExclusive())
                continue;
            const auto& pixel = image.pixels()[static_cast<std::size_t>(
                (y - window.originY()) * width + (x - window.originX()))];
            sum += static_cast<double>(pixel.alpha()) * static_cast<double>(1 + (x % 7) + (y % 5));
        }
    return sum;
}

const std::string* activeAt(const std::vector<ExpectedCue>& cues, const std::uint64_t frame) {
    for (const auto& cue : cues)
        if (cue.first <= frame && frame < cue.end)
            return &cue.text;
    return nullptr;
}

void testTracksMatchThePicture() {
    const Fixture fixture;
    std::uint64_t reported = 0;
    const auto tracks = collect(fixture, std::nullopt, &reported);
    expect(reported == kLast - kFirst + 1, "progress reaches every frame of the range");
    expect(tracks.tracks.size() == 3, "three nodes export a track (Delta has Export as track off)");
    expect(trackOf(tracks, fixture.delta) == nullptr,
           "a node with Export as track off exports none");
    const auto* alpha = trackOf(tracks, fixture.alpha);
    const auto* bravo = trackOf(tracks, fixture.bravo);
    const auto* charlie = trackOf(tracks, fixture.charlie);
    expect(alpha && bravo && charlie, "each exported node has its track");
    if (!alpha || !bravo || !charlie)
        return;
    const auto a = framesOf(*alpha), b = framesOf(*bravo), c = framesOf(*charlie);
    expect(same(a, Fixture::expectedAlpha()), "the offset layer's cues:" + describe(a));
    expect(same(b, Fixture::expectedBravo()), "the trimmed, remapped layer's cues:" + describe(b));
    expect(same(c, Fixture::expectedCharlie()), "the nested layer's cues:" + describe(c));
    expect(alpha->language == "en" && alpha->title == "Alpha" && bravo->language == "fr" &&
               charlie->language == "en",
           "language and title come from the node, else the list");
    expect(alpha->cues.problem() == std::nullopt && bravo->cues.problem() == std::nullopt &&
               charlie->cues.problem() == std::nullopt,
           "each track is a valid cue list");

    // Every frame of the range against the CPU picture.
    const auto plan = compile(fixture);
    const runtime::CpuCompositionEvaluator evaluator;
    struct Band {
        const char* name;
        std::int64_t top, bottom;
        const std::vector<ExpectedCue>* cues;
        std::map<std::string, double> seen;
    };
    std::array<Band, 3> bands{Band{"Alpha", 0, 40, &a, {}}, Band{"Bravo", 52, 92, &b, {}},
                              Band{"Charlie", 104, 144, &c, {}}};
    for (auto frame = kFirst; frame <= kLast; ++frame) {
        runtime::EvaluationRequest request{.time = at(static_cast<std::int64_t>(frame), kRate),
                                           .output = plan->output(),
                                           .resolution = runtime::CompositionFormatResolution{},
                                           .quality = runtime::EvaluationQuality::Reference,
                                           .colorIntent =
                                               runtime::EvaluationColorIntent::LinearRec709Scene,
                                           .pixelStorageByteLimit = 256ULL << 20U};
        request.bypassOperationCache = true;
        const auto result = evaluator.evaluate(plan, request, {});
        expect(result.frame() != nullptr, "frame " + std::to_string(frame) + " evaluates");
        if (!result.frame())
            continue;
        for (auto& band : bands) {
            const auto signature = bandSignature(*result.frame(), band.top, band.bottom);
            const auto* text = activeAt(*band.cues, frame);
            const auto where = std::string(band.name) + " at frame " + std::to_string(frame);
            if (text == nullptr) {
                expect(signature == 0.0, where + ": the picture shows a cue the track does not");
                continue;
            }
            expect(signature > 0.0, where + ": the track has a cue the picture does not show");
            const auto [found, inserted] = band.seen.emplace(*text, signature);
            expect(inserted || found->second == signature,
                   where + ": the picture shows another cue than the track's \"" + *text + "\"");
        }
    }
    for (const auto& band : bands) {
        expect(band.seen.size() == band.cues->size(),
               std::string(band.name) + ": every track cue was seen in the picture");
        for (const auto& [text, signature] : band.seen)
            for (const auto& [other, otherSignature] : band.seen)
                expect(text == other || signature != otherSignature,
                       std::string(band.name) + ": two cues draw the same pixels");
    }
}

void testNodeList() {
    const Fixture fixture;
    const auto chosen = collect(fixture, std::vector<document::NodeId>{fixture.charlie});
    expect(chosen.tracks.size() == 1 && chosen.tracks[0].node == fixture.charlie,
           "an explicit node list exports exactly those nodes");
    // A node with Export as track off is exported when named.
    const auto named = collect(fixture, std::vector<document::NodeId>{fixture.delta});
    expect(named.tracks.size() == 1 && named.tracks[0].node == fixture.delta,
           "a named node exports even with Export as track off");
    const auto missing = collect(
        fixture, std::vector<document::NodeId>{fixture.alpha, document::NodeId::fromRaw(999999)});
    expect(missing.tracks.size() == 1 && !missing.notes.empty(),
           "a named node that never reaches the output is a note, not a track");
}

void testCancellation() {
    const Fixture fixture;
    runtime::TaskSchedulerConfig config = runtime::TaskSchedulerConfig::defaults();
    config.cpuWorkerCount = 1;
    config.blockingIoWorkerCount = 1;
    runtime::TaskScheduler scheduler(config);
    const auto plan = compile(fixture);
    std::mutex mutex;
    std::condition_variable condition;
    bool started = false;
    auto submission = scheduler.submit<bool>(
        runtime::TaskRequest("subtitle track cancel", {.kind = runtime::TaskOwnerKind::Composition,
                                                       .id = runtime::TaskOwnerId::fromRaw(752)}),
        [&](runtime::TaskContext& context) {
            const auto outcome = runtime::collectSubtitleTracks(
                {plan, 0, 95, std::nullopt}, fixture.document.snapshot().project(),
                context.cancellation(), [&](std::uint64_t, std::uint64_t) {
                    {
                        std::lock_guard lock(mutex);
                        started = true;
                    }
                    condition.notify_all();
                    // Hold each frame until the cancel lands.
                    while (!context.isCancellationRequested())
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                });
            const auto* failed = std::get_if<runtime::SubtitleTrackFailure>(&outcome);
            return runtime::TaskResult<bool>::succeeded(failed != nullptr && failed->cancelled);
        });
    expect(submission.accepted(), "the cancel task is accepted");
    {
        std::unique_lock lock(mutex);
        condition.wait(lock, [&] { return started; });
    }
    submission.handle.cancel();
    std::optional<runtime::TaskResult<bool>> result;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!(result = submission.handle.tryTakeResult()) &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    expect(result.has_value(), "the cancelled pass ends");
    expect(result && (result->state() == runtime::TaskState::Cancelled ||
                      (result->value() && *result->value())),
           "a cancelled pass reports cancellation");
    scheduler.beginShutdown();
    while (!scheduler.isQuiescent())
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

} // namespace

int main() {
    try {
        testTracksMatchThePicture();
        testNodeList();
        testCancellation();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    if (failures == 0)
        std::cout << "subtitle tracks: all checks passed\n";
    return failures == 0 ? 0 : 1;
}
