// #642: an SVG imported as one layer (a Vector Artwork layer) on the production GPU route
// (docs/architecture/vector-artwork.md, "Compiling a Vector Artwork layer").
//
// The snapshot compiler expands the layer into the nodes of the editable import and draws the
// layer's transform through the artwork's geometry; nothing here is a pixel operation of its own,
// so the GPU route is the production CpuGpuSceneBuilder and GpuSceneExecutor over the ordinary,
// covered lowerings. Proven against the CPU reference (2e-6):
//
// - 512 x 288 over frames 0 to 2 with the layer's position, scale, rotation and opacity keyed
//   (scaled up and turned: the sharp, composed route), cold and warm, with the bounds the CPU
//   publishes; the same at half resolution.
// - The one layer equals the editable import of the same file on the device, whole frame, on the
//   in-repo synthetic files (gradients, clips, masks, opacity groups, blur, dashes, runs of more
//   than 256 siblings).
// - 1920 x 1080 and 3840 x 2160 prepare and run on the device (no comparison at these sizes; the
//   export route's provenance is read by bloom.ui.viewer_export_svg_artwork).
// - Admission (the phase admits the scene; a machine too small refuses it and holds nothing),
//   retirement (releasing the scene returns every claimed byte), cancellation (a cancelled
//   preparation and a cancelled run publish nothing) and the executor's request budget.
//
// The CPU assertions always run. Without a device the GPU half reports an explicit SKIP (exit 77)
// after the CPU half passed; --require-device makes that a failure.

#include "document_render_parity_support.hpp"
#include "gpu_coverage_ocio_support.hpp"

#include <bloom/commands/animation_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/commands/svg_import_operations.hpp>
#include <bloom/document/merge_entries.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/vector_artwork_nodes.hpp>
#include <bloom/runtime/gpu_processing_phase_policy.hpp>
#include <bloom/runtime/task_scheduler.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>

namespace {

using namespace bloom;
using namespace bloom::test::document_render;
using render::composite_proof::Options;
using render::composite_proof::parseOptions;

constexpr std::uint64_t kGib = 1ULL << 30;

std::filesystem::path writeSvg(const std::string& name, const std::string& text) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto path = std::filesystem::temp_directory_path() /
                      ("bloom-gpu-svg-layer-" + name + "-" + std::to_string(stamp) + ".svg");
    std::ofstream(path, std::ios::binary) << text;
    return path;
}

// Gradients, a clip, a mask, nested opacity, a blurred element, dashes and a compound path.
const std::string kComposite = R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="120">
<defs><linearGradient id="g" x1="0" x2="1"><stop offset="0" stop-color="#ff0"/><stop offset="1" stop-color="#08f"/></linearGradient>
<radialGradient id="r" gradientTransform="translate(0.5 0.5) scale(0.5 0.3) translate(-0.5 -0.5)"><stop offset="0" stop-color="#fff"/><stop offset="1" stop-color="#f0f" stop-opacity="0.4"/></radialGradient>
<clipPath id="c"><circle cx="60" cy="60" r="40"/></clipPath>
<mask id="m"><rect x="120" y="10" width="70" height="100" fill="#fff"/><circle cx="155" cy="60" r="20" fill="#000"/></mask>
<filter id="b"><feGaussianBlur stdDeviation="1.5"/></filter></defs>
<rect width="200" height="120" fill="#123"/>
<g id="Clipped" clip-path="url(#c)" opacity="0.8"><rect x="10" y="10" width="100" height="100" fill="url(#g)"/>
<path d="M20 100 L60 20 L100 100 Z" fill="#e33" fill-rule="evenodd"/></g>
<g id="Masked" mask="url(#m)"><rect x="110" y="0" width="90" height="120" fill="url(#r)"/></g>
<circle id="Soft" cx="160" cy="95" r="12" fill="#0c6" filter="url(#b)"/>
<polyline id="Dashes" points="5,115 60,105 120,115 195,105" fill="none" stroke="#fff" stroke-width="3" stroke-dasharray="6 3" stroke-linecap="round"/>
<path id="Ring" d="M140 20 h40 v30 h-40 z M150 28 h20 v14 h-20 z" fill="#fc0" fill-rule="evenodd" stroke="#000" stroke-width="1.5"/>
</svg>)svg";

// Flat outlines only (crisp edges to compare when scaled up and turned).
const std::string kIcon = R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" width="48" height="48">
<path d="M12 2a10 10 0 1 0 0 20a10 10 0 1 0 0-20zm0 4a6 6 0 1 1 0 12a6 6 0 1 1 0-12z" fill="#36c" fill-rule="evenodd"/>
<path d="M11 7h2v6h-2zM11 15h2v2h-2z" fill="#fc0" stroke="#000" stroke-width="0.5" stroke-linejoin="bevel"/></svg>)svg";

// A deterministic generator, so the file is the same on every machine.
struct Lcg final {
    std::uint64_t state = 642;
    double next() {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<double>(state >> 11) / static_cast<double>(1ULL << 53);
    }
};

// 700 stroked shapes: 600 siblings (three runs of at most 256, so nested Merges) and an opacity
// group of 100.
std::string denseSvg() {
    Lcg random;
    std::ostringstream svg;
    svg << R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 256 144" width="256" height="144">)svg" << '\n';
    const auto element = [&](const int index) {
        const double x = random.next() * 256;
        const double y = random.next() * 144;
        const double r = 2 + random.next() * 8;
        const int hue = static_cast<int>(random.next() * 0xffffff);
        svg << (index % 2 == 0 ? "<ellipse cx=\"" : "<polygon points=\"");
        if (index % 2 == 0)
            svg << x << "\" cy=\"" << y << "\" rx=\"" << r << "\" ry=\"" << r * 0.6 << '"';
        else
            svg << x << ',' << y << ' ' << x + r << ',' << y + r * 0.3 << ' ' << x + r * 0.2 << ',' << y + r << '"';
        svg << " fill=\"#" << std::hex << hue << std::dec << "\" stroke=\"#202020\" stroke-width=\"0.6\"/>\n";
    };
    int index = 0;
    svg << "<g id=\"dense\">\n";
    for (; index < 600; ++index)
        element(index);
    svg << "</g>\n<g id=\"veil\" opacity=\"0.7\">\n";
    for (; index < 700; ++index)
        element(index);
    svg << "</g>\n</svg>\n";
    return svg.str();
}

template <typename Operation, typename... Arguments>
commands::CommandResult apply(document::Document& document, commands::CommandStack& commands,
                              Arguments&&... arguments) {
    commands::Transaction transaction("Fixture", document.snapshot().revision());
    transaction.emplace<Operation>(std::forward<Arguments>(arguments)...);
    auto result = commands.execute(std::move(transaction));
    if (!result.changed())
        throw std::runtime_error("a fixture edit was refused");
    return result;
}

struct Fixture final {
    document::NewProject initial;
    document::CompositionId main;
    document::Document document;
    commands::CommandStack commands;
    document::LayerId art;

    Fixture(const std::filesystem::path& file, const commands::SvgImportMode mode, const std::uint32_t width,
            const std::uint32_t height)
        : initial(document::makeNewProject(
              "SVG layer", "Main", core::RationalTime::fromInteger(4),
              *document::CompositionFormat::create(width, height, core::PixelAspectRatio::square(),
                                                   *document::FrameRate::create(24, 1)))),
          main(initial.initialCompositionId), document(std::move(initial.project)), commands(document) {
        const auto imported = apply<commands::AddSvgArtwork>(document, commands, main, nullptr, file, "art", true, mode);
        art = *imported.outputId<document::LayerId>("layer");
    }
    [[nodiscard]] document::ParameterId layerParameter(const std::string_view role) const {
        const auto& graph = composition(document, main).graph();
        return document::layerParameterBinding(graph, *graph.findLayer(art), role)->parameterId;
    }
    template <typename Value> void key(const std::string_view role, const Value first, const Value last) {
        // The first key holds the value the constant already has, so the moved key goes in first.
        apply<commands::SetKeyframeAtTimeForParameter>(document, commands, main, layerParameter(role),
                                                       *core::RationalTime::create(2, 24), last);
        apply<commands::SetKeyframeAtTimeForParameter>(document, commands, main, layerParameter(role),
                                                       core::RationalTime{}, first);
    }
};

core::RationalTime frameAt(const std::int64_t index) { return *core::RationalTime::create(index, 24); }

// The CPU reference of a composition at a time and resolution.
[[nodiscard]] std::optional<Reference> referenceAt(Expectations& expectations, const Fixture& fixture,
                                                   const core::RationalTime time, const std::string& label,
                                                   const runtime::EvaluationResolution resolution = {}) {
    const runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
    const auto compiled = compiler.compile({fixture.document.snapshot(), fixture.main}, {});
    expectations.expect(compiled.plan != nullptr,
                        label + ": compiles: " +
                            runtime::describeCompileFailure(compiled, fixture.document.snapshot().project(), fixture.main));
    if (!compiled.plan)
        return std::nullopt;
    expectations.expect(std::ranges::any_of(compiled.plan->operations(),
                                            [](const auto& operation) {
                                                const auto* layer = std::get_if<runtime::CompiledLayerOutput>(&operation);
                                                return layer != nullptr && layer->composedArtwork.has_value();
                                            }) ||
                            label.find("shapes") != std::string::npos,
                        label + ": the layer's transform is drawn through its artwork's geometry");
    auto request = requestFor(*compiled.plan);
    request.time = time;
    request.resolution = resolution;
    request.pixelStorageByteLimit = kGib;
    auto oracle = request;
    oracle.bypassOperationCache = true;
    const auto result = runtime::CpuCompositionEvaluator{}.evaluate(compiled.plan, oracle, {});
    expectations.expect(result.frame() != nullptr, label + ": the CPU reference evaluates");
    if (result.frame() == nullptr)
        return std::nullopt;
    return Reference{compiled.plan, request, result.frame()};
}

[[nodiscard]] runtime::CancellationToken cancelledToken() {
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
        TaskRequest("gpu svg layer cancel token", {.kind = TaskOwnerKind::Composition, .id = TaskOwnerId::fromRaw(642)}),
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
        throw std::logic_error("cancel token task was refused");
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
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return token;
}

// ---- CPU half: admission and retirement of the expanded scene ----

std::shared_ptr<runtime::GpuProcessingPhasePolicy> policyWithHost(const std::uint64_t limitBytes,
                                                                  const std::uint64_t currentBytes) {
    using namespace runtime;
    auto policy = std::make_shared<GpuProcessingPhasePolicy>(4);
    const auto now = GpuProcessingPhasePolicy::Clock::now();
    MemoryBudgetState sample;
    sample.machine.availableBytes = 60 * kGib;
    sample.process.status = ProcessMemoryStatus::Finite;
    sample.process.source = ProcessMemorySource::LinuxCgroupV2;
    sample.process.sampledAt = now;
    sample.process.domains.push_back(
        {.path = "/own", .kind = ProcessMemoryLimitKind::Finite, .limitBytes = limitBytes, .currentBytes = currentBytes});
    sample.processSampleFresh = true;
    sample.sampleSerial = 1;
    policy->applyHostSample(sample, 60 * kGib, now);
    static_cast<void>(policy->reportDevice(642, {.source = render::GpuAllocationBudgetSource::MemoryBudget,
                                                 .device_local_bytes = 8 * kGib,
                                                 .available_bytes = 8 * kGib}));
    return policy;
}

runtime::GpuScenePreparationPhase phaseFor(std::shared_ptr<runtime::GpuProcessingPhasePolicy> policy,
                                           const std::uint64_t frameBytes) {
    return {.policy = std::move(policy),
            .envelope = {.output = runtime::GpuProcessingPhaseOutput::FloatFinal,
                         .metadataBytes = 1024ULL * 1024,
                         .retainedCoverageByteLimit = 16ULL * 1024 * 1024,
                         .growsRetainedSceneStorage = true,
                         .requestedExecutorBytes = 4 * kGib,
                         .minimumExecutorBytes = frameBytes,
                         .deviceWorkingBytes = 1024ULL * 1024,
                         .admissionClass = runtime::GpuAdmissionClass::Export}};
}

[[nodiscard]] runtime::CpuGpuSceneBuilder toolsBuilder(Expectations& expectations,
                                                       runtime::CpuCompositionEvaluator& evaluator,
                                                       runtime::GpuScenePreparationPhase phase = {}) {
    const auto ocio = gpu_coverage_ocio::sharedContext();
    expectations.expect(ocio != nullptr && ocio->preparer != nullptr,
                        "the packaged shader tools are available: " +
                            std::string(gpu_coverage_ocio::unavailableReason()));
    auto context = runtime::GpuSceneMediaContext::fromEvaluator(evaluator);
    if (ocio == nullptr)
        return runtime::CpuGpuSceneBuilder(nullptr, context, {}, std::move(phase));
    return runtime::CpuGpuSceneBuilder(nullptr, context, *ocio, std::move(phase));
}

void admissionAndRetirement(Expectations& expectations, const Reference& reference) {
    using runtime::PreparedGpuSceneDiagnosticCode;
    const auto extent = reference.frame->processImage().descriptor()->dataWindow().extent();
    const std::uint64_t frameBytes = std::uint64_t{extent.width()} * extent.height() * sizeof(render::Rgba32f);
    runtime::CpuCompositionEvaluator evaluator;
    auto ample = policyWithHost(8 * kGib, 2 * kGib);
    {
        const auto builder = toolsBuilder(expectations, evaluator, phaseFor(ample, frameBytes));
        const auto built = builder.build(reference.plan, reference.request);
        expectations.expect(built.hasValue() && built.phaseLease != nullptr,
                            "admission: the expanded layer's scene is admitted: " + built.diagnostic.message);
        expectations.expect(ample->snapshot().reservations.admittedPhases == 1,
                            "admission: the phase is counted at the root while the scene is held");
    }
    expectations.expect(ample->snapshot().reservations.totalHostClaimBytes == 0 &&
                            ample->snapshot().reservations.admittedPhases == 0,
                        "retirement: releasing the scene returns every claimed byte");
    {
        auto small = policyWithHost(64 * 1024, 0);
        const auto builder = toolsBuilder(expectations, evaluator, phaseFor(small, frameBytes));
        const auto refused = builder.build(reference.plan, reference.request);
        expectations.expect(!refused.hasValue() && (refused.diagnostic.code == PreparedGpuSceneDiagnosticCode::BudgetRefused ||
                                                    refused.diagnostic.code == PreparedGpuSceneDiagnosticCode::AwaitingCapacity),
                            "admission: a machine too small for the scene refuses it, never draws past it: " +
                                refused.diagnostic.message);
        expectations.expect(small->snapshot().reservations.totalHostClaimBytes == 0,
                            "retirement: a refused scene holds nothing");
    }
    const runtime::CpuGpuSceneBuilder plain;
    const auto cancelled = plain.build(reference.plan, reference.request, cancelledToken());
    expectations.expect(!cancelled.hasValue() && cancelled.diagnostic.code == PreparedGpuSceneDiagnosticCode::Cancelled,
                        "cancellation: a cancelled preparation of the layer reports Cancelled");
}

// ---- GPU half ----

struct Device final {
    std::unique_ptr<render::GpuDevice> device;
    [[nodiscard]] std::unique_ptr<runtime::GpuSceneCache> cache() const {
        return std::move(runtime::GpuSceneCache::create(*device, {}).cache);
    }
};

[[nodiscard]] std::unique_ptr<runtime::GpuSceneExecutor> executorFor(render::GpuDevice& device,
                                                                     runtime::GpuSceneCache& cache) {
    return std::move(runtime::GpuSceneExecutor::create(device, cache, {}).executor);
}

// The executor's request budget. The composite file's radial gradient is drawn in its own window,
// larger than the frame (about 3,760 x 5,120 at every frame size, vector-artwork.md "What a layer
// draws"), past the shared support's 256 MiB, in either import mode.
constexpr std::uint64_t kRequestBudget = kGib;

[[nodiscard]] std::vector<render::Rgba32f> runOn(runtime::GpuSceneExecutor& executor,
                                                 const std::shared_ptr<const runtime::PreparedGpuScene>& scene,
                                                 const std::uint64_t budget = kRequestBudget) {
    if (executor.begin(scene, budget).code != runtime::GpuSceneExecutorDiagnosticCode::None)
        return {};
    auto poll = executor.poll();
    while (poll == runtime::GpuSceneExecutorPollResult::Pending)
        poll = executor.poll();
    std::vector<render::Rgba32f> pixels;
    if (poll == runtime::GpuSceneExecutorPollResult::Ready && executor.image() != nullptr)
        if (const auto readback = render::readbackResidentImage(*executor.image(), budget))
            pixels = readback.pixels;
    static_cast<void>(executor.takeImage());
    return pixels;
}

// Cold and warm parity with the CPU reference (2e-6) and the bounds the CPU publishes, as
// gpuParity() checks them, at kRequestBudget.
void parityOn(Expectations& expectations, const Device& device, const Reference& reference, const std::string& label) {
    auto cache = device.cache();
    auto executor = cache ? executorFor(*device.device, *cache) : nullptr;
    expectations.expect(executor != nullptr, label + ": the executor creates");
    if (!executor)
        return;
    runtime::CpuCompositionEvaluator evaluator;
    const auto prepared = toolsBuilder(expectations, evaluator).build(reference.plan, reference.request);
    expectations.expect(prepared.hasValue(), label + ": prepares: " + prepared.diagnostic.message);
    if (!prepared)
        return;
    const auto& cpuBounds = reference.frame->evaluatedBounds();
    const auto& gpuBounds = prepared.scene->bounds();
    bool boundsMatch = cpuBounds.size() == gpuBounds.size();
    for (std::size_t index = 0; boundsMatch && index < cpuBounds.size(); ++index)
        boundsMatch = cpuBounds[index].local == gpuBounds[index].local && cpuBounds[index].output == gpuBounds[index].output &&
                      cpuBounds[index].polygon == gpuBounds[index].polygon && cpuBounds[index].anchor == gpuBounds[index].anchor;
    expectations.expect(boundsMatch, label + ": GPU preparation publishes the CPU bounds (the layer's box included)");
    const auto expected = reference.frame->processImage().pixels();
    const auto cold = runOn(*executor, prepared.scene);
    expectations.expect(!cold.empty(), label + ": the native run completes: " + executor->diagnostic().message);
    expectations.expect(parity(cold, expected), label + ": GPU pixels match the CPU oracle (2e-6)");
    const auto before = executor->counters();
    const auto warm = runOn(*executor, prepared.scene);
    expectations.expect(parity(warm, expected) && executor->counters().dispatches == before.dispatches,
                        label + ": a warm rerun matches and dispatches nothing new");
}

// The device frame of a reference, for comparing it with another document's CPU frame.
[[nodiscard]] std::vector<render::Rgba32f> deviceFrame(Expectations& expectations, const Device& device,
                                                       const Reference& reference, const std::string& label,
                                                       const std::uint64_t budget = kRequestBudget) {
    runtime::CpuCompositionEvaluator evaluator;
    const auto builder = toolsBuilder(expectations, evaluator);
    const auto built = builder.build(reference.plan, reference.request);
    expectations.expect(built.hasValue(), label + ": prepares on the production builder: " + built.diagnostic.message);
    auto cache = device.cache();
    auto executor = cache ? executorFor(*device.device, *cache) : nullptr;
    if (!built.hasValue() || !executor)
        return {};
    const auto begun = executor->begin(built.scene, budget);
    expectations.expect(begun.code == runtime::GpuSceneExecutorDiagnosticCode::None,
                        label + ": the executor admits it: " + begun.message);
    auto poll = executor->poll();
    while (poll == runtime::GpuSceneExecutorPollResult::Pending)
        poll = executor->poll();
    std::vector<render::Rgba32f> pixels;
    if (poll == runtime::GpuSceneExecutorPollResult::Ready && executor->image() != nullptr)
        if (const auto readback = render::readbackResidentImage(*executor->image(), budget))
            pixels = readback.pixels;
    static_cast<void>(executor->takeImage());
    expectations.expect(!pixels.empty(), label + ": the device runs it: " + executor->diagnostic().message);
    return pixels;
}

void animatedOnTheDevice(Expectations& expectations, const Device& device, const std::filesystem::path& file) {
    Fixture layer(file, commands::SvgImportMode::Layer, 512, 288);
    layer.key(document::kPositionParameterRole, document::Vec2d{256, 144}, document::Vec2d{230, 160});
    layer.key(document::kScaleParameterRole, document::Vec2d{1, 1}, document::Vec2d{2.4, 1.9});
    layer.key(document::kRotationParameterRole, 0.0, 33.0);
    layer.key(document::kOpacityParameterRole, 1.0, 0.55);
    std::vector<std::vector<render::Rgba32f>> frames;
    for (const std::int64_t index : {0, 1, 2}) {
        const auto label = "animated frame " + std::to_string(index);
        const auto reference = referenceAt(expectations, layer, frameAt(index), label);
        if (!reference)
            continue;
        parityOn(expectations, device, *reference, label);
        const auto pixels = reference->frame->processImage().pixels();
        frames.emplace_back(pixels.begin(), pixels.end());
    }
    expectations.expect(frames.size() == 3 && !parity(frames[0], frames[1]) && !parity(frames[1], frames[2]),
                        "animated: the keyed transform and opacity change every frame");
    // Reduced render scale.
    const auto half = render::ImageExtent::create(256, 144);
    if (const auto reference = referenceAt(expectations, layer, frameAt(1), "animated at half resolution",
                                           runtime::ProxyResolution{*half.value()}))
        parityOn(expectations, device, *reference, "animated at half resolution");
}

void layerIsTheEditableImportOnTheDevice(Expectations& expectations, const Device& device) {
    for (const auto& [name, svg] : {std::pair{std::string("composite"), kComposite}, std::pair{std::string("icon"), kIcon},
                                    std::pair{std::string("dense"), denseSvg()}}) {
        const auto file = writeSvg(name, svg);
        Fixture layer(file, commands::SvgImportMode::Layer, 512, 288);
        Fixture shapes(file, commands::SvgImportMode::Shapes, 512, 288);
        std::filesystem::remove(file);
        const auto one = referenceAt(expectations, layer, {}, name + " one layer");
        const auto editable = referenceAt(expectations, shapes, {}, name + " editable shapes");
        if (!one || !editable)
            continue;
        const auto pixels = deviceFrame(expectations, device, *one, name + " one layer");
        expectations.expect(parity(pixels, editable->frame->processImage().pixels()),
                            name + ": the one layer on the device is the editable import's CPU frame (2e-6)");
        parityOn(expectations, device, *one, name + " one layer");
    }
}

// 1080p and 4K: the production builder prepares the scaled, turned layer and the device runs it.
void largeOnTheDevice(Expectations& expectations, const Device& device, const std::filesystem::path& file) {
    for (const auto& [width, height] : {std::pair{1920U, 1080U}, std::pair{3840U, 2160U}}) {
        const auto label = std::to_string(width) + "x" + std::to_string(height);
        Fixture layer(file, commands::SvgImportMode::Layer, width, height);
        apply<commands::SetParameterSource>(layer.document, layer.commands, layer.main, layer.layerParameter(document::kScaleParameterRole),
                                            document::ConstantValueSource{document::ParameterValue{document::Vec2d{1.3, 1.3}}});
        apply<commands::SetParameterSource>(layer.document, layer.commands, layer.main, layer.layerParameter(document::kRotationParameterRole),
                                            document::ConstantValueSource{document::ParameterValue{-12.0}});
        const runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
        const auto compiled = compiler.compile({layer.document.snapshot(), layer.main}, {});
        expectations.expect(compiled.plan != nullptr, label + ": compiles");
        if (!compiled.plan)
            continue;
        auto request = requestFor(*compiled.plan);
        request.pixelStorageByteLimit = 8 * kGib;
        const auto pixels = deviceFrame(expectations, device, Reference{compiled.plan, request, nullptr}, label, 4 * kGib);
        expectations.expect(pixels.size() == std::size_t{width} * height,
                            label + ": the device frame is the composition's size");
        std::size_t covered = 0;
        for (const auto& pixel : pixels)
            covered += pixel.alpha() > 0.5F ? 1 : 0;
        expectations.expect(covered > pixels.size() / 4,
                            label + ": the artwork covers the frame on the device (" + std::to_string(covered) + " pixels)");
        std::cout << label << ": the production GPU route prepared and ran the layer\n";
    }
}

void cancellationOnTheDevice(Expectations& expectations, const Device& device, const Reference& reference) {
    runtime::CpuCompositionEvaluator evaluator;
    const auto built = toolsBuilder(expectations, evaluator).build(reference.plan, reference.request);
    auto cache = device.cache();
    auto executor = cache ? executorFor(*device.device, *cache) : nullptr;
    if (!built.hasValue() || !executor)
        return;
    expectations.expect(executor->begin(built.scene, kRequestBudget).code == runtime::GpuSceneExecutorDiagnosticCode::None,
                        "native cancel: begins");
    static_cast<void>(executor->poll());
    executor->cancel();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (executor->state() == runtime::GpuSceneExecutorJobState::Pending && std::chrono::steady_clock::now() < deadline)
        static_cast<void>(executor->poll());
    expectations.expect(executor->state() != runtime::GpuSceneExecutorJobState::Ready && executor->image() == nullptr,
                        "native cancel: a cancelled run of the layer publishes no image");
    while (executor->hasUnretiredSubmission() && std::chrono::steady_clock::now() < deadline)
        static_cast<void>(executor->poll());
    expectations.expect(parity(runOn(*executor, built.scene), reference.frame->processImage().pixels()),
                        "native cancel: the same executor completes the layer afterwards");
    const auto extent = reference.frame->processImage().descriptor()->dataWindow().extent();
    const std::uint64_t frameBytes = std::uint64_t{extent.width()} * extent.height() * sizeof(render::Rgba32f);
    expectations.expect(executor->begin(built.scene, frameBytes / 2).code != runtime::GpuSceneExecutorDiagnosticCode::None,
                        "budget: the executor refuses a request below one frame");
}

} // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parseOptions(argc, argv);
        if (!options.valid) {
            std::cerr << "invalid arguments\n";
            return 2;
        }
        Expectations expectations;
        const auto composite = writeSvg("composite", kComposite);
        Fixture admission(composite, commands::SvgImportMode::Layer, 512, 288);
        apply<commands::SetParameterSource>(admission.document, admission.commands, admission.main,
                                            admission.layerParameter(document::kScaleParameterRole),
                                            document::ConstantValueSource{document::ParameterValue{document::Vec2d{2.0, 2.0}}});
        const auto admissionFrame = referenceAt(expectations, admission, {}, "admission");
        if (admissionFrame)
            admissionAndRetirement(expectations, *admissionFrame);
        if (expectations.failures() != 0) {
            std::cerr << expectations.failures() << " Vector Artwork layer expectation(s) failed\n";
            std::filesystem::remove(composite);
            return 1;
        }

        render::GpuDeviceCreationOptions createOptions;
        createOptions.loader_path = options.loader_path;
        auto created = render::GpuDevice::create(createOptions);
        if (!created) {
            std::filesystem::remove(composite);
            if (options.require_device) {
                std::cerr << "FAIL: required device unavailable: " << created.diagnostic.message << '\n';
                return 1;
            }
            std::cout << "SKIP: CPU half passed; no compatible Vulkan device available: " << created.diagnostic.message
                      << '\n';
            return 77;
        }
        const Device device{std::move(created.device)};
        animatedOnTheDevice(expectations, device, composite);
        layerIsTheEditableImportOnTheDevice(expectations, device);
        if (admissionFrame)
            cancellationOnTheDevice(expectations, device, *admissionFrame);
        largeOnTheDevice(expectations, device, composite);
        std::filesystem::remove(composite);
        if (expectations.failures() != 0) {
            std::cerr << expectations.failures() << " Vector Artwork layer expectation(s) failed\n";
            return 1;
        }
        std::cout << "PASS: a Vector Artwork layer renders on the GPU as the CPU reference and the editable import, "
                     "keyed, turned, scaled, at half resolution, at 1080p and 4K, admitted, retired and cancelled\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected test exception: " << exception.what() << '\n';
        return 1;
    }
}
