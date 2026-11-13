#include <bloom/scripting/scope_read.hpp>

#include <bloom/core/frame_time_mapping.hpp>
#include <bloom/document/node_definition_registry.hpp>
#include <bloom/output/display_output.hpp>
#include <bloom/render/image_types.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/input_color_context.hpp>
#include <bloom/runtime/layer_bounds_query.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/scripting/render.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <new>
#include <thread>
#include <utility>

namespace bloom::scripting {
namespace {

using runtime::ScopeSignal;
namespace scopes = runtime::scopes;

constexpr std::array<std::string_view, 6> kImageNames{"waveform",  "parade",       "vectorscope",
                                                      "histogram", "chromaticity", "all"};
constexpr std::array<std::string_view, 3> kResolutionNames{"full", "half", "quarter"};

[[nodiscard]] std::string joinQuoted(const std::span<const std::string_view> names) {
    std::string text;
    for (const auto name : names) {
        text += text.empty() ? "\"" : ", \"";
        text += std::string(name);
        text += '"';
    }
    return text;
}

[[nodiscard]] ScopeReadResult refused(ScopeReadRequest request, const ScopeReadStatus status,
                                      std::string argument, std::string message) {
    ScopeReadResult result;
    result.settings = std::move(request);
    result.error = {
        .status = status, .argument = std::move(argument), .message = std::move(message)};
    return result;
}

[[nodiscard]] std::uint32_t tablesForImage(const ScopeReadImageKind kind) noexcept {
    switch (kind) {
    case ScopeReadImageKind::Waveform:
    case ScopeReadImageKind::Parade:
        return render::kScopeTableWaveform;
    case ScopeReadImageKind::Vectorscope:
        return render::kScopeTableVectorscope;
    case ScopeReadImageKind::Histogram:
        return render::kScopeTableHistogram;
    case ScopeReadImageKind::Chromaticity:
        return render::kScopeTableChromaticity;
    case ScopeReadImageKind::All:
        break;
    }
    return render::kScopeTableAll;
}

[[nodiscard]] scopes::Kind pictureKind(const ScopeReadImageKind kind) noexcept {
    switch (kind) {
    case ScopeReadImageKind::Waveform:
        return scopes::Kind::Waveform;
    case ScopeReadImageKind::Parade:
        return scopes::Kind::Parade;
    case ScopeReadImageKind::Vectorscope:
        return scopes::Kind::Vectorscope;
    case ScopeReadImageKind::Histogram:
        return scopes::Kind::Histogram;
    case ScopeReadImageKind::Chromaticity:
    case ScopeReadImageKind::All:
        break;
    }
    return scopes::Kind::Chromaticity;
}

[[nodiscard]] std::uint32_t divisorOf(const ScopeReadResolution resolution) noexcept {
    switch (resolution) {
    case ScopeReadResolution::Full:
        return 1;
    case ScopeReadResolution::Half:
        return 2;
    case ScopeReadResolution::Quarter:
        break;
    }
    return 4;
}

[[nodiscard]] runtime::EvaluationResolution
evaluationResolution(const document::Composition& composition,
                     const ScopeReadResolution resolution) {
    const auto divisor = divisorOf(resolution);
    if (divisor == 1)
        return runtime::CompositionFormatResolution{};
    const auto format = composition.format();
    const auto extent = render::ImageExtent::create((format.width() + divisor - 1) / divisor,
                                                    (format.height() + divisor - 1) / divisor);
    if (!extent.hasValue())
        return runtime::CompositionFormatResolution{};
    return runtime::ProxyResolution{*extent.value()};
}

[[nodiscard]] std::string compositionIds(const document::Snapshot& snapshot) {
    std::string text;
    for (const auto& composition : snapshot.project().compositions())
        text += (text.empty() ? "" : ", ") + std::to_string(composition.id().value());
    return text.empty() ? "none" : text;
}

[[nodiscard]] std::string displayChoices(const std::span<const color::DisplayViewEntry> displays) {
    std::string text;
    std::size_t listed = 0;
    for (const auto& pair : displays) {
        if (listed == 24) {
            text += "; ...";
            break;
        }
        text += (text.empty() ? "" : "; ") + std::string("display \"") + pair.display +
                "\" with view \"" + pair.view + "\"";
        ++listed;
    }
    return text;
}

[[nodiscard]] std::size_t digitsOf(std::uint32_t value) noexcept {
    std::size_t digits = 1;
    while (value >= 10U) {
        value /= 10U;
        ++digits;
    }
    return digits;
}

// Puts `cells` into `table` as Dense values or Sparse [index, count] pairs, adding the numbers'
// JSON text size to `bytes`.
void fill(ScopeTableData& table, const std::vector<std::uint32_t>& cells,
          const ScopeReadEncoding encoding, std::size_t& bytes) {
    if (encoding == ScopeReadEncoding::Dense) {
        table.values = cells;
    } else {
        for (std::size_t index = 0; index < cells.size(); ++index) {
            if (cells[index] == 0U)
                continue;
            table.values.push_back(static_cast<std::uint32_t>(index));
            table.values.push_back(cells[index]);
        }
    }
    for (const auto value : table.values)
        bytes += digitsOf(value) + 1;
}

void countBytes(const ScopeTableData& table, std::size_t& bytes) {
    for (const auto value : table.under)
        bytes += digitsOf(value) + 1;
    for (const auto value : table.over)
        bytes += digitsOf(value) + 1;
}

struct PictureSizes final {
    int width = 0;
    int height = 0;
};

} // namespace

// ---- Vocabulary

std::string_view scopeImageKindName(const ScopeReadImageKind kind) noexcept {
    return kImageNames[static_cast<std::size_t>(kind)];
}

std::optional<ScopeReadImageKind> scopeImageKindFromName(const std::string_view name) {
    for (std::size_t index = 0; index < kImageNames.size(); ++index)
        if (kImageNames[index] == name)
            return static_cast<ScopeReadImageKind>(index);
    return std::nullopt;
}

std::string_view scopeResolutionName(const ScopeReadResolution resolution) noexcept {
    return kResolutionNames[static_cast<std::size_t>(resolution)];
}

std::optional<ScopeReadResolution> scopeResolutionFromName(const std::string_view name) {
    for (std::size_t index = 0; index < kResolutionNames.size(); ++index)
        if (kResolutionNames[index] == name)
            return static_cast<ScopeReadResolution>(index);
    return std::nullopt;
}

std::optional<std::uint32_t> scopeTablesForNames(const std::vector<std::string>& names,
                                                 std::string& error) {
    std::uint32_t tables = 0;
    for (const auto& name : names) {
        if (name == "waveform" || name == "parade")
            tables |= render::kScopeTableWaveform;
        else if (name == "vectorscope")
            tables |= render::kScopeTableVectorscope;
        else if (name == "histogram")
            tables |= render::kScopeTableHistogram;
        else if (name == "chromaticity")
            tables |= render::kScopeTableChromaticity;
        else {
            error = "\"" + name + "\" is not a scope; the scopes are " + joinQuoted(kScopeNames) +
                    " (parade uses the waveform table)";
            return std::nullopt;
        }
    }
    if (tables == 0U) {
        error = "scopes is empty; name at least one of " + joinQuoted(kScopeNames) +
                ", or leave it out for all five";
        return std::nullopt;
    }
    return tables;
}

std::vector<std::string> scopeNamesForTables(const std::uint32_t scopes) {
    std::vector<std::string> names;
    if ((scopes & render::kScopeTableWaveform) != 0U) {
        names.emplace_back("waveform");
        names.emplace_back("parade");
    }
    if ((scopes & render::kScopeTableVectorscope) != 0U)
        names.emplace_back("vectorscope");
    if ((scopes & render::kScopeTableHistogram) != 0U)
        names.emplace_back("histogram");
    if ((scopes & render::kScopeTableChromaticity) != 0U)
        names.emplace_back("chromaticity");
    return names;
}

// ---- Routes

runtime::ScopeFrameRoutes
headlessScopeRoutes(const std::shared_ptr<host::GpuExportProvider>& provider,
                    runtime::TaskScheduler& scheduler, const std::function<bool()>& cancelled) {
    runtime::ScopeFrameRoutes routes;
    if (provider == nullptr)
        return routes;
    provider->prepare(scheduler);
    while (!provider->prepared()) {
        if (cancelled && cancelled())
            return routes;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    routes.device = [provider] { return provider->evaluator(); };
    routes.ocio = [provider] { return provider->sceneOcioContext(); };
    return routes;
}

// ---- Time

std::optional<std::uint64_t> scopeFrameForTime(const document::Snapshot& snapshot,
                                               const document::CompositionId composition,
                                               const core::RationalTime time) {
    const auto* found = snapshot.project().findComposition(composition);
    if (found == nullptr)
        return std::nullopt;
    const auto rate = found->format().frameRate();
    const auto mapping =
        core::FrameTimeMapping::create(found->duration(), rate.numerator(), rate.denominator());
    if (!mapping.hasValue())
        return std::nullopt;
    return mapping.value()->nearestFrameIndex(time);
}

// ---- Tables

ScopeTablesData describeScopeTables(const runtime::ScopeCounts& counts,
                                    const ScopeReadEncoding encoding) {
    ScopeTablesData data;
    const auto& layout = counts.layout;
    const auto& words = counts.words;
    if (words.size() < layout.wordCount)
        return data;
    if (layout.has(render::kScopeTableHistogram)) {
        ScopeTableData table;
        table.shape = {render::kScopeChannels, render::kScopeCodes};
        table.order = "channel (red, green, blue, luma), then code 0 to 1023";
        table.underOverShape = {render::kScopeChannels};
        std::vector<std::uint32_t> cells;
        cells.reserve(std::size_t{render::kScopeChannels} * render::kScopeCodes);
        for (std::uint32_t channel = 0; channel < render::kScopeChannels; ++channel) {
            const auto base =
                layout.histogramOffset + channel * render::kScopeHistogramChannelWords;
            cells.insert(cells.end(), words.begin() + base,
                         words.begin() + base + render::kScopeCodes);
            table.under.push_back(words[base + render::kScopeCodeUnder]);
            table.over.push_back(words[base + render::kScopeCodeOver]);
        }
        fill(table, cells, encoding, data.jsonBytes);
        countBytes(table, data.jsonBytes);
        data.histogram = std::move(table);
    }
    if (layout.has(render::kScopeTableWaveform)) {
        ScopeTableData table;
        table.shape = {render::kScopeChannels, layout.columns, render::kScopeWaveformLevels};
        table.order = "channel (red, green, blue, luma), then column from the left of the measured "
                      "area, then level 0 (code 0 to 1) to 511 (code 1022 to 1023)";
        table.underOverShape = {render::kScopeChannels, layout.columns};
        std::vector<std::uint32_t> cells;
        cells.reserve(std::size_t{render::kScopeChannels} * layout.columns *
                      render::kScopeWaveformLevels);
        for (std::uint32_t channel = 0; channel < render::kScopeChannels; ++channel) {
            for (std::uint32_t column = 0; column < layout.columns; ++column) {
                const auto base = layout.waveformOffset + (channel * layout.columns + column) *
                                                              render::kScopeWaveformColumnWords;
                cells.insert(cells.end(), words.begin() + base,
                             words.begin() + base + render::kScopeWaveformLevels);
                table.under.push_back(words[base + render::kScopeWaveformLevelUnder]);
                table.over.push_back(words[base + render::kScopeWaveformLevelOver]);
            }
        }
        fill(table, cells, encoding, data.jsonBytes);
        countBytes(table, data.jsonBytes);
        data.waveform = std::move(table);
    }
    const auto grid = [&](const std::uint32_t offset, std::string order) {
        ScopeTableData table;
        table.shape = {render::kScopeCells, render::kScopeCells};
        table.order = std::move(order);
        const std::vector<std::uint32_t> cells(words.begin() + offset,
                                               words.begin() + offset +
                                                   render::kScopeCells * render::kScopeCells);
        fill(table, cells, encoding, data.jsonBytes);
        return table;
    };
    if (layout.has(render::kScopeTableVectorscope))
        data.vectorscope = grid(layout.vectorscopeOffset,
                                "row = Cr cell 0 to 511 (Cr -0.5 to 0.5, upward), column = Cb cell "
                                "0 to 511 (Cb -0.5 to 0.5, to the right)");
    if (layout.has(render::kScopeTableChromaticity))
        data.chromaticity =
            grid(layout.chromaticityOffset,
                 "row = y cell 0 to 511 (CIE y 0 to 0.9, upward), column = x cell 0 "
                 "to 511 (CIE x 0 to 0.8, to the right)");
    return data;
}

// ---- The read

namespace {

struct Job final {
    SessionRenderSourceSnapshot source;
    ScopeReadRequest request;
};

[[nodiscard]] ScopeReadColor colorOf(const runtime::ScopeSignalColor& color) {
    ScopeReadColor result;
    result.luma = color.luma();
    const auto& colorimetry = color.colorimetry();
    result.red = {colorimetry.red.x, colorimetry.red.y};
    result.green = {colorimetry.green.x, colorimetry.green.y};
    result.blue = {colorimetry.blue.x, colorimetry.blue.y};
    result.white = {colorimetry.white.x, colorimetry.white.y};
    result.colorSpace = colorimetry.colorSpaceName;
    if (color.signal() == ScopeSignal::Display) {
        result.display = color.display();
        result.view = color.view();
    }
    return result;
}

// Draws the requested picture of one frame's tables; false with `message` when it cannot.
[[nodiscard]] bool drawPicture(const ScopeReadRequest& request, const runtime::ScopeTables* tables,
                               const runtime::ScopeSignalColor& color,
                               const runtime::CancellationToken& cancellation,
                               ScopeReadFrame& frame, ScopeReadError& error) {
    scopes::PictureInput input;
    input.tables = tables;
    input.luma = &color.luma();
    input.colorimetry = &color.colorimetry();
    input.signal = request.signal;
    input.axisMaximum = request.axisMaximum;
    input.gain = request.drawing.brightness;
    input.waveformChannels = request.drawing.waveformChannels;
    input.histogramChannels = request.drawing.histogramChannels;
    input.histogramScale = request.drawing.histogramScale;
    const scopes::PictureOptions options;
    scopes::ScopeImage image;
    if (request.image == ScopeReadImageKind::All) {
        auto picture = scopes::renderAllScopesPicture(request.width, input, options, cancellation,
                                                      kScopeImageByteLimit);
        if (picture.error == scopes::PictureError::Cancelled) {
            error = {ScopeReadStatus::Cancelled, {}, "The read was cancelled"};
            return false;
        }
        if (!picture.ok()) {
            error = {ScopeReadStatus::Invalid, "width",
                     "The picture of all five scopes cannot be drawn at this width; use a width "
                     "from 128 to 2048 pixels"};
            return false;
        }
        image = std::move(picture.image);
    } else {
        auto picture =
            scopes::renderScopePicture(pictureKind(request.image), request.width, request.height,
                                       input, options, cancellation, kScopeImageByteLimit);
        if (picture.error == scopes::PictureError::Cancelled) {
            error = {ScopeReadStatus::Cancelled, {}, "The read was cancelled"};
            return false;
        }
        if (!picture.ok()) {
            error = {ScopeReadStatus::Invalid, "width",
                     "The picture cannot be drawn at this size; use a width from 128 to 2048 and a "
                     "height from 128 to 2048 pixels"};
            return false;
        }
        image = std::move(picture.image);
    }
    auto png = scopes::encodeScopePng(image, kScopeImageByteLimit);
    if (!png.ok()) {
        error = {ScopeReadStatus::Failed, {}, "The picture could not be encoded as PNG"};
        return false;
    }
    frame.png = std::move(png.bytes);
    frame.imageWidth = image.width;
    frame.imageHeight = image.height;
    return true;
}

// The task body: compile once, prepare the signal's color once, then measure each frame.
[[nodiscard]] ScopeReadResult execute(const Job& job, const ScopeReadPorts& ports,
                                      const runtime::CancellationToken& cancellation,
                                      std::atomic<std::uint64_t>& done) {
    ScopeReadResult result;
    result.settings = job.request;
    auto& request = result.settings;
    const auto& source = job.source;
    result.revision = source.snapshot.revision().value();
    const auto fail = [&result](const ScopeReadStatus status, std::string argument,
                                std::string message) {
        result.error = {
            .status = status, .argument = std::move(argument), .message = std::move(message)};
        return std::move(result);
    };
    const auto* composition = source.snapshot.project().findComposition(request.composition);
    if (composition == nullptr)
        return fail(ScopeReadStatus::Failed, {}, "The composition is no longer in the project");
    result.compositionWidth = composition->format().width();
    result.compositionHeight = composition->format().height();
    const auto colorIntent = captureRenderColorIntent(source, *composition);
    if (!colorIntent)
        return fail(ScopeReadStatus::Failed, {},
                    "The project's color settings are unavailable, so no scope can be measured; "
                    "open or reload the project");
    const auto rate = composition->format().frameRate();
    const auto mapping = core::FrameTimeMapping::create(composition->duration(), rate.numerator(),
                                                        rate.denominator());
    if (!mapping.hasValue())
        return fail(ScopeReadStatus::Failed, {}, "The composition's frame grid is unavailable");

    // The signal's color objects.
    auto config = runtime::detail::resolveInputColorConfig(*colorIntent);
    if (!config)
        return fail(ScopeReadStatus::Failed, {},
                    "The project's color configuration could not be read; check Project Settings > "
                    "Color");
    // The Display signal's display and view: the ones named, else the pair a capture or an export
    // of the composition uses by default (the Viewer's default for the configuration).
    std::string display;
    std::string view;
    if (request.signal == ScopeSignal::Display) {
        const auto chosen =
            output::outputDisplayViewV1(config->displays(), request.display, request.view);
        display = chosen.display;
        view = chosen.view;
        const auto displays = config->displays();
        const bool known = std::ranges::any_of(displays, [&](const auto& pair) {
            return pair.display == display && pair.view == view;
        });
        if (!known) {
            const bool displayKnown = std::ranges::any_of(
                displays, [&](const auto& pair) { return pair.display == request.display; });
            return fail(ScopeReadStatus::Invalid,
                        !request.display.empty() && !displayKnown ? "display" : "view",
                        "The color configuration has no display \"" + display + "\" with view \"" +
                            view + "\". Valid choices: " + displayChoices(displays) +
                            ". Leave display and view out for the default pair");
        }
    }
    const auto prepared = runtime::ScopeSignalColor::prepare(
        {.config = std::make_shared<const color::ResolvedBloomNeutralConfig>(std::move(*config)),
         .signal = request.signal,
         .display = display,
         .view = view,
         .viewAdjust = {}});
    if (!prepared)
        return fail(ScopeReadStatus::Failed, {},
                    prepared.message.empty()
                        ? std::string("The scope's color could not be prepared")
                        : prepared.message);
    const auto& color = *prepared.color;
    result.color = colorOf(color);
    request.display = result.color.display;
    request.view = result.color.view;

    // The plan.
    const runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
    const auto compiled = compiler.compile(
        {.snapshot = source.snapshot, .compositionId = request.composition}, cancellation);
    if (compiled.status == runtime::SnapshotCompileStatus::Cancelled)
        return fail(ScopeReadStatus::Cancelled, {}, "The read was cancelled");
    if (compiled.status != runtime::SnapshotCompileStatus::Compiled || compiled.plan == nullptr) {
        std::string reason = "The composition cannot be compiled";
        for (const auto& diagnostic : compiled.diagnostics)
            if (!diagnostic.summary.empty()) {
                reason = diagnostic.summary;
                break;
            }
        return fail(ScopeReadStatus::Failed, {}, reason);
    }
    const auto& plan = compiled.plan;

    // The routes: the session's device when it has a working one, the CPU reference always.
    runtime::ScopeFrameRoutes routes = ports.routes;
    const runtime::CpuCompositionEvaluator cpu;
    cpu.setAssetBaseDirectory(source.assetBaseDirectory);
    if (!routes.evaluateCpu)
        routes.evaluateCpu = [&cpu](std::shared_ptr<const runtime::CompiledCompositionPlan> frame,
                                    const runtime::EvaluationRequest& evaluation,
                                    const runtime::CancellationToken& token) {
            return cpu.evaluate(std::move(frame), evaluation, token);
        };
    const auto device = routes.device ? routes.device() : nullptr;
    const bool preferDevice = device != nullptr && device->gpuAvailable();

    runtime::ScopeMeasureSettings settings;
    settings.scopes =
        request.output == ScopeReadOutput::Image ? tablesForImage(request.image) : request.scopes;
    settings.signal = request.signal;
    settings.axisMaximum = request.axisMaximum;
    settings.region = request.region;
    settings.ignoreTransparent = request.ignoreTransparent;
    request.scopes = settings.scopes;
    const auto resolution = evaluationResolution(*composition, request.resolution);

    for (auto index = request.firstFrame; index <= request.lastFrame; ++index) {
        if (cancellation.isCancellationRequested())
            return fail(ScopeReadStatus::Cancelled, {}, "The read was cancelled");
        const auto time = mapping.value()->timeForFrame(index);
        if (!time.hasValue())
            return fail(ScopeReadStatus::Invalid, "frame",
                        "Frame " + std::to_string(index) + " is outside the composition");
        ScopeReadFrame frame;
        frame.frame = index;
        frame.time = *time.value();

        std::optional<ScopeWarmHit> hit;
        if (ports.warm)
            hit = ports.warm(
                {.composition = request.composition,
                 .time = frame.time,
                 .resolution = resolution,
                 .quality = runtime::EvaluationQuality::Reference,
                 .colorIntent = *colorIntent,
                 .display = request.display,
                 .view = request.view,
                 .defaultDisplayView = job.request.display.empty() && job.request.view.empty(),
                 .settings = settings});
        runtime::ScopeTables tables;
        if (hit) {
            frame.fromCache = true;
            frame.route = hit->tables.route();
            frame.routeNote = hit->note;
            frame.area = hit->area;
            frame.counts = hit->tables.unpack();
            tables = std::move(hit->tables);
        } else {
            const runtime::EvaluationRequest evaluation{
                .time = frame.time,
                .output = plan->output(),
                .resolution = resolution,
                .quality = runtime::EvaluationQuality::Reference,
                .colorIntent = *colorIntent,
                .pixelStorageByteLimit = runtime::kDefaultLayerBoundsPixelBudget};
            auto measured = runtime::measureScopeFrame(
                routes, plan, evaluation, settings, color, preferDevice, cancellation,
                std::optional<std::filesystem::path>(source.assetBaseDirectory));
            switch (measured.status) {
            case runtime::ScopeFrameStatus::Cancelled:
                return fail(ScopeReadStatus::Cancelled, {}, "The read was cancelled");
            case runtime::ScopeFrameStatus::Failed:
            case runtime::ScopeFrameStatus::Unavailable:
                return fail(ScopeReadStatus::Failed, {},
                            "Frame " + std::to_string(index) + " could not be measured: " +
                                (measured.message.empty()
                                     ? std::string("the frame did not evaluate")
                                     : measured.message));
            case runtime::ScopeFrameStatus::EmptyArea:
                frame.empty = true;
                break;
            case runtime::ScopeFrameStatus::Measured:
                break;
            }
            frame.route = measured.route;
            frame.area = measured.area;
            if (!frame.empty) {
                frame.routeNote = measured.message;
                if (frame.routeNote.empty() && measured.route == runtime::ScopeRoute::Cpu &&
                    !preferDevice)
                    frame.routeNote = "No GPU device was available to this session; the CPU "
                                      "reference counted the frame.";
                tables = runtime::ScopeTables::pack(measured.counts, measured.route);
                frame.counts = std::move(measured.counts);
            }
        }
        if (!frame.empty)
            frame.counted = frame.counts.counted();
        if (request.output == ScopeReadOutput::Summary && !frame.empty)
            frame.summary = runtime::summarizeScopes(frame.counts);
        if (request.output == ScopeReadOutput::Image) {
            ScopeReadError error;
            if (!drawPicture(request, frame.empty ? nullptr : &tables, color, cancellation, frame,
                             error))
                return fail(error.status, error.argument, error.message);
        }
        // Tables stay only for the output that returns them.
        if (request.output != ScopeReadOutput::Tables)
            frame.counts = {};
        result.frames.push_back(std::move(frame));
        done.store(index - request.firstFrame + 1);
    }
    return result;
}

} // namespace

ScopeReadResult readScopes(const Session& session, ScopeReadRequest request,
                           const ScopeReadPorts& ports) {
    if (ports.scheduler == nullptr)
        return refused(std::move(request), ScopeReadStatus::Failed, {},
                       "The read has no scheduler to run on");
    Job job{.source = session.renderSourceSnapshot(), .request = {}};
    const auto& snapshot = job.source.snapshot;
    const auto* composition = snapshot.project().findComposition(request.composition);
    if (composition == nullptr)
        return refused(std::move(request), ScopeReadStatus::Invalid, "composition",
                       "Composition " + std::to_string(request.composition.value()) +
                           " does not exist; the project's compositions are " +
                           compositionIds(snapshot) + ". Pick one of them");

    // Frames.
    const auto rate = composition->format().frameRate();
    const auto mapping = core::FrameTimeMapping::create(composition->duration(), rate.numerator(),
                                                        rate.denominator());
    if (!mapping.hasValue())
        return refused(std::move(request), ScopeReadStatus::Failed, {},
                       "The composition's frame grid is unavailable");
    const auto lastValid = mapping.value()->maximumFrameIndex();
    const auto range = "0 to " + std::to_string(lastValid);
    if (request.firstFrame > lastValid)
        return refused(std::move(request), ScopeReadStatus::Invalid, "frame",
                       "Frame " + std::to_string(request.firstFrame) +
                           " is outside the composition, which has frames " + range +
                           "; choose a frame in that range");
    if (request.lastFrame < request.firstFrame)
        return refused(std::move(request), ScopeReadStatus::Invalid, "lastFrame",
                       "lastFrame " + std::to_string(request.lastFrame) +
                           " is before the first frame " + std::to_string(request.firstFrame) +
                           "; give a lastFrame at or after it");
    if (request.lastFrame > lastValid)
        return refused(std::move(request), ScopeReadStatus::Invalid, "lastFrame",
                       "lastFrame " + std::to_string(request.lastFrame) +
                           " is outside the composition, which has frames " + range +
                           "; choose a lastFrame in that range");
    if (request.lastFrame - request.firstFrame >= kScopeReadMaximumFrames)
        return refused(std::move(request), ScopeReadStatus::Invalid, "lastFrame",
                       "A read covers at most " + std::to_string(kScopeReadMaximumFrames) +
                           " frames; split the range into several reads");

    // The signal.
    if (request.signal == ScopeSignal::Display) {
        if (request.axisMaximum != 1)
            return refused(std::move(request), ScopeReadStatus::Invalid, "axisMaximum",
                           "axisMaximum applies to the working signal only; the display signal's "
                           "axis is always 0 to 1. Leave axisMaximum out or set signal to "
                           "\"working\"");
    } else {
        if (!runtime::validScopeAxisMaximum(request.axisMaximum))
            return refused(std::move(request), ScopeReadStatus::Invalid, "axisMaximum",
                           "axisMaximum is " + std::string("1, 2, 4, 8 or 16 (the largest linear "
                                                           "value the axis shows)"));
        if (!request.display.empty() || !request.view.empty())
            return refused(std::move(request), ScopeReadStatus::Invalid,
                           request.display.empty() ? "view" : "display",
                           "display and view apply to the display signal only; leave them out or "
                           "set signal to \"display\"");
    }
    if (request.region) {
        if (request.region->width == 0U || request.region->height == 0U)
            return refused(
                std::move(request), ScopeReadStatus::Invalid, "region",
                "region is [x, y, width, height] in composition pixels and its width and "
                "height must be at least 1; leave region out to measure the whole frame");
    }
    if (request.drawing.brightness < kScopeBrightnessMinimum ||
        request.drawing.brightness > kScopeBrightnessMaximum ||
        request.drawing.brightness != request.drawing.brightness)
        return refused(std::move(request), ScopeReadStatus::Invalid, "brightness",
                       "brightness is a gain from 0.1 to 16 (1 is the panel's default)");

    // What is measured and returned.
    if (request.output == ScopeReadOutput::Image) {
        if (request.width == 0)
            request.width = kScopeImageDefaultWidth;
        if (request.width < kScopeImageMinimumSide || request.width > kScopeImageMaximumWidth)
            return refused(std::move(request), ScopeReadStatus::Invalid, "width",
                           "width is " + std::to_string(kScopeImageMinimumSide) + " to " +
                               std::to_string(kScopeImageMaximumWidth) +
                               " pixels; pick a width in that range");
        if (request.image == ScopeReadImageKind::All) {
            if (request.height != 0)
                return refused(std::move(request), ScopeReadStatus::Invalid, "height",
                               "height applies to a single scope; the picture of all five scopes "
                               "takes its height from its width. Leave height out or name one "
                               "scope in image");
        } else {
            if (request.height == 0) {
                scopes::GraticuleRequest sizing;
                sizing.kind = pictureKind(request.image);
                sizing.histogramChannels = request.drawing.histogramChannels;
                sizing.measure = scopes::embeddedTextMeasure(1.0);
                request.height = scopes::scopeHeightForWidth(sizing, request.width);
            }
            if (request.height < kScopeImageMinimumSide ||
                request.height > kScopeImageMaximumHeight)
                return refused(std::move(request), ScopeReadStatus::Invalid, "height",
                               "height is " + std::to_string(kScopeImageMinimumSide) + " to " +
                                   std::to_string(kScopeImageMaximumHeight) +
                                   " pixels; pick a height in that range");
        }
    } else {
        if (request.width != 0 || request.height != 0)
            return refused(std::move(request), ScopeReadStatus::Invalid,
                           request.width != 0 ? "width" : "height",
                           "width and height size a picture; set result to \"image\" or leave them "
                           "out");
        if (request.output == ScopeReadOutput::Summary) {
            // The summary reads the Histogram and the Vectorscope.
            request.scopes &= render::kScopeTableHistogram | render::kScopeTableVectorscope;
            if (request.scopes == 0U)
                return refused(std::move(request), ScopeReadStatus::Invalid, "scopes",
                               "A summary is computed from the histogram and the vectorscope; "
                               "include one of them in scopes, or leave scopes out");
        }
    }
    job.request = std::move(request);

    // One task on the caller's scheduler; this thread forwards progress and cancellation.
    const auto total = job.request.lastFrame - job.request.firstFrame + 1;
    std::atomic<std::uint64_t> done{0};
    auto submission = ports.scheduler->submit<std::shared_ptr<ScopeReadResult>>(
        runtime::TaskRequest(
            "Read scopes",
            {.kind = runtime::TaskOwnerKind::Application, .id = runtime::TaskOwnerId::fromRaw(4)},
            runtime::TaskPriority::Foreground, runtime::TaskExecutor::Cpu),
        [&job, &ports, &done](runtime::TaskContext& context) {
            using Result = runtime::TaskResult<std::shared_ptr<ScopeReadResult>>;
            if (context.isCancellationRequested())
                return Result::cancelled();
            auto value = std::make_shared<ScopeReadResult>();
            try {
                *value = execute(job, ports, context.cancellation(), done);
            } catch (const std::bad_alloc&) {
                value->settings = job.request;
                value->error = {ScopeReadStatus::Failed,
                                {},
                                "The read ran out of memory; ask for fewer frames, a smaller "
                                "region, half or quarter resolution, or fewer scopes"};
            } catch (const std::exception& error) {
                value->settings = job.request;
                value->error = {ScopeReadStatus::Failed, {}, error.what()};
            }
            if (value->error.status == ScopeReadStatus::Cancelled ||
                context.isCancellationRequested())
                return Result::cancelled();
            return Result::succeeded(std::move(value));
        });
    if (!submission.accepted()) {
        auto result = refused(job.request, ScopeReadStatus::Failed, {},
                              "The scheduler is not accepting work; try again, or restart Bloom if "
                              "it is shutting down");
        return result;
    }
    const auto handle = submission.handle;
    ScopeReadResult outcome;
    bool cancelSent = false;
    std::uint64_t reported = 0;
    for (;;) {
        if (auto taken = handle.tryTakeResult()) {
            std::shared_ptr<ScopeReadResult> shared;
            if (taken->state() == runtime::TaskState::Succeeded && taken->value() != nullptr)
                shared = *taken->value();
            if (shared != nullptr) {
                outcome = std::move(*shared);
            } else {
                outcome =
                    refused(job.request, ScopeReadStatus::Cancelled, {}, "The read was cancelled");
            }
            break;
        }
        // Asked on every turn, so a caller can tell that its cancel has been passed on.
        if (ports.cancelled && ports.cancelled() && !cancelSent) {
            handle.cancel();
            cancelSent = true;
        }
        if (const auto finished = done.load(); finished != reported) {
            reported = finished;
            if (ports.progress)
                ports.progress(finished, total);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (const auto finished = done.load(); finished != reported && ports.progress)
        ports.progress(finished, total);
    if (outcome.error.status == ScopeReadStatus::Cancelled && outcome.error.message.empty())
        outcome.error.message = "The read was cancelled";
    return outcome;
}

} // namespace bloom::scripting
