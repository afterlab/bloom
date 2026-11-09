// #347 Audio Amplitude, Audio Waveform and Audio Spectrum on the CPU reference: known answers read from
// a generated WAV (the levels of a window, the bar of a bin-centred sine), the cache identity (the same
// frame is a hit; another time, parameter or source revision is a new analysis), refusal of a missing or
// changed file, an input with no audio, the region of interest, and the retirement of the bounded cache.

#include "audio_plot_test_support.hpp"

#include <bloom/runtime/audio_analysis.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>

#include <algorithm>
#include <array>
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
                const std::optional<render::ImageWindow>& roi = std::nullopt) {
    runtime::CpuCompositionEvaluator evaluator;
    evaluator.setAssetBaseDirectory(base);
    runtime::EvaluationRequest request{core::RationalTime::fromInteger(0), plan->output(),
                                       runtime::CompositionFormatResolution{}};
    request.pixelStorageByteLimit = 1U << 28U;
    if (roi)
        request.roi = *roi;
    const auto result = evaluator.evaluate(plan, request, {});
    Rendered out;
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

const std::filesystem::path scratch = "/tmp/opencode/gpu-contract/audio";

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
    // The file is changed since import: its digest no longer matches.
    apt::writeFixtureWav(scratch / "plot_fixture_doomed.wav", 0.125);
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
    retirement();
    if (failures == 0)
        std::cout << "audio analysis tests passed\n";
    return failures == 0 ? 0 : 1;
}
