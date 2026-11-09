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
#include "gpu_scene_preparation_builders.hpp"

#include <bloom/commands/animation_operations.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/commands/svg_import_operations.hpp>
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
#include <mutex>
#include <sstream>
#include <thread>

namespace {

using namespace bloom;
using namespace bloom::test::document_render;
using render::composite_proof::Options;
using render::composite_proof::parseOptions;

constexpr std::uint32_t kWidth = 256;
constexpr std::uint32_t kHeight = 144;
constexpr std::uint64_t kFrameBytes = std::uint64_t{kWidth} * kHeight * sizeof(render::Rgba32f);
// The CPU reference of the dense file holds a whole run of 256 layers at once.
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
                                       name, true);
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
                                       main, nullptr, file, file.stem().string(), true);
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
    expectations.expect(result.frame() != nullptr, label + ": the CPU reference evaluates");
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
policyWithHost(const std::uint64_t limitBytes, const std::uint64_t currentBytes) {
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
                                   .available_bytes = 8 * kGib}));
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
    expectations.expect(cpuPlan.requiredPreflightBytes &&
                            *cpuPlan.requiredPreflightBytes >= 256 * kFrameBytes,
                        "memory: the CPU reference holds a whole run of 256 layers at once");
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

// ---- Corpus mode: a real file at a real size ----
//
// `--svg <file> --size <W>x<H>` imports the file, prepares and runs the whole frame through the
// production CpuGpuSceneBuilder and GpuSceneExecutor, and compares every pixel with the CPU
// reference (2e-6). The CPU reference is evaluated in ROI tiles: it holds a whole Merge of layers
// at once, which at 3840x2160 is more than this machine gives it for the large files. Not part of
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
        const auto claimed = std::max(
            frameBytes,
            std::min<std::uint64_t>(checked.resolved->peakPixelStorageBytes,
                                    runtime::detail::gpuExecutorLivePeakBytes(
                                        *compiled.plan, request.output.value(), frameBytes)));
        std::cout << file.filename().string() << ' ' << width << 'x' << height << ": "
                  << built.scene->commands().size() << " commands; live image peak "
                  << frames(serialPeak) << " frames serial, " << frames(peak)
                  << " frames batched; the phase's admission floor " << frames(claimed)
                  << " frames; " << cachedImages << " cached images; largest image "
                  << frames(executor->counters().peakStepImageBytes) << " frames\n";
        // Images are taken as frames by the floor; one larger than the frame (a layer's unclipped
        // support) is drawn against live device capacity when allocated, so a peak above the floor
        // is reported rather than failed here. The generated dense file asserts the bound itself.
        if (serialPeak == 0) {
            std::cerr << "FAIL: the serial run did not complete\n";
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
        if (!denseFrame || !edgeFrame || expectations.failures() != 0) {
            std::cerr << expectations.failures() << " SVG artwork expectation(s) failed\n";
            return 1;
        }
        admissionAndRetirement(expectations, *denseFrame);
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
