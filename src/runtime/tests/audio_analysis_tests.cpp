// #347 Audio Amplitude, Audio Waveform and Audio Spectrum on the CPU reference: known answers read from
// a generated WAV (the levels of a window, the bar of a bin-centred sine), the cache identity (the same
// frame is a hit; another time, parameter or source revision is a new analysis), refusal of a missing or
// changed file, an input with no audio, the region of interest, and the retirement of the bounded cache.

#include "audio_plot_test_support.hpp"

#include <bloom/runtime/audio_analysis.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/prepared_gpu_scene.hpp>
#include <bloom/runtime/task_scheduler.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

using namespace bloom;
namespace apt = bloom::audio_plot_test;

int failures = 0;
void expect(const bool value, const std::string& message) {
    if (!value) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

using Pixel = std::array<float, 4>;

struct Rendered final {
    bool evaluated = false;
    bool cancelled = false;
    std::string diagnostic;
    std::int64_t left = 0, top = 0;
    std::uint32_t width = 0, height = 0;
    std::vector<Pixel> pixels;
    [[nodiscard]] Pixel at(const std::int64_t x, const std::int64_t y) const {
        if (x < left || y < top || x >= left + width || y >= top + height)
            return {};
        return pixels[static_cast<std::size_t>((y - top) * width + (x - left))];
    }
};

Rendered render(const std::shared_ptr<const runtime::CompiledCompositionPlan>& plan,
                const std::filesystem::path& base,
                const std::optional<render::ImageWindow>& roi = std::nullopt,
                const std::optional<runtime::ProxyResolution>& proxy = std::nullopt,
                const runtime::CancellationToken& cancellation = {}) {
    runtime::CpuCompositionEvaluator evaluator;
    evaluator.setAssetBaseDirectory(base);
    runtime::EvaluationRequest request{core::RationalTime::fromInteger(0), plan->output(),
                                       runtime::CompositionFormatResolution{}};
    request.pixelStorageByteLimit = 1U << 28U;
    if (roi)
        request.roi = *roi;
    if (proxy)
        request.resolution = *proxy;
    const auto result = evaluator.evaluate(plan, request, cancellation);
    Rendered out;
    out.cancelled = result.status() == runtime::EvaluationStatus::Cancelled;
    for (const auto& diagnostic : result.diagnostics())
        out.diagnostic += diagnostic.summary + " " + diagnostic.detail + "; ";
    if (result.frame() == nullptr)
        return out;
    out.evaluated = true;
    const auto& image = result.frame()->processImage();
    const auto window = image.descriptor()->dataWindow();
    out.left = window.originX();
    out.top = window.originY();
    out.width = static_cast<std::uint32_t>(window.extent().width());
    out.height = static_cast<std::uint32_t>(window.extent().height());
    for (auto y = out.top; y < out.top + static_cast<std::int64_t>(out.height); ++y)
        for (auto x = out.left; x < out.left + static_cast<std::int64_t>(out.width); ++x) {
            Pixel value{};
            if (const auto pixel = image.read(x, y))
                std::ranges::copy(pixel.value()->components(), value.begin());
            out.pixels.push_back(value);
        }
    return out;
}

bool close(const float a, const float b, const float tolerance = 1e-4F) {
    return std::fabs(a - b) <= tolerance;
}
bool same(const Pixel& a, const Pixel& b, const float tolerance = 1e-4F) {
    return close(a[0], b[0], tolerance) && close(a[1], b[1], tolerance) &&
           close(a[2], b[2], tolerance) && close(a[3], b[3], tolerance);
}

// This process's own fixture directory (see sharedAudioFixture).
const std::filesystem::path scratch = apt::sharedAudioFixture().directory;

// The canvas the plot draws: the bounding box of the pixels with any alpha.
struct Box final {
    std::int64_t x0 = 0, y0 = 0, x1 = -1, y1 = -1;
};
Box canvasOf(const Rendered& frame) {
    Box box{1 << 30, 1 << 30, -1, -1};
    for (std::int64_t y = frame.top; y < frame.top + static_cast<std::int64_t>(frame.height); ++y)
        for (std::int64_t x = frame.left; x < frame.left + static_cast<std::int64_t>(frame.width); ++x)
            if (frame.at(x, y)[3] > 0.0F) {
                box.x0 = std::min(box.x0, x);
                box.y0 = std::min(box.y0, y);
                box.x1 = std::max(box.x1, x);
                box.y1 = std::max(box.y1, y);
            }
    return box;
}

constexpr Pixel kLine{1.0F, 0.5F, 0.25F, 1.0F};
// The fill behind, premultiplied: (0, 0, 0.1) at alpha 0.5.
constexpr Pixel kFill{0.0F, 0.0F, 0.05F, 0.5F};

void spectrumKnownAnswer() {
    const auto frame = render(apt::audioPlotPlan(apt::spectrumValues(), 347100), scratch);
    expect(frame.evaluated, "the spectrum evaluates: " + frame.diagnostic);
    if (!frame.evaluated)
        return;
    const auto box = canvasOf(frame);
    expect(box.x1 - box.x0 + 1 == 48 && box.y1 - box.y0 + 1 == 32, "the plot is its width and height");
    // 937.5 Hz is column 2 of 48 over 20 Hz..20 kHz. The channels average, so the bin holds a sine of
    // 0.25: -12.04 dB over a 90 dB range, a bar 0.866 of the height (27.7 of 32 rows).
    const auto bottom = box.y1;
    expect(same(frame.at(box.x0 + 2, bottom), kLine), "the bar stands on the bottom of the column");
    expect(same(frame.at(box.x0 + 2, bottom - 20), kLine), "the bar is solid under its top");
    expect(same(frame.at(box.x0 + 2, box.y0), kFill), "the column is the fill above its bar");
    expect(same(frame.at(box.x0 + 40, bottom), kFill), "a column with no energy shows only the fill");
    expect(same(frame.at(box.x0 + 2, bottom - 26), kLine), "the bar covers its first 27 rows");
    const auto partial = frame.at(box.x0 + 2, bottom - 27);
    expect(partial[3] > 0.6F && partial[3] < 0.99F, "the bar's last row is a partial coverage");
    expect(same(frame.at(box.x0 + 2, bottom - 28), kFill), "the bar stops under 29 rows");
}

void waveformKnownAnswer() {
    auto values = apt::AudioPlotValues{};
    const auto frame = render(apt::audioPlotPlan(values, 347200), scratch);
    expect(frame.evaluated, "the waveform evaluates: " + frame.diagnostic);
    if (!frame.evaluated)
        return;
    const auto box = canvasOf(frame);
    // The mean of the channels swings a sine of 0.25 about 0.125, so the trace is near the middle and
    // does not reach the top or bottom row; some column draws the line.
    int drawn = 0;
    for (std::int64_t x = box.x0; x <= box.x1; ++x) {
        expect(same(frame.at(x, box.y0), kFill), "the top row of every column is the fill");
        for (std::int64_t y = box.y0; y <= box.y1; ++y)
            if (frame.at(x, y)[0] > 0.5F) {
                ++drawn;
                break;
            }
    }
    expect(drawn == 48, "every column draws the trace");
}

void noAudioIsSilence() {
    auto values = apt::spectrumValues();
    values.withSource = false;
    const auto frame = render(apt::audioPlotPlan(values, 347300), scratch);
    expect(frame.evaluated, "an input with no audio evaluates");
    const auto box = canvasOf(frame);
    bool onlyFill = frame.evaluated;
    for (std::int64_t y = box.y0; y <= box.y1 && onlyFill; ++y)
        for (std::int64_t x = box.x0; x <= box.x1; ++x)
            if (!same(frame.at(x, y), kFill)) {
                onlyFill = false;
                break;
            }
    expect(onlyFill, "silence draws only the fill");
}

void cacheIdentity() {
    runtime::clearAudioAnalysis();
    auto values = apt::spectrumValues();
    const auto plan = apt::audioPlotPlan(values, 347400);
    // The operation cache is per evaluator, so a second render computes the frame again and must read
    // the prepared analysis, not decode.
    (void)render(plan, scratch);
    const auto first = runtime::audioAnalysisStatistics();
    expect(first.computed == 1, "the first frame computes one analysis");
    (void)render(plan, scratch);
    const auto second = runtime::audioAnalysisStatistics();
    expect(second.computed == 1 && second.hits > first.hits, "the same frame is a cache hit");
    auto later = values;
    later.startFrame = -13;
    (void)render(apt::audioPlotPlan(later, 347400), scratch);
    expect(runtime::audioAnalysisStatistics().computed == 2, "another time is another analysis");
    auto otherSettings = values;
    otherSettings.fftSize = 4096;
    (void)render(apt::audioPlotPlan(otherSettings, 347400), scratch);
    expect(runtime::audioAnalysisStatistics().computed == 3, "another transform size is another analysis");
    auto otherRange = values;
    otherRange.values[0] = 40.0;
    (void)render(apt::audioPlotPlan(otherRange, 347400), scratch);
    expect(runtime::audioAnalysisStatistics().computed == 4, "another frequency range is another analysis");
    // A different revision of the file is a different source: its digest differs.
    const auto revised = apt::makeAudioFixture(scratch, "plot_fixture_revised.wav", 0.25);
    (void)render(apt::audioPlotPlan(values, 347400, revised), scratch);
    expect(runtime::audioAnalysisStatistics().computed == 5, "another source revision is another analysis");
}

void unavailableSources() {
    const auto doomed = apt::makeAudioFixture(scratch, "plot_fixture_doomed.wav");
    auto values = apt::spectrumValues();
    expect(render(apt::audioPlotPlan(values, 347500, doomed), scratch).evaluated, "a present file evaluates");
    // The file is changed since import: its digest no longer matches. #707: an artist's edit lands
    // well after the import, but this rewrite follows it at once, with the same size; where the
    // file clock ticks coarsely (the Windows system time, and no change journal on the volume) both
    // writes could share one modification and change time, so the source identity, which is the
    // contract for "unchanged", could not tell them apart and the analysis read the stale verified
    // digest. The rewritten file is dated a few seconds later, as an edit after an import is.
    const auto imported = std::filesystem::last_write_time(scratch / "plot_fixture_doomed.wav");
    apt::writeFixtureWav(scratch / "plot_fixture_doomed.wav", 0.125);
    std::filesystem::last_write_time(scratch / "plot_fixture_doomed.wav",
                                     imported + std::chrono::seconds(3));
    runtime::clearAudioAnalysis();
    const auto changed = render(apt::audioPlotPlan(values, 347500, doomed), scratch);
    expect(!changed.evaluated, "a changed file is refused");
    expect(!changed.diagnostic.empty(), "a changed file explains itself");
    std::filesystem::remove(scratch / "plot_fixture_doomed.wav");
    runtime::clearAudioAnalysis();
    const auto missing = render(apt::audioPlotPlan(values, 347500, doomed), scratch);
    expect(!missing.evaluated, "a missing file is refused");
    expect(!missing.diagnostic.empty(), "a missing file explains itself");
    expect(runtime::audioAnalysisStatistics().entries == 0, "a refused analysis caches nothing");
}

void regionOfInterest() {
    const auto plan = apt::audioPlotPlan(apt::spectrumValues(), 347600);
    const auto full = render(plan, scratch);
    const auto box = canvasOf(full);
    const auto window = render::ImageWindow::create(box.x0 + 1, box.y0 + 4, 12, 20);
    const auto roi = render(plan, scratch, *window.value());
    expect(full.evaluated && roi.evaluated, "the full frame and the region evaluate");
    bool identical = true;
    for (std::int64_t y = box.y0 + 4; y < box.y0 + 24; ++y)
        for (std::int64_t x = box.x0 + 1; x < box.x0 + 13; ++x)
            identical = identical && same(full.at(x, y), roi.at(x, y), 0.0F);
    expect(identical, "a region reads the pixels of the full frame");
}

runtime::ProxyResolution proxyOf(const std::uint32_t width, const std::uint32_t height) {
    auto extent = render::ImageExtent::create(width, height);
    return runtime::ProxyResolution{*extent.value()};
}

// The share of a column the shape covers: the fill is alpha 0.5 and the line alpha 1.
double columnCoverage(const Rendered& frame, const Box& box, const std::int64_t x) {
    double covered = 0.0;
    for (std::int64_t y = box.y0; y <= box.y1; ++y)
        covered += std::clamp((static_cast<double>(frame.at(x, y)[3]) - 0.5) / 0.5, 0.0, 1.0);
    return covered / static_cast<double>(box.y1 - box.y0 + 1);
}

// #347: below full render scale (an Image Preview always renders at its drawn size; the Viewer can) the
// canvas is the plot's size scaled, and the plot draws the same picture over it. The analysis stays the
// node's own width, so the scale changes the pixel size and not what the plot says.
void reducedRenderScale() {
    // Spectrum: the 937.5 Hz bar stands in column 2 of 48; at half size it is column 1 of 24.
    {
        const auto plan = apt::audioPlotPlan(apt::spectrumValues(), 347800);
        const auto full = render(plan, scratch);
        const auto reduced = render(plan, scratch, std::nullopt, proxyOf(32, 32));
        expect(reduced.evaluated, "a spectrum evaluates at half the render scale: " + reduced.diagnostic);
        if (!full.evaluated || !reduced.evaluated)
            return;
        const auto fullBox = canvasOf(full);
        const auto box = canvasOf(reduced);
        expect(box.x1 - box.x0 + 1 == 24 && box.y1 - box.y0 + 1 == 16,
               "the spectrum's canvas is its size scaled");
        const double want = columnCoverage(full, fullBox, fullBox.x0 + 2);
        const double got = columnCoverage(reduced, box, box.x0 + 1);
        expect(std::fabs(got - want) < 0.08, "the bar is as tall, relative to the plot, as at full scale");
        expect(same(reduced.at(box.x0 + 1, box.y1), kLine), "the reduced bar stands on the bottom");
        expect(same(reduced.at(box.x0 + 1, box.y0), kFill), "the reduced column is the fill above its bar");
        expect(same(reduced.at(box.x0 + 20, box.y1), kFill), "a column with no energy shows only the fill");
    }
    // Waveform: every column draws a trace one or more rows thick, in the same place in the plot.
    {
        const auto plan = apt::audioPlotPlan(apt::AudioPlotValues{}, 347900);
        const auto full = render(plan, scratch);
        const auto reduced = render(plan, scratch, std::nullopt, proxyOf(32, 32));
        expect(reduced.evaluated, "a waveform evaluates at half the render scale: " + reduced.diagnostic);
        if (!full.evaluated || !reduced.evaluated)
            return;
        const auto fullBox = canvasOf(full);
        const auto box = canvasOf(reduced);
        expect(box.x1 - box.x0 + 1 == 24 && box.y1 - box.y0 + 1 == 16,
               "the waveform's canvas is its size scaled");
        const auto centreOf = [](const Rendered& frame, const Box& b, const std::int64_t x) {
            double sum = 0.0, weight = 0.0;
            for (std::int64_t y = b.y0; y <= b.y1; ++y) {
                const double c = std::clamp((static_cast<double>(frame.at(x, y)[0])) / 1.0, 0.0, 1.0);
                sum += c * static_cast<double>(y - b.y0);
                weight += c;
            }
            return weight > 0.0 ? sum / weight / static_cast<double>(b.y1 - b.y0 + 1) : -1.0;
        };
        int drawn = 0;
        for (std::int64_t x = box.x0; x <= box.x1; ++x) {
            const double reducedCentre = centreOf(reduced, box, x);
            const double fullCentre = centreOf(full, fullBox, fullBox.x0 + (x - box.x0) * 2);
            if (reducedCentre >= 0.0) {
                ++drawn;
                expect(std::fabs(reducedCentre - fullCentre) < 0.12, "the trace is where it is at full scale");
            }
            expect(same(reduced.at(x, box.y0), kFill), "the top row of a reduced column is the fill");
        }
        expect(drawn == 24, "every reduced column draws the trace");
    }
    // An odd extent, a region of interest and a canvas that is not a whole half all draw.
    {
        const auto plan = apt::audioPlotPlan(apt::spectrumValues(), 347950);
        const auto odd = render(plan, scratch, std::nullopt, proxyOf(37, 29));
        expect(odd.evaluated, "a spectrum evaluates at an odd render scale: " + odd.diagnostic);
        const auto box = canvasOf(odd);
        expect(odd.evaluated && box.x1 - box.x0 + 1 == 28 && box.y1 - box.y0 + 1 == 15,
               "an odd scale rounds the canvas up");
    }
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
        TaskRequest("audio analysis cancel token",
                    {.kind = TaskOwnerKind::Composition, .id = TaskOwnerId::fromRaw(347)}),
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
    while (!submission.handle.tryTakeResult().has_value() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    scheduler.beginShutdown();
    while (!scheduler.isQuiescent() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    return token;
}

// #347: a cancelled preparation ends as cancelled and publishes nothing: no frame, no failure text, no
// cached analysis, and the same plan then evaluates in full on a live token (nothing stale or half
// built was left behind). The device route's builder reports Cancelled the same way.
void cancellation() {
    const auto cancelled = makeCancelledToken();
    expect(cancelled.isCancellationRequested(), "the test token is cancelled");
    runtime::clearAudioAnalysis();
    const auto plan = apt::audioPlotPlan(apt::spectrumValues(), 348000);

    runtime::AudioAnalysisQuery query;
    query.kind = runtime::AudioAnalysisKind::Spectrum;
    query.source = apt::audioSource(apt::sharedAudioFixture(), -12);
    query.time = core::RationalTime::fromInteger(0);
    query.columns = 48;
    query.spectrum.fftSize = 2048;
    query.spectrum.lowHz = 20.0;
    query.spectrum.highHz = 20000.0;
    query.spectrum.floorDb = -90.0;
    query.spectrum.ceilingDb = 0.0;
    const auto direct = runtime::prepareAudioAnalysis(query, scratch, cancelled);
    expect(direct.outcome == runtime::AudioAnalysisOutcome::Cancelled, "a cancelled preparation is Cancelled");
    expect(!runtime::findAudioAnalysis(query), "a cancelled preparation caches nothing");

    const auto cpu = render(plan, scratch, std::nullopt, std::nullopt, cancelled);
    expect(!cpu.evaluated && cpu.cancelled, "a cancelled evaluation ends as cancelled, with no frame");
    expect(cpu.diagnostic.empty(), "a cancelled evaluation reports no failure: " + cpu.diagnostic);

    runtime::CpuCompositionEvaluator evaluator;
    evaluator.setAssetBaseDirectory(scratch);
    auto media = runtime::GpuSceneMediaContext::fromEvaluator(evaluator);
    media.assetBaseDirectory = scratch;
    const runtime::CpuGpuSceneBuilder builder(nullptr, media);
    runtime::EvaluationRequest request{core::RationalTime::fromInteger(0), plan->output(),
                                       runtime::CompositionFormatResolution{}};
    request.pixelStorageByteLimit = 1U << 28U;
    const auto scene = builder.build(plan, request, cancelled);
    expect(!scene.hasValue() &&
               scene.diagnostic.code == runtime::PreparedGpuSceneDiagnosticCode::Cancelled,
           "a cancelled scene preparation reports Cancelled");

    const auto statistics = runtime::audioAnalysisStatistics();
    expect(statistics.entries == 0 && statistics.bytes == 0,
           "nothing was published by the cancelled preparations");

    const auto after = render(plan, scratch);
    expect(after.evaluated, "the same plan then evaluates: " + after.diagnostic);
    const auto box = canvasOf(after);
    expect(same(after.at(box.x0 + 2, box.y1), kLine), "the frame after a cancellation is the known answer");
    expect(runtime::audioAnalysisStatistics().computed == 1, "it computed its analysis once, itself");
    runtime::clearAudioAnalysis();
}

void amplitude() {
    runtime::clearAudioAnalysis();
    runtime::CompiledValueAudioAmplitude kernel;
    kernel.source = apt::audioSource(apt::sharedAudioFixture(), 0);
    const auto time = core::RationalTime::create(1, 2);
    for (const auto measure : {0, 1}) {
        kernel.measure = measure;
        auto query = runtime::audioAmplitudeQuery(kernel, *time, 50.0, 0.0);
        expect(query.has_value(), "the amplitude query is valid");
        if (!query)
            continue;
        expect(!runtime::findAudioAnalysis(*query), "nothing is prepared before the worker prepares it");
        const auto status = runtime::prepareAudioAnalysis(*query, scratch, {});
        expect(status.outcome == runtime::AudioAnalysisOutcome::Ready, "the amplitude prepares");
        const auto found = runtime::findAudioAnalysis(*query);
        expect(found != nullptr, "the prepared amplitude is found without I/O");
        if (found) {
            // Every channel's samples count: the left sine of 0.5 peaks at 0.5, and the RMS over both
            // channels is sqrt((0.5^2 / 2 + 0.25^2) / 2).
            const double want = measure == 0 ? 0.5 : std::sqrt((0.25 / 2.0 + 0.0625) / 2.0);
            expect(std::fabs(found->amplitude - want) < 2e-3,
                   std::string(measure == 0 ? "peak" : "rms") + " reads " + std::to_string(found->amplitude));
        }
    }
    expect(!runtime::audioAmplitudeQuery(kernel, *time, 0.0, 0.0).has_value(), "a zero window is refused");
    expect(!runtime::audioAmplitudeQuery(kernel, *time, 50.0, 1e12).has_value(), "an absurd offset is refused");
}

void retirement() {
    runtime::clearAudioAnalysis();
    auto values = apt::AudioPlotValues{};
    values.width = 4096;
    values.height = 16;
    values.separate = true;
    // Distinct times fill the cache past its byte budget; the least recently used entries retire and the
    // cache never holds more than its budget.
    for (int step = 0; step < 520; ++step) {
        values.startFrame = -static_cast<std::int64_t>(step);
        const auto frame = render(apt::audioPlotPlan(values, 347700), scratch);
        if (!frame.evaluated) {
            expect(false, "a large plot evaluates: " + frame.diagnostic);
            return;
        }
    }
    const auto statistics = runtime::audioAnalysisStatistics();
    expect(statistics.evicted > 0, "old analyses retire");
    expect(statistics.bytes <= runtime::kAudioAnalysisCacheBytes, "the cache stays under its byte budget");
    expect(statistics.openSources <= runtime::kAudioAnalysisOpenSources, "open sources stay bounded");
    runtime::clearAudioAnalysis();
    const auto cleared = runtime::audioAnalysisStatistics();
    expect(cleared.entries == 0 && cleared.bytes == 0 && cleared.openSources == 0, "a purge drops everything");
}

} // namespace

int main() {
    spectrumKnownAnswer();
    waveformKnownAnswer();
    noAudioIsSilence();
    cacheIdentity();
    unavailableSources();
    regionOfInterest();
    amplitude();
    reducedRenderScale();
    cancellation();
    retirement();
    if (failures == 0)
        std::cout << "audio analysis tests passed\n";
    return failures == 0 ? 0 : 1;
}
