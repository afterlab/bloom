// #531: Image Switch and Channel Set Switch through real documents
// (docs/architecture/image-switch.md).
//
// A 512x288 composition gets a Text layer through AddTextLayer; an Image Switch is added with
// AddNode and wired between the Text Source and the layer, with a blurred Checker and a Solid on its
// other inputs, and the production SnapshotCompiler compiles it. The CPU half always runs:
//
// - the selection rule (whole part from 1, clamped, or wrapped) against a table;
// - a constant index compiles to the chosen input wired directly: no switch and no operation of an
//   unselected branch reaches the plan, and the pixels equal the same input wired without a switch;
// - an animated index compiles to one CompiledImageSwitch, each frame equals the input it chooses
//   wired directly, and only that input's nodes are evaluated (evaluation counters, not timing);
// - an unconnected input shows nothing; a muted switch shows its first input;
// - the cache: returning to a frame evaluates nothing, and a change to an unselected branch keeps
//   the shown frame's identity while the branch's own frame is evaluated again;
// - a Channel Set Switch: a constant index compiles away, an animated or driven one switches its
//   Channel Pass readers per input, and a driven Image Switch and Channel Set Switch follow one
//   driver together; an animated one feeding written channel layers is refused, naming the reason;
// - budget refusal and cancellation on both routes, and save and reopen.
//
// Then every state is prepared by the production CpuGpuSceneBuilder, run by the production
// GpuSceneExecutor, and must match the CPU reference (2e-6), cold and with a warm rerun that
// dispatches nothing; the prepared scene of an unselected blur holds no command of the blur, and
// returning to a frame already drawn dispatches nothing new. ROI and proxy states are included.
// Without a device the GPU half reports an explicit SKIP (exit 77) after the CPU half passed;
// --require-device makes that a failure.

#include "cpu_composition_evaluator_support.hpp"
#include "document_render_parity_support.hpp"

#include <bloom/commands/animation_operations.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/core/color.hpp>
#include <bloom/core/executable_suffix.hpp>
#include <bloom/document/channel_nodes.hpp>
#include <bloom/document/color_settings.hpp>
#include <bloom/document/image_switch_nodes.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/value_nodes.hpp>
#include <bloom/project/canonical_document.hpp>
#include <bloom/project/canonical_manifest.hpp>
#include <bloom/project/open_archive.hpp>
#include <bloom/project/save_archive.hpp>
#include <bloom/runtime/gpu_ocio_preparation.hpp>
#include <bloom/runtime/task_scheduler.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <iostream>
#include <mutex>
#include <thread>
#include <typeinfo>

namespace {

using namespace bloom;
using namespace bloom::test::document_render;
using render::composite_proof::Options;
using render::composite_proof::parseOptions;

constexpr std::int64_t kWidth = 512;
constexpr std::int64_t kHeight = 288;

document::CompositionFormat format() {
    return *document::CompositionFormat::create(kWidth, kHeight, core::PixelAspectRatio::square(),
                                                *document::FrameRate::create(24, 1));
}

[[nodiscard]] core::RationalTime frameTime(const std::int64_t frame) {
    return *core::RationalTime::create(frame, 24);
}

using References = std::vector<std::pair<std::string, Reference>>;

// One command in its own transaction; a refusal throws, naming what refused it.
template <typename Operation, typename... Arguments>
void apply(document::Document& document, commands::CommandStack& commands,
           Arguments&&... arguments) {
    commands::Transaction transaction("Fixture", document.snapshot().revision());
    transaction.emplace<Operation>(std::forward<Arguments>(arguments)...);
    const auto result = commands.execute(std::move(transaction));
    if (result.changed())
        return;
    std::string reason;
    for (const auto& failure : result.operationFailures)
        reason += " " + failure.operationType + ": " + failure.issue.message;
    for (const auto& issue : result.validation.issues())
        reason += " " + issue.path + ": " + issue.message;
    throw std::runtime_error(std::string("fixture command ") + typeid(Operation).name() +
                             " refused (status " +
                             std::to_string(static_cast<int>(result.status)) + "):" + reason);
}

// Which source a fixture's switch input (or the direct wiring) carries.
enum class Source : std::uint8_t { Text, Blur, Solid };

struct Fixture final {
    document::Document document;
    commands::CommandStack commands;
    document::CompositionId main;
    document::NodeId text;
    document::NodeId checker;
    document::NodeId blur;
    document::NodeId solid;
    document::NodeId switcher;
    document::InputPortRef fed;

    explicit Fixture(document::NewProject initial)
        : document(std::move(initial.project)), commands(document),
          main(initial.initialCompositionId) {}

    [[nodiscard]] document::ParameterId id(const document::NodeId node,
                                           const std::string_view role) const {
        return parameter(document, main, node, role);
    }
    void set(const document::NodeId node, const std::string_view role,
             document::ParameterValue value) {
        if (const auto* held = composition(document, main).parameters().find(id(node, role)))
            if (const auto* constant = std::get_if<document::ConstantValueSource>(&held->source);
                constant != nullptr && constant->value == value)
                return;
        apply<commands::SetParameterSource>(document, commands, main, id(node, role),
                                              document::ConstantValueSource{std::move(value)});
    }
    document::NodeId add(const std::string_view type) {
        apply<commands::AddNode>(document, commands, main, std::string(type),
                                   document::Vec2d{0.0, 0.0});
        return lastNode(document, main, type);
    }
    void connect(const document::NodeId from, const std::string_view port,
                 document::InputPortRef to) {
        // Wiring a node into an edge can carry the old source to its first input already.
        for (const auto& edge : composition(document, main).graph().edges())
            if (edge.source.nodeId == from && edge.source.port == port && edge.destination == to)
                return;
        apply<commands::ConnectPorts>(document, commands, main,
                                        document::OutputPortRef{from, std::string(port)},
                                        std::move(to));
    }
    [[nodiscard]] document::OutputPortRef output(const Source source) const {
        switch (source) {
        case Source::Text:
            return {text, std::string(document::kTextSourceOutputPort)};
        case Source::Blur:
            return {blur, "image"};
        case Source::Solid:
            return {solid, std::string(document::kSolidSourceOutputPort)};
        }
        return {text, std::string(document::kTextSourceOutputPort)};
    }
    // Keys the switch index: `first` at frame 0, `last` at frame 10 (linear between).
    void keyIndex(const double first, const double last) {
        set(switcher, document::kImageSwitchIndexRole, first);
        const auto parameterId = id(switcher, document::kImageSwitchIndexRole);
        apply<commands::CreateAnimationForParameter>(document, commands, main, parameterId,
                                                       frameTime(0));
        const auto* record = composition(document, main).parameters().find(parameterId);
        const auto* curve = record == nullptr
                                ? nullptr
                                : std::get_if<document::AnimationCurveSource>(&record->source);
        if (curve == nullptr)
            throw std::runtime_error("keying the index made no curve");
        apply<commands::InsertScalarKeyframe>(document, commands, main, curve->curveId,
                                                frameTime(10), last);
    }
};

// A Text layer whose content is an Image Switch over (1) its Text Source, (2) a Checker blurred by
// a Gaussian Blur and (3) a blue Solid -- or, with `direct`, that one source wired straight into the
// layer with no switch.
[[nodiscard]] std::unique_ptr<Fixture> fixture(const std::optional<Source> direct = std::nullopt) {
    auto made = std::make_unique<Fixture>(document::makeNewProject(
        "Switched", "Main", core::RationalTime::fromInteger(10), format()));
    apply<commands::AddTextLayer>(made->document, made->commands, made->main, "Text",
                                    std::string("Switch"),
                                    document::Vec2d{24.0, static_cast<double>(kHeight) / 2.0}, 1.0,
                                    48.0);
    made->text = lastNode(made->document, made->main, document::kTextSourceNodeType);
    for (const auto& edge : composition(made->document, made->main).graph().edges())
        if (edge.source.nodeId == made->text)
            made->fed = edge.destination;
    made->checker = made->add(document::kCheckerSourceNodeType);
    made->set(made->checker, "cellWidth", std::int64_t{16});
    made->set(made->checker, "cellHeight", std::int64_t{16});
    made->blur = made->add("bloom.gaussian-blur");
    made->connect(made->checker, "image", document::NodeInputRef{made->blur, "input"});
    made->set(made->blur, "radius", 6.0);
    made->solid = made->add(document::kSolidSourceNodeType);
    made->set(made->solid, "color", core::Color4d{0.1, 0.2, 0.9, 1.0});
    if (direct) {
        const auto source = made->output(*direct);
        made->connect(source.nodeId, source.port, made->fed);
        return made;
    }
    made->switcher = made->add(document::kImageSwitchNodeType);
    made->connect(made->switcher, document::kImageSwitchOutputPort, made->fed);
    const std::array sources{Source::Text, Source::Blur, Source::Solid};
    for (std::size_t position = 0; position < sources.size(); ++position) {
        const auto source = made->output(sources[position]);
        made->connect(source.nodeId, source.port,
                      document::NodeInputRef{made->switcher,
                                             std::string(document::kImageSwitchInputPorts[position])});
    }
    return made;
}

[[nodiscard]] std::shared_ptr<const runtime::CompiledCompositionPlan>
compileDocument(Expectations& expectations, const document::Document& document,
                const document::CompositionId main, const std::string& label,
                std::vector<runtime::CompileDiagnostic>* diagnostics = nullptr,
                const bool expectCompiled = true) {
    const runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
    const auto compiled = compiler.compile({document.snapshot(), main}, {});
    if (expectCompiled)
        expectations.expect(compiled.plan != nullptr,
                            label + ": compiles: " +
                                runtime::describeCompileFailure(compiled, document.snapshot().project(),
                                                                main));
    if (diagnostics != nullptr)
        *diagnostics = compiled.diagnostics;
    return compiled.plan;
}

[[nodiscard]] std::optional<Reference>
referenceOf(Expectations& expectations, const document::Document& document,
            const document::CompositionId main, const core::RationalTime time,
            const std::string& label, const std::optional<render::ImageWindow>& roi = {},
            const std::optional<runtime::EvaluationResolution>& resolution = {}) {
    const auto plan = compileDocument(expectations, document, main, label);
    if (!plan)
        return std::nullopt;
    Reference reference{plan, requestFor(*plan), nullptr};
    reference.request.time = time;
    reference.request.roi = roi;
    if (resolution)
        reference.request.resolution = *resolution;
    auto oracle = reference.request;
    oracle.bypassOperationCache = true;
    const auto result = runtime::CpuCompositionEvaluator{}.evaluate(plan, oracle, {});
    std::string reason;
    for (const auto& diagnostic : result.diagnostics())
        reason += " " + diagnostic.summary + ": " + diagnostic.detail;
    expectations.expect(result.frame() != nullptr,
                        label + ": the CPU reference evaluates:" + reason);
    if (result.frame() == nullptr)
        return std::nullopt;
    reference.frame = result.frame();
    return reference;
}

[[nodiscard]] std::optional<Reference> referenceAt(Expectations& expectations,
                                                   const Fixture& fixture,
                                                   const core::RationalTime time,
                                                   const std::string& label) {
    return referenceOf(expectations, fixture.document, fixture.main, time, label);
}

[[nodiscard]] render::ImageWindow windowOf(const std::int64_t x, const std::int64_t y,
                                           const std::uint64_t width, const std::uint64_t height) {
    auto made = render::ImageWindow::create(x, y, width, height);
    return *made.value();
}

[[nodiscard]] render::Rgba32f pixelAt(const Reference& reference, const std::int64_t x,
                                      const std::int64_t y) {
    const auto& image = reference.frame->processImage();
    const auto window = image.descriptor()->dataWindow();
    if (x < window.originX() || y < window.originY() || x >= window.maxXExclusive() ||
        y >= window.maxYExclusive())
        return render::Rgba32f::transparent();
    const auto width = static_cast<std::int64_t>(window.extent().width());
    return image.pixels()[static_cast<std::size_t>((y - window.originY()) * width +
                                                   (x - window.originX()))];
}

[[nodiscard]] bool samePicture(const Reference& a, const Reference& b) {
    for (std::int64_t y = 0; y < kHeight; ++y)
        for (std::int64_t x = 0; x < kWidth; ++x) {
            const auto p = pixelAt(a, x, y);
            const auto q = pixelAt(b, x, y);
            if (std::memcmp(&p, &q, sizeof(render::Rgba32f)) != 0)
                return false;
        }
    return true;
}

[[nodiscard]] bool emptyPicture(const Reference& reference) {
    for (std::int64_t y = 0; y < kHeight; ++y)
        for (std::int64_t x = 0; x < kWidth; ++x)
            if (pixelAt(reference, x, y).alpha() != 0.0F)
                return false;
    return true;
}

[[nodiscard]] std::size_t switchCount(const runtime::CompiledCompositionPlan& plan) {
    return static_cast<std::size_t>(std::ranges::count_if(plan.operations(), [](const auto& op) {
        return std::holds_alternative<runtime::CompiledImageSwitch>(op);
    }));
}

// Whether any operation of `plan` was lowered from `node`.
[[nodiscard]] bool lowers(const runtime::CompiledCompositionPlan& plan, const document::NodeId node) {
    for (std::size_t index = 0; index < plan.operations().size(); ++index)
        if (runtime::detail::subjectFor(runtime::OperationIndex::fromRaw(index),
                                        plan.operations()[index])
                .nodeId == node)
            return true;
    return false;
}

[[nodiscard]] std::optional<std::size_t> operationOf(const runtime::CompiledCompositionPlan& plan,
                                                     const document::NodeId node) {
    for (std::size_t index = 0; index < plan.operations().size(); ++index)
        if (runtime::detail::subjectFor(runtime::OperationIndex::fromRaw(index),
                                        plan.operations()[index])
                .nodeId == node)
            return index;
    return std::nullopt;
}

[[nodiscard]] runtime::CancellationToken makeCancelledToken() {
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
        TaskRequest("image switch cancel token",
                    {.kind = TaskOwnerKind::Composition, .id = TaskOwnerId::fromRaw(531)}),
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
    while (!submission.handle.tryTakeResult().has_value() &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    scheduler.beginShutdown();
    while (!scheduler.isQuiescent() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    return token;
}

void selectionRuleTests(Expectations& expectations) {
    struct Row final {
        std::size_t count;
        double index;
        bool wrap;
        std::optional<std::size_t> expected;
    };
    const std::array rows{
        Row{3, 1.0, false, 0},  Row{3, 2.0, false, 1},  Row{3, 2.999, false, 1},
        Row{3, 3.0, false, 2},  Row{3, 0.0, false, 0},  Row{3, -5.0, false, 0},
        Row{3, 9.0, false, 2},  Row{3, 4.0, true, 0},   Row{3, 0.0, true, 2},
        Row{3, -1.0, true, 1},  Row{3, 0.5, true, 2},   Row{3, 7.0, true, 0},
        Row{8, 8.0, true, 7},   Row{8, 9.0, true, 0},   Row{1, 5.0, true, 0},
        Row{0, 1.0, false, std::nullopt},
        Row{3, std::numeric_limits<double>::infinity(), false, std::nullopt},
        Row{3, std::numeric_limits<double>::quiet_NaN(), true, std::nullopt},
    };
    bool all = true;
    for (const auto& row : rows)
        all = all && document::imageSwitchPosition(row.count, row.index, row.wrap) == row.expected;
    expectations.expect(all, "the selection rule counts from 1, clamps, and wraps both ways");
}

void constantTests(Expectations& expectations, References& gpu) {
    struct Case final {
        double index;
        bool wrap;
        Source shown;
        std::string label;
    };
    const std::array cases{
        Case{1.0, false, Source::Text, "index 1"},
        Case{2.0, false, Source::Blur, "index 2"},
        Case{3.0, false, Source::Solid, "index 3"},
        Case{0.0, false, Source::Text, "index 0 clamps to 1"},
        Case{7.0, false, Source::Solid, "index 7 clamps to 3"},
        Case{4.0, true, Source::Text, "index 4 wraps to 1"},
        Case{-1.0, true, Source::Blur, "index -1 wraps to 2"},
    };
    for (const auto& item : cases) {
        auto made = fixture();
        made->set(made->switcher, document::kImageSwitchIndexRole, item.index);
        made->set(made->switcher, document::kImageSwitchWrapRole, item.wrap);
        const auto plan = compileDocument(expectations, made->document, made->main, item.label);
        if (!plan)
            continue;
        expectations.expect(switchCount(*plan) == 0,
                            item.label + ": a constant index leaves no switch in the plan");
        const bool textShown = item.shown == Source::Text;
        const bool blurShown = item.shown == Source::Blur;
        const bool solidShown = item.shown == Source::Solid;
        expectations.expect(lowers(*plan, made->text) == textShown &&
                                lowers(*plan, made->blur) == blurShown &&
                                lowers(*plan, made->checker) == blurShown &&
                                lowers(*plan, made->solid) == solidShown,
                            item.label + ": only the shown branch is compiled");
        const auto switched = referenceAt(expectations, *made, frameTime(0), item.label);
        const auto wired = fixture(item.shown);
        const auto direct = referenceAt(expectations, *wired, frameTime(0), item.label + " wired");
        expectations.expect(switched && direct && samePicture(*switched, *direct) &&
                                !emptyPicture(*switched),
                            item.label + ": the frame is the shown input wired directly");
        if (switched && item.index == 2.0)
            gpu.emplace_back(item.label, *switched);
    }
}

void animatedTests(Expectations& expectations, References& gpu) {
    auto made = fixture();
    made->keyIndex(1.0, 3.0);
    const auto plan = compileDocument(expectations, made->document, made->main, "animated");
    if (!plan)
        return;
    expectations.expect(switchCount(*plan) == 1 && lowers(*plan, made->text) &&
                            lowers(*plan, made->blur) && lowers(*plan, made->solid),
                        "an animated index compiles one switch over every input");
    const auto switchIndex = operationOf(*plan, made->switcher);
    expectations.expect(switchIndex && plan->operationTimeDependent(
                                           runtime::OperationIndex::fromRaw(*switchIndex)),
                        "the switch varies with time");
    struct Frame final {
        std::int64_t frame;
        Source shown;
    };
    for (const auto& [frame, shown] :
         std::array{Frame{0, Source::Text}, Frame{4, Source::Text}, Frame{5, Source::Blur},
                    Frame{9, Source::Blur}, Frame{10, Source::Solid}}) {
        const auto label = "animated frame " + std::to_string(frame);
        const auto switched = referenceAt(expectations, *made, frameTime(frame), label);
        const auto wired = fixture(shown);
        const auto direct = referenceAt(expectations, *wired, frameTime(frame), label + " wired");
        expectations.expect(switched && direct && samePicture(*switched, *direct),
                            label + ": the frame is the chosen input wired directly");
        // Only the chosen branch is evaluated (evaluation counters, every operation a miss).
        auto request = requestFor(*plan);
        request.time = frameTime(frame);
        request.bypassOperationCache = true;
        runtime::OperationCacheStatistics statistics;
        const auto result =
            runtime::CpuCompositionEvaluator{}.evaluate(plan, request, {}, {}, nullptr, &statistics);
        const auto evaluated = [&](const document::NodeId node) {
            return std::ranges::count(statistics.evaluatedNodes, node) > 0;
        };
        expectations.expect(result.frame() != nullptr &&
                                evaluated(made->text) == (shown == Source::Text) &&
                                evaluated(made->blur) == (shown == Source::Blur) &&
                                evaluated(made->checker) == (shown == Source::Blur) &&
                                evaluated(made->solid) == (shown == Source::Solid),
                            label + ": only the chosen input's nodes are evaluated");
        if (switched)
            gpu.emplace_back(label, *switched);
    }
    // ROI and a reduced render scale over the blurred input.
    if (const auto roi = referenceOf(expectations, made->document, made->main, frameTime(5),
                                     "animated ROI", windowOf(100, 60, 200, 120)))
        gpu.emplace_back("animated ROI", *roi);
    const auto proxy = render::ImageExtent::create(256, 144);
    if (const auto reduced =
            referenceOf(expectations, made->document, made->main, frameTime(5), "animated proxy",
                        std::nullopt, runtime::EvaluationResolution{runtime::ProxyResolution{
                                          *proxy.value()}}))
        gpu.emplace_back("animated proxy", *reduced);
}

void emptyAndMutedTests(Expectations& expectations, References& gpu) {
    {
        auto made = fixture();
        apply<commands::DisconnectInput>(
            made->document, made->commands, made->main,
            document::NodeInputRef{made->switcher, std::string(document::kImageSwitchInputPorts[1])});
        made->set(made->switcher, document::kImageSwitchIndexRole, 2.0);
        const auto constant = referenceAt(expectations, *made, frameTime(0), "unconnected input");
        expectations.expect(constant && emptyPicture(*constant),
                            "a constant index on an unconnected input shows nothing");
        made->keyIndex(1.0, 3.0);
        const auto animated = referenceAt(expectations, *made, frameTime(5), "unconnected animated");
        expectations.expect(animated && emptyPicture(*animated),
                            "an animated index on an unconnected input shows nothing");
        const auto last = referenceAt(expectations, *made, frameTime(10), "past the gap");
        const auto wired = fixture(Source::Solid);
        const auto direct = referenceAt(expectations, *wired, frameTime(10), "past the gap wired");
        expectations.expect(last && direct && samePicture(*last, *direct),
                            "the input after the gap still shows");
        if (animated)
            gpu.emplace_back("unconnected animated", *animated);
    }
    {
        auto made = fixture();
        made->set(made->switcher, document::kImageSwitchIndexRole, 3.0);
        apply<commands::SetNodeMuted>(made->document, made->commands, made->main, made->switcher,
                                        true);
        const auto plan = compileDocument(expectations, made->document, made->main, "muted");
        expectations.expect(plan && !lowers(*plan, made->solid) && !lowers(*plan, made->blur),
                            "a muted switch compiles only its first input");
        const auto muted = referenceAt(expectations, *made, frameTime(0), "muted");
        const auto wired = fixture(Source::Text);
        const auto direct = referenceAt(expectations, *wired, frameTime(0), "muted wired");
        expectations.expect(muted && direct && samePicture(*muted, *direct),
                            "a muted switch shows its first input");
    }
}

void cacheTests(Expectations& expectations) {
    auto made = fixture();
    made->keyIndex(1.0, 3.0);
    const runtime::CpuCompositionEvaluator evaluator;
    const auto evaluated = [&](const std::string& label, const std::int64_t frame) {
        const auto plan = compileDocument(expectations, made->document, made->main, label);
        std::vector<document::NodeId> nodes;
        if (!plan)
            return nodes;
        auto request = requestFor(*plan);
        request.time = frameTime(frame);
        runtime::OperationCacheStatistics statistics;
        const auto result = evaluator.evaluate(plan, request, {}, {}, nullptr, &statistics);
        expectations.expect(result.frame() != nullptr, label + ": evaluates");
        return statistics.evaluatedNodes;
    };
    const auto has = [](const std::vector<document::NodeId>& nodes, const document::NodeId node) {
        return std::ranges::count(nodes, node) > 0;
    };
    const auto first = evaluated("frame 0", 0);
    expectations.expect(has(first, made->text) && !has(first, made->blur),
                        "frame 0 evaluates the text only");
    const auto second = evaluated("frame 5", 5);
    expectations.expect(has(second, made->blur) && has(second, made->checker) &&
                            !has(second, made->text) && !has(second, made->solid),
                        "switching to the blur evaluates only the blur's branch");
    const auto back = evaluated("frame 0 again", 0);
    expectations.expect(!has(back, made->text) && !has(back, made->blur) &&
                            !has(back, made->switcher),
                        "returning to frame 0 evaluates nothing of either branch");
    made->set(made->blur, "radius", 9.0);
    const auto unselected = evaluated("unselected change", 0);
    expectations.expect(!has(unselected, made->text) && !has(unselected, made->blur),
                        "a change to an unselected branch keeps the shown frame's identity");
    const auto selected = evaluated("selected change", 5);
    expectations.expect(has(selected, made->blur) && !has(selected, made->checker),
                        "the changed branch is evaluated again when it shows");
}

// A Solid layer whose content is a Channel Pass of a Channel Set Switch over two Image to Channel
// Set nodes (a red Solid's and a Checker's channels), or one of them wired straight into the Pass.
struct ChannelFixture final {
    std::unique_ptr<Fixture> base;
    document::NodeId pass;
    document::NodeId channelSwitch;
    std::array<document::NodeId, 2> packed{};
    std::array<document::NodeId, 2> sources{};
};

[[nodiscard]] ChannelFixture channelFixture(const std::optional<std::size_t> direct = std::nullopt) {
    ChannelFixture made;
    made.base = std::make_unique<Fixture>(document::makeNewProject(
        "Passes", "Main", core::RationalTime::fromInteger(10), format()));
    auto& doc = *made.base;
    apply<commands::AddSolidLayer>(doc.document, doc.commands, doc.main, "Solid",
                                     core::Color4d{0.9, 0.1, 0.1, 1.0});
    const auto red = lastNode(doc.document, doc.main, document::kSolidSourceNodeType);
    for (const auto& edge : composition(doc.document, doc.main).graph().edges())
        if (edge.source.nodeId == red)
            doc.fed = edge.destination;
    const auto checker = doc.add(document::kCheckerSourceNodeType);
    made.sources = {red, checker};
    for (std::size_t index = 0; index < 2; ++index) {
        made.packed[index] = doc.add(document::kImageToChannelSetNodeType);
        doc.connect(made.sources[index], "image",
                    document::NodeInputRef{made.packed[index],
                                           std::string(document::kImageToChannelSetImagePort)});
    }
    made.pass = doc.add(document::kChannelPassNodeType);
    doc.set(made.pass, document::kChannelPassParameterRole, std::string("pass"));
    doc.connect(made.pass, document::kChannelPassImagePort, doc.fed);
    const document::NodeInputRef passInput{made.pass, std::string(document::kChannelSetPort)};
    if (direct) {
        doc.connect(made.packed[*direct], document::kChannelSetPort, passInput);
        return made;
    }
    made.channelSwitch = doc.add(document::kChannelSetSwitchNodeType);
    doc.switcher = made.channelSwitch;
    doc.connect(made.channelSwitch, document::kChannelSetSwitchOutputPort, passInput);
    for (std::size_t index = 0; index < 2; ++index)
        doc.connect(made.packed[index], document::kChannelSetPort,
                    document::NodeInputRef{made.channelSwitch,
                                           std::string(document::kChannelSetSwitchInputPorts[index])});
    return made;
}

void channelSetTests(Expectations& expectations, References& gpu) {
    const auto direct = [&](const std::size_t which, const std::int64_t frame) {
        auto wired = channelFixture(which);
        return referenceAt(expectations, *wired.base, frameTime(frame),
                           "passes wired " + std::to_string(which + 1));
    };
    for (const std::size_t which : {std::size_t{0}, std::size_t{1}}) {
        auto made = channelFixture();
        made.base->set(made.channelSwitch, document::kImageSwitchIndexRole,
                       static_cast<double>(which + 1));
        const auto label = "constant pass " + std::to_string(which + 1);
        const auto plan = compileDocument(expectations, made.base->document, made.base->main, label);
        expectations.expect(plan && switchCount(*plan) == 0 && !lowers(*plan, made.sources[1 - which]),
                            label + ": a constant Channel Set Switch compiles away");
        const auto switched = referenceAt(expectations, *made.base, frameTime(0), label);
        const auto wired = direct(which, 0);
        expectations.expect(switched && wired && samePicture(*switched, *wired) &&
                                !emptyPicture(*switched),
                            label + ": the Pass reads the shown channel set");
    }
    {
        auto made = channelFixture();
        made.base->keyIndex(1.0, 2.0);
        const auto plan =
            compileDocument(expectations, made.base->document, made.base->main, "animated passes");
        expectations.expect(plan && switchCount(*plan) == 1,
                            "an animated Channel Set Switch switches its Pass per input");
        for (const auto& [frame, which] : std::array{std::pair{std::int64_t{0}, std::size_t{0}},
                                                     std::pair{std::int64_t{10}, std::size_t{1}}}) {
            const auto label = "animated pass frame " + std::to_string(frame);
            const auto switched = referenceAt(expectations, *made.base, frameTime(frame), label);
            const auto wired = direct(which, frame);
            expectations.expect(switched && wired && samePicture(*switched, *wired),
                                label + ": the Pass reads the chosen channel set");
            if (plan) {
                auto request = requestFor(*plan);
                request.time = frameTime(frame);
                request.bypassOperationCache = true;
                runtime::OperationCacheStatistics statistics;
                const auto result = runtime::CpuCompositionEvaluator{}.evaluate(
                    plan, request, {}, {}, nullptr, &statistics);
                expectations.expect(
                    result.frame() != nullptr &&
                        std::ranges::count(statistics.evaluatedNodes, made.sources[1 - which]) == 0,
                    label + ": the other channel set's image is not evaluated");
            }
            if (switched)
                gpu.emplace_back(label, *switched);
        }
    }
    {
        // One Integer value node drives an Image Switch of the beauty and the Channel Set Switch of
        // its passes: both follow it together.
        auto made = channelFixture();
        auto& doc = *made.base;
        const auto value = doc.add(document::kIntegerValueNodeType);
        doc.set(value, "value", std::int64_t{2});
        doc.connect(value, document::kValuePortName,
                    document::NodeInputRef{made.channelSwitch,
                                           std::string(document::kImageSwitchIndexRole)});
        const auto driven = referenceAt(expectations, doc, frameTime(0), "driven pass 2");
        const auto wired = direct(1, 0);
        expectations.expect(driven && wired && samePicture(*driven, *wired) &&
                                switchCount(*driven->plan) == 1,
                            "a driven Channel Set Switch follows its driver");
        doc.set(value, "value", std::int64_t{1});
        const auto back = referenceAt(expectations, doc, frameTime(0), "driven pass 1");
        const auto first = direct(0, 0);
        expectations.expect(back && first && samePicture(*back, *first),
                            "and switches back when the driver does");
        if (driven)
            gpu.emplace_back("driven pass", *driven);
        // The same driver on an Image Switch of the beauty (the Solid and the Checker).
        auto beauty = fixture();
        const auto beautyValue = beauty->add(document::kIntegerValueNodeType);
        beauty->set(beautyValue, "value", std::int64_t{2});
        beauty->connect(beautyValue, document::kValuePortName,
                        document::NodeInputRef{beauty->switcher,
                                               std::string(document::kImageSwitchIndexRole)});
        const auto beautyDriven = referenceAt(expectations, *beauty, frameTime(0), "driven beauty");
        const auto beautyWired = fixture(Source::Blur);
        const auto beautyDirect =
            referenceAt(expectations, *beautyWired, frameTime(0), "driven beauty wired");
        expectations.expect(beautyDriven && beautyDirect && samePicture(*beautyDriven, *beautyDirect),
                            "a driven Image Switch follows its driver the same way");
    }
    {
        // #346's rule through #531: an animated Image Switch's `selected` output drives the
        // Channel Set Switch of the passes, so the passes follow the beauty frame by frame.
        auto made = channelFixture();
        auto& doc = *made.base;
        const auto beauty = doc.add(document::kImageSwitchNodeType);
        doc.switcher = beauty;
        for (std::size_t index = 0; index < 2; ++index)
            doc.connect(made.sources[index], "image",
                        document::NodeInputRef{beauty,
                                               std::string(document::kImageSwitchInputPorts[index])});
        doc.keyIndex(1.0, 2.0);
        doc.connect(beauty, document::kImageSwitchSelectedPort,
                    document::NodeInputRef{made.channelSwitch,
                                           std::string(document::kImageSwitchIndexRole)});
        for (const auto& [frame, which] : std::array{std::pair{std::int64_t{0}, std::size_t{0}},
                                                     std::pair{std::int64_t{9}, std::size_t{0}},
                                                     std::pair{std::int64_t{10}, std::size_t{1}}}) {
            const auto label = "passes follow the beauty, frame " + std::to_string(frame);
            const auto followed = referenceAt(expectations, doc, frameTime(frame), label);
            const auto wired = direct(which, frame);
            expectations.expect(followed && wired && samePicture(*followed, *wired),
                                label + ": the switch's selected output chooses the passes");
            if (followed && frame == 10)
                gpu.emplace_back(label, *followed);
        }
    }
    {
        // An animated Channel Set Switch cannot choose a composition's written channel layers.
        auto made = channelFixture();
        auto& doc = *made.base;
        doc.keyIndex(1.0, 2.0);
        const auto output = lastNode(doc.document, doc.main, document::kCompositionOutputNodeType);
        doc.connect(made.channelSwitch, document::kChannelSetSwitchOutputPort,
                    document::NodeInputRef{output,
                                           std::string(document::kCompositionOutputChannelsInputPort)});
        const runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
        const auto compiled = compiler.compile({doc.document.snapshot(), doc.main}, {});
        const auto described =
            runtime::describeCompileFailure(compiled, doc.document.snapshot().project(), doc.main);
        expectations.expect(compiled.plan == nullptr &&
                                described.find("Channel Set Switch") != std::string::npos,
                            "an animated switch on written channel layers is refused, named: " +
                                described);
        // A constant index (the curve replaced by a constant) compiles.
        doc.set(made.channelSwitch, document::kImageSwitchIndexRole, 2.0);
        expectations.expect(compileDocument(expectations, doc.document, doc.main,
                                            "constant written layers") != nullptr,
                            "a constant switch on written channel layers compiles");
    }
}

void budgetAndCancelTests(Expectations& expectations) {
    auto made = fixture();
    made->keyIndex(1.0, 3.0);
    const auto reference = referenceAt(expectations, *made, frameTime(5), "budget");
    if (!reference)
        return;
    auto tight = reference->request;
    tight.pixelStorageByteLimit = 4096;
    tight.bypassOperationCache = true;
    const auto refused = runtime::CpuCompositionEvaluator{}.evaluate(reference->plan, tight, {});
    expectations.expect(refused.frame() == nullptr &&
                            std::ranges::any_of(refused.diagnostics(),
                                                [](const auto& diagnostic) {
                                                    return diagnostic.code ==
                                                           runtime::EvaluationDiagnosticCode::
                                                               PixelStorageBudgetExceeded;
                                                }),
                        "a frame past its pixel budget is refused with the budget diagnostic");
    const auto gpuRefused = runtime::CpuGpuSceneBuilder{}.build(reference->plan, tight);
    expectations.expect(!gpuRefused.hasValue(),
                        "the GPU preparation refuses it too: " + gpuRefused.diagnostic.message);
    const auto cancelled = runtime::CpuCompositionEvaluator{}.evaluate(
        reference->plan, reference->request, makeCancelledToken());
    expectations.expect(cancelled.status() == runtime::EvaluationStatus::Cancelled &&
                            cancelled.frame() == nullptr,
                        "a cancelled evaluation publishes nothing");
    const auto prepared = runtime::CpuGpuSceneBuilder{}.build(reference->plan, reference->request,
                                                              makeCancelledToken());
    expectations.expect(!prepared.hasValue() &&
                            prepared.diagnostic.code ==
                                runtime::PreparedGpuSceneDiagnosticCode::Cancelled,
                        "a cancelled GPU preparation is cancelled");
}

[[nodiscard]] project::ProjectIoOperationMemory memory() {
    constexpr std::uint64_t budget = 64U << 20U;
    auto coordinator = project::ProjectIoMemoryCoordinator::create(budget);
    if (!coordinator)
        throw std::runtime_error("memory coordinator");
    auto operation = coordinator->createOperation(budget, budget);
    if (!operation)
        throw std::runtime_error("memory operation");
    return std::move(*operation);
}

void persistenceTests(Expectations& expectations, References& gpu) {
    auto made = fixture();
    made->keyIndex(1.0, 3.0);
    made->set(made->switcher, document::kImageSwitchWrapRole, true);
    const auto before = referenceAt(expectations, *made, frameTime(5), "before save");
    const auto settings = document::makeBloomNeutralColorSettingsV1({});
    const auto snapshot = made->document.snapshot();
    const project::CanonicalManifestV1 manifest;
    const project::CanonicalDocumentV1 input{.snapshot = &snapshot, .colorSettings = &settings};
    auto saved = project::buildVerifiedSaveArchive(manifest, input, {}, memory());
    expectations.expect(saved && saved.archive(), "the switched project saves");
    if (!saved || !saved.archive() || !before)
        return;
    const auto bytes = saved.archive()->bytes();
    auto opened = project::openProjectArchive(bytes, {}, memory());
    expectations.expect(opened.outcome() == project::OpenArchiveOutcome::Opened,
                        "the switched project opens");
    if (opened.outcome() != project::OpenArchiveOutcome::Opened)
        return;
    auto reopened = std::move(opened).takeOpened();
    expectations.expect(!reopened.roundTrip, "nothing unknown is retained");
    const auto after =
        referenceOf(expectations, *reopened.document, made->main, frameTime(5), "after reopen");
    expectations.expect(after && samePicture(*before, *after),
                        "save and reopen draw the same pixels");
    expectations.expect(after && switchCount(*after->plan) == 1,
                        "the reopened index is still animated");
    gpu.emplace_back("reopened", *before);
}

#ifdef BLOOM_GPUSHADER_TOOLS_DIR
// The pinned shader tools prepare the blur's working-space programs.
[[nodiscard]] runtime::GpuSceneOcioContext ocioContext() {
    runtime::GpuOcioCompileOptions options;
    options.glslangValidatorPath =
        std::string(BLOOM_GPUSHADER_TOOLS_DIR) + "/glslangValidator" BLOOM_EXECUTABLE_SUFFIX;
    options.spirvValPath =
        std::string(BLOOM_GPUSHADER_TOOLS_DIR) + "/spirv-val" BLOOM_EXECUTABLE_SUFFIX;
    runtime::GpuSceneOcioContext context;
    context.preparer = std::make_shared<runtime::GpuOcioProgramPreparer>();
    context.compileOptions = options;
    return context;
}

// Dispatches one cold run of `scene` takes on a cache and executor of its own.
[[nodiscard]] std::optional<std::uint64_t>
coldDispatches(render::GpuDevice& device,
               const std::shared_ptr<const runtime::PreparedGpuScene>& scene) {
    auto cache = runtime::GpuSceneCache::create(device, runtime::GpuSceneCacheBudgets{});
    if (!cache)
        return std::nullopt;
    auto executor = runtime::GpuSceneExecutor::create(device, *cache.cache);
    if (!executor)
        return std::nullopt;
    if (runScene(*executor.executor, scene).empty())
        return std::nullopt;
    return executor.executor->counters().dispatches;
}

// The prepared scene of a frame that shows the text holds no command of the blur or the checker
// and dispatches exactly what the text wired directly does; switching to the blur dispatches it,
// and returning to a frame already drawn dispatches nothing new.
void gpuLivenessTests(Expectations& expectations, render::GpuDevice& device,
                      const runtime::CpuGpuSceneBuilder& builder) {
    auto made = fixture();
    made->keyIndex(1.0, 3.0);
    const auto first = referenceAt(expectations, *made, frameTime(0), "gpu frame 0");
    const auto blurred = referenceAt(expectations, *made, frameTime(5), "gpu frame 5");
    if (!first || !blurred)
        return;
    const auto blur = operationOf(*first->plan, made->blur);
    const auto checker = operationOf(*first->plan, made->checker);
    const auto shown = builder.build(first->plan, first->request);
    expectations.expect(shown.hasValue(), "frame 0 prepares: " + shown.diagnostic.message);
    if (!shown || !blur || !checker)
        return;
    const auto& map = shown.scene->commandForOperation();
    bool none = *blur < map.size() && map[*blur] == runtime::kInvalidGpuSceneCommand &&
                *checker < map.size() && map[*checker] == runtime::kInvalidGpuSceneCommand;
    for (const auto& command : shown.scene->commands())
        std::visit(
            [&](const auto& item) {
                if constexpr (requires { item.sourceOperation; })
                    none = none && item.sourceOperation.value() != *blur &&
                           item.sourceOperation.value() != *checker;
            },
            command);
    expectations.expect(none, "an unselected blur is not prepared: no command, no dispatch");
    const auto wired = fixture(Source::Text);
    const auto direct = referenceAt(expectations, *wired, frameTime(0), "gpu frame 0 wired");
    if (direct) {
        const auto directScene = builder.build(direct->plan, direct->request);
        const auto switchedCold = coldDispatches(device, shown.scene);
        const auto directCold =
            directScene ? coldDispatches(device, directScene.scene) : std::nullopt;
        expectations.expect(directScene.hasValue() &&
                                directScene.scene->commands().size() ==
                                    shown.scene->commands().size() &&
                                switchedCold && directCold && *switchedCold == *directCold,
                            "the switched frame prepares and dispatches exactly what its input "
                            "wired directly does");
    }
    auto cache = runtime::GpuSceneCache::create(device, runtime::GpuSceneCacheBudgets{});
    auto executor = cache ? runtime::GpuSceneExecutor::create(device, *cache.cache)
                          : decltype(runtime::GpuSceneExecutor::create(device, *cache.cache)){};
    expectations.expect(cache.hasValue() && executor.hasValue(), "a fresh executor creates");
    if (!cache || !executor)
        return;
    auto& run = *executor.executor;
    static_cast<void>(runScene(run, shown.scene));
    const auto blurScene = builder.build(blurred->plan, blurred->request);
    expectations.expect(blurScene.hasValue(), "frame 5 prepares: " + blurScene.diagnostic.message);
    if (!blurScene)
        return;
    const auto beforeBlur = run.counters().dispatches;
    static_cast<void>(runScene(run, blurScene.scene));
    expectations.expect(run.counters().dispatches > beforeBlur,
                        "switching to the blur dispatches the blur");
    const auto again = builder.build(first->plan, first->request);
    const auto beforeAgain = run.counters().dispatches;
    const auto pixels = again ? runScene(run, again.scene) : std::vector<render::Rgba32f>{};
    expectations.expect(again.hasValue() && run.counters().dispatches == beforeAgain &&
                            parity(pixels, first->frame->processImage().pixels()),
                        "returning to frame 0 dispatches nothing new and draws frame 0");
}
#endif

} // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parseOptions(argc, argv);
        if (!options.valid) {
            std::cerr << "invalid arguments\n";
            return 2;
        }
        Expectations expectations;
        References references;
        selectionRuleTests(expectations);
        constantTests(expectations, references);
        animatedTests(expectations, references);
        emptyAndMutedTests(expectations, references);
        cacheTests(expectations);
        channelSetTests(expectations, references);
        budgetAndCancelTests(expectations);
        persistenceTests(expectations, references);
        if (expectations.failures() != 0) {
            std::cerr << expectations.failures() << " image switch expectation(s) failed\n";
            return 1;
        }

#ifndef BLOOM_GPUSHADER_TOOLS_DIR
        std::cout << "NOTRUN: the switched blur needs the pinned shader tools\n";
        if (options.require_device) {
            std::cerr << "FAIL: the image switch GPU cases need the pinned shader tools\n";
            return 1;
        }
        return 77;
#else
        render::GpuDeviceCreationOptions createOptions;
        createOptions.loader_path = options.loader_path;
        auto device = render::GpuDevice::create(createOptions);
        if (!device) {
            if (options.require_device) {
                std::cerr << "FAIL: required device unavailable: " << device.diagnostic.message
                          << '\n';
                return 1;
            }
            std::cout << "SKIP: CPU checks passed; no compatible Vulkan device available: "
                      << device.diagnostic.message << '\n';
            return 77;
        }
        auto cache =
            runtime::GpuSceneCache::create(*device.device, runtime::GpuSceneCacheBudgets{});
        expectations.expect(cache.hasValue(), "the scene cache creates");
        if (!cache)
            return 1;
        auto executor = runtime::GpuSceneExecutor::create(*device.device, *cache.cache);
        expectations.expect(executor.hasValue(), "the scene executor creates");
        if (!executor)
            return 1;
        const runtime::CpuGpuSceneBuilder builder(nullptr, {}, ocioContext());
        for (const auto& [label, reference] : references)
            gpuParity(expectations, *executor.executor, reference, label, builder);
        gpuLivenessTests(expectations, *device.device, builder);
        if (expectations.failures() != 0) {
            std::cerr << expectations.failures() << " image switch expectation(s) failed\n";
            return 1;
        }
        std::cout << "PASS: Image Switch and Channel Set Switch draw only the chosen input, alike "
                     "on the GPU and the CPU reference ("
                  << references.size() << " states)\n";
        return 0;
#endif
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected test exception: " << exception.what() << '\n';
        return 1;
    }
}
