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
// - A layer the composition changes (a mask, a Gaussian Blur and a Drop Shadow between the artwork
//   and its slot, its scale, rotation and opacity keyed; a layer pivoting on its content; a
//   retimed layer) is drawn at its output scale (the oversampled route) and matches the CPU
//   reference cold and warm with its bounds, 512 x 288 over frames 0 to 2.
// - A Layer node pivoting on its content above the oversampled picture (a blur between, masked,
//   keyed from 1x to 3x, turned and panned) matches the CPU reference cold and warm with its
//   bounds, 512 x 288 over frames 0 to 2, its box the same at every frame and only the window the
//   frame shows drawn; at 3840 x 2160, scaled 4x and 8x and turned, a crop is the scaled
//   pre-composition's on the device, with at most two frames of pixels drawn (#642 B4).
// - 1920 x 1080 and 3840 x 2160 prepare and run on the device, the plain layer and the oversampled
//   one (no comparison at these sizes; the export route's provenance is read by
//   bloom.ui.viewer_export_svg_artwork).
// - Admission (the phase admits the scene; a machine too small refuses it and holds nothing),
//   retirement (releasing the scene returns every claimed byte), cancellation (a cancelled
//   preparation and a cancelled run publish nothing) and the executor's request budget.
//
// The CPU assertions always run. Without a device the GPU half reports an explicit SKIP (exit 77)
// after the CPU half passed; --require-device makes that a failure.

#include "document_render_parity_support.hpp"
#include "gpu_coverage_ocio_support.hpp"

#include <bloom/commands/animation_operations.hpp>
#include <bloom/commands/entry_layer_operations.hpp>
#include <bloom/commands/layer_mask_operations.hpp>
#include <bloom/commands/layer_time_operations.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/commands/svg_import_operations.hpp>
#include <bloom/document/compositing_recipe_nodes.hpp>
#include <bloom/document/merge_entries.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/svg_artwork.hpp>
#include <bloom/platform/font_catalog.hpp>
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
#include <limits>
#include <mutex>
#include <sstream>
#include <thread>
#include <tuple>
#include <variant>

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
    std::optional<document::NodeId> artworkNode;
    std::optional<document::NodeId> rootMerge;

    Fixture(const std::filesystem::path& file, const commands::SvgImportMode mode, const std::uint32_t width,
            const std::uint32_t height, const bool fit = true)
        : initial(document::makeNewProject(
              "SVG layer", "Main", core::RationalTime::fromInteger(4),
              *document::CompositionFormat::create(width, height, core::PixelAspectRatio::square(),
                                                   *document::FrameRate::create(24, 1)))),
          main(initial.initialCompositionId), document(std::move(initial.project)), commands(document) {
        const auto imported = apply<commands::AddSvgArtwork>(document, commands, main, nullptr, file, "art", fit, mode);
        art = *imported.outputId<document::LayerId>("layer");
        artworkNode = imported.outputId<document::NodeId>("node");
        rootMerge = imported.outputId<document::NodeId>("merge");
    }
    [[nodiscard]] document::ParameterId layerParameter(const std::string_view role) const {
        return layerParameter(art, role);
    }
    [[nodiscard]] document::ParameterId layerParameter(const document::LayerId layer, const std::string_view role) const {
        const auto& graph = composition(document, main).graph();
        return document::layerParameterBinding(graph, *graph.findLayer(layer), role)->parameterId;
    }
    [[nodiscard]] document::ParameterId nodeParameter(const document::NodeId node, const std::string_view role) const {
        for (const auto& binding : composition(document, main).graph().findNode(node)->parameters)
            if (binding.role == role)
                return binding.parameterId;
        throw std::runtime_error("a node parameter is missing");
    }
    void set(const document::ParameterId parameter, const document::ParameterValue value) {
        apply<commands::SetParameterSource>(document, commands, main, parameter, document::ConstantValueSource{value});
    }
    document::NodeId addNode(const std::string_view type) {
        return *apply<commands::AddNode>(document, commands, main, std::string(type), document::Vec2d{0, -300})
                    .outputId<document::NodeId>(commands::kAddNodeOutput);
    }
    template <typename Input> void connect(const document::NodeId from, const Input to) {
        apply<commands::ConnectPorts>(document, commands, main, document::OutputPortRef{from, "image"}, to);
    }
    // Puts `nodes`, in order, between the artwork and the layer's slot.
    void insertBetween(const std::vector<document::NodeId>& nodes) {
        const auto* boundary = composition(document, main).graph().findLayer(art);
        const auto merge = boundary->nodeId;
        const auto slot = *boundary->entrySlot;
        auto from = *artworkNode;
        for (const auto node : nodes) {
            connect(from, document::NodeInputRef{node, "input"});
            from = node;
        }
        connect(from, document::LayerStackInputRef{merge, slot, std::string(document::kLayerStackContentInputRole)});
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

// How the layer's artwork is drawn sharp: B's composed route (the layer's transform through the
// geometry) or the oversampled route (drawn at the layer's output scale, one resample after).
enum class Sharp { Composed, Oversampled };

// The CPU reference of a composition at a time and resolution.
[[nodiscard]] std::optional<Reference> referenceAt(Expectations& expectations, const Fixture& fixture,
                                                   const core::RationalTime time, const std::string& label,
                                                   const runtime::EvaluationResolution resolution = {},
                                                   const Sharp sharp = Sharp::Composed) {
    const runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
    const auto compiled = compiler.compile({fixture.document.snapshot(), fixture.main}, {});
    expectations.expect(compiled.plan != nullptr,
                        label + ": compiles: " +
                            runtime::describeCompileFailure(compiled, fixture.document.snapshot().project(), fixture.main));
    if (!compiled.plan)
        return std::nullopt;
    expectations.expect(std::ranges::any_of(compiled.plan->operations(),
                                            [sharp](const auto& operation) {
                                                const auto* layer = std::get_if<runtime::CompiledLayerOutput>(&operation);
                                                if (layer == nullptr)
                                                    return false;
                                                return sharp == Sharp::Composed ? layer->composedArtwork.has_value()
                                                                                : layer->contentScale > 1.0;
                                            }) ||
                            label.find("shapes") != std::string::npos,
                        label + (sharp == Sharp::Composed ? ": the layer's transform is drawn through its artwork's geometry"
                                                          : ": the layer's artwork is drawn at its output scale"));
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

// The content scale the compiled plan draws `layer`'s artwork at (1 when it is not oversampled).
[[nodiscard]] double contentScaleOf(const runtime::CompiledCompositionPlan& plan, const document::LayerId layer) {
    for (const auto& operation : plan.operations())
        if (const auto* output = std::get_if<runtime::CompiledLayerOutput>(&operation); output != nullptr && output->layerId == layer)
            return output->contentScale;
    return 0.0;
}

void addMask(Fixture& fixture, const document::LayerId layer, const std::vector<document::Vec2d>& points, const double feather) {
    document::PathValue mask;
    document::PathContour contour;
    contour.closed = true;
    for (const auto& point : points)
        contour.anchors.push_back(document::PathAnchor{point, std::nullopt, std::nullopt});
    mask.contours.push_back(contour);
    apply<commands::AddLayerMask>(fixture.document, fixture.commands, fixture.main, layer, mask);
    fixture.set(composition(fixture.document, fixture.main).graph().findLayer(layer)->masks.back().feather,
                document::ParameterValue{feather});
}

// A Gaussian Blur then a Drop Shadow between the artwork and its slot, every length in the
// composition's pixels.
void effectsBetween(Fixture& fixture) {
    const auto blur = fixture.addNode("bloom.gaussian-blur");
    fixture.set(fixture.nodeParameter(blur, "radius"), document::ParameterValue{1.5});
    const auto shadow = fixture.addNode(document::kDropShadowNodeType);
    fixture.set(fixture.nodeParameter(shadow, "distance"), document::ParameterValue{4.0});
    fixture.set(fixture.nodeParameter(shadow, "softness"), document::ParameterValue{2.0});
    fixture.insertBetween({blur, shadow});
}

// #642 B2: the layers the composition changes, drawn at their output scale, on the device.
void oversampledOnTheDevice(Expectations& expectations, const Device& device) {
    const auto file = writeSvg("icon", kIcon);
    {
        // Masked, a blur and a drop shadow between, scale (1 to 2.6), rotation and opacity keyed:
        // the largest scale 2.6 draws the artwork at 4x for every frame.
        Fixture layer(file, commands::SvgImportMode::Layer, 512, 288);
        addMask(layer, layer.art, {{190, 60}, {330, 70}, {320, 230}, {180, 220}}, 2.0);
        effectsBetween(layer);
        layer.key(document::kScaleParameterRole, document::Vec2d{1, 1}, document::Vec2d{2.6, 2.6});
        layer.key(document::kRotationParameterRole, 0.0, 20.0);
        layer.key(document::kOpacityParameterRole, 1.0, 0.7);
        std::vector<std::vector<render::Rgba32f>> frames;
        for (const std::int64_t index : {0, 1, 2}) {
            const auto label = "oversampled frame " + std::to_string(index);
            const auto reference = referenceAt(expectations, layer, frameAt(index), label, {}, Sharp::Oversampled);
            if (!reference)
                continue;
            expectations.expect(contentScaleOf(*reference->plan, layer.art) == 4.0, label + ": drawn at 4x");
            parityOn(expectations, device, *reference, label);
            const auto pixels = reference->frame->processImage().pixels();
            frames.emplace_back(pixels.begin(), pixels.end());
        }
        expectations.expect(frames.size() == 3 && !parity(frames[0], frames[1]) && !parity(frames[1], frames[2]),
                            "oversampled: the keyed transform and opacity change every frame");
    }
    {
        // A Layer node pivoting on its content between the artwork and the entry's own Layer node.
        Fixture layer(file, commands::SvgImportMode::Layer, 512, 288);
        const auto pivoting = layer.addNode(document::kLayerOutputNodeType);
        const auto outer = layer.addNode(document::kLayerOutputNodeType);
        layer.connect(*layer.artworkNode, document::NodeInputRef{pivoting, std::string(document::kLayerOutputContentInputPort)});
        layer.connect(pivoting, document::NodeInputRef{outer, std::string(document::kLayerOutputContentInputPort)});
        const auto* entry = composition(layer.document, layer.main).graph().findLayer(layer.art);
        layer.connect(outer, document::LayerStackInputRef{entry->nodeId, *entry->entrySlot,
                                                          std::string(document::kLayerStackContentInputRole)});
        std::optional<document::LayerId> held;
        for (const auto& boundary : composition(layer.document, layer.main).graph().layerOutputs())
            if (!boundary.entryOwned() && boundary.nodeId == pivoting)
                held = boundary.layerId;
        expectations.expect(held.has_value() && composition(layer.document, layer.main).graph().findLayer(*held)->pivot ==
                                                    document::LayerPivot::Content,
                            "content pivot: the Layer node pivots on its content");
        if (held) {
            layer.set(layer.layerParameter(*held, document::kScaleParameterRole), document::ParameterValue{document::Vec2d{2.5, 2.5}});
            layer.set(layer.layerParameter(*held, document::kRotationParameterRole), document::ParameterValue{15.0});
            layer.set(layer.layerParameter(*held, document::kAnchorParameterRole), document::ParameterValue{document::Vec2d{6.0, -4.0}});
            if (const auto reference = referenceAt(expectations, layer, frameAt(0), "content pivot", {}, Sharp::Oversampled)) {
                expectations.expect(contentScaleOf(*reference->plan, *held) == 4.0, "content pivot: drawn at 4x");
                parityOn(expectations, device, *reference, "content pivot");
            }
        }
    }
    {
        // Retimed (starting a frame late) and scaled 3x: drawn at 4x, frames 1 and 2.
        Fixture layer(file, commands::SvgImportMode::Layer, 512, 288);
        apply<commands::SetLayerTiming>(layer.document, layer.commands, layer.main, layer.art, frameAt(1), core::RationalTime{},
                                        core::RationalTime::fromInteger(4));
        layer.set(layer.layerParameter(document::kScaleParameterRole), document::ParameterValue{document::Vec2d{3.0, 3.0}});
        for (const std::int64_t index : {1, 2}) {
            const auto label = "retimed frame " + std::to_string(index);
            if (const auto reference = referenceAt(expectations, layer, frameAt(index), label, {}, Sharp::Oversampled)) {
                expectations.expect(contentScaleOf(*reference->plan, layer.art) == 4.0, label + ": drawn at 4x");
                parityOn(expectations, device, *reference, label);
            }
        }
    }
    // 1080p and 4K: a blur between, scaled 1.5x and turned, drawn at 2x (1.5x turned asks for 2x).
    for (const auto& [width, height] : {std::pair{1920U, 1080U}, std::pair{3840U, 2160U}}) {
        const auto label = "oversampled " + std::to_string(width) + "x" + std::to_string(height);
        Fixture layer(file, commands::SvgImportMode::Layer, width, height);
        const auto blur = layer.addNode("bloom.gaussian-blur");
        layer.set(layer.nodeParameter(blur, "radius"), document::ParameterValue{3.0});
        layer.insertBetween({blur});
        layer.set(layer.layerParameter(document::kScaleParameterRole), document::ParameterValue{document::Vec2d{1.5, 1.5}});
        layer.set(layer.layerParameter(document::kRotationParameterRole), document::ParameterValue{-12.0});
        const runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
        const auto compiled = compiler.compile({layer.document.snapshot(), layer.main}, {});
        expectations.expect(compiled.plan != nullptr && contentScaleOf(*compiled.plan, layer.art) == 2.0,
                            label + ": compiles, drawn at 2x");
        if (!compiled.plan)
            continue;
        auto request = requestFor(*compiled.plan);
        request.pixelStorageByteLimit = 8 * kGib;
        const auto pixels = deviceFrame(expectations, device, Reference{compiled.plan, request, nullptr}, label, 4 * kGib);
        expectations.expect(pixels.size() == std::size_t{width} * height, label + ": the device frame is the composition's size");
        std::size_t covered = 0;
        for (const auto& pixel : pixels)
            covered += pixel.alpha() > 0.5F ? 1 : 0;
        expectations.expect(covered > pixels.size() / 8,
                            label + ": the artwork covers the frame on the device (" + std::to_string(covered) + " pixels)");
        std::cout << label << ": the production GPU route prepared and ran the oversampled layer\n";
    }
    std::filesystem::remove(file);
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


// ---- #642 B3: the window, and groups above the layer, on the device ----

// The picture of `source` (the artwork node, or the editable import's root Merge) alone in a new
// group: a Merge holding it in one entry whose layer is at its defaults (the identity), read by the
// slot `source` fed, whose layer (`fixture.art`) becomes the group's layer.
struct Group final {
    document::NodeId merge;
    document::LayerId inner;
};
Group groupAround(Fixture& fixture, const document::NodeId source) {
    const auto* boundary = composition(fixture.document, fixture.main).graph().findLayer(fixture.art);
    const auto top = boundary->nodeId;
    const auto slot = *boundary->entrySlot;
    const auto merge = fixture.addNode(document::kLayerStackNodeType);
    const auto entered = apply<commands::ConnectPorts>(
        fixture.document, fixture.commands, fixture.main, document::OutputPortRef{source, "image"},
        document::LayerStackInputRef{merge, {}, std::string(document::kLayerStackContentInputRole)});
    const auto innerSlot = *entered.outputId<document::LayerSlotId>(commands::kConnectPortsSlotOutput);
    fixture.connect(merge, document::LayerStackInputRef{top, slot, std::string(document::kLayerStackContentInputRole)});
    const auto ensured = apply<commands::EnsureEntryLayer>(fixture.document, fixture.commands, fixture.main, merge, innerSlot);
    return {merge, *ensured.outputId<document::LayerId>(commands::kEntryLayerOutput)};
}

// A solid sibling in a group, its own entry layer moved and turned.
document::LayerId addSibling(Fixture& fixture, const document::NodeId merge, const document::Vec2d position) {
    const auto solid = fixture.addNode(document::kSolidSourceNodeType);
    fixture.set(fixture.nodeParameter(solid, "color"), document::ParameterValue{core::Color4d{0.9, 0.5, 0.1, 1.0}});
    fixture.set(fixture.nodeParameter(solid, document::kSolidWidthParameterRole), document::ParameterValue{std::int64_t{60}});
    fixture.set(fixture.nodeParameter(solid, document::kSolidHeightParameterRole), document::ParameterValue{std::int64_t{30}});
    const auto entered = apply<commands::ConnectPorts>(
        fixture.document, fixture.commands, fixture.main, document::OutputPortRef{solid, "image"},
        document::LayerStackInputRef{merge, {}, std::string(document::kLayerStackContentInputRole)});
    const auto slot = *entered.outputId<document::LayerSlotId>(commands::kConnectPortsSlotOutput);
    const auto sibling = *apply<commands::EnsureEntryLayer>(fixture.document, fixture.commands, fixture.main, merge, slot)
                              .outputId<document::LayerId>(commands::kEntryLayerOutput);
    fixture.set(fixture.layerParameter(sibling, document::kPositionParameterRole), document::ParameterValue{position});
    fixture.set(fixture.layerParameter(sibling, document::kRotationParameterRole), document::ParameterValue{10.0});
    return sibling;
}

// The largest window any command of a prepared scene draws, in device pixels.
[[nodiscard]] std::uint64_t largestWindow(const runtime::PreparedGpuScene& scene) {
    std::uint64_t largest = 0;
    const auto area = [&](const render::ImageWindow& window) {
        largest = std::max(largest, std::uint64_t{window.extent().width()} * window.extent().height());
    };
    for (const auto& command : scene.commands())
        std::visit(
            [&](const auto& item) {
                if constexpr (requires { item.outputWindow; })
                    area(item.outputWindow);
                else if constexpr (requires { item.dataWindow; })
                    area(item.dataWindow);
            },
            command);
    return largest;
}

// The production builder's scene of a reference, for its windows.
[[nodiscard]] std::uint64_t largestWindowOf(Expectations& expectations, const Reference& reference, const std::string& label) {
    runtime::CpuCompositionEvaluator evaluator;
    const auto built = toolsBuilder(expectations, evaluator).build(reference.plan, reference.request);
    expectations.expect(built.hasValue(), label + ": prepares: " + built.diagnostic.message);
    return built.hasValue() ? largestWindow(*built.scene) : 0;
}

// #642 B3: an artwork in a group whose layer is keyed from 2x to 3x and turned, with a mask on the
// artwork's own layer and a solid sibling in the group: the group is drawn at 4x on the device as
// on the CPU reference, cold and warm, with its bounds, 512 x 288 over frames 0 to 2.
void groupOnTheDevice(Expectations& expectations, const Device& device) {
    const auto file = writeSvg("icon", kIcon);
    Fixture layer(file, commands::SvgImportMode::Layer, 512, 288);
    std::filesystem::remove(file);
    const auto group = groupAround(layer, *layer.artworkNode);
    addMask(layer, group.inner, {{180, 50}, {340, 60}, {330, 240}, {170, 230}}, 2.0);
    static_cast<void>(addSibling(layer, group.merge, {150, 90}));
    layer.key(document::kScaleParameterRole, document::Vec2d{2, 2}, document::Vec2d{3, 3});
    layer.key(document::kRotationParameterRole, 0.0, 15.0);
    layer.key(document::kOpacityParameterRole, 1.0, 0.8);
    std::vector<std::vector<render::Rgba32f>> frames;
    for (const std::int64_t index : {0, 1, 2}) {
        const auto label = "group frame " + std::to_string(index);
        const auto reference = referenceAt(expectations, layer, frameAt(index), label, {}, Sharp::Oversampled);
        if (!reference)
            continue;
        expectations.expect(contentScaleOf(*reference->plan, layer.art) == 4.0 &&
                                contentScaleOf(*reference->plan, group.inner) == 4.0,
                            label + ": the group and its artwork are drawn at 4x");
        parityOn(expectations, device, *reference, label);
        const auto pixels = reference->frame->processImage().pixels();
        frames.emplace_back(pixels.begin(), pixels.end());
    }
    expectations.expect(frames.size() == 3 && !parity(frames[0], frames[1]) && !parity(frames[1], frames[2]),
                        "group: the keyed group changes every frame");
}

// #642 B3: a whole-frame artwork scaled 5x (drawn at 8x), masked, with a blur, a drop shadow and a
// glow between, panned so most of its 8x picture is off the frame: only the window the frame shows
// is drawn (through the mask, the glow's keys and octaves and the shadow's move), on the device as
// on the CPU reference, 512 x 288 over frames 0 to 2.
void clippedOnTheDevice(Expectations& expectations, const Device& device) {
    const auto file = writeSvg("icon", kIcon);
    Fixture layer(file, commands::SvgImportMode::Layer, 512, 288);
    std::filesystem::remove(file);
    addMask(layer, layer.art, {{150, 20}, {380, 30}, {370, 270}, {140, 260}}, 3.0);
    const auto blur = layer.addNode("bloom.gaussian-blur");
    layer.set(layer.nodeParameter(blur, "radius"), document::ParameterValue{1.5});
    const auto shadow = layer.addNode(document::kDropShadowNodeType);
    layer.set(layer.nodeParameter(shadow, "distance"), document::ParameterValue{4.0});
    layer.set(layer.nodeParameter(shadow, "softness"), document::ParameterValue{2.0});
    const auto glow = layer.addNode(document::kGlowNodeType);
    layer.set(layer.nodeParameter(glow, "size"), document::ParameterValue{6.0});
    layer.insertBetween({blur, shadow, glow});
    layer.set(layer.layerParameter(document::kScaleParameterRole), document::ParameterValue{document::Vec2d{5.0, 5.0}});
    layer.key(document::kPositionParameterRole, document::Vec2d{256, 144}, document::Vec2d{420, 60});
    // The frame's own region of the 8x picture (the layer shows it 5/8 as large), with a margin of
    // 256 of its pixels for the effects' reach (the glow's octaves, the blur, the shadow's move and
    // the mask's feather), against the whole 8x picture of the 288 x 288 fitted icon.
    const double region = (512.0 * 8.0 / 5.0 + 512.0) * (288.0 * 8.0 / 5.0 + 512.0);
    const double whole = 288.0 * 288.0 * 64.0;
    for (const std::int64_t index : {0, 1, 2}) {
        const auto label = "clipped frame " + std::to_string(index);
        const auto reference = referenceAt(expectations, layer, frameAt(index), label, {}, Sharp::Oversampled);
        if (!reference)
            continue;
        expectations.expect(contentScaleOf(*reference->plan, layer.art) == 8.0, label + ": drawn at 8x");
        parityOn(expectations, device, *reference, label);
        const auto largest = largestWindowOf(expectations, *reference, label);
        std::cout << label << ": the largest window drawn is " << largest << " pixels (the frame's region with "
                  << "the effects' margin is " << region << "; the whole 8x picture would be " << whole << ")\n";
        expectations.expect(largest > 0 && static_cast<double>(largest) <= region && static_cast<double>(largest) * 4.0 <= whole,
                            label + ": only the window the frame shows is drawn (its region and the effects' margin, "
                                    "under a quarter of the whole picture)");
    }
}

// Flat shapes, a compound path and a stroke (the CPU test's sharp file), 120 x 80.
std::string sharpSvg(const std::string& transform) {
    return R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="120" height="80" viewBox="0 0 120 80"><g transform=")svg" +
           transform + R"svg(">
<rect x="8" y="8" width="40" height="28" fill="#e33"/>
<circle cx="84" cy="24" r="16" fill="#3a6"/>
<path d="M14 46 H58 V74 H14 Z M26 54 H46 V66 H26 Z" fill="#36c" fill-rule="evenodd"/>
<path d="M66 70 L90 46 L112 70" fill="none" stroke="#fd0" stroke-width="3" stroke-linejoin="round"/>
</g></svg>)svg";
}

// Pixels of a `width`-wide frame inside the rectangle.
[[nodiscard]] std::vector<render::Rgba32f> crop(const std::vector<render::Rgba32f>& frame, const std::uint32_t width,
                                                const std::uint32_t left, const std::uint32_t top,
                                                const std::uint32_t cropWidth, const std::uint32_t cropHeight) {
    std::vector<render::Rgba32f> pixels;
    if (frame.size() < std::size_t{width} * (top + cropHeight))
        return pixels;
    for (std::uint32_t y = top; y < top + cropHeight; ++y)
        for (std::uint32_t x = left; x < left + cropWidth; ++x)
            pixels.push_back(frame[std::size_t{y} * width + x]);
    return pixels;
}

[[nodiscard]] std::size_t partialPixels(const std::vector<render::Rgba32f>& pixels) {
    return static_cast<std::size_t>(std::ranges::count_if(pixels, [](const auto& pixel) {
        return pixel.alpha() > 0.02F && pixel.alpha() < 0.98F;
    }));
}

[[nodiscard]] std::size_t differingPixels(const std::vector<render::Rgba32f>& a, const std::vector<render::Rgba32f>& b,
                                          const double tolerance) {
    std::size_t count = 0;
    for (std::size_t i = 0; i < std::min(a.size(), b.size()); ++i)
        for (std::size_t c = 0; c < 4; ++c)
            if (std::abs(static_cast<double>(a[i].components()[c]) - static_cast<double>(b[i].components()[c])) > tolerance) {
                ++count;
                break;
            }
    return count;
}

[[nodiscard]] double worstDifference(const std::vector<render::Rgba32f>& a, const std::vector<render::Rgba32f>& b) {
    if (a.size() != b.size() || a.empty())
        return std::numeric_limits<double>::infinity();
    double worst = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i)
        for (std::size_t c = 0; c < 4; ++c) {
            const auto x = static_cast<double>(a[i].components()[c]);
            const auto y = static_cast<double>(b[i].components()[c]);
            worst = std::max(worst, std::abs(x - y) / std::max(1.0, std::max(std::abs(x), std::abs(y))));
        }
    return worst;
}

// The device frame of a compiled document at 4K, and the largest window its scene draws.
struct LargeFrame final {
    std::vector<render::Rgba32f> pixels;
    std::uint64_t largestWindow = 0;
};
[[nodiscard]] LargeFrame largeFrame(Expectations& expectations, const Device& device, const document::Document& document,
                                    const document::CompositionId composition, const std::string& label) {
    const runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
    const auto compiled = compiler.compile({document.snapshot(), composition}, {});
    expectations.expect(compiled.plan != nullptr, label + ": compiles");
    if (!compiled.plan)
        return {};
    auto request = requestFor(*compiled.plan);
    request.pixelStorageByteLimit = 8 * kGib;
    const Reference reference{compiled.plan, request, nullptr};
    LargeFrame result;
    result.largestWindow = largestWindowOf(expectations, reference, label);
    result.pixels = deviceFrame(expectations, device, reference, label, 4 * kGib);
    return result;
}

// #642 B3: at 3840 x 2160, a whole-frame artwork scaled 4x and 8x (masked: drawn at its output
// scale) and one in a group scaled 3x render on the production GPU route with only the window the
// frame shows drawn, and a crop around a curved edge is the scaled pre-composition's (the file baked
// that many times larger, drawn through the editable import on the device) up to the flattenings'
// coverage samples, far sharper than the resampled picture. No full-size CPU frame.
//
// The one layer flattens the circle in the file's units through the band's matrix, the baked file
// at its own size; each polyline is within 1/32 pixel of the curve, so where the curve passes that
// close to a pixel's 4 x 4 coverage samples the two may land some on opposite sides. The crop is
// centred on the circle's leftmost point, where its tangent runs along a column of samples, so up
// to one column (four of the sixteen) can flip there; elsewhere one or two (vector-artwork.md
// "The band"). At most a quarter of the crop's edge pixels may differ at all.
void sharpAt4KOnTheDevice(Expectations& expectations, const Device& device) {
    constexpr std::uint32_t kWidth = 3840;
    constexpr std::uint32_t kHeight = 2160;
    const document::Vec2d centre{kWidth / 2.0, kHeight / 2.0};
    // Fitted, the 120 x 80 file is 27x about (300, 0); the circle's left edge lands at (2136, 648),
    // which the layer's anchor puts at the frame's centre.
    const document::Vec2d anchor{2136.0 - centre.x, 648.0 - centre.y};
    struct Case final {
        std::string name;
        double scale;
        bool group;
        double factor;
    };
    const std::vector<Case> cases{{"4K, masked, scaled 4x", 4.0, false, 4.0},
                                  {"4K, masked, scaled 8x", 8.0, false, 8.0},
                                  {"4K, in a group scaled 3x", 3.0, true, 4.0}};
    const auto plain = writeSvg("b3-4k", sharpSvg(""));
    for (const auto& item : cases) {
        // The file baked `factor` times larger, imported unfitted: a point p lands at
        // factor * (27 p + (300, 0)) once the unfitted viewport's offset (1860, 1040) is taken off.
        const double s = item.factor;
        const auto baked = writeSvg("b3-4k-baked", sharpSvg("translate(" + std::to_string(s * 300.0 - 1860.0) + " " +
                                                            std::to_string(-1040.0) + ") scale(" + std::to_string(27.0 * s) + ")"));
        Fixture layer(plain, commands::SvgImportMode::Layer, kWidth, kHeight);
        Fixture reference(baked, commands::SvgImportMode::Shapes, kWidth, kHeight, false);
        std::filesystem::remove(baked);
        // The layer that scales the picture: the artwork's own, or the group's.
        std::optional<Group> group;
        std::optional<Group> referenceGroup;
        if (item.group) {
            group = groupAround(layer, *layer.artworkNode);
            referenceGroup = groupAround(reference, *reference.rootMerge);
        } else {
            const std::vector<document::Vec2d> quad{{1950, 540}, {2330, 560}, {2310, 760}, {1960, 740}};
            addMask(layer, layer.art, quad, 2.0);
            std::vector<document::Vec2d> scaled;
            for (const auto& point : quad)
                scaled.push_back({point.x * s, point.y * s});
            addMask(reference, reference.art, scaled, 2.0 * s);
        }
        layer.set(layer.layerParameter(document::kAnchorParameterRole), document::ParameterValue{anchor});
        layer.set(layer.layerParameter(document::kScaleParameterRole), document::ParameterValue{document::Vec2d{item.scale, item.scale}});
        // The scaled pre-composition: the same matrix about s times the pivot, the scale over s.
        reference.set(reference.layerParameter(document::kAnchorParameterRole),
                      document::ParameterValue{document::Vec2d{s * (centre.x + anchor.x) - centre.x, s * (centre.y + anchor.y) - centre.y}});
        if (item.scale != s)
            reference.set(reference.layerParameter(document::kScaleParameterRole),
                          document::ParameterValue{document::Vec2d{item.scale / s, item.scale / s}});
        const runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
        const auto compiled = compiler.compile({layer.document.snapshot(), layer.main}, {});
        expectations.expect(compiled.plan != nullptr && contentScaleOf(*compiled.plan, layer.art) == s &&
                                (!group || contentScaleOf(*compiled.plan, group->inner) == s),
                            item.name + ": drawn at " + std::to_string(static_cast<int>(s)) + "x, not capped by the composition's size");

        const auto one = largeFrame(expectations, device, layer.document, layer.main, item.name + " one layer");
        const auto precomposed = largeFrame(expectations, device, reference.document, reference.main,
                                            item.name + " scaled pre-composition");
        // The resampled picture: the expansion asked not to compose (nor oversample).
        document::VectorArtworkExpansionContext resample;
        resample.composeLayerTransform = false;
        auto fonts = std::make_shared<platform::FontCatalogue>();
        fonts->status = platform::FontCatalogueStatus::Available;
        resample.fonts = fonts;
        const auto snapshot = layer.document.snapshot();
        const auto expansion = document::expandVectorArtwork(snapshot.project(), *snapshot.project().findComposition(layer.main),
                                                             resample);
        document::Project project = snapshot.project();
        *project.findComposition(layer.main) = *expansion.composition;
        const document::Document softDocument(std::move(project));
        const auto soft = largeFrame(expectations, device, softDocument, layer.main, item.name + " resampled");

        const double frameArea = double{kWidth} * kHeight;
        std::cout << item.name << ": the largest window drawn is " << one.largestWindow << " pixels (the frame is "
                  << frameArea << "; the whole " << s << "x picture would be " << 3240.0 * 2160.0 * s * s << ")\n";
        expectations.expect(one.largestWindow > 0 && static_cast<double>(one.largestWindow) <= 2.0 * frameArea,
                            item.name + ": only the window the frame shows is drawn (at most two frames of pixels)");
        // A crop around the frame's centre, where the circle's edge crosses.
        constexpr std::uint32_t kCropWidth = 384;
        constexpr std::uint32_t kCropHeight = 216;
        const auto left = kWidth / 2 - kCropWidth / 2;
        const auto top = kHeight / 2 - kCropHeight / 2;
        const auto sharp = crop(one.pixels, kWidth, left, top, kCropWidth, kCropHeight);
        const auto truth = crop(precomposed.pixels, kWidth, left, top, kCropWidth, kCropHeight);
        const auto blurry = crop(soft.pixels, kWidth, left, top, kCropWidth, kCropHeight);
        if (sharp.empty() || truth.empty() || blurry.empty())
            continue;
        const double worst = worstDifference(sharp, truth);
        const auto moved = differingPixels(sharp, truth, 1e-3);
        const auto edges = partialPixels(sharp);
        const auto softMoved = differingPixels(blurry, truth, 1e-2);
        const auto softEdges = partialPixels(blurry);
        std::cout << item.name << " (crop " << kCropWidth << " x " << kCropHeight << "): worst difference to the scaled "
                  << "pre-composition " << worst << " on " << moved << " pixels (edge pixels " << edges
                  << "); the resampled picture differs on " << softMoved << " pixels (edge pixels " << softEdges << ")\n";
        expectations.expect(edges > 100, item.name + ": the crop holds an edge");
        expectations.expect(worst <= 4.0 / 16.0 + 1.0 / 255.0 && moved * 4 <= edges,
                            item.name + ": the crop is the scaled pre-composition's up to one column of a pixel's "
                                        "sixteen coverage samples where the curve runs along it");
        expectations.expect(softMoved >= 10 * moved + 200 && softEdges > edges + edges / 2,
                            item.name + ": the resampled picture would be visibly softer");
    }
    std::filesystem::remove(plain);
}

// ---- #642 B4: a content pivot draws only the window the frame shows, on the device ----

// A new Layer node, which pivots on its content, reading `source` through `between` and feeding the
// slot of `fixture.art` through a second Layer node at its defaults (fed straight into the slot it
// would take the entry's own layer's place, and its frame pivot). Returns the first one's layer.
document::LayerId pivotOnContent(Fixture& fixture, const document::NodeId source,
                                 const std::vector<document::NodeId>& between = {}) {
    const auto node = fixture.addNode(document::kLayerOutputNodeType);
    const auto outer = fixture.addNode(document::kLayerOutputNodeType);
    auto from = source;
    for (const auto next : between) {
        fixture.connect(from, document::NodeInputRef{next, "input"});
        from = next;
    }
    fixture.connect(from, document::NodeInputRef{node, std::string(document::kLayerOutputContentInputPort)});
    fixture.connect(node, document::NodeInputRef{outer, std::string(document::kLayerOutputContentInputPort)});
    const auto* entry = composition(fixture.document, fixture.main).graph().findLayer(fixture.art);
    fixture.connect(outer, document::LayerStackInputRef{entry->nodeId, *entry->entrySlot,
                                                        std::string(document::kLayerStackContentInputRole)});
    for (const auto& boundary : composition(fixture.document, fixture.main).graph().layerOutputs())
        if (!boundary.entryOwned() && boundary.nodeId == node) {
            if (boundary.pivot != document::LayerPivot::Content)
                throw std::runtime_error("the Layer node does not pivot on its content");
            return boundary.layerId;
        }
    throw std::runtime_error("the Layer node has no layer");
}

template <typename Value>
void keyLayer(Fixture& fixture, const document::LayerId layer, const std::string_view role, const Value first,
              const Value last) {
    apply<commands::SetKeyframeAtTimeForParameter>(fixture.document, fixture.commands, fixture.main,
                                                   fixture.layerParameter(layer, role), frameAt(2), last);
    apply<commands::SetKeyframeAtTimeForParameter>(fixture.document, fixture.commands, fixture.main,
                                                   fixture.layerParameter(layer, role), core::RationalTime{}, first);
}

// The plan's Layer Output of `layer` and the operation it reads.
[[nodiscard]] std::optional<std::pair<std::size_t, std::size_t>> layerOperation(const runtime::CompiledCompositionPlan& plan,
                                                                                const document::LayerId layer) {
    const auto operations = plan.operations();
    for (std::size_t index = 0; index < operations.size(); ++index)
        if (const auto* output = std::get_if<runtime::CompiledLayerOutput>(&operations[index]);
            output != nullptr && output->layerId == layer)
            return std::pair{index, output->input.value()};
    return std::nullopt;
}

// #642 B4: a Layer node pivoting on its content above the oversampled picture, a blur between and a
// mask, its scale keyed from 1x to 3x (drawn at 4x) and turned, panned so most of its picture is off
// the frame at frame 2: on the device as on the CPU reference, cold and warm, with its bounds, 512 x
// 288 over frames 0 to 2; its box (and so its pivot) the same at every frame, while only the window
// the frame shows is drawn at frame 2.
void contentPivotOnTheDevice(Expectations& expectations, const Device& device) {
    const auto file = writeSvg("icon", kIcon);
    Fixture layer(file, commands::SvgImportMode::Layer, 512, 288);
    std::filesystem::remove(file);
    const auto blur = layer.addNode("bloom.gaussian-blur");
    layer.set(layer.nodeParameter(blur, "radius"), document::ParameterValue{1.5});
    const auto held = pivotOnContent(layer, *layer.artworkNode, {blur});
    addMask(layer, held, {{150, 20}, {380, 30}, {370, 270}, {140, 260}}, 3.0);
    keyLayer(layer, held, document::kScaleParameterRole, document::Vec2d{1, 1}, document::Vec2d{3, 3});
    keyLayer(layer, held, document::kPositionParameterRole, document::Vec2d{256, 144}, document::Vec2d{420, 60});
    layer.set(layer.layerParameter(held, document::kRotationParameterRole), document::ParameterValue{20.0});
    layer.set(layer.layerParameter(held, document::kAnchorParameterRole), document::ParameterValue{document::Vec2d{6.0, -4.0}});
    // The whole 4x picture of the 288 x 288 fitted icon.
    const double whole = 288.0 * 288.0 * 16.0;
    std::optional<runtime::ContentBounds> box;
    std::vector<std::vector<render::Rgba32f>> frames;
    for (const std::int64_t index : {0, 1, 2}) {
        const auto label = "content pivot frame " + std::to_string(index);
        const auto reference = referenceAt(expectations, layer, frameAt(index), label, {}, Sharp::Oversampled);
        if (!reference)
            continue;
        expectations.expect(contentScaleOf(*reference->plan, held) == 4.0, label + ": drawn at 4x");
        parityOn(expectations, device, *reference, label);
        const auto pixels = reference->frame->processImage().pixels();
        frames.emplace_back(pixels.begin(), pixels.end());
        const auto found = layerOperation(*reference->plan, held);
        expectations.expect(found.has_value(), label + ": the Layer node is in the plan");
        if (found) {
            const auto& own = reference->frame->evaluatedBounds()[found->first].local;
            if (!box)
                box = own;
            expectations.expect(own == *box, label + ": its box, and so its pivot, is the one it has with all of it shown");
        }
        const auto largest = largestWindowOf(expectations, *reference, label);
        std::cout << label << ": the largest window drawn is " << largest << " pixels (the whole 4x picture would be "
                  << whole << ")\n";
        if (index == 2)
            expectations.expect(largest > 0 && static_cast<double>(largest) * 2.0 <= whole,
                                label + ": panned, only the window the frame shows is drawn (under half the picture)");
    }
    expectations.expect(frames.size() == 3 && !parity(frames[0], frames[1]) && !parity(frames[1], frames[2]),
                        "content pivot: the keyed layer changes every frame");
}

// The centre of what the Layer Output of `layer` reads, as the production builder publishes it
// (the bounds a content pivot is taken from), without a CPU frame.
[[nodiscard]] std::optional<document::Vec2d> pivotSource(Expectations& expectations, const Fixture& fixture,
                                                         const document::LayerId layer, const std::string& label) {
    const runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
    const auto compiled = compiler.compile({fixture.document.snapshot(), fixture.main}, {});
    const auto found = compiled.plan ? layerOperation(*compiled.plan, layer) : std::nullopt;
    expectations.expect(found.has_value(), label + ": compiles with the Layer node");
    if (!found)
        return std::nullopt;
    auto request = requestFor(*compiled.plan);
    request.pixelStorageByteLimit = 8 * kGib;
    runtime::CpuCompositionEvaluator evaluator;
    const auto built = toolsBuilder(expectations, evaluator).build(compiled.plan, request);
    expectations.expect(built.hasValue(), label + ": prepares: " + built.diagnostic.message);
    if (!built.hasValue())
        return std::nullopt;
    return built.scene->bounds()[found->second].output.centre();
}

// #642 B4: at 3840 x 2160, a Layer node pivoting on its content above the fitted, whole-frame
// artwork, scaled 4x and 8x and turned 12 degrees, renders on the production GPU route with only
// the window the frame shows drawn, and a crop around a curved edge is the scaled
// pre-composition's (the file baked that many times larger, through the editable import, turned
// about the same point by its own layer, which pivots on the frame, so that only its window is
// drawn too) up to the flattenings' coverage samples, far sharper than the resampled picture. The
// layer's anchor is set from its picture's bounds as the builder publishes them, to put the
// circle's leftmost point at the frame's centre. No full-size CPU frame.
void contentPivotSharpAt4KOnTheDevice(Expectations& expectations, const Device& device) {
    constexpr std::uint32_t kWidth = 3840;
    constexpr std::uint32_t kHeight = 2160;
    const document::Vec2d centre{kWidth / 2.0, kHeight / 2.0};
    // Fitted, the circle's left edge lands at (2136, 648) (sharpAt4KOnTheDevice).
    const document::Vec2d target{2136.0, 648.0};
    const auto plain = writeSvg("b4-4k", sharpSvg(""));
    for (const double s : {4.0, 8.0}) {
        const auto name = "4K content pivot, scaled " + std::to_string(static_cast<int>(s)) + "x and turned";
        const auto baked = writeSvg("b4-4k-baked", sharpSvg("translate(" + std::to_string(s * 300.0 - 1860.0) + " " +
                                                            std::to_string(-1040.0) + ") scale(" + std::to_string(27.0 * s) + ")"));
        Fixture layer(plain, commands::SvgImportMode::Layer, kWidth, kHeight);
        Fixture reference(baked, commands::SvgImportMode::Shapes, kWidth, kHeight, false);
        std::filesystem::remove(baked);
        const auto held = pivotOnContent(layer, *layer.artworkNode);
        layer.set(layer.layerParameter(held, document::kScaleParameterRole), document::ParameterValue{document::Vec2d{s, s}});
        layer.set(layer.layerParameter(held, document::kRotationParameterRole), document::ParameterValue{12.0});
        // At the frame's centre (a value the parameter already holds is not an edit).
        const auto position = layer.layerParameter(held, document::kPositionParameterRole);
        const auto* record = composition(layer.document, layer.main).parameters().find(position);
        const auto* constant = record ? std::get_if<document::ConstantValueSource>(&record->source) : nullptr;
        if (constant == nullptr || !(constant->value == document::ParameterValue{centre}))
            layer.set(position, document::ParameterValue{centre});
        const runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
        const auto compiled = compiler.compile({layer.document.snapshot(), layer.main}, {});
        expectations.expect(compiled.plan != nullptr && contentScaleOf(*compiled.plan, held) == s,
                            name + ": drawn at " + std::to_string(static_cast<int>(s)) + "x");
        // A content pivot is its content's centre plus the anchor, in the file's fitted units (its
        // s x picture's centre over s): the anchor puts the target there.
        const auto content = pivotSource(expectations, layer, held, name + " one layer");
        if (!content)
            continue;
        layer.set(layer.layerParameter(held, document::kAnchorParameterRole),
                  document::ParameterValue{document::Vec2d{target.x - content->x / s, target.y - content->y / s}});
        // The scaled pre-composition: the same matrix, about s times the target (the entry's own
        // layer pivots on the frame: the frame's centre plus its anchor).
        reference.set(reference.layerParameter(document::kAnchorParameterRole),
                      document::ParameterValue{document::Vec2d{s * target.x - centre.x, s * target.y - centre.y}});
        reference.set(reference.layerParameter(document::kRotationParameterRole), document::ParameterValue{12.0});

        const auto one = largeFrame(expectations, device, layer.document, layer.main, name + " one layer");
        const auto precomposed = largeFrame(expectations, device, reference.document, reference.main,
                                            name + " scaled pre-composition");
        document::VectorArtworkExpansionContext resample;
        resample.composeLayerTransform = false;
        auto fonts = std::make_shared<platform::FontCatalogue>();
        fonts->status = platform::FontCatalogueStatus::Available;
        resample.fonts = fonts;
        const auto snapshot = layer.document.snapshot();
        const auto expansion = document::expandVectorArtwork(snapshot.project(), *snapshot.project().findComposition(layer.main),
                                                             resample);
        document::Project project = snapshot.project();
        *project.findComposition(layer.main) = *expansion.composition;
        const document::Document softDocument(std::move(project));
        const auto soft = largeFrame(expectations, device, softDocument, layer.main, name + " resampled");

        const double frameArea = double{kWidth} * kHeight;
        std::cout << name << ": the largest window drawn is " << one.largestWindow << " pixels (the frame is "
                  << frameArea << "; the whole " << s << "x picture would be " << 3240.0 * 2160.0 * s * s << ")\n";
        expectations.expect(one.largestWindow > 0 && static_cast<double>(one.largestWindow) <= 2.0 * frameArea,
                            name + ": only the window the frame shows is drawn (at most two frames of pixels)");
        constexpr std::uint32_t kCropWidth = 384;
        constexpr std::uint32_t kCropHeight = 216;
        const auto left = kWidth / 2 - kCropWidth / 2;
        const auto top = kHeight / 2 - kCropHeight / 2;
        const auto sharp = crop(one.pixels, kWidth, left, top, kCropWidth, kCropHeight);
        const auto truth = crop(precomposed.pixels, kWidth, left, top, kCropWidth, kCropHeight);
        const auto blurry = crop(soft.pixels, kWidth, left, top, kCropWidth, kCropHeight);
        expectations.expect(!sharp.empty() && !truth.empty() && !blurry.empty(), name + ": all three frames render");
        if (sharp.empty() || truth.empty() || blurry.empty())
            continue;
        const double worst = worstDifference(sharp, truth);
        const auto moved = differingPixels(sharp, truth, 1e-3);
        const auto edges = partialPixels(sharp);
        const auto softMoved = differingPixels(blurry, truth, 1e-2);
        const auto softEdges = partialPixels(blurry);
        std::cout << name << " (crop " << kCropWidth << " x " << kCropHeight << "): worst difference to the scaled "
                  << "pre-composition " << worst << " on " << moved << " pixels (edge pixels " << edges
                  << "); the resampled picture differs on " << softMoved << " pixels (edge pixels " << softEdges << ")\n";
        expectations.expect(edges > 100, name + ": the crop holds an edge");
        expectations.expect(worst <= 4.0 / 16.0 + 1.0 / 255.0 && moved * 4 <= edges,
                            name + ": the crop is the scaled pre-composition's up to a column of a pixel's sixteen "
                                   "coverage samples");
        expectations.expect(softMoved >= 10 * moved + 200 && softEdges > edges + edges / 2,
                            name + ": the resampled picture would be visibly softer");
    }
    std::filesystem::remove(plain);
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
        oversampledOnTheDevice(expectations, device);
        groupOnTheDevice(expectations, device);
        clippedOnTheDevice(expectations, device);
        sharpAt4KOnTheDevice(expectations, device);
        contentPivotOnTheDevice(expectations, device);
        contentPivotSharpAt4KOnTheDevice(expectations, device);
        std::filesystem::remove(composite);
        if (expectations.failures() != 0) {
            std::cerr << expectations.failures() << " Vector Artwork layer expectation(s) failed\n";
            return 1;
        }
        std::cout << "PASS: a Vector Artwork layer renders on the GPU as the CPU reference and the editable import, "
                     "keyed, turned, scaled, masked, retimed, with effects between, in scaled groups, on a "
                     "content pivot, clipped to the window the frame shows, at half resolution, at 1080p and 4K, admitted, retired and cancelled\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected test exception: " << exception.what() << '\n';
        return 1;
    }
}
