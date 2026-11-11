// #343: imported SVG artwork at real sizes on the production GPU route
// (docs/architecture/vector-artwork.md, "Rendering and memory").
//
// A generated file of 1,600 stroked curves and polygons (runs of more than 256 siblings and opacity
// groups, so nested Merges) and a file of edge cases (shapes larger than the frame, a gradient, a
// clip used as a track matte, nested opacity groups) import through AddSvgArtwork into real
// documents. Each state is prepared by the production CpuGpuSceneBuilder, run by the production
// GpuSceneExecutor and compared to the CPU reference (2e-6), cold and warm. Memory is asserted as
// image sizes, never times: the executor holds a few frames however many layers a Merge has, the
// phase claims that live set rather than the CPU reference's whole-Merge peak, retained coverage
// past the stated allowance is claimed from the host root (and refused or waited for by it) and
// returned on release, and the scene cache keeps stacks rather than thousands of layer images.
// Also: animated bounds (a key that moves points beyond the first frame's bounds), Trim Path on an
// imported shape, reduced render scale, cache invalidation by one edited path, and cancellation.
//
// The CPU assertions always run. Without a device the GPU half reports an explicit SKIP (exit 77)
// after the CPU half passed; --require-device makes that a failure.

#include "document_render_parity_support.hpp"
#include "gpu_scene_effect_emission.hpp"
#include "gpu_coverage_ocio_support.hpp"
#include "gpu_scene_preparation_builders.hpp"

#include <bloom/commands/animation_operations.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/commands/svg_import_operations.hpp>
#include <bloom/document/merge_entries.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/value_nodes.hpp>
#include <bloom/document/value_utility_nodes.hpp>
#include <bloom/runtime/gpu_processing_phase_policy.hpp>
#include <bloom/runtime/task_scheduler.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <mutex>
#include <sstream>
#include <thread>
#include <tuple>

namespace {

using namespace bloom;
using namespace bloom::test::document_render;
using render::composite_proof::Options;
using render::composite_proof::parseOptions;

constexpr std::uint32_t kWidth = 256;
constexpr std::uint32_t kHeight = 144;
constexpr std::uint64_t kFrameBytes = std::uint64_t{kWidth} * kHeight * sizeof(render::Rgba32f);
// Room enough for the CPU reference of every fixture here.
constexpr std::uint64_t kCpuRequestBytes = 1ULL << 30;

// A deterministic generator, so the file is the same on every machine.
struct Lcg final {
    std::uint64_t state = 343;
    double next() {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<double>(state >> 11) / static_cast<double>(1ULL << 53);
    }
};

std::string colour(Lcg& random) {
    std::ostringstream text;
    text << "rgb(" << static_cast<int>(random.next() * 255) << ','
         << static_cast<int>(random.next() * 255) << ',' << static_cast<int>(random.next() * 255)
         << ')';
    return text.str();
}

// 1,600 elements: 1,000 siblings in one group (four runs of at most 256) and six opacity groups
// of 100. Ellipses and cubic paths are curves, so flattening is exercised; every one is stroked.
std::string denseSvg() {
    Lcg random;
    std::ostringstream svg;
    svg << "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 " << kWidth << ' ' << kHeight
        << "\" width=\"" << kWidth << "\" height=\"" << kHeight << "\">\n";
    const auto element = [&](const int index) {
        const double x = random.next() * kWidth;
        const double y = random.next() * kHeight;
        const double r = 2 + random.next() * 9;
        svg << "<";
        switch (index % 3) {
        case 0:
            svg << "ellipse cx=\"" << x << "\" cy=\"" << y << "\" rx=\"" << r << "\" ry=\""
                << r * 0.6 << '"';
            break;
        case 1:
            svg << "path d=\"M" << x << ' ' << y << " c" << r << ' ' << -r << ' ' << 2 * r << ' '
                << r << ' ' << r * 0.5 << ' ' << 2 * r << " s" << -2 * r << ' ' << -r << ' '
                << -r * 0.5 << ' ' << -2 * r << "z\"";
            break;
        default:
            svg << "polygon points=\"" << x << ',' << y << ' ' << x + r << ',' << y + r * 0.3 << ' '
                << x + r * 0.2 << ',' << y + r << "\"";
            break;
        }
        svg << " id=\"e" << index << "\" fill=\"" << colour(random) << "\" stroke=\""
            << colour(random) << "\" stroke-width=\"0.6\"/>\n";
    };
    svg << "<g id=\"dense\">\n";
    int index = 0;
    for (; index < 1000; ++index)
        element(index);
    svg << "</g>\n";
    for (int group = 0; group < 6; ++group) {
        svg << "<g id=\"group" << group << "\" opacity=\"0.85\">\n";
        for (int member = 0; member < 100; ++member)
            element(index++);
        svg << "</g>\n";
    }
    svg << "</svg>\n";
    return svg.str();
}

// Shapes larger than the frame, a gradient, a clip (a track matte), nested opacity groups and an
// open stroked curve.
std::string edgeSvg() {
    std::ostringstream svg;
    svg << "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 " << kWidth << ' ' << kHeight
        << "\" width=\"" << kWidth << "\" height=\"" << kHeight << R"svg(">
<defs>
  <linearGradient id="ramp" x1="0" y1="0" x2="1" y2="0">
    <stop offset="0" stop-color="#ff2000"/><stop offset="1" stop-color="#0030ff"/>
  </linearGradient>
  <clipPath id="disc"><circle cx="190" cy="72" r="40"/></clipPath>
</defs>
<ellipse id="huge" cx="128" cy="72" rx="600" ry="420" fill="#204060" fill-opacity="0.5"/>
<rect id="wide" x="-300" y="20" width="900" height="18" fill="url(#ramp)"/>
<rect id="clipped" x="0" y="0" width="256" height="144" fill="#30c040" clip-path="url(#disc)"/>
<g id="outer" opacity="0.6"><g id="inner" opacity="0.7">
  <path id="blob" d="M30 90 C 40 60, 90 60, 100 90 S 150 130, 60 130 Z" fill="#e0e020"/>
</g></g>
<path id="curve" d="M10 10 Q 120 140 240 12" fill="none" stroke="#ffffff" stroke-width="3"/>
</svg>
)svg";
    return svg.str();
}

std::filesystem::path writeSvg(const std::string& name, const std::string& text) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto path = std::filesystem::temp_directory_path() /
                      ("bloom-gpu-svg-" + name + "-" + std::to_string(stamp) + ".svg");
    std::ofstream(path, std::ios::binary) << text;
    return path;
}

// Applies one edit, naming it and the operation's own message when it is refused.
template <typename Operation, typename... Arguments>
void apply(const std::string& label, document::Document& document, commands::CommandStack& commands,
           Arguments&&... arguments) {
    commands::Transaction transaction(label, document.snapshot().revision());
    transaction.emplace<Operation>(std::forward<Arguments>(arguments)...);
    const auto result = commands.execute(std::move(transaction));
    if (result.changed())
        return;
    std::string reason = label + " was refused";
    for (const auto& failure : result.operationFailures)
        reason += ": " + failure.issue.message;
    throw std::runtime_error(reason);
}

struct Fixture final {
    document::NewProject initial = document::makeNewProject(
        "SVG artwork", "Main", core::RationalTime::fromInteger(4),
        *document::CompositionFormat::create(kWidth, kHeight, core::PixelAspectRatio::square(),
                                             *document::FrameRate::create(24, 1)));
    document::CompositionId main = initial.initialCompositionId;
    document::Document document{std::move(initial.project)};
    commands::CommandStack commands{document};

    explicit Fixture(const std::string& name, const std::string& svg) {
        const auto path = writeSvg(name, svg);
        apply<commands::AddSvgArtwork>("import " + name, document, commands, main, nullptr, path,
                                       name, true, commands::SvgImportMode::Shapes);
        std::filesystem::remove(path);
    }
    // An existing file, fitted to a composition of the given size.
    Fixture(const std::filesystem::path& file, const std::uint32_t width,
            const std::uint32_t height)
        : initial(document::makeNewProject(
              "SVG artwork", "Main", core::RationalTime::fromInteger(4),
              *document::CompositionFormat::create(width, height, core::PixelAspectRatio::square(),
                                                   *document::FrameRate::create(24, 1)))) {
        apply<commands::AddSvgArtwork>("import " + file.filename().string(), document, commands,
                                       main, nullptr, file, file.stem().string(), true,
                                       commands::SvgImportMode::Shapes);
    }
    [[nodiscard]] std::vector<document::NodeId> pathNodes() const {
        std::vector<document::NodeId> nodes;
        for (const auto& node : composition(document, main).graph().nodes())
            if (node.typeId == document::kPathValueNodeType)
                nodes.push_back(node.id);
        return nodes;
    }
    [[nodiscard]] document::PathValue pathOf(const document::NodeId node) const {
        const auto* record =
            composition(document, main)
                .parameters()
                .find(parameter(document, main, node, document::kValueParameterRole));
        // A keyed path reads as empty here; callers look for constant ones.
        const auto* constant = std::get_if<document::ConstantValueSource>(&record->source);
        const auto* value = constant ? std::get_if<document::PathValue>(&constant->value) : nullptr;
        return value ? *value : document::PathValue{};
    }
    // The Shape Source whose path parameter is bound to a Path node's value.
    [[nodiscard]] document::NodeId shapeDrivenBy(const document::NodeId producer) const {
        for (const auto& node : composition(document, main).graph().nodes()) {
            if (node.typeId != document::kShapeSourceNodeType)
                continue;
            const auto* record = composition(document, main)
                                     .parameters()
                                     .find(parameter(document, main, node.id, "path"));
            const auto* binding =
                record ? std::get_if<document::DriverBindingSource>(&record->source) : nullptr;
            if (binding != nullptr && binding->sourceNodeId == producer)
                return node.id;
        }
        throw std::runtime_error("the Path node drives no shape");
    }
};

struct Rendered final {
    std::shared_ptr<const runtime::CompiledCompositionPlan> plan;
    runtime::EvaluationRequest request;
    std::shared_ptr<const runtime::ProcessFrame> frame;
};

// The CPU reference at a time and resolution, with the larger request the dense file needs.
[[nodiscard]] std::optional<Rendered>
referenceAt(Expectations& expectations, const Fixture& fixture, const core::RationalTime time,
            const std::string& label, const runtime::EvaluationResolution resolution = {}) {
    const runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
    const auto compiled = compiler.compile({fixture.document.snapshot(), fixture.main}, {});
    expectations.expect(compiled.plan != nullptr, label + ": compiles");
    if (!compiled.plan)
        return std::nullopt;
    auto request = requestFor(*compiled.plan);
    request.time = time;
    request.resolution = resolution;
    request.pixelStorageByteLimit = kCpuRequestBytes;
    auto oracle = request;
    oracle.bypassOperationCache = true;
    const auto result = runtime::CpuCompositionEvaluator{}.evaluate(compiled.plan, oracle, {});
    std::string why;
    for (const auto& diagnostic : result.diagnostics())
        why += " " + diagnostic.summary + " " + diagnostic.detail;
    expectations.expect(result.frame() != nullptr, label + ": the CPU reference evaluates:" + why);
    if (result.frame() == nullptr)
        return std::nullopt;
    return Rendered{compiled.plan, request, result.frame()};
}

[[nodiscard]] float alphaOf(const Rendered& rendered, const std::int64_t x, const std::int64_t y) {
    const auto& image = rendered.frame->processImage();
    const auto window = image.descriptor()->dataWindow();
    if (x < window.originX() || y < window.originY() || x >= window.maxXExclusive() ||
        y >= window.maxYExclusive())
        return 0.0F;
    const auto width = static_cast<std::int64_t>(window.extent().width());
    return image
        .pixels()[static_cast<std::size_t>((y - window.originY()) * width + (x - window.originX()))]
        .alpha();
}

[[nodiscard]] bool samePixels(const Rendered& a, const Rendered& b) {
    const auto left = a.frame->processImage().pixels();
    const auto right = b.frame->processImage().pixels();
    return left.size_bytes() == right.size_bytes() &&
           std::memcmp(left.data(), right.data(), left.size_bytes()) == 0;
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
        TaskRequest("gpu svg artwork cancel token",
                    {.kind = TaskOwnerKind::Composition, .id = TaskOwnerId::fromRaw(343)}),
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
    while (!submission.handle.tryTakeResult().has_value() &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return token;
}

// ---- CPU half: what the phase claims and how retained storage grows, is refused and returns ----

constexpr std::uint64_t kGib = 1ULL << 30;

std::shared_ptr<runtime::GpuProcessingPhasePolicy>
policyWithHost(const std::uint64_t limitBytes, const std::uint64_t currentBytes,
               std::shared_ptr<const render::GpuAllocationSizes> allocationSizes = nullptr) {
    using namespace runtime;
    auto policy = std::make_shared<GpuProcessingPhasePolicy>(4);
    const auto now = GpuProcessingPhasePolicy::Clock::now();
    MemoryBudgetState sample;
    sample.machine.availableBytes = 60 * kGib;
    sample.process.status = ProcessMemoryStatus::Finite;
    sample.process.source = ProcessMemorySource::LinuxCgroupV2;
    sample.process.sampledAt = now;
    sample.process.domains.push_back({.path = "/own",
                                      .kind = ProcessMemoryLimitKind::Finite,
                                      .limitBytes = limitBytes,
                                      .currentBytes = currentBytes});
    sample.processSampleFresh = true;
    sample.sampleSerial = 1;
    policy->applyHostSample(sample, 60 * kGib, now);
    static_cast<void>(
        policy->reportDevice(343, {.source = render::GpuAllocationBudgetSource::MemoryBudget,
                                   .device_local_bytes = 8 * kGib,
                                   .available_bytes = 8 * kGib,
                                   .allocation_sizes = std::move(allocationSizes)}));
    return policy;
}

runtime::GpuScenePreparationPhase
phaseFor(std::shared_ptr<runtime::GpuProcessingPhasePolicy> policy, const bool grows) {
    return {.policy = std::move(policy),
            .envelope = {.output = runtime::GpuProcessingPhaseOutput::FloatFinal,
                         .metadataBytes = 1024ULL * 1024,
                         .retainedCoverageByteLimit = 64ULL * 1024,
                         .growsRetainedSceneStorage = grows,
                         .requestedExecutorBytes = 4 * kGib,
                         .minimumExecutorBytes = kFrameBytes,
                         .deviceWorkingBytes = 1024ULL * 1024,
                         .admissionClass = runtime::GpuAdmissionClass::Export}};
}

// The fixed host need of such a phase for this frame (no sources).
std::uint64_t fixedHostNeed(const runtime::GpuScenePreparationPhase& phase) {
    auto envelope = phase.envelope;
    envelope.pixels = std::uint64_t{kWidth} * kHeight;
    envelope.sourceFree = true;
    const auto request = runtime::processingPhaseRequest(envelope);
    return request ? request->fixedHostBytes : 0;
}

void admissionAndRetirement(Expectations& expectations, const Rendered& dense) {
    using runtime::PreparedGpuSceneDiagnosticCode;
    // The device need follows the executor's live set, not the CPU reference's whole-Merge peak.
    const auto cpuPlan =
        runtime::CpuCompositionEvaluator{}.planPreflightPixelStorage(dense.plan, dense.request);
    const auto livePeak = runtime::detail::gpuExecutorLivePeakBytes(
        *dense.plan, dense.request.output.value(), kFrameBytes);
    // The CPU reference folds each Merge entry by entry, as the executor does, rather than holding
    // a whole run of 256 layers at once.
    expectations.expect(
        cpuPlan.requiredPreflightBytes && *cpuPlan.requiredPreflightBytes <= 8 * kFrameBytes,
        "memory: the CPU reference folds its Merges into a few frames (" +
            std::to_string(cpuPlan.requiredPreflightBytes.value_or(0) / kFrameBytes) + " frames)");
    {
        // ...and renders the same pixels within that much storage.
        auto tight = dense.request;
        tight.pixelStorageByteLimit = 8 * kFrameBytes;
        tight.bypassOperationCache = true;
        const auto folded = runtime::CpuCompositionEvaluator{}.evaluate(dense.plan, tight, {});
        std::string why;
        for (const auto& diagnostic : folded.diagnostics())
            why += " " + diagnostic.summary + " " + diagnostic.detail;
        expectations.expect(folded.frame() != nullptr &&
                                samePixels(dense, Rendered{dense.plan, tight, folded.frame()}),
                            "memory: the folded CPU reference renders within eight frames:" + why);
    }
    expectations.expect(livePeak <= 8 * kFrameBytes,
                        "memory: the executor's live set is a few frames, not one per layer (" +
                            std::to_string(livePeak / kFrameBytes) + " frames)");

    auto ample = policyWithHost(8 * kGib, 6 * kGib);
    {
        const auto phase = phaseFor(ample, true);
        const runtime::CpuGpuSceneBuilder builder(nullptr, {}, {}, phase);
        const auto built = builder.build(dense.plan, dense.request);
        expectations.expect(built.hasValue() && built.phaseLease != nullptr,
                            "admission: the dense scene is admitted: " + built.diagnostic.message);
        if (built.phaseLease) {
            expectations.expect(built.phaseLease->deviceNeedBytes() <=
                                    phase.envelope.deviceWorkingBytes + 8 * kFrameBytes,
                                "admission: the device need is the live set, image-sized");
            expectations.expect(built.phaseLease->hostBytes() > fixedHostNeed(phase),
                                "admission: retained coverage past the allowance is claimed");
            expectations.expect(ample->snapshot().reservations.totalHostClaimBytes >=
                                    built.phaseLease->hostBytes(),
                                "admission: the grown claim is counted at the root");
        }
        // The stated allowance stays a hard ceiling for a phase that does not grow.
        const runtime::CpuGpuSceneBuilder fixed(nullptr, {}, {}, phaseFor(ample, false));
        const auto refused = fixed.build(dense.plan, dense.request);
        expectations.expect(!refused.hasValue() &&
                                refused.diagnostic.code ==
                                    PreparedGpuSceneDiagnosticCode::BudgetRefused,
                            "admission: a phase that does not grow keeps its coverage ceiling");
    }
    expectations.expect(ample->snapshot().reservations.totalHostClaimBytes == 0 &&
                            ample->snapshot().reservations.admittedPhases == 0,
                        "retirement: releasing the scene returns every grown byte");

    // Capacity just above the fixed need: within the machine's ceiling the growth waits for room,
    // beyond it the scene is refused. Neither falls back.
    const auto fixedNeed = fixedHostNeed(phaseFor(ample, true));
    const std::uint64_t room = fixedNeed + 512 * 1024;
    {
        auto tight = policyWithHost(8 * kGib, 8 * kGib - room);
        const runtime::CpuGpuSceneBuilder builder(nullptr, {}, {}, phaseFor(tight, true));
        const auto waiting = builder.build(dense.plan, dense.request);
        expectations.expect(!waiting.hasValue() &&
                                waiting.diagnostic.code ==
                                    PreparedGpuSceneDiagnosticCode::AwaitingCapacity &&
                                waiting.diagnostic.admissionRetry.has_value(),
                            "admission: growth that other memory holds waits for room");
        expectations.expect(tight->snapshot().reservations.totalHostClaimBytes == 0,
                            "retirement: a waiting scene holds nothing");
    }
    {
        auto small = policyWithHost(room, 0);
        const runtime::CpuGpuSceneBuilder builder(nullptr, {}, {}, phaseFor(small, true));
        const auto refused = builder.build(dense.plan, dense.request);
        expectations.expect(!refused.hasValue() &&
                                refused.diagnostic.code ==
                                    PreparedGpuSceneDiagnosticCode::BudgetRefused,
                            "admission: growth beyond what the machine could ever give is refused");
        expectations.expect(small->snapshot().reservations.totalHostClaimBytes == 0,
                            "retirement: a refused scene holds nothing");
    }
}

// ---- GPU half ----

struct Device final {
    std::unique_ptr<render::GpuDevice> device;
    [[nodiscard]] std::unique_ptr<runtime::GpuSceneCache>
    cache(runtime::GpuSceneCacheBudgets budgets = {}) const {
        auto created = runtime::GpuSceneCache::create(*device, budgets);
        return std::move(created.cache);
    }
};

[[nodiscard]] std::unique_ptr<runtime::GpuSceneExecutor>
executorFor(render::GpuDevice& device, runtime::GpuSceneCache& cache,
            runtime::GpuSceneExecutorBudgets budgets = {}) {
    auto created = runtime::GpuSceneExecutor::create(device, cache, budgets);
    return std::move(created.executor);
}

[[nodiscard]] std::shared_ptr<const runtime::PreparedGpuScene>
prepare(Expectations& expectations, const Rendered& rendered, const std::string& label) {
    const auto built = runtime::CpuGpuSceneBuilder{}.build(rendered.plan, rendered.request);
    expectations.expect(built.hasValue(), label + ": prepares: " + built.diagnostic.message);
    return built.scene;
}

// Cold and warm parity on a fresh executor, and the bounds the CPU publishes.
void parityOf(Expectations& expectations, const Device& device, const Rendered& rendered,
              const std::string& label) {
    auto cache = device.cache();
    auto executor = cache ? executorFor(*device.device, *cache) : nullptr;
    expectations.expect(executor != nullptr, label + ": the executor creates");
    if (!executor)
        return;
    gpuParity(expectations, *executor, Reference{rendered.plan, rendered.request, rendered.frame},
              label);
}

void denseOnTheDevice(Expectations& expectations, const Device& device, Fixture& fixture,
                      const Rendered& dense) {
    const auto scene = prepare(expectations, dense, "dense");
    if (!scene)
        return;
    expectations.expect(scene->commands().size() > 4096,
                        "dense: the scene is past the former 4,096-command ceiling (" +
                            std::to_string(scene->commands().size()) + " commands)");
    auto cache = device.cache();
    auto executor = cache ? executorFor(*device.device, *cache) : nullptr;
    if (!executor)
        return;
    const auto expected = dense.frame->processImage().pixels();
    const auto cold = runScene(*executor, scene);
    expectations.expect(parity(cold, expected),
                        "dense: GPU matches the CPU reference: " + executor->diagnostic().message);
    const auto counters = executor->counters();
    // Batched submission keeps up to eight kernels' outputs until their fence, within its own
    // byte bound; the live set itself is measured serially.
    runtime::GpuSceneExecutorBudgets serialBudgets;
    serialBudgets.enableNativeBatches = false;
    auto serialCache = device.cache();
    auto serial = serialCache ? executorFor(*device.device, *serialCache, serialBudgets) : nullptr;
    if (serial) {
        const auto pixels = runScene(*serial, scene);
        const auto peak = serial->counters().peakLiveImageBytes;
        std::cout << "dense " << kWidth << 'x' << kHeight << ": serial live peak " << peak
                  << " bytes (" << static_cast<double>(peak) / kFrameBytes
                  << " frames), batched peak " << counters.peakLiveImageBytes << " bytes ("
                  << static_cast<double>(counters.peakLiveImageBytes) / kFrameBytes << " frames), "
                  << scene->commands().size() << " commands\n";
        expectations.expect(parity(pixels, expected), "dense: the serial run matches too");
        expectations.expect(peak <= runtime::detail::gpuExecutorLivePeakBytes(
                                        *dense.plan, dense.request.output.value(), kFrameBytes) +
                                        kFrameBytes,
                            "memory: the serial live set is what the phase claims for it");
        expectations.expect(counters.peakLiveImageBytes <=
                                peak + runtime::GpuSceneExecutorBudgets{}.maxNativeBatchOutputBytes,
                            "memory: batching adds at most its own output bound");
    }
    expectations.expect(cache->entryCount() < 64,
                        "memory: the scene cache keeps stacks, not every layer image (" +
                            std::to_string(cache->entryCount()) + " entries)");
    const auto coldDispatches = counters.dispatches;
    const auto warm = runScene(*executor, scene);
    expectations.expect(parity(warm, expected) && executor->counters().dispatches == coldDispatches,
                        "dense: a warm rerun matches and dispatches nothing new");

    // One path edited: its run of layers runs again, the untouched stacks are reused.
    const auto nodes = fixture.pathNodes();
    auto value = fixture.pathOf(nodes[10]);
    value.contours[0].anchors[0].point.x += 9.0;
    apply<commands::SetParameterSource>(
        "SetParameterSource", fixture.document, fixture.commands, fixture.main,
        parameter(fixture.document, fixture.main, nodes[10], document::kValueParameterRole),
        document::ConstantValueSource{std::move(value)});
    const auto edited = referenceAt(expectations, fixture, {}, "dense edited");
    if (!edited)
        return;
    expectations.expect(!samePixels(*edited, dense), "invalidation: the edit changes the frame");
    const auto editedScene = prepare(expectations, *edited, "dense edited");
    if (!editedScene)
        return;
    const auto before = executor->counters().dispatches;
    const auto editedPixels = runScene(*executor, editedScene);
    const auto editedDispatches = executor->counters().dispatches - before;
    expectations.expect(parity(editedPixels, edited->frame->processImage().pixels()),
                        "invalidation: the edited frame matches the CPU reference");
    expectations.expect(editedDispatches > 0 && editedDispatches < coldDispatches / 2,
                        "invalidation: the edit re-runs its own stack, not the scene (" +
                            std::to_string(editedDispatches) + " of " +
                            std::to_string(coldDispatches) + " dispatches)");

    // A cache that may keep only eight images still renders the scene right.
    auto small = device.cache({.maxRetainedImages = 8});
    auto smallExecutor = small ? executorFor(*device.device, *small) : nullptr;
    if (smallExecutor) {
        const auto pixels = runScene(*smallExecutor, editedScene);
        expectations.expect(parity(pixels, edited->frame->processImage().pixels()) &&
                                small->entryCount() <= 8,
                            "memory: a cache bounded to eight images keeps eight and renders");
    }

    // Reduced render scale.
    const auto halfExtent = render::ImageExtent::create(kWidth / 2, kHeight / 2);
    const auto half = referenceAt(expectations, fixture, {}, "dense half",
                                  runtime::ProxyResolution{*halfExtent.value()});
    if (half)
        parityOf(expectations, device, *half, "dense at half resolution");
}

void cancellation(Expectations& expectations, const Device& device, const Rendered& dense) {
    const runtime::CpuGpuSceneBuilder builder;
    const auto cancelled = builder.build(dense.plan, dense.request, cancelledToken());
    expectations.expect(!cancelled.hasValue() &&
                            cancelled.diagnostic.code ==
                                runtime::PreparedGpuSceneDiagnosticCode::Cancelled,
                        "cancellation: a cancelled preparation reports Cancelled");
    const auto scene = prepare(expectations, dense, "cancel");
    auto cache = device.cache();
    auto executor = cache ? executorFor(*device.device, *cache) : nullptr;
    if (!scene || !executor)
        return;
    expectations.expect(executor->begin(scene, kSceneBudget).code ==
                            runtime::GpuSceneExecutorDiagnosticCode::None,
                        "native cancel: begins");
    for (int step = 0; step < 50; ++step)
        static_cast<void>(executor->poll());
    executor->cancel();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (executor->state() == runtime::GpuSceneExecutorJobState::Pending &&
           std::chrono::steady_clock::now() < deadline)
        static_cast<void>(executor->poll());
    expectations.expect(executor->state() != runtime::GpuSceneExecutorJobState::Ready &&
                            executor->image() == nullptr,
                        "native cancel: a cancelled dense scene publishes no image");
    while (executor->hasUnretiredSubmission() && std::chrono::steady_clock::now() < deadline)
        static_cast<void>(executor->poll());
    expectations.expect(parity(runScene(*executor, scene), dense.frame->processImage().pixels()),
                        "native cancel: the same executor completes the scene afterwards");
    const auto refused = executor->begin(scene, kFrameBytes / 2);
    expectations.expect(refused.code != runtime::GpuSceneExecutorDiagnosticCode::None,
                        "budget: the executor refuses a request below one frame");
}

void editsOnTheDevice(Expectations& expectations, const Device& device, Fixture& edges) {
    // Animated bounds: the blob's points move 150 px right by two seconds, far beyond the bounds
    // the first frame has, and partly beyond the frame.
    const auto nodes = edges.pathNodes();
    document::NodeId blob{};
    for (const auto node : nodes) {
        const auto value = edges.pathOf(node);
        if (!value.contours.empty() && !value.contours[0].anchors.empty() &&
            value.contours[0].anchors[0].point.x < 40 && value.contours[0].anchors[0].point.y > 80)
            blob = node;
    }
    expectations.expect(blob.isValid(), "edges: the blob has its Path node");
    if (!blob.isValid())
        return;
    const auto start = edges.pathOf(blob);
    auto moved = start;
    for (auto& contour : moved.contours)
        for (auto& anchor : contour.anchors) {
            anchor.point.x += 150.0;
            if (anchor.inHandle)
                anchor.inHandle->x += 150.0;
            if (anchor.outHandle)
                anchor.outHandle->x += 150.0;
        }
    const auto valueOf = [&](const document::NodeId node) {
        return parameter(edges.document, edges.main, node, document::kValueParameterRole);
    };
    // The first key holds the value the constant already has, so the moved key goes in first.
    apply<commands::SetKeyframeAtTimeForParameter>("key the moved blob", edges.document,
                                                   edges.commands, edges.main, valueOf(blob),
                                                   core::RationalTime::fromInteger(2), moved);
    apply<commands::SetKeyframeAtTimeForParameter>("key the blob's start", edges.document,
                                                   edges.commands, edges.main, valueOf(blob),
                                                   core::RationalTime{}, start);
    const auto first = referenceAt(expectations, edges, {}, "animated first");
    const auto between =
        referenceAt(expectations, edges, core::RationalTime::fromInteger(1), "animated between");
    const auto last =
        referenceAt(expectations, edges, core::RationalTime::fromInteger(2), "animated last");
    if (!first || !between || !last)
        return;
    // Inside the moved blob at two seconds, outside the first frame's blob.
    expectations.expect(alphaOf(*last, 215, 110) > alphaOf(*first, 215, 110),
                        "animated bounds: the key draws beyond the first frame's bounds");
    parityOf(expectations, device, *between, "animated bounds between keys");
    parityOf(expectations, device, *last, "animated bounds at the second key");

    // Trim Path on the open curve's Path node, between it and its shape.
    document::NodeId curve{};
    for (const auto node : nodes) {
        const auto value = edges.pathOf(node);
        if (!value.contours.empty() && !value.contours[0].closed)
            curve = node;
    }
    expectations.expect(curve.isValid(), "edges: the open curve has its Path node");
    if (!curve.isValid())
        return;
    const auto shape = edges.shapeDrivenBy(curve);
    apply<commands::AddNode>("AddNode", edges.document, edges.commands, edges.main,
                             std::string(document::kTrimPathNodeType), document::Vec2d{0, 0});
    const auto trim = lastNode(edges.document, edges.main, document::kTrimPathNodeType);
    // Bound the way the importer binds its own Path Stroke nodes.
    apply<commands::SetParameterSource>(
        "bind the trim to the curve", edges.document, edges.commands, edges.main,
        parameter(edges.document, edges.main, trim, "path"),
        document::DriverBindingSource{curve, std::string(document::kValuePortName)});
    apply<commands::SetParameterSource>("bind the shape to the trim", edges.document,
                                        edges.commands, edges.main,
                                        parameter(edges.document, edges.main, shape, "path"),
                                        document::DriverBindingSource{trim, "path"});
    apply<commands::SetParameterSource>(
        "trim end", edges.document, edges.commands, edges.main,
        parameter(edges.document, edges.main, trim, "end"),
        document::ConstantValueSource{document::ParameterValue{40.0}});
    const auto trimmed = referenceAt(expectations, edges, {}, "trimmed");
    if (!trimmed)
        return;
    expectations.expect(!samePixels(*trimmed, *first), "trim: Trim Path changes the curve");
    parityOf(expectations, device, *trimmed, "Trim Path on an imported shape");
}

// ---- Gradients drawn in their own window (#343, Package 1b) ----
//
// Firefox-like artwork at 512x288: elliptical radial gradients (an objectBoundingBox gradient on a
// non-square shape, turned by gradientTransform), one on a shape wider than the frame, inside
// nested opacity groups, and a group clipped by a circle (a track matte over a Merge entry). Each
// gradient layer used to be drawn over its whole transformed window (about ten frames) and its
// matte and the Merges above inherited that window. Now a layer is drawn only where the frame can
// show it and its track matte lets it through, on both routes; what is still larger than a frame
// (the gradient node's own window) is counted at its real size by the phase.

constexpr std::uint32_t kGradientWidth = 512;
constexpr std::uint32_t kGradientHeight = 288;
constexpr std::uint64_t kGradientFrameBytes =
    std::uint64_t{kGradientWidth} * kGradientHeight * sizeof(render::Rgba32f);

std::string gradientSvg() {
    return R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 512 288" width="512" height="288">
<defs>
  <radialGradient id="a" cx="60%" cy="30%" r="120%" gradientTransform="rotate(25 .5 .5)">
    <stop offset=".1" stop-color="#ffe226"/><stop offset=".8" stop-color="#ff7139" stop-opacity=".6"/>
  </radialGradient>
  <radialGradient id="b" cx="20%" cy="110%" r="90%">
    <stop offset="0" stop-color="#960e18"/><stop offset=".6" stop-color="#ff3750" stop-opacity="0"/>
  </radialGradient>
  <linearGradient id="l" x1="0" y1="0" x2="1" y2="1">
    <stop offset="0" stop-color="#20c0ff"/><stop offset="1" stop-color="#4020ff"/>
  </linearGradient>
  <clipPath id="disc"><circle cx="380" cy="150" r="90"/></clipPath>
</defs>
<g id="flame" opacity="0.9">
  <ellipse id="body" cx="200" cy="150" rx="150" ry="60" fill="url(#a)"/>
  <g id="inner" opacity="0.8">
    <path id="tail" d="M60 40 C 200 0, 300 120, 470 60 L 520 140 C 300 200, 150 120, 40 120 Z" fill="url(#b)"/>
    <rect id="wide" x="-80" y="200" width="700" height="40" fill="url(#b)"/>
  </g>
</g>
<g id="clipped" clip-path="url(#disc)">
  <rect id="sheet" x="250" y="40" width="250" height="230" fill="url(#l)"/>
  <ellipse id="glow" cx="400" cy="120" rx="120" ry="30" fill="url(#a)"/>
</g>
</svg>
)svg";
}

// Every operation's prepared output window, read back from the scene's commands.
[[nodiscard]] std::vector<std::optional<render::ImageWindow>>
preparedWindows(const runtime::PreparedGpuScene& scene) {
    std::vector<std::optional<render::ImageWindow>> windows(scene.commandForOperation().size());
    for (std::size_t index = 0; index < windows.size(); ++index) {
        const auto command = scene.commandForOperation()[index];
        if (const auto descriptor =
                runtime::detail::gpuSceneCommandDescriptorOf(scene.commands(), command, 0))
            windows[index] = descriptor->data;
    }
    return windows;
}

[[nodiscard]] std::uint64_t windowBytes(const render::ImageWindow window) {
    return std::uint64_t{window.extent().width()} * window.extent().height() *
           sizeof(render::Rgba32f);
}

[[nodiscard]] bool within(const render::ImageWindow inner, const render::ImageWindow outer) {
    return inner.originX() >= outer.originX() && inner.originY() >= outer.originY() &&
           inner.maxXExclusive() <= outer.maxXExclusive() &&
           inner.maxYExclusive() <= outer.maxYExclusive();
}

// The entry layer the importer made for a group or element of the file.
[[nodiscard]] const document::LayerOutputBoundary& layerNamed(const Fixture& fixture,
                                                              const std::string& name) {
    for (const auto& layer : composition(fixture.document, fixture.main).graph().layerOutputs())
        if (layer.name == name)
            return layer;
    throw std::runtime_error("no layer is named " + name);
}

// Window sizes and the phase's claim, without a device: every layer image, track matte and Merge
// is at most a frame and a matted gradient is drawn only inside its matte; the gradient node's own
// window is larger than the frame, and the phase claims the live set at those real sizes.
void gradientWindowsAndAdmission(Expectations& expectations, const Rendered& gradients) {
    const auto built = runtime::CpuGpuSceneBuilder{}.build(gradients.plan, gradients.request);
    expectations.expect(built.hasValue(), "gradients: prepares: " + built.diagnostic.message);
    if (!built.hasValue())
        return;
    const auto& commands = built.scene->commands();
    // One pixel of border per nesting level: a layer's bilinear support around a stack.
    const std::uint64_t frameAndBorder =
        std::uint64_t{kGradientWidth + 8} * (kGradientHeight + 8) * sizeof(render::Rgba32f);
    std::uint64_t largestLayerImage = 0;
    std::uint64_t largestGradientNode = 0;
    std::size_t mattes = 0;
    bool mattesBounded = true;
    for (const auto& command : commands) {
        const auto index = std::visit([](const auto& item) { return item.index; }, command);
        const auto descriptor = runtime::detail::gpuSceneCommandDescriptorOf(commands, index, 0);
        if (!descriptor)
            continue;
        const auto bytes = windowBytes(descriptor->data);
        if (std::holds_alternative<runtime::GpuSceneSolidCommand>(command)) {
            largestGradientNode = std::max(largestGradientNode, bytes);
            continue;
        }
        if (!std::holds_alternative<runtime::GpuSceneCompositionOutputCommand>(command))
            largestLayerImage = std::max(largestLayerImage, bytes);
        if (const auto* blend = std::get_if<runtime::GpuSceneBlendCommand>(&command);
            blend != nullptr && blend->imageCombine) {
            ++mattes;
            const auto matte =
                runtime::detail::gpuSceneCommandDescriptorOf(commands, blend->source, 0);
            mattesBounded = mattesBounded && matte && within(blend->outputWindow, matte->data);
        }
    }
    expectations.expect(
        largestLayerImage <= frameAndBorder,
        "memory: every layer image, matte and Merge is at most a frame (" +
            std::to_string(static_cast<double>(largestLayerImage) / kGradientFrameBytes) +
            " frames)");
    expectations.expect(mattes >= 4 && mattesBounded,
                        "memory: each matted gradient is drawn only inside its matte (" +
                            std::to_string(mattes) + " mattes)");
    expectations.expect(
        largestGradientNode > kGradientFrameBytes,
        "memory: a gradient node's own window is larger than the frame (" +
            std::to_string(static_cast<double>(largestGradientNode) / kGradientFrameBytes) +
            " frames)");

    // The admission floor takes each image as a frame; the prepared scene's images at their real
    // sizes are more, and the phase claims that.
    const auto windows = preparedWindows(*built.scene);
    const auto output = gradients.request.output.value();
    const auto realPeak = runtime::detail::gpuExecutorLivePeakBytes(*gradients.plan, output,
                                                                    kGradientFrameBytes, &windows);
    const auto cpuPlan = runtime::CpuCompositionEvaluator{}.planPreflightPixelStorage(
        gradients.plan, gradients.request);
    const auto floor =
        std::max(kGradientFrameBytes,
                 std::min<std::uint64_t>(cpuPlan.requiredPreflightBytes.value_or(0),
                                         runtime::detail::gpuExecutorLivePeakBytes(
                                             *gradients.plan, output, kGradientFrameBytes)));
    expectations.expect(
        realPeak > floor && realPeak >= largestGradientNode,
        "admission: images at their real sizes need more than a frame each (" +
            std::to_string(static_cast<double>(realPeak) / kGradientFrameBytes) + " against " +
            std::to_string(static_cast<double>(floor) / kGradientFrameBytes) + " frames)");
    auto policy = policyWithHost(8 * kGib, 6 * kGib);
    auto phase = phaseFor(policy, true);
    phase.envelope.minimumExecutorBytes = kGradientFrameBytes;
    {
        const runtime::CpuGpuSceneBuilder builder(nullptr, {}, {}, phase);
        const auto admitted = builder.build(gradients.plan, gradients.request);
        expectations.expect(admitted.hasValue() && admitted.phaseLease != nullptr,
                            "admission: the gradients are admitted: " +
                                admitted.diagnostic.message);
        if (admitted.phaseLease) {
            expectations.expect(admitted.phaseLease->deviceNeedBytes() >=
                                        phase.envelope.deviceWorkingBytes + realPeak &&
                                    admitted.phaseLease->executorByteLimit() >= realPeak,
                                "admission: the phase claims its images at their real sizes");
        }
    }
    expectations.expect(policy->snapshot().reservations.admittedPhases == 0 &&
                            policy->snapshot().reservations.totalHostClaimBytes == 0,
                        "retirement: the raised claim returns with the phase");
    // The allocator takes more than width * height * 16 for an image: rows and tiles are padded
    // (on one GPU a 1920-wide float image takes the room of a 2048-wide one). The device reports
    // that to the root with its budget and the claim counts it. Here a stand-in device pads every
    // image's width to a multiple of 64 pixels and its height to a multiple of 8.
    {
        auto paddedSizes = std::make_shared<render::GpuAllocationSizes>();
        paddedSizes->image = [](const std::uint32_t width, const std::uint32_t height) -> std::uint64_t {
            return std::uint64_t{(width + 63) / 64 * 64} * ((height + 7) / 8 * 8) *
                   sizeof(render::Rgba32f);
        };
        paddedSizes->buffer = [](const std::uint64_t bytes) -> std::uint64_t {
            return (bytes + 4095) / 4096 * 4096;
        };
        const std::shared_ptr<const render::GpuAllocationSizes> padded = paddedSizes;
        const auto roundedFrame = padded->image(kGradientWidth, kGradientHeight);
        const auto roundedPeak = runtime::detail::gpuExecutorLivePeakBytes(
            *gradients.plan, output, roundedFrame, &windows, padded.get(),
            {kGradientWidth, kGradientHeight});
        expectations.expect(roundedPeak > realPeak,
                            "allocation: padded images need more than their requested extents (" +
                                std::to_string(roundedPeak) + " against " +
                                std::to_string(realPeak) + " bytes)");
        auto paddedPolicy = policyWithHost(8 * kGib, 6 * kGib, padded);
        auto paddedPhase = phaseFor(paddedPolicy, true);
        paddedPhase.envelope.minimumExecutorBytes = kGradientFrameBytes;
        {
            const runtime::CpuGpuSceneBuilder builder(nullptr, {}, {}, paddedPhase);
            const auto admitted = builder.build(gradients.plan, gradients.request);
            expectations.expect(admitted.hasValue() && admitted.phaseLease != nullptr,
                                "allocation: the padded scene is admitted: " +
                                    admitted.diagnostic.message);
            if (admitted.phaseLease) {
                expectations.expect(admitted.phaseLease->imageAllocationBytes(
                                        kGradientWidth, kGradientHeight) == roundedFrame,
                                    "allocation: the lease answers with the device's sizes");
                expectations.expect(admitted.phaseLease->executorByteLimit() >= roundedPeak,
                                    "allocation: the claim counts what the allocator takes (" +
                                        std::to_string(admitted.phaseLease->executorByteLimit()) +
                                        " against " + std::to_string(roundedPeak) + " bytes)");
            }
        }
        // A ceiling that held the requested extents but not the allocations is refused now, where
        // the executor would have been charged past it; a ceiling at the rounded need renders.
        auto tight = paddedPhase;
        tight.envelope.requestedExecutorBytes = realPeak;
        {
            const runtime::CpuGpuSceneBuilder builder(nullptr, {}, {}, tight);
            const auto refused = builder.build(gradients.plan, gradients.request);
            expectations.expect(!refused.hasValue() &&
                                    refused.diagnostic.code ==
                                        runtime::PreparedGpuSceneDiagnosticCode::BudgetRefused,
                                "allocation: a ceiling at the requested extents is refused: " +
                                    refused.diagnostic.message);
        }
        auto exact = paddedPhase;
        exact.envelope.requestedExecutorBytes = roundedPeak;
        {
            const runtime::CpuGpuSceneBuilder builder(nullptr, {}, {}, exact);
            const auto admitted = builder.build(gradients.plan, gradients.request);
            expectations.expect(admitted.hasValue() && admitted.phaseLease != nullptr &&
                                    admitted.phaseLease->executorByteLimit() == roundedPeak,
                                "allocation: a ceiling at the rounded need admits the scene: " +
                                    admitted.diagnostic.message);
        }
        expectations.expect(paddedPolicy->snapshot().reservations.admittedPhases == 0,
                            "retirement: the padded claims return with their phases");
    }
    // An executor ceiling between the floor and the real need refuses the scene rather than
    // drawing past it.
    auto ceiling = phase;
    ceiling.envelope.requestedExecutorBytes = floor + kGradientFrameBytes;
    if (floor + kGradientFrameBytes < realPeak) {
        const runtime::CpuGpuSceneBuilder builder(nullptr, {}, {}, ceiling);
        const auto refused = builder.build(gradients.plan, gradients.request);
        expectations.expect(!refused.hasValue() &&
                                refused.diagnostic.code ==
                                    runtime::PreparedGpuSceneDiagnosticCode::BudgetRefused,
                            "admission: a ceiling below the real need is refused, not exceeded: " +
                                refused.diagnostic.message);
        expectations.expect(policy->snapshot().reservations.admittedPhases == 0,
                            "retirement: a refused scene holds nothing");
    }
}


// The claim counts what the allocator takes. The device probes the driver for each image's real
// allocation size and reports it with its budget; the executor is charged those sizes. A request
// whose byte budget is exactly the rounded claim renders the scene, with the live set inside it.
void allocationClaim(Expectations& expectations, const Device& device, const Rendered& rendered,
                     const std::shared_ptr<const runtime::PreparedGpuScene>& scene,
                     const std::vector<std::optional<render::ImageWindow>>& windows) {
    const auto budget = device.device->availableAllocationBudget();
    expectations.expect(budget.allocation_sizes && budget.allocation_sizes->image &&
                            budget.allocation_sizes->buffer,
                        "allocation: the device reports its allocation sizes");
    if (!budget.allocation_sizes || !budget.allocation_sizes->image || !budget.allocation_sizes->buffer)
        return;
    const auto& sizes = *budget.allocation_sizes;
    const auto& sizer = sizes.image;
    const auto output = rendered.request.output.value();
    const auto& descriptor = *rendered.frame->processImage().descriptor();
    const auto extent = descriptor.dataWindow().extent();
    const auto frame = sizer(extent.width(), extent.height());
    expectations.expect(frame >= std::uint64_t{extent.width()} * extent.height() *
                                     sizeof(render::Rgba32f),
                        "allocation: an image is never counted below its extent");
    const auto requested = runtime::detail::gpuExecutorLivePeakBytes(
        *rendered.plan, output, std::uint64_t{extent.width()} * extent.height() * sizeof(render::Rgba32f), &windows);
    const auto rounded = runtime::detail::gpuExecutorLivePeakBytes(*rendered.plan, output, frame,
                                                                   &windows, &sizes, {extent.width(), extent.height()});
    expectations.expect(rounded >= requested, "allocation: rounding never lowers the claim");

    runtime::GpuSceneExecutorBudgets serialBudgets;
    serialBudgets.enableNativeBatches = false;
    auto cache = device.cache();
    auto executor = cache ? executorFor(*device.device, *cache, serialBudgets) : nullptr;
    if (!executor)
        return;
    const auto run = [&](const std::uint64_t byteBudget) {
        std::vector<render::Rgba32f> pixels;
        if (executor->begin(scene, byteBudget).code != runtime::GpuSceneExecutorDiagnosticCode::None)
            return pixels;
        auto poll = executor->poll();
        while (poll == runtime::GpuSceneExecutorPollResult::Pending)
            poll = executor->poll();
        if (poll == runtime::GpuSceneExecutorPollResult::Ready && executor->image() != nullptr) {
            // The output image's real allocation is what the probe said for its extent.
            expectations.expect(executor->image()->allocationBytes() ==
                                    sizer(extent.width(), extent.height()),
                                "allocation: the probe equals the allocator's size for the output "
                                "image (" +
                                    std::to_string(executor->image()->allocationBytes()) + " against " +
                                    std::to_string(sizer(extent.width(), extent.height())) + ")");
            if (const auto readback = render::readbackResidentImage(*executor->image(), kSceneBudget))
                pixels = readback.pixels;
        }
        static_cast<void>(executor->takeImage());
        return pixels;
    };
    const auto pixels = run(rounded);
    expectations.expect(!pixels.empty() &&
                            parity(pixels, rendered.frame->processImage().pixels()),
                        "allocation: a request whose byte budget is exactly the rounded claim "
                        "renders and matches the CPU reference: " +
                            executor->diagnostic().message);
    const auto peak = executor->counters().peakLiveImageBytes;
    std::cout << "allocation " << extent.width() << "x" << extent.height() << ": requested claim " << requested << " bytes, rounded claim "
              << rounded << " bytes, serial live peak " << peak << " bytes\n";
    expectations.expect(peak <= rounded,
                        "allocation: the serial live set is inside the rounded claim (" +
                            std::to_string(peak) + " against " + std::to_string(rounded) + ")");
}

// The same picture on the device: cold and warm parity over keyed frames (a frame-pivot group
// turned and moved partly off the frame, so the visible window passes through a rotation), at half
// resolution, and a serial live set within what the phase claims.
void gradientsOnTheDevice(Expectations& expectations, const Device& device, Fixture& fixture) {
    const auto& flame = layerNamed(fixture, "flame");
    const auto& graph = composition(fixture.document, fixture.main).graph();
    const auto* rotation =
        document::layerParameterBinding(graph, flame, document::kRotationParameterRole);
    const auto* position =
        document::layerParameterBinding(graph, flame, document::kPositionParameterRole);
    expectations.expect(flame.pivot == document::LayerPivot::Frame && rotation && position,
                        "gradients: the group is a frame-pivot entry layer");
    if (!rotation || !position)
        return;
    const auto rotationId = rotation->parameterId;
    const auto positionId = position->parameterId;
    const auto* record = composition(fixture.document, fixture.main).parameters().find(positionId);
    const auto* constant =
        record ? std::get_if<document::ConstantValueSource>(&record->source) : nullptr;
    const auto* start = constant ? std::get_if<document::Vec2d>(&constant->value) : nullptr;
    expectations.expect(start != nullptr, "gradients: the group's position is a constant");
    if (start == nullptr)
        return;
    const auto origin = *start;
    apply<commands::SetKeyframeAtTimeForParameter>("key the group's turn", fixture.document,
                                                   fixture.commands, fixture.main, rotationId,
                                                   core::RationalTime::fromInteger(2), 35.0);
    apply<commands::SetKeyframeAtTimeForParameter>("key the group's start turn", fixture.document,
                                                   fixture.commands, fixture.main, rotationId,
                                                   core::RationalTime{}, 0.0);
    apply<commands::SetKeyframeAtTimeForParameter>(
        "key the group off the frame", fixture.document, fixture.commands, fixture.main, positionId,
        core::RationalTime::fromInteger(2), document::Vec2d{origin.x + 230.0, origin.y - 40.0});
    apply<commands::SetKeyframeAtTimeForParameter>("key the group's start", fixture.document,
                                                   fixture.commands, fixture.main, positionId,
                                                   core::RationalTime{}, origin);
    for (const int second : {0, 1, 2}) {
        const auto frame =
            referenceAt(expectations, fixture, core::RationalTime::fromInteger(second),
                        "gradients at " + std::to_string(second) + " s");
        if (!frame)
            return;
        parityOf(expectations, device, *frame, "gradients at " + std::to_string(second) + " s");
        if (second == 1) {
            const auto scene = prepare(expectations, *frame, "gradients live set");
            runtime::GpuSceneExecutorBudgets serialBudgets;
            serialBudgets.enableNativeBatches = false;
            auto cache = device.cache();
            auto serial = cache ? executorFor(*device.device, *cache, serialBudgets) : nullptr;
            if (scene && serial) {
                expectations.expect(
                    parity(runScene(*serial, scene), frame->frame->processImage().pixels()),
                    "gradients: the serial run matches");
                const auto windows = preparedWindows(*scene);
                const auto claimed = runtime::detail::gpuExecutorLivePeakBytes(
                    *frame->plan, frame->request.output.value(), kGradientFrameBytes, &windows);
                const auto peak = serial->counters().peakLiveImageBytes;
                std::cout << "gradients 512x288: serial live peak "
                          << static_cast<double>(peak) / kGradientFrameBytes
                          << " frames; the phase claims "
                          << static_cast<double>(claimed) / kGradientFrameBytes << " frames\n";
                // The executor is charged each image's real allocation size, which a device
                // rounds up by its own granularity (a whole page per image on Apple silicon), so
                // the claim to hold it to is the one made with the device's sizes. A device that
                // reports none is held to the claim by extent, with one frame of room.
                const auto budget = device.device->availableAllocationBudget();
                auto limit = claimed + kGradientFrameBytes;
                if (budget.allocation_sizes && budget.allocation_sizes->image &&
                    budget.allocation_sizes->buffer) {
                    const auto extent =
                        frame->frame->processImage().descriptor()->dataWindow().extent();
                    limit = runtime::detail::gpuExecutorLivePeakBytes(
                        *frame->plan, frame->request.output.value(),
                        budget.allocation_sizes->image(extent.width(), extent.height()), &windows,
                        &*budget.allocation_sizes, {extent.width(), extent.height()});
                }
                expectations.expect(
                    peak <= limit, "memory: the serial live set is within what the phase claims (" +
                                       std::to_string(peak) + " against " + std::to_string(limit) +
                                       ")");
                allocationClaim(expectations, device, *frame, scene, windows);
            }
            const auto halfExtent =
                render::ImageExtent::create(kGradientWidth / 2, kGradientHeight / 2);
            const auto half = referenceAt(expectations, fixture, core::RationalTime::fromInteger(1),
                                          "gradients at half resolution",
                                          runtime::ProxyResolution{*halfExtent.value()});
            if (half)
                parityOf(expectations, device, *half, "gradients at half resolution");
        }
    }
}


// ---- Pivot and blur ----
//
// An imported element and group turn about the centre of their own bounds, with nothing moved on
// screen at import, and a file's Gaussian blur filter becomes the native blur.
std::string pivotSvg() {
    return R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 256 144" width="256" height="144">
<defs><filter id="soft"><feGaussianBlur stdDeviation="4"/></filter></defs>
<rect id="square" x="30" y="30" width="40" height="40" fill="#ffffff"/>
<g id="pair"><rect id="left" x="140" y="20" width="30" height="20" fill="#ff0000"/>
<rect id="right" x="200" y="20" width="30" height="20" fill="#00ff00"/></g>
<rect id="soft" x="100" y="90" width="40" height="30" fill="#ffffff" filter="url(#soft)"/>
</svg>
)svg";
}

[[nodiscard]] render::Rgba32f pixelOf(const Rendered& rendered, const std::int64_t x,
                                      const std::int64_t y) {
    const auto& image = rendered.frame->processImage();
    const auto window = image.descriptor()->dataWindow();
    if (x < window.originX() || y < window.originY() || x >= window.maxXExclusive() ||
        y >= window.maxYExclusive())
        return render::Rgba32f::transparent();
    const auto width = static_cast<std::int64_t>(window.extent().width());
    return image
        .pixels()[static_cast<std::size_t>((y - window.originY()) * width + (x - window.originX()))];
}

// The scene for a reference frame, prepared with the packaged shader tools an image effect (the
// blur) needs.
[[nodiscard]] std::shared_ptr<const runtime::PreparedGpuScene>
prepareWithTools(Expectations& expectations, const Rendered& rendered, const std::string& label) {
    const auto ocio = gpu_coverage_ocio::sharedContext();
    if (ocio == nullptr || ocio->preparer == nullptr) {
        expectations.expect(false, label + ": the packaged shader tools are unavailable: " +
                                       std::string(gpu_coverage_ocio::unavailableReason()));
        return nullptr;
    }
    runtime::CpuCompositionEvaluator evaluator;
    auto context = runtime::GpuSceneMediaContext::fromEvaluator(evaluator);
    const runtime::CpuGpuSceneBuilder builder(nullptr, context, *ocio);
    const auto built = builder.build(rendered.plan, rendered.request);
    expectations.expect(built.hasValue(), label + ": prepares: " + built.diagnostic.message);
    return built.scene;
}

// Cold and warm parity of a reference frame whose scene needs the shader tools.
void parityWithTools(Expectations& expectations, const Device& device, const Rendered& rendered,
                     const std::string& label) {
    const auto scene = prepareWithTools(expectations, rendered, label);
    auto cache = device.cache();
    auto executor = cache ? executorFor(*device.device, *cache) : nullptr;
    expectations.expect(executor != nullptr, label + ": the executor creates");
    if (!scene || !executor)
        return;
    const auto expected = rendered.frame->processImage().pixels();
    const auto cold = runScene(*executor, scene);
    expectations.expect(!cold.empty() && parity(cold, expected),
                        label + ": GPU pixels match the CPU oracle (2e-6): " +
                            executor->diagnostic().message);
    const auto before = executor->counters().dispatches;
    const auto warm = runScene(*executor, scene);
    expectations.expect(parity(warm, expected) && executor->counters().dispatches == before,
                        label + ": a warm rerun matches and dispatches nothing new");
}

// The frames the device half compares against the GPU.
using KeyedFrames = std::vector<std::pair<std::string, Rendered>>;

KeyedFrames pivotAndBlur(Expectations& expectations, Fixture& fixture) {
    KeyedFrames frames;
    const auto& graph = composition(fixture.document, fixture.main).graph();
    const auto rest = referenceAt(expectations, fixture, {}, "pivot at import");
    if (!rest)
        return frames;
    frames.emplace_back("pivot at import", *rest);
    const auto role = [&](const std::string& name, const std::string_view which) {
        const auto& layer = layerNamed(fixture, name);
        const auto* binding = document::layerParameterBinding(graph, layer, which);
        if (binding == nullptr)
            throw std::runtime_error(name + " has no " + std::string(which));
        return binding->parameterId;
    };
    const auto vec2Of = [&](const document::ParameterId id) {
        const auto* record = composition(fixture.document, fixture.main).parameters().find(id);
        const auto* constant =
            record ? std::get_if<document::ConstantValueSource>(&record->source) : nullptr;
        const auto* value = constant ? std::get_if<document::Vec2d>(&constant->value) : nullptr;
        return value ? *value : document::Vec2d{-1, -1};
    };
    const document::Vec2d frameCentre{kWidth / 2.0, kHeight / 2.0};
    // The pivots sit at the centres of the bounds: Position names where the pivot lands and Anchor
    // is its offset from the frame's centre.
    const auto squarePosition = vec2Of(role("square", document::kPositionParameterRole));
    const auto squareAnchor = vec2Of(role("square", document::kAnchorParameterRole));
    const auto pairPosition = vec2Of(role("pair", document::kPositionParameterRole));
    const auto pairAnchor = vec2Of(role("pair", document::kAnchorParameterRole));
    const auto softPosition = vec2Of(role("soft", document::kPositionParameterRole));
    const auto softAnchor = vec2Of(role("soft", document::kAnchorParameterRole));
    expectations.expect(squarePosition.x == 50.0 && squarePosition.y == 50.0 &&
                            squareAnchor.x == 50.0 - frameCentre.x &&
                            squareAnchor.y == 50.0 - frameCentre.y,
                        "pivot: the square's pivot is the centre of its bounds");
    expectations.expect(pairPosition.x == 185.0 && pairPosition.y == 30.0 &&
                            pairAnchor.x == 185.0 - frameCentre.x &&
                            pairAnchor.y == 30.0 - frameCentre.y,
                        "pivot: the group's pivot is the centre of its children's bounds");
    // Nothing moved on screen: the same frame as a frame-pivot layer at the frame's centre.
    for (const char* name : {"square", "pair", "soft"}) {
        apply<commands::SetParameterSource>(
            std::string("frame pivot ") + name, fixture.document, fixture.commands, fixture.main,
            role(name, document::kAnchorParameterRole), document::ConstantValueSource{document::Vec2d{}});
        apply<commands::SetParameterSource>(
            std::string("frame position ") + name, fixture.document, fixture.commands, fixture.main,
            role(name, document::kPositionParameterRole), document::ConstantValueSource{frameCentre});
    }
    const auto framePivot = referenceAt(expectations, fixture, {}, "pivot at the frame");
    expectations.expect(framePivot && samePixels(*framePivot, *rest),
                        "pivot: importing at the bounds' centres moves nothing on screen");
    // Back to the pivots at the bounds' centres.
    for (const auto& [name, centre, anchor] :
         {std::tuple{"square", squarePosition, squareAnchor}, std::tuple{"pair", pairPosition, pairAnchor},
          std::tuple{"soft", softPosition, softAnchor}}) {
        apply<commands::SetParameterSource>(std::string("restore anchor ") + name, fixture.document,
                                            fixture.commands, fixture.main,
                                            role(name, document::kAnchorParameterRole),
                                            document::ConstantValueSource{anchor});
        apply<commands::SetParameterSource>(std::string("restore position ") + name, fixture.document,
                                            fixture.commands, fixture.main,
                                            role(name, document::kPositionParameterRole),
                                            document::ConstantValueSource{centre});
    }
    const auto restored = referenceAt(expectations, fixture, {}, "pivot restored");
    expectations.expect(restored && samePixels(*restored, *rest), "pivot: the pivots restore");

    // The square turns a quarter about its own centre; the pair turns half a turn about theirs.
    for (const char* name : {"square", "pair"}) {
        const auto rotation = role(name, document::kRotationParameterRole);
        apply<commands::SetKeyframeAtTimeForParameter>(
            std::string("key ") + name + " turn", fixture.document, fixture.commands, fixture.main, rotation,
            core::RationalTime::fromInteger(1), std::string_view(name) == "square" ? 90.0 : 180.0);
        apply<commands::SetKeyframeAtTimeForParameter>(std::string("key ") + name + " start",
                                                       fixture.document, fixture.commands, fixture.main,
                                                       rotation, core::RationalTime{}, 0.0);
    }
    const auto turned = referenceAt(expectations, fixture, core::RationalTime::fromInteger(1), "pivot turned");
    if (turned) {
        frames.emplace_back("pivot turned", *turned);
        // A square turned a quarter about its centre covers the same pixels. Swung about the
        // frame's centre it would be elsewhere.
        bool same = true;
        for (std::int64_t y = 26; y < 75 && same; ++y)
            for (std::int64_t x = 26; x < 75; ++x)
                same = same && std::abs(pixelOf(*turned, x, y).alpha() - pixelOf(*rest, x, y).alpha()) < 1e-4F;
        expectations.expect(same, "pivot: a keyed quarter turn leaves the square where it is");
        expectations.expect(pixelOf(*turned, 50, 50).alpha() > 0.99F, "pivot: the square still covers its centre");
        // The pair swaps sides about the centre of the group.
        const auto leftNow = pixelOf(*turned, 215, 30);
        const auto rightNow = pixelOf(*turned, 155, 30);
        expectations.expect(leftNow.red() > 0.5F && leftNow.green() < 0.05F,
                            "pivot: after half a turn the red rectangle is where the green one was");
        expectations.expect(rightNow.green() > 0.5F && rightNow.red() < 0.05F,
                            "pivot: after half a turn the green rectangle is where the red one was");
        expectations.expect(pixelOf(*turned, 185, 30).alpha() < 0.001F,
                            "pivot: the gap between the pair stays empty");
    }

    // The blur filter: one Gaussian Blur node, radius three times the file's deviation.
    const auto& nodes = graph.nodes();
    const auto blur = std::ranges::find_if(nodes, [](const auto& node) {
        return node.typeId == "bloom.gaussian-blur";
    });
    expectations.expect(blur != nodes.end(), "blur: the filter became a Gaussian Blur node");
    if (blur != nodes.end()) {
        const auto* record = composition(fixture.document, fixture.main)
                                 .parameters()
                                 .find(parameter(fixture.document, fixture.main, blur->id, "radius"));
        const auto* constant =
            record ? std::get_if<document::ConstantValueSource>(&record->source) : nullptr;
        const auto* radius = constant ? std::get_if<double>(&constant->value) : nullptr;
        expectations.expect(radius != nullptr && std::abs(*radius - 12.0) < 1e-9,
                            "blur: the radius is three times the deviation of 4");
    }
    // The rectangle's edge is soft: half covered at the edge, a little beyond it, none far away.
    expectations.expect(std::abs(pixelOf(*rest, 120, 105).alpha() - 1.0F) < 0.02F,
                        "blur: the middle of the rectangle stays solid");
    expectations.expect(pixelOf(*rest, 98, 105).alpha() > 0.02F && pixelOf(*rest, 98, 105).alpha() < 0.45F,
                        "blur: the edge spreads beyond the rectangle");
    expectations.expect(pixelOf(*rest, 80, 105).alpha() < 0.001F, "blur: nothing far from the rectangle");
    return frames;
}


// ---- Path nodes over imported shapes ----
//
// Offset Path, Round Corners, Boolean Paths, Merge Paths and Path Stroke each wired to an imported
// compound shape (a ring with a counter) whose Path node is keyed: a corner and a counter point move
// between 0 and 1 s. Each is rendered at both times and read at points chosen well inside or
// outside the edges.
std::string ringSvg() {
    return R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 256 144" width="256" height="144">
<path id="ring" fill="#ffffff" fill-rule="evenodd" d="M30 20H110V100H30Z M50 40H90V80H50Z"/>
<rect id="block" x="80" y="60" width="70" height="60" fill="#ffffff"/>
</svg>
)svg";
}

[[nodiscard]] document::NodeId addNodeOf(Fixture& fixture, const std::string_view type) {
    commands::Transaction transaction("add " + std::string(type), fixture.document.snapshot().revision());
    transaction.emplace<commands::AddNode>(fixture.main, std::string(type), document::Vec2d{});
    const auto result = fixture.commands.execute(std::move(transaction));
    const auto id = result.succeeded() ? result.outputId<document::NodeId>(commands::kAddNodeOutput)
                                       : std::nullopt;
    if (!id)
        throw std::runtime_error("adding " + std::string(type) + " was refused");
    return *id;
}

template <typename Value>
void setConstant(Fixture& fixture, const document::NodeId node, const std::string_view role, Value value) {
    apply<commands::SetParameterSource>("set " + std::string(role), fixture.document, fixture.commands,
                                        fixture.main, parameter(fixture.document, fixture.main, node, role),
                                        document::ConstantValueSource{std::move(value)});
}
void setDriver(Fixture& fixture, const document::NodeId node, const std::string_view role,
               const document::NodeId source, const std::string_view port) {
    apply<commands::SetParameterSource>(
        "wire " + std::string(role), fixture.document, fixture.commands, fixture.main,
        parameter(fixture.document, fixture.main, node, role),
        document::DriverBindingSource{source, std::string(port)});
}

struct PathProbe final {
    std::int64_t x, y;
    bool at0, at1; // covered at 0 s, covered at 1 s
};

KeyedFrames pathNodesOnImportedShapes(Expectations& expectations, Fixture& fixture) {
    KeyedFrames frames;
    document::NodeId ringPath, blockPath;
    for (const auto node : fixture.pathNodes())
        (fixture.pathOf(node).contours.size() == 2 ? ringPath : blockPath) = node;
    expectations.expect(ringPath.isValid() && blockPath.isValid(),
                        "path nodes: the ring is one compound Path node and the block another");
    if (!ringPath.isValid() || !blockPath.isValid())
        return frames;
    const auto ringShape = fixture.shapeDrivenBy(ringPath);

    // Key the compound shape: a corner of the outer contour and a point of the counter move.
    const auto base = fixture.pathOf(ringPath);
    auto moved = base;
    moved.contours[0].anchors[1].point = {130.0, 10.0};
    moved.contours[1].anchors[1].point = {80.0, 50.0};
    const auto valueParameter = parameter(fixture.document, fixture.main, ringPath, document::kValueParameterRole);
    apply<commands::SetKeyframeAtTimeForParameter>("key the moved ring", fixture.document, fixture.commands,
                                                   fixture.main, valueParameter,
                                                   core::RationalTime::fromInteger(1), moved);
    apply<commands::SetKeyframeAtTimeForParameter>("key the ring at rest", fixture.document, fixture.commands,
                                                   fixture.main, valueParameter, core::RationalTime{}, base);

    const auto render = [&](const core::RationalTime time, const std::string& label) {
        const auto frame = referenceAt(expectations, fixture, time, label);
        if (frame)
            frames.emplace_back(label, *frame);
        return frame;
    };
    const auto check = [&](const std::string& name, const std::vector<PathProbe>& probes) {
        for (const auto time : {0, 1}) {
            const auto frame = render(core::RationalTime::fromInteger(time), name + " at " + std::to_string(time) + " s");
            if (!frame)
                continue;
            for (const auto& probe : probes) {
                const auto alpha = alphaOf(*frame, probe.x, probe.y);
                const bool wanted = time == 0 ? probe.at0 : probe.at1;
                expectations.expect(wanted ? alpha > 0.9F : alpha < 0.05F,
                                    name + " at " + std::to_string(time) + " s: (" + std::to_string(probe.x) +
                                        ", " + std::to_string(probe.y) + ") is " + (wanted ? "covered" : "empty") +
                                        " (alpha " + std::to_string(alpha) + ")");
            }
        }
    };
    // The ring itself, before any node: the probes the cases are read against.
    check("ring", {{40, 60, true, true}, {70, 60, false, false}, {118, 30, false, true}, {86, 45, false, true},
                   {27, 60, false, false}});

    const auto wireRing = [&](const document::NodeId processed, const std::string_view port) {
        setDriver(fixture, ringShape, "path", processed, port);
    };
    // Offset Path: the outer edge grows and the counter shrinks.
    {
        const auto node = addNodeOf(fixture, document::kOffsetPathNodeType);
        setDriver(fixture, node, "path", ringPath, document::kValuePortName);
        setConstant(fixture, node, "amount", 6.0);
        // Miter is the node's own join.
        wireRing(node, "path");
        check("offset path", {{27, 60, true, true}, {53, 60, true, true}, {70, 60, false, false},
                              {118, 30, false, true}, {20, 60, false, false}});
    }
    // Round Corners: the outer corner is cut away by its arc.
    {
        const auto node = addNodeOf(fixture, document::kRoundCornersNodeType);
        setDriver(fixture, node, "path", ringPath, document::kValuePortName);
        setConstant(fixture, node, "radius", 8.0);
        wireRing(node, "path");
        check("round corners", {{31, 21, false, false}, {40, 60, true, true}, {118, 30, false, true}});
    }
    // Boolean Paths: the ring less the block.
    {
        const auto node = addNodeOf(fixture, document::kBooleanPathsNodeType);
        setDriver(fixture, node, "first", ringPath, document::kValuePortName);
        setDriver(fixture, node, "second", blockPath, document::kValuePortName);
        setConstant(fixture, node, "operation", document::selectorStoredValue(document::PathCombine::Subtract));
        wireRing(node, "path");
        // The block's own shape still draws it; the cutter is not meant to show.
        setConstant(fixture, fixture.shapeDrivenBy(blockPath), "fillEnabled", false);
        check("boolean paths", {{100, 90, false, false}, {40, 60, true, true}, {118, 30, false, true},
                                {86, 45, false, true}});
    }
    // Merge Paths: the union of the ring's two contours fills the counter.
    {
        const auto node = addNodeOf(fixture, document::kMergePathsNodeType);
        setDriver(fixture, node, "path", ringPath, document::kValuePortName);
        // Union is the node's own operation.
        wireRing(node, "path");
        check("merge paths", {{70, 60, true, true}, {40, 60, true, true}, {118, 30, false, true}});
    }
    // Path Stroke: only the outline of each edge remains.
    {
        const auto node = addNodeOf(fixture, document::kPathStrokeNodeType);
        setDriver(fixture, node, "path", ringPath, document::kValuePortName);
        setConstant(fixture, node, "width", 6.0);
        wireRing(node, "path");
        check("path stroke", {{40, 60, false, false}, {31, 60, true, true}, {51, 60, true, true}, {70, 41, true, false},
                              {125, 30, false, true}});
    }
    return frames;
}


// Letters, drawn in the file's own units of the 256x144 frame: a B with two counters and an O
// with one, each a single compound path.
std::string lettersSvg() {
    return R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 256 144" width="256" height="144">
<path id="letter-B" d="M20 20 H70 C100 20 100 65 70 65 C105 65 105 120 70 120 H20 Z M40 38 V52 H66 C78 52 78 38 66 38 Z M40 80 V104 H68 C84 104 84 80 68 80 Z" fill="#22aa66" fill-rule="evenodd"/>
<path id="letter-O" d="M170 20 A45 50 0 1 0 170.01 20 Z M170 40 A25 30 0 1 1 169.99 40 Z" fill="#dd5522" fill-rule="evenodd"/>
</svg>
)svg";
}

// The anchor of a contour nearest to a point: an artist grabs a corner by where it is.
[[nodiscard]] std::size_t anchorNear(const document::PathContour& contour, const double x, const double y) {
    std::size_t best = 0;
    double bestDistance = std::numeric_limits<double>::max();
    for (std::size_t index = 0; index < contour.anchors.size(); ++index) {
        const auto& point = contour.anchors[index].point;
        const double distance = (point.x - x) * (point.x - x) + (point.y - y) * (point.y - y);
        if (distance < bestDistance) {
            bestDistance = distance;
            best = index;
        }
    }
    return best;
}

// Drags a corner of the B's outline and a corner of its upper counter, as an artist would (the
// other points stay), keyed from frame 0. Frame 0 is the file as imported; one second later the
// corner and the counter have moved and every other contour, the lower counter and the whole O are
// unchanged. Both counters survive the drag.
KeyedFrames lettersCornerAndCounter(Expectations& expectations, Fixture& fixture) {
    KeyedFrames frames;
    document::NodeId bPath;
    for (const auto node : fixture.pathNodes())
        if (fixture.pathOf(node).contours.size() == 3)
            bPath = node;
    expectations.expect(bPath.isValid(), "letters: the B imports as one compound Path node of three contours");
    if (!bPath.isValid())
        return frames;
    const auto atRest = referenceAt(expectations, fixture, core::RationalTime{}, "letters before the edit");
    const auto base = fixture.pathOf(bPath);
    auto moved = base;
    const auto corner = anchorNear(moved.contours[0], 20.0, 20.0);
    const auto counter = anchorNear(moved.contours[1], 40.0, 38.0);
    // A corner has no handles of its own here, so the point moves with them.
    const auto drag = [](document::PathAnchor& anchor, const double dx, const double dy) {
        anchor.point.x += dx;
        anchor.point.y += dy;
    };
    drag(moved.contours[0].anchors[corner], 10.0, -12.0);
    drag(moved.contours[1].anchors[counter], 6.0, 4.0);
    expectations.expect(moved.contours.size() == 3 &&
                            moved.contours[2].anchors.size() == base.contours[2].anchors.size(),
                        "letters: the drag keeps every contour");
    const auto value = parameter(fixture.document, fixture.main, bPath, document::kValueParameterRole);
    apply<commands::SetKeyframeAtTimeForParameter>("key the dragged B", fixture.document, fixture.commands,
                                                   fixture.main, value, core::RationalTime::fromInteger(1), moved);
    apply<commands::SetKeyframeAtTimeForParameter>("key the B at rest", fixture.document, fixture.commands,
                                                   fixture.main, value, core::RationalTime{}, base);
    const auto first = referenceAt(expectations, fixture, core::RationalTime{}, "letters at 0 s");
    const auto second = referenceAt(expectations, fixture, core::RationalTime::fromInteger(1), "letters at 1 s");
    if (!first || !second || !atRest)
        return frames;
    expectations.expect(samePixels(*atRest, *first), "letters: frame 0 is unchanged by keying the drag");
    frames.emplace_back("letters at 0 s", *first);
    frames.emplace_back("letters at 1 s", *second);
    struct Probe {
        std::int64_t x, y;
        bool at0, at1;
        const char* what;
    };
    const Probe probes[] = {
        {22, 22, true, false, "the old top-left corner is vacated"},
        {40, 12, false, true, "the dragged corner covers new ground"},
        {42, 40, false, true, "the dragged counter corner uncovers the old counter's corner"},
        {55, 45, false, false, "the upper counter is still open"},
        {50, 92, false, false, "the lower counter is untouched and open"},
        {28, 60, true, true, "the stem is untouched"},
        {135, 70, true, true, "the O's ring is untouched"},
        {170, 70, false, false, "the O's counter is untouched and open"},
    };
    for (const auto& probe : probes)
        for (const auto time : {0, 1}) {
            const auto& frame = time == 0 ? *first : *second;
            const auto alpha = alphaOf(frame, probe.x, probe.y);
            const bool wanted = time == 0 ? probe.at0 : probe.at1;
            expectations.expect(wanted ? alpha > 0.9F : alpha < 0.05F,
                                std::string("letters at ") + std::to_string(time) + " s: " + probe.what +
                                    " (alpha " + std::to_string(alpha) + ")");
        }
    return frames;
}

// ---- Corpus mode: a real file at a real size ----
//
// `--svg <file> --size <W>x<H>` imports the file, prepares and runs the whole frame through the
// production CpuGpuSceneBuilder and GpuSceneExecutor, and compares every pixel with the CPU
// reference (2e-6). The CPU reference is evaluated in ROI tiles so the comparison runs on several
// threads; it folds each Merge entry by entry, and prints what the whole frame holds. Not part of
// the ctest run (it needs the corpus files); docs/architecture/vector-artwork.md records the
// results.
// `--pass device --frame <file>` runs only the device half and writes the GPU frame there;
// `--pass compare --frame <file>` reads it and runs only the CPU tiles, so the device lock is not
// held while the CPU works. Without `--pass` both halves run in one process.
struct CorpusRun final {
    std::filesystem::path file;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::string pass;
    std::filesystem::path frame;
};

int corpusParity(const CorpusRun& run, const Options& options) {
    const auto& file = run.file;
    const auto width = run.width;
    const auto height = run.height;
    const Fixture fixture(file, width, height);
    const runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
    const auto compiled = compiler.compile({fixture.document.snapshot(), fixture.main}, {});
    if (!compiled.plan) {
        std::cerr << "FAIL: " << file.filename() << " does not compile\n";
        return 1;
    }
    auto request = requestFor(*compiled.plan);
    request.pixelStorageByteLimit = std::size_t{64} * kGib;

    std::optional<render::ImageWindow> gpuWindow;
    std::vector<render::Rgba32f> gpuPixels;
    if (run.pass == "compare") {
        std::ifstream in(run.frame, std::ios::binary);
        std::array<std::int64_t, 4> window{};
        in.read(reinterpret_cast<char*>(window.data()), sizeof(window));
        const auto created =
            render::ImageWindow::create(window[0], window[1], static_cast<std::uint64_t>(window[2]),
                                        static_cast<std::uint64_t>(window[3]));
        if (!in || !created) {
            std::cerr << "FAIL: the GPU frame file could not be read\n";
            return 1;
        }
        gpuWindow = *created.value();
        gpuPixels.assign(static_cast<std::size_t>(window[2] * window[3]),
                         render::Rgba32f::transparent());
        in.read(reinterpret_cast<char*>(gpuPixels.data()),
                static_cast<std::streamsize>(gpuPixels.size() * sizeof(render::Rgba32f)));
        if (!in) {
            std::cerr << "FAIL: the GPU frame file is short\n";
            return 1;
        }
    } else {
        render::GpuDeviceCreationOptions createOptions;
        createOptions.loader_path = options.loader_path;
        auto created = render::GpuDevice::create(createOptions);
        if (!created) {
            std::cerr << (options.require_device ? "FAIL" : "SKIP")
                      << ": no compatible Vulkan device: " << created.diagnostic.message << '\n';
            return options.require_device ? 1 : 77;
        }
        const Device device{std::move(created.device)};
        const auto built = runtime::CpuGpuSceneBuilder{}.build(compiled.plan, request);
        if (!built.hasValue()) {
            std::cerr << "FAIL: preparation: " << built.diagnostic.message << '\n';
            return 1;
        }
        // The final render's executor shape: one image may be as large as the request budget
        // (gpu_process_frame.cpp), here 8 GiB.
        runtime::GpuSceneExecutorBudgets finalBudgets;
        finalBudgets.maxImageBytes = 8 * kGib;
        auto cache = device.cache();
        auto executor = cache ? executorFor(*device.device, *cache, finalBudgets) : nullptr;
        if (!executor || executor->begin(built.scene, 8 * kGib).code !=
                             runtime::GpuSceneExecutorDiagnosticCode::None) {
            std::cerr << "FAIL: the executor did not start: "
                      << (executor ? executor->diagnostic().message : std::string("no executor"))
                      << '\n';
            return 1;
        }
        auto poll = executor->poll();
        while (poll == runtime::GpuSceneExecutorPollResult::Pending)
            poll = executor->poll();
        if (poll != runtime::GpuSceneExecutorPollResult::Ready || executor->image() == nullptr) {
            std::cerr << "FAIL: the native run did not complete: " << executor->diagnostic().message
                      << '\n';
            return 1;
        }
        gpuWindow = executor->image()->dataWindow();
        auto readback = render::readbackResidentImage(*executor->image(), kGib);
        static_cast<void>(executor->takeImage());
        gpuPixels = std::move(readback.pixels);
        if (!gpuWindow || gpuPixels.empty()) {
            std::cerr << "FAIL: the GPU frame could not be read back\n";
            return 1;
        }
        const std::uint64_t frameBytes = std::uint64_t{width} * height * sizeof(render::Rgba32f);
        const auto frames = [frameBytes](const std::uint64_t bytes) {
            return static_cast<double>(bytes) / static_cast<double>(frameBytes);
        };
        const auto peak = executor->counters().peakLiveImageBytes;
        const auto cachedImages = cache->entryCount();
        // The live set without batching, against what the phase claims for it.
        auto serialBudgets = finalBudgets;
        serialBudgets.enableNativeBatches = false;
        auto serialCache = device.cache();
        auto serial =
            serialCache ? executorFor(*device.device, *serialCache, serialBudgets) : nullptr;
        std::uint64_t serialPeak = 0;
        if (serial && serial->begin(built.scene, 8 * kGib).code ==
                          runtime::GpuSceneExecutorDiagnosticCode::None) {
            auto serialPoll = serial->poll();
            while (serialPoll == runtime::GpuSceneExecutorPollResult::Pending)
                serialPoll = serial->poll();
            static_cast<void>(serial->takeImage());
            serialPeak = serial->counters().peakLiveImageBytes;
        }
        // The device claim exactly as the builder makes it (cpu_gpu_scene_preparation.cpp).
        auto validation = request;
        validation.pixelStorageByteLimit = std::numeric_limits<std::size_t>::max();
        const auto checked =
            runtime::detail::preflight(compiled.plan, validation, {}, {}, nullptr, nullptr);
        if (!checked.resolved) {
            std::cerr << "FAIL: the preflight did not resolve\n";
            return 1;
        }
        const auto windows = preparedWindows(*built.scene);
        const auto realNeed =
            std::max(frameBytes, runtime::detail::gpuExecutorLivePeakBytes(
                                     *compiled.plan, request.output.value(), frameBytes, &windows));
        const auto claimed = std::max(
            frameBytes,
            std::min<std::uint64_t>(checked.resolved->peakPixelStorageBytes,
                                    runtime::detail::gpuExecutorLivePeakBytes(
                                        *compiled.plan, request.output.value(), frameBytes)));
        std::cout << file.filename().string() << ' ' << width << 'x' << height << ": "
                  << built.scene->commands().size() << " commands; live image peak "
                  << frames(serialPeak) << " frames serial, " << frames(peak)
                  << " frames batched; the phase's admission floor " << frames(claimed)
                  << " frames, its claim at real image sizes " << frames(realNeed) << " frames; "
                  << cachedImages << " cached images; largest image "
                  << frames(executor->counters().peakStepImageBytes) << " frames\n";
        // The floor takes images as frames; the phase raises its claim to the prepared images'
        // real sizes (the builder does exactly this), and the serial live set stays within it.
        if (serialPeak == 0) {
            std::cerr << "FAIL: the serial run did not complete\n";
            return 1;
        }
        if (serialPeak > std::max(claimed, realNeed) + frameBytes) {
            std::cerr << "FAIL: the serial live set is more than the phase claims\n";
            return 1;
        }
        if (run.pass == "device") {
            std::ofstream out(run.frame, std::ios::binary);
            const std::array<std::int64_t, 4> window{
                gpuWindow->originX(), gpuWindow->originY(),
                static_cast<std::int64_t>(gpuWindow->extent().width()),
                static_cast<std::int64_t>(gpuWindow->extent().height())};
            out.write(reinterpret_cast<const char*>(window.data()), sizeof(window));
            out.write(reinterpret_cast<const char*>(gpuPixels.data()),
                      static_cast<std::streamsize>(gpuPixels.size() * sizeof(render::Rgba32f)));
            if (!out) {
                std::cerr << "FAIL: the GPU frame file could not be written\n";
                return 1;
            }
            return 0;
        }
    }

    // The CPU reference, tile by tile, on a few threads (BLOOM_SVG_CORPUS_THREADS, default 4). A
    // tile may hold a whole Merge at the size of its unclipped support, hence the generous limit.
    constexpr std::uint32_t kTileWidth = 240;
    constexpr std::uint32_t kTileHeight = 135;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> origins;
    for (std::uint32_t top = 0; top < height; top += kTileHeight)
        for (std::uint32_t left = 0; left < width; left += kTileWidth)
            origins.emplace_back(left, top);
    // BLOOM_SVG_CORPUS_TILE_STRIDE=n compares every n-th tile (a stride coprime with the tile
    // columns walks a diagonal spread over every row and column) when the whole frame would hold
    // the CPU for too long.
    const std::size_t allTiles = origins.size();
    if (const char* stride = std::getenv("BLOOM_SVG_CORPUS_TILE_STRIDE")) {
        const auto step = std::max<std::size_t>(1, std::strtoul(stride, nullptr, 10));
        std::vector<std::pair<std::uint32_t, std::uint32_t>> sampled;
        for (std::size_t index = 0; index < origins.size(); index += step)
            sampled.push_back(origins[index]);
        origins = std::move(sampled);
    }
    const char* threadSetting = std::getenv("BLOOM_SVG_CORPUS_THREADS");
    const std::size_t threadCount = std::clamp<std::size_t>(
        threadSetting ? std::strtoul(threadSetting, nullptr, 10) : 4, 1, 16);
    std::atomic<std::size_t> next{0};
    std::mutex merge;
    std::uint64_t mismatched = 0;
    float largest = 0.0F;
    std::size_t tiles = 0;
    std::string failure;
    const auto gpuWidth = static_cast<std::int64_t>(gpuWindow->extent().width());
    const auto compareTiles = [&] {
        for (auto index = next++; index < origins.size(); index = next++) {
            const auto [left, top] = origins[index];
            auto tile = request;
            tile.pixelStorageByteLimit = std::size_t{8} * kGib;
            tile.bypassOperationCache = true;
            const auto roi = render::ImageWindow::create(
                left, top, std::min(kTileWidth, width - left), std::min(kTileHeight, height - top));
            if (!roi)
                return;
            tile.roi = *roi.value();
            const auto result =
                runtime::CpuCompositionEvaluator{}.evaluate(compiled.plan, tile, {});
            if (result.frame() == nullptr) {
                const std::lock_guard lock(merge);
                failure =
                    "the CPU reference tile at " + std::to_string(left) + ',' +
                    std::to_string(top) + " did not evaluate: " +
                    (result.diagnostics().empty() ? std::string("no diagnostic")
                                                  : result.diagnostics().front().summary + " " +
                                                        result.diagnostics().front().detail);
                next = origins.size();
                return;
            }
            const auto& image = result.frame()->processImage();
            const auto window = image.descriptor()->dataWindow();
            const auto pixels = image.pixels();
            std::uint64_t tileMismatched = 0;
            float tileLargest = 0.0F;
            for (std::int64_t y = window.originY(); y < window.maxYExclusive(); ++y)
                for (std::int64_t x = window.originX(); x < window.maxXExclusive(); ++x) {
                    const auto& expected = pixels[static_cast<std::size_t>(
                        (y - window.originY()) *
                            static_cast<std::int64_t>(window.extent().width()) +
                        (x - window.originX()))];
                    const bool inside = x >= gpuWindow->originX() && y >= gpuWindow->originY() &&
                                        x < gpuWindow->maxXExclusive() &&
                                        y < gpuWindow->maxYExclusive();
                    const auto actual = inside ? gpuPixels[static_cast<std::size_t>(
                                                     (y - gpuWindow->originY()) * gpuWidth +
                                                     (x - gpuWindow->originX()))]
                                               : render::Rgba32f::transparent();
                    bool same = true;
                    for (std::size_t c = 0; c < 4; ++c) {
                        const float a = actual.components()[c];
                        const float e = expected.components()[c];
                        const float difference = std::abs(a - e);
                        tileLargest = std::max(tileLargest, difference);
                        if (!std::isfinite(a) || !std::isfinite(e) ||
                            difference > 2e-6F * std::max(1.0F, std::max(std::abs(a), std::abs(e))))
                            same = false;
                    }
                    if (!same)
                        ++tileMismatched;
                }
            const std::lock_guard lock(merge);
            ++tiles;
            mismatched += tileMismatched;
            largest = std::max(largest, tileLargest);
        }
    };
    std::vector<std::thread> workers;
    for (std::size_t worker = 0; worker < threadCount; ++worker)
        workers.emplace_back(compareTiles);
    for (auto& worker : workers)
        worker.join();
    if (!failure.empty() || tiles != origins.size()) {
        std::cerr << "FAIL: "
                  << (failure.empty() ? std::string("a tile was not compared") : failure) << '\n';
        return 1;
    }
    const auto wholeFrame =
        runtime::CpuCompositionEvaluator{}.planPreflightPixelStorage(compiled.plan, request);
    const auto frameBytes = static_cast<double>(width) * height * sizeof(render::Rgba32f);
    std::cout << file.filename().string() << ' ' << width << 'x' << height
              << ": the CPU reference holds "
              << static_cast<double>(wholeFrame.requiredPreflightBytes.value_or(0)) / frameBytes
              << " frames for the whole frame\n";
    std::cout << file.filename().string() << ' ' << width << 'x' << height << ": " << tiles
              << " of " << allTiles << " CPU reference tiles, " << mismatched
              << " pixels outside 2e-6, largest difference " << largest << '\n';
    if (mismatched != 0) {
        std::cerr << "FAIL: the GPU frame differs from the CPU reference\n";
        return 1;
    }
    std::cout << "PASS: " << file.filename().string() << " matches the CPU reference on the GPU at "
              << width << 'x' << height << '\n';
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    try {
        std::vector<char*> rest{argv[0]};
        CorpusRun corpus;
        for (int index = 1; index < argc; ++index) {
            const std::string_view argument = argv[index];
            if (argument == "--svg" && index + 1 < argc) {
                corpus.file = argv[++index];
            } else if (argument == "--pass" && index + 1 < argc) {
                corpus.pass = argv[++index];
            } else if (argument == "--frame" && index + 1 < argc) {
                corpus.frame = argv[++index];
            } else if (argument == "--size" && index + 1 < argc) {
                const std::string size = argv[++index];
                const auto x = size.find('x');
                if (x == std::string::npos)
                    return 2;
                corpus.width = static_cast<std::uint32_t>(std::stoul(size.substr(0, x)));
                corpus.height = static_cast<std::uint32_t>(std::stoul(size.substr(x + 1)));
            } else {
                rest.push_back(argv[index]);
            }
        }
        const Options options = parseOptions(static_cast<int>(rest.size()), rest.data());
        if (options.valid && !corpus.file.empty() && corpus.width > 0 && corpus.height > 0) {
            if ((corpus.pass == "device" || corpus.pass == "compare") == corpus.frame.empty())
                return 2;
            return corpusParity(corpus, options);
        }
        if (!options.valid) {
            std::cerr << "invalid arguments\n";
            return 2;
        }
        Expectations expectations;
        Fixture dense("dense", denseSvg());
        Fixture edges("edges", edgeSvg());
        const auto gradientFile = writeSvg("gradients", gradientSvg());
        Fixture gradients(gradientFile, kGradientWidth, kGradientHeight);
        std::filesystem::remove(gradientFile);
        const auto gradientFrame = referenceAt(expectations, gradients, {}, "gradients");
        const auto denseFrame = referenceAt(expectations, dense, {}, "dense");
        const auto edgeFrame = referenceAt(expectations, edges, {}, "edges");
        expectations.expect(dense.pathNodes().size() == 1600,
                            "dense: every element imports as a Path node");
        if (edgeFrame) {
            // The huge ellipse is larger than the frame and covers every corner.
            expectations.expect(alphaOf(*edgeFrame, 1, 1) > 0.0F &&
                                    alphaOf(*edgeFrame, 254, 142) > 0.0F,
                                "edges: the frame-sized ellipse covers the corners");
        }
        if (!denseFrame || !edgeFrame || !gradientFrame || expectations.failures() != 0) {
            std::cerr << expectations.failures() << " SVG artwork expectation(s) failed\n";
            return 1;
        }
        admissionAndRetirement(expectations, *denseFrame);
        Fixture pivots("pivots", pivotSvg());
        const auto pivotFrames = pivotAndBlur(expectations, pivots);
        Fixture rings("rings", ringSvg());
        const auto ringFrames = pathNodesOnImportedShapes(expectations, rings);
        Fixture letters("letters", lettersSvg());
        const auto letterFrames = lettersCornerAndCounter(expectations, letters);
        if (gradientFrame)
            gradientWindowsAndAdmission(expectations, *gradientFrame);
        if (expectations.failures() != 0) {
            std::cerr << expectations.failures() << " SVG artwork expectation(s) failed\n";
            return 1;
        }

        render::GpuDeviceCreationOptions createOptions;
        createOptions.loader_path = options.loader_path;
        auto created = render::GpuDevice::create(createOptions);
        if (!created) {
            if (options.require_device) {
                std::cerr << "FAIL: required device unavailable: " << created.diagnostic.message
                          << '\n';
                return 1;
            }
            std::cout << "SKIP: CPU half passed; no compatible Vulkan device available: "
                      << created.diagnostic.message << '\n';
            return 77;
        }
        const Device device{std::move(created.device)};
        parityOf(expectations, device, *edgeFrame, "edges");
        cancellation(expectations, device, *denseFrame);
        denseOnTheDevice(expectations, device, dense, *denseFrame);
        editsOnTheDevice(expectations, device, edges);
        gradientsOnTheDevice(expectations, device, gradients);
        for (const auto& [label, frame] : ringFrames)
            parityOf(expectations, device, frame, label);
        for (const auto& [label, frame] : letterFrames)
            parityOf(expectations, device, frame, label);
        for (const auto& [label, frame] : pivotFrames)
            parityWithTools(expectations, device, frame, label);
        for (const auto& [label, frame] : pivotFrames)
            if (const auto scene = prepareWithTools(expectations, frame, label))
                allocationClaim(expectations, device, frame, scene, preparedWindows(*scene));
        if (const auto scene = prepare(expectations, *edgeFrame, "edges"))
            allocationClaim(expectations, device, *edgeFrame, scene, preparedWindows(*scene));

        if (expectations.failures() != 0) {
            std::cerr << expectations.failures() << " SVG artwork expectation(s) failed\n";
            return 1;
        }
        std::cout << "PASS: imported SVG artwork renders on the GPU in a few frames of memory, "
                     "matching the CPU reference cold and warm, edited, keyed, trimmed, at half "
                     "resolution, cancelled, budgeted and released\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected test exception: " << exception.what() << '\n';
        return 1;
    }
}
