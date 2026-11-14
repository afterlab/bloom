// #724 Scopes, section 8b: the read that MCP's `query` kind `scopes` and Python's `bloom.scopes`
// share (scripting/scope_read.hpp). A 64 x 64 composition of eight color bars (eight solids) is
// read headless, with no panel, no application and no Qt: tables equal the CPU reference's cell
// for cell for both signals; the summary and the picture; a range with progress and a cancel; the
// warm-sidecar port; region, Ignore Transparent Pixels, half resolution, display and view; every
// refusal with its remedy.

#include <bloom/commands/layer_operations.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/project.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/input_color_context.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/scripting/render.hpp>
#include <bloom/scripting/scope_read.hpp>
#include <bloom/scripting/session.hpp>

#include <zlib.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <iostream>
#include <source_location>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace bloom;
namespace scopes = runtime::scopes;

int failures = 0;

void expect(const bool condition, const std::string& message,
            const std::source_location location = std::source_location::current()) {
    if (!condition) {
        ++failures;
        std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
    }
}

[[nodiscard]] bool contains(const std::string& text, const std::string_view part) {
    return text.find(part) != std::string::npos;
}

// Eight bars of 8 x 64 pixels, as 75% linear colors: white, yellow, cyan, green, magenta, red,
// blue and black.
constexpr std::array<std::array<double, 3>, 8> kBars{{{0.75, 0.75, 0.75},
                                                      {0.75, 0.75, 0.0},
                                                      {0.0, 0.75, 0.75},
                                                      {0.0, 0.75, 0.0},
                                                      {0.75, 0.0, 0.75},
                                                      {0.75, 0.0, 0.0},
                                                      {0.0, 0.0, 0.75},
                                                      {0.0, 0.0, 0.0}}};

struct Bars final {
    std::unique_ptr<scripting::Session> session;
    document::CompositionId composition;

    Bars() {
        auto created = scripting::Session::createNew(
            "Scopes", "Bars", core::RationalTime::fromInteger(1),
            *document::CompositionFormat::create(64, 64, core::PixelAspectRatio::square(),
                                                 *document::FrameRate::create(24, 1)));
        if (!created)
            std::abort();
        session = std::move(created).takeSession();
        composition = session->snapshot().project().compositions().front().id();
        for (std::size_t index = 0; index < kBars.size(); ++index) {
            const auto added = run<commands::AddSolidLayer>(
                composition, "Bar " + std::to_string(index),
                core::Color4d{kBars[index][0], kBars[index][1], kBars[index][2], 1.0},
                document::Vec2d{8.0 * static_cast<double>(index) + 4.0, 32.0});
            const auto solid =
                added.command->outputId<document::NodeId>(commands::kAddSolidLayerSolidNodeOutput)
                    .value();
            run<commands::SetParameterSource>(composition,
                                              role(solid, document::kSolidWidthParameterRole),
                                              document::ConstantValueSource{8.0});
            run<commands::SetParameterSource>(composition,
                                              role(solid, document::kSolidHeightParameterRole),
                                              document::ConstantValueSource{64.0});
        }
    }

    template <typename Operation, typename... Arguments>
    scripting::SessionCommandResult run(Arguments&&... args) {
        commands::Transaction transaction("Bars", session->snapshot().revision());
        transaction.emplace<Operation>(std::forward<Arguments>(args)...);
        auto result = session->execute(std::move(transaction));
        expect(result.succeeded(), "a fixture command applies");
        return result;
    }

    [[nodiscard]] document::ParameterId role(const document::NodeId node,
                                             const std::string_view name) const {
        const auto snapshot = session->snapshot();
        const auto* record =
            snapshot.project().findComposition(composition)->graph().findNode(node);
        for (const auto& binding : record->parameters)
            if (binding.role == name)
                return binding.parameterId;
        return {};
    }
};

// A scheduler for the reads; it outlives them.
struct Reads final {
    runtime::TaskScheduler scheduler;
    ~Reads() {
        scheduler.beginShutdown();
        while (!scheduler.isQuiescent())
            std::this_thread::yield();
    }
    [[nodiscard]] scripting::ScopeReadPorts ports() {
        scripting::ScopeReadPorts value;
        value.scheduler = &scheduler;
        return value;
    }
};

// The CPU reference of one frame, measured the way the test's own eyes see it: evaluated by the CPU
// evaluator, counted by measureScopeFrameCpu, with the configuration's own display and view.
struct Reference final {
    runtime::ScopeCounts counts;
    runtime::ScopeLuma luma;
    color::ScopeColorimetry colorimetry;
};

[[nodiscard]] Reference
reference(const Bars& bars, const std::uint64_t frame,
          const runtime::ScopeMeasureSettings& settings, const std::string& display = {},
          const std::string& view = {},
          const runtime::EvaluationResolution resolution = runtime::CompositionFormatResolution{}) {
    const auto source = bars.session->renderSourceSnapshot();
    const auto* composition = source.snapshot.project().findComposition(bars.composition);
    const auto intent = scripting::captureRenderColorIntent(source, *composition);
    auto config = runtime::detail::resolveInputColorConfig(*intent);
    expect(config.has_value(), "the project color configuration resolves");
    const auto shownDisplay = display.empty() ? std::string(config->displayName()) : display;
    const auto shownView = view.empty() ? std::string(config->viewName()) : view;
    const auto prepared = runtime::ScopeSignalColor::prepare(
        {.config = std::make_shared<const color::ResolvedBloomNeutralConfig>(std::move(*config)),
         .signal = settings.signal,
         .display = shownDisplay,
         .view = shownView});
    expect(static_cast<bool>(prepared), "the signal's color prepares");
    const runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
    const auto compiled =
        compiler.compile({.snapshot = source.snapshot, .compositionId = bars.composition},
                         runtime::CancellationToken{});
    const runtime::CpuCompositionEvaluator evaluator;
    const auto rate = composition->format().frameRate();
    const auto mapping = core::FrameTimeMapping::create(composition->duration(), rate.numerator(),
                                                        rate.denominator());
    const auto time = mapping.value()->timeForFrame(frame);
    const auto evaluated = evaluator.evaluate(
        compiled.plan,
        runtime::EvaluationRequest{.time = *time.value(),
                                   .output = compiled.plan->output(),
                                   .resolution = resolution,
                                   .quality = runtime::EvaluationQuality::Reference,
                                   .colorIntent = *intent,
                                   .pixelStorageByteLimit = std::size_t{1} << 30U},
        runtime::CancellationToken{});
    expect(evaluated.frame() != nullptr, "the reference frame evaluates");
    auto measured = runtime::measureScopeFrameCpu(*evaluated.frame(), settings, *prepared.color);
    Reference result;
    result.counts = std::move(measured.counts);
    result.luma = prepared.color->luma();
    result.colorimetry = prepared.color->colorimetry();
    return result;
}

[[nodiscard]] scripting::ScopeReadRequest requestFor(const Bars& bars, const std::uint64_t frame) {
    scripting::ScopeReadRequest request;
    request.composition = bars.composition;
    request.firstFrame = frame;
    request.lastFrame = frame;
    return request;
}

[[nodiscard]] std::uint32_t sumOf(const std::vector<std::uint32_t>& values, const std::size_t first,
                                  const std::size_t count) {
    std::uint32_t sum = 0;
    for (std::size_t index = first; index < first + count; ++index)
        sum += values[index];
    return sum;
}

// ---- Tables ------------------------------------------------------------------------------------

void tablesMatchTheReference(const Bars& bars, Reads& reads) {
    for (const auto signal : {runtime::ScopeSignal::Display, runtime::ScopeSignal::Working}) {
        const bool working = signal == runtime::ScopeSignal::Working;
        auto request = requestFor(bars, 0);
        request.signal = signal;
        request.axisMaximum = working ? 2U : 1U;
        const auto read = scripting::readScopes(*bars.session, request, reads.ports());
        expect(read.ok() && read.frames.size() == 1,
               "a read of one frame answers: " + read.error.message);
        if (!read.ok() || read.frames.size() != 1)
            continue;
        const auto& frame = read.frames.front();
        runtime::ScopeMeasureSettings settings;
        settings.signal = signal;
        settings.axisMaximum = request.axisMaximum;
        const auto expected = reference(bars, 0, settings);
        expect(frame.counts == expected.counts,
               std::string("the tables equal the CPU reference cell for cell, ") +
                   (working ? "working" : "display"));
        expect(frame.counted == 4096U && frame.counts.counted() == 4096U,
               "every pixel of the 64 x 64 frame is counted");
        expect(frame.route == runtime::ScopeRoute::Cpu && !frame.fromCache &&
                   contains(frame.routeNote, "CPU reference"),
               "a session with no device names the CPU route and says why: " + frame.routeNote);
        expect(frame.area == runtime::ScopeMeasuredArea{0, 0, 64, 64}, "the area is the frame");
        expect(read.settings.signal == signal && read.settings.axisMaximum == request.axisMaximum &&
                   read.settings.resolution == scripting::ScopeReadResolution::Full &&
                   !read.settings.region && !read.settings.ignoreTransparent,
               "the settings measured are the request after defaults");
        expect(read.compositionWidth == 64 && read.compositionHeight == 64,
               "the composition's size is reported");
        expect(std::abs(read.color.luma.red - 0.2126F) < 1e-4F &&
                   std::abs(read.color.luma.green - 0.7152F) < 1e-4F &&
                   std::abs(read.color.luma.blue - 0.0722F) < 1e-4F,
               "the luma coefficients are Rec. 709's");
        expect(std::abs(read.color.red[0] - 0.64) < 5e-5 &&
                   std::abs(read.color.red[1] - 0.33) < 5e-5 &&
                   std::abs(read.color.green[0] - 0.30) < 5e-5 &&
                   std::abs(read.color.green[1] - 0.60) < 5e-5 &&
                   std::abs(read.color.blue[0] - 0.15) < 5e-5 &&
                   std::abs(read.color.blue[1] - 0.06) < 5e-5 &&
                   std::abs(read.color.white[0] - 0.3127) < 5e-5 &&
                   std::abs(read.color.white[1] - 0.3290) < 5e-5,
               "the primaries and white are Rec. 709's");
        expect(read.color.display.empty() == working && read.color.view.empty() == working,
               "the display signal names its display and view, the working signal none");

        // The sum rule: a channel's bins plus under and over are the pixels counted.
        const auto data =
            scripting::describeScopeTables(frame.counts, scripting::ScopeReadEncoding::Dense);
        expect(data.histogram && data.waveform && data.vectorscope && data.chromaticity,
               "every table is described");
        if (!data.histogram || !data.waveform || !data.vectorscope || !data.chromaticity)
            continue;
        bool sums = true;
        for (std::size_t channel = 0; channel < 4; ++channel)
            sums = sums && sumOf(data.histogram->values, channel * 1024, 1024) +
                                   data.histogram->under[channel] + data.histogram->over[channel] ==
                               4096U;
        expect(sums, "each histogram channel's bins plus under and over equal the pixels counted");
        bool columns = data.waveform->shape == std::vector<std::uint32_t>{4, 64, 512};
        for (std::size_t channel = 0; columns && channel < 4; ++channel)
            for (std::size_t column = 0; columns && column < 64; ++column)
                columns = sumOf(data.waveform->values, (channel * 64 + column) * 512, 512) +
                              data.waveform->under[channel * 64 + column] +
                              data.waveform->over[channel * 64 + column] ==
                          64U;
        expect(columns, "each waveform column's levels plus under and over equal its 64 pixels");
        expect(sumOf(data.vectorscope->values, 0, 512 * 512) + frame.counts.vectorscopeOutside() ==
                   4096U,
               "the vectorscope's cells plus its outside count equal the pixels counted");
        expect(sumOf(data.chromaticity->values, 0, 512 * 512) + frame.counts.chromaticityOutside() +
                       frame.counts.noChromaticity() ==
                   4096U,
               "the chromaticity's cells plus its outside and no-chromaticity counts equal the "
               "pixels counted");

        // The dense table is the packed words, and the sparse one is its non-zero cells.
        const auto sparse =
            scripting::describeScopeTables(frame.counts, scripting::ScopeReadEncoding::Sparse);
        std::vector<std::uint32_t> rebuilt(data.vectorscope->values.size(), 0U);
        for (std::size_t at = 0; sparse.vectorscope && at + 1 < sparse.vectorscope->values.size();
             at += 2)
            rebuilt[sparse.vectorscope->values[at]] = sparse.vectorscope->values[at + 1];
        expect(rebuilt == data.vectorscope->values && sparse.jsonBytes < data.jsonBytes,
               "the sparse encoding lists exactly the non-zero cells and is smaller");
    }
}

// The Working signal of the bars is known without any measure: 75% (code 767) or 0 in each channel.
void workingTablesAreTheBarsOwn(const Bars& bars, Reads& reads) {
    auto request = requestFor(bars, 0);
    request.signal = runtime::ScopeSignal::Working;
    const auto read = scripting::readScopes(*bars.session, request, reads.ports());
    if (!read.ok() || read.frames.empty()) {
        expect(false, "the working read answers: " + read.error.message);
        return;
    }
    const auto& counts = read.frames.front().counts;
    // White, yellow, magenta and red carry red; white, yellow, cyan and green carry green; white,
    // cyan, magenta and blue carry blue: half the pixels each, at code 767 (0.75 * 1023 + 0.5).
    for (std::uint32_t channel = 0; channel < 3; ++channel)
        expect(counts.histogram(channel, 767) == 2048U && counts.histogram(channel, 0) == 2048U,
               "each color channel holds half its pixels at 0 and half at code 767");
    // The waveform's first column is the white bar: 64 pixels at level 383 (code 767 >> 1) in red,
    // green and blue; the last column is black.
    expect(counts.waveform(0U, 0, 383) == 64U && counts.waveform(1U, 0, 383) == 64U &&
               counts.waveform(2U, 0, 383) == 64U && counts.waveform(0U, 63, 0) == 64U,
           "the waveform columns are the bars' own levels");
}

// ---- Summary -----------------------------------------------------------------------------------

void summaryMatches(const Bars& bars, Reads& reads) {
    auto request = requestFor(bars, 0);
    request.signal = runtime::ScopeSignal::Working;
    request.output = scripting::ScopeReadOutput::Summary;
    const auto read = scripting::readScopes(*bars.session, request, reads.ports());
    expect(read.ok() && read.frames.size() == 1 && read.frames.front().summary.has_value(),
           "a summary read answers: " + read.error.message);
    if (!read.ok() || read.frames.empty() || !read.frames.front().summary)
        return;
    const auto& frame = read.frames.front();
    const auto& summary = *frame.summary;
    runtime::ScopeMeasureSettings settings;
    settings.signal = runtime::ScopeSignal::Working;
    const auto expected = runtime::summarizeScopes(reference(bars, 0, settings).counts);
    expect(summary == expected, "the summary is the reference tables' summary");
    expect(frame.counted == 4096U && frame.counts.words.empty(),
           "a summary read keeps the count and none of the tables");
    // Red is 0 in half the pixels and code 767 in the other half.
    const auto& red = summary.channels[0];
    expect(red.available && red.minimum == 0 && red.maximum == 767 && red.mean == 383.5 &&
               red.percentile1 == 0 && red.percentile50 == 0 && red.percentile99 == 767 &&
               red.clippedUnder == 0 && red.clippedOver == 0,
           "the red summary is computed from the known picture");
    expect(summary.vectorscopeAvailable && summary.meanSaturation > 0.0,
           "the vectorscope's mean saturation and hue are reported");
    // The summary reads the histogram and the vectorscope.
    auto onlyWaveform = request;
    onlyWaveform.scopes = render::kScopeTableWaveform;
    const auto refused = scripting::readScopes(*bars.session, onlyWaveform, reads.ports());
    expect(refused.error.status == scripting::ScopeReadStatus::Invalid &&
               contains(refused.error.message, "histogram and the vectorscope"),
           "a summary of the waveform alone is refused with the remedy");
}

// ---- Pictures ----------------------------------------------------------------------------------

struct DecodedPng final {
    bool valid = false;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgba;
};

[[nodiscard]] std::uint32_t be32(const std::vector<std::uint8_t>& bytes, const std::size_t at) {
    return (static_cast<std::uint32_t>(bytes[at]) << 24U) |
           (static_cast<std::uint32_t>(bytes[at + 1]) << 16U) |
           (static_cast<std::uint32_t>(bytes[at + 2]) << 8U) | bytes[at + 3];
}

[[nodiscard]] DecodedPng decodePng(const std::vector<std::uint8_t>& bytes) {
    DecodedPng result;
    constexpr std::array<std::uint8_t, 8> signature{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (bytes.size() < 8 || !std::equal(signature.begin(), signature.end(), bytes.begin()))
        return result;
    std::size_t at = 8;
    std::vector<std::uint8_t> idat;
    while (at + 12 <= bytes.size()) {
        const auto length = be32(bytes, at);
        if (at + 12 + length > bytes.size())
            return result;
        const std::string type(bytes.begin() + static_cast<std::ptrdiff_t>(at) + 4,
                               bytes.begin() + static_cast<std::ptrdiff_t>(at) + 8);
        if (type == "IHDR" && length == 13) {
            result.width = be32(bytes, at + 8);
            result.height = be32(bytes, at + 12);
        } else if (type == "IDAT") {
            idat.insert(idat.end(), bytes.begin() + static_cast<std::ptrdiff_t>(at) + 8,
                        bytes.begin() + static_cast<std::ptrdiff_t>(at) + 8 + length);
        }
        at += 12 + length;
    }
    const std::size_t rowBytes = 1U + static_cast<std::size_t>(result.width) * 4U;
    std::vector<std::uint8_t> raw(rowBytes * result.height);
    uLongf size = static_cast<uLongf>(raw.size());
    if (uncompress(raw.data(), &size, idat.data(), static_cast<uLong>(idat.size())) != Z_OK ||
        size != raw.size())
        return result;
    for (std::uint32_t y = 0; y < result.height; ++y)
        result.rgba.insert(result.rgba.end(),
                           raw.begin() + static_cast<std::ptrdiff_t>(y * rowBytes + 1),
                           raw.begin() + static_cast<std::ptrdiff_t>((y + 1) * rowBytes));
    result.valid = true;
    return result;
}

// How many pixels of the PNG differ from `blank`, the same scope drawn with no tables.
[[nodiscard]] std::size_t tracePixels(const DecodedPng& png, const scopes::ScopeImage& blank) {
    std::size_t differing = 0;
    for (std::uint32_t y = 0; y < png.height; ++y)
        for (std::uint32_t x = 0; x < png.width; ++x) {
            const auto at = (static_cast<std::size_t>(y) * png.width + x) * 4U;
            const auto drawn = scopes::packArgb(png.rgba[at + 3], png.rgba[at], png.rgba[at + 1],
                                                png.rgba[at + 2]);
            if (drawn != blank.pixel(static_cast<int>(x), static_cast<int>(y)))
                ++differing;
        }
    return differing;
}

void pictures(const Bars& bars, Reads& reads) {
    const std::array<std::pair<scripting::ScopeReadImageKind, scopes::Kind>, 5> kinds{
        {{scripting::ScopeReadImageKind::Waveform, scopes::Kind::Waveform},
         {scripting::ScopeReadImageKind::Parade, scopes::Kind::Parade},
         {scripting::ScopeReadImageKind::Vectorscope, scopes::Kind::Vectorscope},
         {scripting::ScopeReadImageKind::Histogram, scopes::Kind::Histogram},
         {scripting::ScopeReadImageKind::Chromaticity, scopes::Kind::Chromaticity}}};
    for (const auto& [name, kind] : kinds) {
        auto request = requestFor(bars, 0);
        request.output = scripting::ScopeReadOutput::Image;
        request.image = name;
        request.width = 320;
        request.height = 256;
        const auto read = scripting::readScopes(*bars.session, request, reads.ports());
        expect(read.ok() && read.frames.size() == 1,
               "an image read answers: " + read.error.message);
        if (!read.ok() || read.frames.empty())
            continue;
        const auto decoded = decodePng(read.frames.front().png);
        expect(decoded.valid && decoded.width == 320 && decoded.height == 256 &&
                   read.frames.front().imageWidth == 320 && read.frames.front().imageHeight == 256,
               "the picture has the requested size");
        scopes::PictureInput blankInput;
        const auto blank =
            scopes::renderScopePicture(kind, 320, 256, blankInput, {}, {}, std::size_t{1} << 26U);
        expect(blank.ok() && decoded.valid && tracePixels(decoded, blank.image) > 100,
               "the trace area is not empty: the picture differs from the bare graticule");
    }
    auto all = requestFor(bars, 0);
    all.output = scripting::ScopeReadOutput::Image;
    all.image = scripting::ScopeReadImageKind::All;
    all.width = 320;
    const auto read = scripting::readScopes(*bars.session, all, reads.ports());
    expect(read.ok() && read.frames.size() == 1,
           "an image of all five answers: " + read.error.message);
    if (read.ok() && !read.frames.empty()) {
        const auto decoded = decodePng(read.frames.front().png);
        scopes::PictureInput blankInput;
        const auto size = scopes::allScopesPictureSize(320, blankInput, {});
        expect(decoded.valid && decoded.width == 320 &&
                   static_cast<int>(decoded.height) == size.height && size.height > 0,
               "all five are stacked in the panel's size for the width");
        const auto blank =
            scopes::renderAllScopesPicture(320, blankInput, {}, {}, std::size_t{1} << 26U);
        expect(blank.ok() && decoded.valid && tracePixels(decoded, blank.image) > 500,
               "every trace of the stacked picture is drawn");
    }
}

// ---- Range, progress, cancellation and the warm port --------------------------------------------

void rangeProgressAndCancel(const Bars& bars, Reads& reads) {
    auto request = requestFor(bars, 0);
    request.lastFrame = 2;
    request.signal = runtime::ScopeSignal::Working;
    request.scopes = render::kScopeTableHistogram;
    auto ports = reads.ports();
    std::vector<std::pair<std::uint64_t, std::uint64_t>> reported;
    ports.progress = [&reported](const std::uint64_t done, const std::uint64_t total) {
        reported.emplace_back(done, total);
    };
    const auto read = scripting::readScopes(*bars.session, request, ports);
    expect(read.ok() && read.frames.size() == 3, "a range answers one result per frame");
    if (read.ok() && read.frames.size() == 3) {
        expect(read.frames[0].frame == 0 && read.frames[1].frame == 1 &&
                   read.frames[2].frame == 2 &&
                   read.frames[1].time == core::RationalTime::create(1, 24).value(),
               "the frames are in order with their times");
        expect(read.frames[0].counts == read.frames[2].counts,
               "a still composition reads the same");
    }
    expect(!reported.empty() && reported.back() == std::pair<std::uint64_t, std::uint64_t>{3, 3} &&
               std::is_sorted(reported.begin(), reported.end()),
           "progress counts the finished frames up to the total");

    // A cancel after the first frame: frame 1's measure waits until the read has passed the cancel
    // on, so the outcome does not depend on timing.
    std::atomic<bool> stopAsked{false};
    std::atomic<int> askedAfterStop{0};
    auto cancelling = reads.ports();
    cancelling.progress = [&stopAsked](const std::uint64_t done, const std::uint64_t) {
        if (done >= 1)
            stopAsked.store(true);
    };
    cancelling.cancelled = [&stopAsked, &askedAfterStop] {
        if (!stopAsked.load())
            return false;
        askedAfterStop.fetch_add(1);
        return true;
    };
    cancelling.warm =
        [&askedAfterStop](
            const scripting::ScopeWarmQuery& query) -> std::optional<scripting::ScopeWarmHit> {
        if (query.time == core::RationalTime::create(1, 24).value())
            while (askedAfterStop.load() < 2)
                std::this_thread::yield();
        return std::nullopt;
    };
    const auto cancelled = scripting::readScopes(*bars.session, request, cancelling);
    expect(cancelled.error.status == scripting::ScopeReadStatus::Cancelled &&
               contains(cancelled.error.message, "cancelled"),
           "a cancelled range answers the cancelled outcome");

    // A read cancelled before it starts never measures.
    auto immediate = reads.ports();
    immediate.cancelled = [] { return true; };
    const auto none = scripting::readScopes(*bars.session, request, immediate);
    expect(none.error.status == scripting::ScopeReadStatus::Cancelled && none.frames.empty(),
           "a read cancelled at once answers cancelled with no frames");
}

void warmSidecars(const Bars& bars, Reads& reads) {
    runtime::ScopeMeasureSettings settings;
    settings.signal = runtime::ScopeSignal::Working;
    settings.scopes = render::kScopeTableHistogram;
    // A sidecar whose tables are not the frame's own, so a frame that came from it was not
    // measured.
    runtime::ScopeMeasureSettings other = settings;
    other.region = runtime::ScopeRegion{0, 0, 8, 64};
    const auto fabricated = reference(bars, 1, other).counts;
    const auto tables = runtime::ScopeTables::pack(fabricated, runtime::ScopeRoute::Gpu);

    auto request = requestFor(bars, 0);
    request.lastFrame = 1;
    request.signal = runtime::ScopeSignal::Working;
    request.scopes = render::kScopeTableHistogram;
    std::vector<scripting::ScopeWarmQuery> asked;
    auto ports = reads.ports();
    ports.warm =
        [&](const scripting::ScopeWarmQuery& query) -> std::optional<scripting::ScopeWarmHit> {
        asked.push_back(query);
        // The application answers only for the frame it measured under this exact identity.
        if (query.time != core::RationalTime::create(1, 24).value() ||
            !(query.settings == settings) ||
            !std::holds_alternative<runtime::CompositionFormatResolution>(query.resolution))
            return std::nullopt;
        return scripting::ScopeWarmHit{tables, {0, 0, 8, 64}, "warm"};
    };
    const auto read = scripting::readScopes(*bars.session, request, ports);
    expect(read.ok() && read.frames.size() == 2,
           "a read with a warm port answers: " + read.error.message);
    if (read.ok() && read.frames.size() == 2) {
        expect(!read.frames[0].fromCache && read.frames[0].counted == 4096U,
               "a frame the port does not know is measured");
        expect(read.frames[1].fromCache && read.frames[1].counts == fabricated &&
                   read.frames[1].route == runtime::ScopeRoute::Gpu &&
                   read.frames[1].routeNote == "warm" && read.frames[1].counted == 512U,
               "a matching sidecar is returned as it is, without measuring, with its route");
    }
    expect(asked.size() == 2 && asked[0].settings == settings &&
               asked[0].composition == bars.composition && asked[0].defaultDisplayView,
           "the port is asked per frame with the measure settings the read would use");

    // The same read under other settings: the port's identity does not match, nothing is reused.
    auto changed = request;
    changed.ignoreTransparent = true;
    const auto measured = scripting::readScopes(*bars.session, changed, ports);
    expect(measured.ok() && measured.frames.size() == 2 && !measured.frames[1].fromCache &&
               measured.frames[1].counted == 4096U,
           "a sidecar of other settings is not used");
}

// A sidecar counted at another detail is never served to a read, which measures at detail 0: the
// settings the port is asked with carry detail 0, an exact-identity port finds nothing under them,
// and a port that wrongly answers with a detail-1 sidecar is not believed.
void warmSidecarOfAnotherDetail(const Bars& bars, Reads& reads) {
    runtime::ScopeMeasureSettings settings;
    settings.signal = runtime::ScopeSignal::Working;
    settings.scopes = render::kScopeTableHistogram | render::kScopeTableVectorscope;
    runtime::ScopeMeasureSettings coarse = settings;
    coarse.detail = 1;
    const auto coarseCounts = reference(bars, 0, coarse).counts;
    expect(coarseCounts.layout.detail == 1U && coarseCounts.layout.cells() == 256U,
           "the fixture sidecar is counted at detail 1");
    const auto coarseTables = runtime::ScopeTables::pack(coarseCounts, runtime::ScopeRoute::Gpu);

    auto request = requestFor(bars, 0);
    request.signal = runtime::ScopeSignal::Working;
    request.scopes = settings.scopes;

    // An exact-identity port that knows only the detail-1 measure.
    std::vector<scripting::ScopeWarmQuery> asked;
    auto ports = reads.ports();
    ports.warm =
        [&](const scripting::ScopeWarmQuery& query) -> std::optional<scripting::ScopeWarmHit> {
        asked.push_back(query);
        if (!(query.settings == coarse))
            return std::nullopt;
        return scripting::ScopeWarmHit{coarseTables, {0, 0, 64, 64}, "coarse"};
    };
    const auto strict = scripting::readScopes(*bars.session, request, ports);
    expect(strict.ok() && strict.frames.size() == 1 && !strict.frames[0].fromCache &&
               strict.frames[0].counts.layout.detail == 0U &&
               strict.frames[0].counts.layout.cells() == 512U,
           "a read measures at detail 0 and finds no sidecar of detail 1");
    expect(asked.size() == 1 && asked[0].settings.detail == 0U && !(asked[0].settings == coarse),
           "the port is asked under detail 0");

    // A port that answers every query with the detail-1 sidecar: the read does not serve it.
    ports.warm = [&](const scripting::ScopeWarmQuery&) -> std::optional<scripting::ScopeWarmHit> {
        return scripting::ScopeWarmHit{coarseTables, {0, 0, 64, 64}, "coarse"};
    };
    const auto careless = scripting::readScopes(*bars.session, request, ports);
    expect(careless.ok() && careless.frames.size() == 1 && !careless.frames[0].fromCache &&
               careless.frames[0].counts.layout.detail == 0U,
           "a sidecar of another detail is never served to a read");
}

// ---- Region, transparency, resolution, display and view -----------------------------------------

void settingsChangeTheResult(const Bars& bars, Reads& reads) {
    auto request = requestFor(bars, 0);
    request.signal = runtime::ScopeSignal::Working;
    request.scopes = render::kScopeTableHistogram | render::kScopeTableWaveform;

    // A region measures its pixels, and the waveform's columns span its width.
    auto region = request;
    region.region = runtime::ScopeRegion{8, 0, 16, 32};
    const auto cut = scripting::readScopes(*bars.session, region, reads.ports());
    expect(cut.ok() && cut.frames.size() == 1 && cut.frames[0].counted == 512U &&
               cut.frames[0].area == runtime::ScopeMeasuredArea{8, 0, 16, 32} &&
               cut.frames[0].counts.layout.columns == 16,
           "a region counts its own pixels and the waveform spans its width");
    runtime::ScopeMeasureSettings expectedSettings;
    expectedSettings.signal = runtime::ScopeSignal::Working;
    expectedSettings.scopes = request.scopes;
    expectedSettings.region = region.region;
    if (cut.ok() && !cut.frames.empty())
        expect(cut.frames[0].counts == reference(bars, 0, expectedSettings).counts,
               "a region's tables equal the reference's");

    // A region outside the frame says the area is empty.
    auto outside = request;
    outside.region = runtime::ScopeRegion{500, 500, 10, 10};
    const auto empty = scripting::readScopes(*bars.session, outside, reads.ports());
    expect(empty.ok() && empty.frames.size() == 1 && empty.frames[0].empty &&
               empty.frames[0].counted == 0U,
           "a region outside the frame answers an empty area");

    // Half resolution measures the half-size picture (a region scales with it).
    auto half = request;
    half.resolution = scripting::ScopeReadResolution::Half;
    const auto small = scripting::readScopes(*bars.session, half, reads.ports());
    expect(small.ok() && small.frames.size() == 1 && small.frames[0].counted == 1024U &&
               small.frames[0].area == runtime::ScopeMeasuredArea{0, 0, 32, 32},
           "half resolution counts a quarter of the pixels");

    // The bars cover the frame, so every pixel has alpha 1: ignoring transparent pixels leaves the
    // count unchanged and the flag is part of the settings measured.
    auto ignoring = request;
    ignoring.ignoreTransparent = true;
    const auto kept = scripting::readScopes(*bars.session, ignoring, reads.ports());
    expect(kept.ok() && kept.frames.size() == 1 && kept.frames[0].counted == 4096U &&
               kept.settings.ignoreTransparent,
           "Ignore Transparent Pixels is applied and reported (opaque pixels stay counted)");
}

void transparentPixelsAreLeftOut(Reads& reads) {
    // One 8 x 8 white solid on a 64 x 64 composition: 64 opaque pixels and 4032 transparent ones.
    auto created = scripting::Session::createNew(
        "Patch", "Patch", core::RationalTime::fromInteger(1),
        *document::CompositionFormat::create(64, 64, core::PixelAspectRatio::square(),
                                             *document::FrameRate::create(24, 1)));
    auto session = std::move(created).takeSession();
    const auto composition = session->snapshot().project().compositions().front().id();
    commands::Transaction add("Patch", session->snapshot().revision());
    add.emplace<commands::AddSolidLayer>(composition, "Patch", core::Color4d{1.0, 1.0, 1.0, 1.0},
                                         document::Vec2d{4.0, 4.0});
    auto added = session->execute(std::move(add));
    const auto solid =
        added.command->outputId<document::NodeId>(commands::kAddSolidLayerSolidNodeOutput).value();
    for (const auto& [name, value] : {std::pair{document::kSolidWidthParameterRole, 8.0},
                                      std::pair{document::kSolidHeightParameterRole, 8.0}}) {
        const auto snapshot = session->snapshot();
        const auto* node = snapshot.project().findComposition(composition)->graph().findNode(solid);
        document::ParameterId parameter;
        for (const auto& binding : node->parameters)
            if (binding.role == name)
                parameter = binding.parameterId;
        commands::Transaction edit("Size", session->snapshot().revision());
        edit.emplace<commands::SetParameterSource>(composition, parameter,
                                                   document::ConstantValueSource{value});
        static_cast<void>(session->execute(std::move(edit)));
    }
    scripting::ScopeReadRequest request;
    request.composition = composition;
    request.signal = runtime::ScopeSignal::Working;
    request.scopes = render::kScopeTableHistogram;
    const auto everything = scripting::readScopes(*session, request, reads.ports());
    request.ignoreTransparent = true;
    const auto opaque = scripting::readScopes(*session, request, reads.ports());
    expect(everything.ok() && opaque.ok() && everything.frames[0].counted == 4096U &&
               opaque.frames[0].counted == 64U,
           "Ignore Transparent Pixels leaves out the pixels whose alpha is 0");
}

void displayAndView(const Bars& bars, Reads& reads) {
    auto request = requestFor(bars, 0);
    request.scopes = render::kScopeTableHistogram;
    const auto defaults = scripting::readScopes(*bars.session, request, reads.ports());
    expect(defaults.ok() && !defaults.settings.display.empty() && !defaults.settings.view.empty(),
           "the default display and view are named in the settings");
    if (!defaults.ok())
        return;
    auto named = request;
    named.display = defaults.settings.display;
    named.view = defaults.settings.view;
    const auto explicitRead = scripting::readScopes(*bars.session, named, reads.ports());
    expect(explicitRead.ok() && explicitRead.frames[0].counts == defaults.frames[0].counts &&
               explicitRead.settings.display == defaults.settings.display,
           "naming the default display and view measures the same");
    auto missing = request;
    missing.display = "No Such Display";
    const auto refused = scripting::readScopes(*bars.session, missing, reads.ports());
    expect(refused.error.status == scripting::ScopeReadStatus::Invalid &&
               contains(refused.error.message, "No Such Display") &&
               contains(refused.error.message, "Valid choices") &&
               contains(refused.error.message, defaults.settings.display),
           "an unknown display lists the valid ones: " + refused.error.message);
}

// ---- Refusals with their remedies ---------------------------------------------------------------

void refusals(const Bars& bars, Reads& reads) {
    const auto refusal = [&](scripting::ScopeReadRequest request) {
        return scripting::readScopes(*bars.session, std::move(request), reads.ports());
    };
    auto request = requestFor(bars, 0);

    auto unknown = request;
    unknown.composition = document::CompositionId::fromRaw(99);
    auto result = refusal(unknown);
    expect(result.error.status == scripting::ScopeReadStatus::Invalid &&
               result.error.argument == "composition" && contains(result.error.message, "99") &&
               contains(result.error.message, "compositions are"),
           "an unknown composition names the valid ones: " + result.error.message);

    auto late = request;
    late.firstFrame = late.lastFrame = 500;
    result = refusal(late);
    expect(result.error.argument == "frame" && contains(result.error.message, "frames 0 to 23") &&
               contains(result.error.message, "choose a frame"),
           "a frame outside the composition names the range: " + result.error.message);

    auto backwards = request;
    backwards.firstFrame = 5;
    backwards.lastFrame = 2;
    result = refusal(backwards);
    expect(result.error.argument == "lastFrame" && contains(result.error.message, "at or after"),
           "a range that runs backwards says what to give: " + result.error.message);

    std::string problem;
    expect(!scripting::scopeTablesForNames({"waveform", "scope"}, problem) &&
               contains(problem, "\"scope\" is not a scope") && contains(problem, "chromaticity"),
           "a bad scope name lists the valid ones: " + problem);

    auto axis = request;
    axis.axisMaximum = 3;
    axis.signal = runtime::ScopeSignal::Working;
    result = refusal(axis);
    expect(result.error.argument == "axisMaximum" &&
               contains(result.error.message, "1, 2, 4, 8 or 16"),
           "a bad axis maximum lists the valid ones: " + result.error.message);
    axis.signal = runtime::ScopeSignal::Display;
    axis.axisMaximum = 4;
    result = refusal(axis);
    expect(result.error.argument == "axisMaximum" &&
               contains(result.error.message, "working signal only"),
           "an axis maximum on the display signal is refused: " + result.error.message);

    auto flat = request;
    flat.region = runtime::ScopeRegion{0, 0, 0, 10};
    result = refusal(flat);
    expect(result.error.argument == "region" && contains(result.error.message, "at least 1"),
           "an empty region rectangle is refused: " + result.error.message);

    auto working = request;
    working.signal = runtime::ScopeSignal::Working;
    working.view = "sRGB";
    result = refusal(working);
    expect(result.error.argument == "view" && contains(result.error.message, "display signal only"),
           "a view on the working signal is refused: " + result.error.message);

    auto wide = request;
    wide.output = scripting::ScopeReadOutput::Image;
    wide.width = 5000;
    result = refusal(wide);
    expect(result.error.argument == "width" && contains(result.error.message, "128 to 2048"),
           "an image width out of bounds names the bounds: " + result.error.message);
    auto tall = request;
    tall.output = scripting::ScopeReadOutput::Image;
    tall.image = scripting::ScopeReadImageKind::Waveform;
    tall.height = 10;
    result = refusal(tall);
    expect(result.error.argument == "height" && contains(result.error.message, "128 to 2048"),
           "an image height out of bounds names the bounds: " + result.error.message);
    auto stacked = request;
    stacked.output = scripting::ScopeReadOutput::Image;
    stacked.image = scripting::ScopeReadImageKind::All;
    stacked.height = 400;
    result = refusal(stacked);
    expect(result.error.argument == "height" && contains(result.error.message, "single scope"),
           "a height with all five scopes says what to do: " + result.error.message);
    auto sized = request;
    sized.width = 300;
    result = refusal(sized);
    expect(result.error.argument == "width" &&
               contains(result.error.message, "result to \"image\""),
           "a picture size without an image is refused: " + result.error.message);

    auto many = request;
    many.lastFrame = 5000;
    result = refusal(many);
    expect(result.error.argument == "lastFrame", "a range past the composition is refused");
}

} // namespace

int main() {
    Bars bars;
    Reads reads;
    tablesMatchTheReference(bars, reads);
    workingTablesAreTheBarsOwn(bars, reads);
    summaryMatches(bars, reads);
    pictures(bars, reads);
    rangeProgressAndCancel(bars, reads);
    warmSidecars(bars, reads);
    warmSidecarOfAnotherDetail(bars, reads);
    settingsChangeTheResult(bars, reads);
    transparentPixelsAreLeftOut(reads);
    displayAndView(bars, reads);
    refusals(bars, reads);
    if (failures != 0) {
        std::cerr << failures << " scope read check(s) failed\n";
        return 1;
    }
    return 0;
}
