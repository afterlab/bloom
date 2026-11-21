// #752: the Subtitles node through real documents (docs/architecture/subtitles.md, "Layout",
// "Rendering", "Checks").
//
// A 512x288 composition gets a Subtitles layer through AddSubtitlesLayer and is compiled by the
// production SnapshotCompiler. The CPU half always runs:
//
// - layout against expectations worked out by hand: Balanced against Greedy breaks, a CJK break,
//   Shrink to fit, the stacking order of overlapping cues, and the placement a WebVTT, an ASS and a
//   TTML file ask for;
// - pixels over a few frames: a cue inside the title-safe area, nothing between cues, nothing with
//   Burn in off, a box behind the text;
// - the `text`, `index` and `progress` outputs, the speaker prefix;
// - the cache identity: frames inside one cue reuse one result, a frame of another cue does not;
// - the budget refusals (17 cues at once, more than 8,192 glyphs) naming the time and the cues;
// - cancellation, and the per-cue checks with the complex-script warning naming the cue.
//
// Then every state is prepared by the production CpuGpuSceneBuilder, run by the production
// GpuSceneExecutor, and must match the CPU reference (2e-6) full frame, over an ROI and at a
// reduced render scale, with a warm rerun that dispatches nothing, a changed value that changes the
// pixels, and a frame later in the same cue that dispatches nothing new.
//
// The node is a vector leaf: a scaled or rotated Subtitles layer redraws its paths through the
// layer matrix instead of resampling the node's image. Against the unscaled render, a layer scaled
// two times covers four times the solid area with an antialiased edge one pixel wide (about twice
// the unscaled edge pixels, where a resampled image's edge would be two pixels wide, about four
// times), and a rotated layer keeps its solid area and edge; both match the CPU reference on the GPU,
// also at half opacity (folded layers, and a single layer), warm and on a later frame of the cue with
// nothing dispatched, and a cancelled redraw publishes nothing on either route. Without a device the GPU
// half reports an explicit SKIP (exit 77) after the CPU half passed; --require-device makes that a
// failure.

#include "document_render_parity_support.hpp"

#include <bloom/commands/operations.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/subtitles_nodes.hpp>
#include <bloom/runtime/compiled_value_graph.hpp>
#include <bloom/runtime/subtitles_layout.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/runtime/value_graph_evaluation.hpp>
#include <bloom/subtitles/subtitles.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <iostream>
#include <mutex>
#include <thread>

namespace {

using namespace bloom;
using namespace bloom::test::document_render;
using render::composite_proof::Options;
using render::composite_proof::parseOptions;

constexpr std::uint32_t kWidth = 512;
constexpr std::uint32_t kHeight = 288;

document::CompositionFormat format() {
    return *document::CompositionFormat::create(kWidth, kHeight, core::PixelAspectRatio::square(),
                                                *document::FrameRate::create(24, 1));
}

[[nodiscard]] core::RationalTime seconds(const std::int64_t numerator,
                                         const std::int64_t denominator = 1) {
    return *core::RationalTime::create(numerator, denominator);
}

[[nodiscard]] document::CueHandle cue(const std::uint64_t id, const core::RationalTime start,
                                      const core::RationalTime end, std::string text,
                                      std::string speaker = {}) {
    auto made = std::make_shared<document::Cue>();
    made->id = id;
    made->start = start;
    made->end = end;
    made->text = std::move(text);
    made->speaker = std::move(speaker);
    return made;
}

[[nodiscard]] document::CueListValue listOf(std::vector<document::CueHandle> cues) {
    document::CueListValue list;
    list.language = "en";
    std::uint64_t next = 1;
    for (const auto& handle : cues)
        next = std::max(next, handle->id + 1);
    list.nextCueId = next;
    list.cues = std::move(cues);
    document::canonicalizeCueList(list);
    return list;
}

// One document with one Subtitles layer, and a way to set the node's parameters.
struct Fixture final {
    document::Document document;
    commands::CommandStack commands;
    document::CompositionId main;
    document::NodeId node;

    explicit Fixture(document::NewProject initial)
        : document(std::move(initial.project)), commands(document),
          main(initial.initialCompositionId) {}

    void set(const std::string_view role, document::ParameterValue value) {
        try {
            execute<commands::SetParameterSource>(document, commands, main,
                                                  parameter(document, main, node, role),
                                                  document::ConstantValueSource{std::move(value)});
        } catch (const std::runtime_error&) {
            throw std::runtime_error("setting " + std::string(role) + " was refused");
        }
    }
};

[[nodiscard]] std::unique_ptr<Fixture> fixture(document::CueListValue list) {
    auto made = std::make_unique<Fixture>(document::makeNewProject(
        "Subtitles", "Main", core::RationalTime::fromInteger(10), format()));
    execute<commands::AddSubtitlesLayer>(made->document, made->commands, made->main, "Subtitles",
                                         std::move(list));
    made->node = lastNode(made->document, made->main, document::kSubtitlesNodeType);
    return made;
}

[[nodiscard]] std::shared_ptr<const runtime::CompiledCompositionPlan>
compile(Expectations& expectations, const Fixture& fixture, const std::string& label) {
    const runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
    const auto compiled = compiler.compile({fixture.document.snapshot(), fixture.main}, {});
    expectations.expect(compiled.plan != nullptr,
                        label + ": compiles: " +
                            runtime::describeCompileFailure(
                                compiled, fixture.document.snapshot().project(), fixture.main));
    return compiled.plan;
}

[[nodiscard]] const runtime::CompiledSubtitles*
subtitlesOf(const runtime::CompiledCompositionPlan& plan) {
    for (const auto& operation : plan.operations())
        if (const auto* found = std::get_if<runtime::CompiledSubtitles>(&operation))
            return found;
    return nullptr;
}

// A CPU reference at `time`, optionally over an ROI or at a reduced render scale.
[[nodiscard]] std::optional<Reference>
referenceAt(Expectations& expectations, const Fixture& fixture, const core::RationalTime time,
            const std::string& label, const std::optional<render::ImageWindow>& roi = {},
            const std::optional<runtime::EvaluationResolution>& resolution = {}) {
    const auto plan = compile(expectations, fixture, label);
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

[[nodiscard]] bool samePixels(const Reference& a, const Reference& b) {
    const auto left = a.frame->processImage().pixels();
    const auto right = b.frame->processImage().pixels();
    return left.size_bytes() == right.size_bytes() &&
           std::memcmp(left.data(), right.data(), left.size_bytes()) == 0;
}

// The color of the CPU reference at a composition pixel.
[[nodiscard]] render::Rgba32f pixelAt(const Reference& reference, const std::int64_t x,
                                      const std::int64_t y) {
    const auto& image = reference.frame->processImage();
    const auto window = image.descriptor()->dataWindow();
    if (x < window.originX() || y < window.originY() || x >= window.maxXExclusive() ||
        y >= window.maxYExclusive())
        return render::Rgba32f::transparent();
    const auto width = static_cast<std::int64_t>(window.extent().width());
    return image
        .pixels()[static_cast<std::size_t>((y - window.originY()) * width + (x - window.originX()))];
}

[[nodiscard]] render::ImageWindow windowOf(const std::int64_t x, const std::int64_t y,
                                           const std::uint64_t width, const std::uint64_t height) {
    auto made = render::ImageWindow::create(x, y, width, height);
    return *made.value();
}

[[nodiscard]] render::ImageExtent halfExtent() {
    auto made = render::ImageExtent::create(kWidth / 2, kHeight / 2);
    return *made.value();
}

// A token of a scheduler task that was cancelled while it ran.
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
        TaskRequest("subtitles cancel token",
                    {.kind = TaskOwnerKind::Composition, .id = TaskOwnerId::fromRaw(752)}),
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

using Layouts = std::vector<runtime::SubtitleCueLayout>;

[[nodiscard]] Layouts layoutOf(Expectations& expectations, const runtime::CompiledSubtitles& node,
                               const document::CueListValue& list,
                               const std::vector<std::size_t>& indices, const std::string& label) {
    const auto laid =
        runtime::layoutSubtitleCues(node, list, indices, runtime::subtitlesConstantStyle(node));
    const auto* layouts = std::get_if<Layouts>(&laid);
    expectations.expect(layouts != nullptr && layouts->size() == indices.size(),
                        label + ": lays out");
    return layouts != nullptr ? *layouts : Layouts{};
}

[[nodiscard]] bool near(const double a, const double b, const double tolerance = 1e-6) {
    return std::abs(a - b) <= tolerance;
}

// Balanced and Greedy breaks, a CJK break, Shrink to fit and stacking, on the compiled node.
void layoutTests(Expectations& expectations) {
    auto made = fixture(listOf({cue(1, seconds(0), seconds(4), "abcd abcd abcd abcd abcd")}));
    made->set("safeArea", std::int64_t{document::kSubtitlesSafeFullFrame});
    made->set("maxLines", std::int64_t{3});
    made->set("size", 16.0);
    const auto plan = compile(expectations, *made, "layout");
    const auto* compiled = plan ? subtitlesOf(*plan) : nullptr;
    expectations.expect(compiled != nullptr, "layout: the plan holds the Subtitles operation");
    if (compiled == nullptr)
        return;
    auto node = *compiled;
    const auto& list = *node.cues.list;
    const auto one = layoutOf(expectations, node, list, {0}, "one line");
    if (one.empty())
        return;
    expectations.expect(one[0].block.lines.size() == 1 && one[0].block.lines[0].characters == 24,
                        "the five words fit one line of the full frame: " +
                            std::to_string(one[0].block.lines.size()) + " lines, " +
                            std::to_string(one[0].block.width) + " wide");
    const double natural = one[0].block.width;
    // Room for four words but not five: Greedy fills the first line, Balanced evens the two lines
    // out and, of the two even splits, keeps the longer line at the bottom.
    node.settings.maxWidth = natural * 0.9 / kWidth;
    node.settings.wrap = document::kSubtitlesWrapGreedy;
    const auto greedy = layoutOf(expectations, node, list, {0}, "greedy");
    expectations.expect(!greedy.empty() && greedy[0].block.lines.size() == 2 &&
                            greedy[0].block.lines[0].characters == 19 &&
                            greedy[0].block.lines[1].characters == 4,
                        "Greedy breaks after four words");
    node.settings.wrap = document::kSubtitlesWrapBalanced;
    const auto balanced = layoutOf(expectations, node, list, {0}, "balanced");
    expectations.expect(!balanced.empty() && balanced[0].block.lines.size() == 2 &&
                            balanced[0].block.lines[0].characters == 9 &&
                            balanced[0].block.lines[1].characters == 14,
                        "Balanced breaks after two words, the longer line at the bottom");

    // Shrink to fit: one line allowed, the text 1.2 times too wide. 85 % is still too wide; 80 %
    // fits, so the cue draws at 80 % of its size on one line.
    node.settings.maxWidth = natural / 1.2 / kWidth;
    node.settings.maxLines = 1;
    node.settings.overflow = document::kSubtitlesOverflowShrink;
    const auto shrunk = layoutOf(expectations, node, list, {0}, "shrink");
    expectations.expect(!shrunk.empty() && shrunk[0].shrunk &&
                            shrunk[0].block.lines.size() == 1 &&
                            near(shrunk[0].options.horizontalSize,
                                 one[0].options.horizontalSize * 0.8),
                        "Shrink to fit draws the cue at 80 % on one line");
    node.settings.overflow = document::kSubtitlesOverflowShowAll;
    const auto all = layoutOf(expectations, node, list, {0}, "show all");
    expectations.expect(!all.empty() && !all[0].shrunk && all[0].block.lines.size() == 2 &&
                            near(all[0].options.horizontalSize, one[0].options.horizontalSize),
                        "Show all lines never scales");

    // A CJK line breaks between ideographs although it has no space: 24 characters wrap onto
    // more than one line and every character still shows.
    std::string cjk;
    for (int repeat = 0; repeat < 4; ++repeat)
        cjk += "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\xe3\x81\xae\xe5\xad\x97\xe5\xb9\x95";
    const auto cjkList = listOf({cue(1, seconds(0), seconds(4), cjk)});
    node.settings.maxWidth = 0.25;
    node.settings.maxLines = 8;
    node.settings.wrap = document::kSubtitlesWrapGreedy;
    const auto wrapped = layoutOf(expectations, node, cjkList, {0}, "cjk");
    std::size_t shown = 0;
    if (!wrapped.empty())
        for (const auto& line : wrapped[0].block.lines)
            shown += line.characters;
    expectations.expect(!wrapped.empty() && wrapped[0].block.lines.size() > 1 && shown == 24,
                        "a CJK cue with no spaces wraps between ideographs");

    // Overlapping cues stack away from the anchored edge in start order: at the bottom the
    // earlier cue is lowest, at the top it is highest, and the boxes never overlap.
    const auto pair = listOf({cue(1, seconds(0), seconds(4), "First"),
                              cue(2, seconds(1), seconds(4), "Second")});
    node.settings = compiled->settings;
    node.settings.safeArea = document::kSubtitlesSafeFullFrame;
    const auto bottom = layoutOf(expectations, node, pair, {0, 1}, "stack bottom");
    expectations.expect(bottom.size() == 2 && bottom[0].index == 0 &&
                            bottom[1].y + bottom[1].block.height <= bottom[0].y + 1e-9 &&
                            near(bottom[0].y + bottom[0].block.height, kHeight),
                        "at the bottom the earliest cue sits on the edge, the next above it");
    node.settings.anchor = 1; // top center
    const auto top = layoutOf(expectations, node, pair, {0, 1}, "stack top");
    expectations.expect(top.size() == 2 && near(top[0].y, 0.0) &&
                            top[0].y + top[0].block.height <= top[1].y + 1e-9,
                        "at the top the earliest cue sits on the edge, the next below it");
}

// The placement a file asks for, laid out with Use cue positions on (the default).
void placementTests(Expectations& expectations) {
    const auto load = [&](const std::string_view text, const subtitles::Format format,
                          const std::string& label) -> std::optional<document::CueListValue> {
        subtitles::LoadOptions options;
        options.format = format;
        auto loaded = subtitles::load(text, options);
        auto* result = std::get_if<subtitles::LoadResult>(&loaded);
        expectations.expect(result != nullptr, label + ": the file loads");
        if (result == nullptr)
            return std::nullopt;
        return std::move(result->cues);
    };
    const auto layoutFor = [&](const document::CueListValue& list,
                               const std::string& label) -> Layouts {
        auto made = fixture(list);
        made->set("safeArea", std::int64_t{document::kSubtitlesSafeFullFrame});
        const auto plan = compile(expectations, *made, label);
        const auto* node = plan ? subtitlesOf(*plan) : nullptr;
        if (node == nullptr)
            return {};
        std::vector<std::size_t> indices(list.cues.size());
        for (std::size_t index = 0; index < indices.size(); ++index)
            indices[index] = index;
        return layoutOf(expectations, *node, *node->cues.list, indices, label);
    };

    // WebVTT: line 10 % aligned at its start, position 20 % with start alignment: the block's
    // top left is at (0.2, 0.1) of the frame. `line:0` is the top line.
    if (const auto vtt = load("WEBVTT\n\n00:00.000 --> 00:02.000 line:10% position:20% "
                              "align:start\nPlaced\n\n00:00.000 --> 00:02.000 line:0\nTop\n",
                              subtitles::Format::WebVtt, "WebVTT")) {
        const auto laid = layoutFor(*vtt, "WebVTT");
        expectations.expect(laid.size() == 2 && near(laid[0].x, 0.2 * kWidth) &&
                                near(laid[0].y, 0.1 * kHeight),
                            "a WebVTT line and position place the block's top left");
        expectations.expect(laid.size() == 2 && laid[1].y < kHeight / 4.0,
                            "WebVTT line 0 is at the top");
    }
    // ASS: \pos(960,100) on a top-center style at 1920x1080 is the block's top center at
    // (0.5, 100/1080) of the frame.
    if (const auto ass =
            load("[Script Info]\nScriptType: v4.00+\nPlayResX: 1920\nPlayResY: 1080\n\n"
                 "[V4+ Styles]\nFormat: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, "
                 "OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, "
                 "Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, "
                 "MarginV, Encoding\nStyle: Sign,Arial,54,&H0000FFFF,&H000000FF,&H00000000,"
                 "&H00000000,0,0,0,0,100,100,0,0,1,2,0,8,10,10,10,1\n\n[Events]\nFormat: Layer, "
                 "Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"
                 "Dialogue: 0,0:00:00.00,0:00:02.00,Sign,,0,0,0,,{\\pos(960,100)}Sign\n",
                 subtitles::Format::Ass, "ASS")) {
        const auto laid = layoutFor(*ass, "ASS");
        expectations.expect(laid.size() == 1 &&
                                near(laid[0].x + laid[0].block.width / 2.0, kWidth / 2.0) &&
                                near(laid[0].y, 100.0 / 1080.0 * kHeight),
                            "an ASS \\pos places the style's alignment point");
        expectations.expect(laid.size() == 1 && near(laid[0].options.verticalSize, 54.0 / 1080.0 *
                                                                                       kHeight),
                            "an ASS font size is a fraction of the script's height");
    }
    // TTML: a region at origin (10 %, 70 %), extent (80 %, 20 %), aligned after and centered:
    // the block's bottom center is at (0.5, 0.9) and it wraps within 80 % of the width.
    if (const auto ttml = load(
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<tt xmlns=\"http://www.w3.org/ns/ttml\" "
            "xmlns:tts=\"http://www.w3.org/ns/ttml#styling\" xml:lang=\"en\"><head><layout>"
            "<region xml:id=\"low\" tts:origin=\"10% 70%\" tts:extent=\"80% 20%\" "
            "tts:displayAlign=\"after\" tts:textAlign=\"center\"/></layout></head><body><div>"
            "<p begin=\"0s\" end=\"2s\" region=\"low\">Region</p></div></body></tt>\n",
            subtitles::Format::Ttml, "TTML")) {
        const auto laid = layoutFor(*ttml, "TTML");
        expectations.expect(laid.size() == 1 &&
                                near(laid[0].x + laid[0].block.width / 2.0, kWidth / 2.0) &&
                                near(laid[0].y + laid[0].block.height, 0.9 * kHeight) &&
                                near(laid[0].options.maxWidth, 0.8 * kWidth),
                            "a TTML region places and wraps the cue");
    }
}

// The node's value outputs over a cue's life.
void valueOutputTests(Expectations& expectations) {
    const auto list = listOf({cue(1, seconds(1), seconds(3), "Hello <i>there</i>", "Ann"),
                              cue(2, seconds(2), seconds(4), "Second")});
    const runtime::CompiledValueOperation operation{
        document::NodeId::fromRaw(7), runtime::ValueOutputIndex::fromRaw(0), 5,
        runtime::CompiledValueSubtitles{
            runtime::CompiledValueOperand{document::ParameterId::fromRaw(8),
                                          runtime::CompiledValue{runtime::CompiledCueList{list}}},
            true}};
    const std::vector operations{operation};
    const auto at = [&](const core::RationalTime time) {
        return runtime::evaluateValueGraph(operations, 5, time, *document::FrameRate::create(24, 1))
            .outputs;
    };
    const auto before = at(seconds(1, 2));
    expectations.expect(std::get<std::string>(before[2]).empty() &&
                            std::get<std::int64_t>(before[3]) == -1 &&
                            std::get<double>(before[4]) == 0.0,
                        "no cue: empty text, index -1, progress 0");
    const auto first = at(seconds(3, 2));
    expectations.expect(std::get<std::string>(first[2]) == "Ann: Hello there" &&
                            std::get<std::int64_t>(first[3]) == 0 &&
                            near(std::get<double>(first[4]), 0.25),
                        "one cue: its plain text with the speaker, its index and progress");
    const auto both = at(seconds(5, 2));
    expectations.expect(std::get<std::string>(both[2]) == "Ann: Hello there\nSecond" &&
                            std::get<std::int64_t>(both[3]) == 0,
                        "two cues: their text joined by a line feed, the first one's index");
    const auto& cues = std::get<runtime::CompiledCueList>(both[1]);
    expectations.expect(cues == runtime::CompiledCueList{list}, "the cues output is the list");
}

// Pixels over a few frames, the cache identity, the budgets and cancellation on the CPU.
void cpuTests(Expectations& expectations, std::vector<std::pair<std::string, Reference>>& gpu) {
    auto made = fixture(listOf({cue(1, seconds(0), seconds(2), "Hello <i>world</i>"),
                                cue(2, seconds(3), seconds(5), "<c.yellow>Second</c> cue")}));
    made->set("size", 32.0);
    const auto inCue = referenceAt(expectations, *made, seconds(1), "in a cue");
    const auto later = referenceAt(expectations, *made, seconds(3, 2), "later in the cue");
    const auto between = referenceAt(expectations, *made, seconds(5, 2), "between cues");
    const auto second = referenceAt(expectations, *made, seconds(4), "the second cue");
    if (!inCue || !later || !between || !second)
        return;
    const auto covered = coverage(*inCue);
    // Title safe (80 %): the cue's block stands on the safe area's bottom edge, centered.
    expectations.expect(!covered.empty() && covered.bottom <= 260 && covered.top > kHeight / 2 &&
                            std::abs((covered.left + covered.right) / 2 -
                                     static_cast<std::int64_t>(kWidth / 2)) <= 8,
                        "a cue draws centered on the title-safe bottom edge");
    expectations.expect(covered.left >= kWidth / 10 && covered.right <= kWidth * 9 / 10,
                        "the cue stays inside the safe area");
    expectations.expect(samePixels(*inCue, *later),
                        "two frames of one cue draw the same pixels");
    expectations.expect(coverage(*between).empty(), "nothing draws between cues");
    expectations.expect(!coverage(*second).empty() && !samePixels(*inCue, *second),
                        "the second cue draws differently");
    // The yellow class: some covered pixel of the second cue has more red than blue.
    bool yellow = false;
    const auto area = coverage(*second);
    for (auto y = area.top; y < area.bottom && !yellow; ++y)
        for (auto x = area.left; x < area.right && !yellow; ++x) {
            const auto pixel = pixelAt(*second, x, y);
            yellow = pixel.alpha() > 0.9F && pixel.red() > 0.5F && pixel.blue() < 0.2F;
        }
    expectations.expect(yellow, "a WebVTT color class colors its run");
    gpu.emplace_back("full frame", *inCue);
    gpu.emplace_back("second cue", *second);
    gpu.emplace_back("empty frame", *between);
    if (auto roi = referenceAt(expectations, *made, seconds(1), "ROI",
                               windowOf(160, 200, 200, 60)))
        gpu.emplace_back("ROI", *roi);
    if (auto proxy =
            referenceAt(expectations, *made, seconds(1), "proxy", std::nullopt,
                        runtime::ProxyResolution{
                            halfExtent()})) {
        const auto half = coverage(*proxy);
        expectations.expect(!half.empty() && half.bottom <= 131, "a half-size frame draws the cue "
                                                                 "at half size");
        gpu.emplace_back("proxy", *proxy);
    }

    // The cache identity: the frame inside one cue evaluates the node once.
    {
        const auto plan = inCue->plan;
        const runtime::CpuCompositionEvaluator evaluator;
        const auto evaluated = [&](const core::RationalTime time) {
            auto request = inCue->request;
            request.time = time;
            runtime::OperationCacheStatistics statistics;
            const auto result = evaluator.evaluate(plan, request, {}, {}, nullptr, &statistics);
            expectations.expect(result.frame() != nullptr, "the cached evaluation evaluates");
            return std::ranges::count(statistics.evaluatedNodes, made->node);
        };
        // The node's value outputs (text, index, progress) are evaluated on every frame; its
        // image only when the cues on screen change.
        expectations.expect(evaluated(seconds(1, 2)) == 2,
                            "the first frame of a cue evaluates its image and its values");
        expectations.expect(evaluated(seconds(3, 2)) == 1,
                            "a later frame of the same cue reuses the image");
        expectations.expect(evaluated(seconds(4)) == 2,
                            "a frame of another cue evaluates the image again");
    }

    // A box: Block draws behind the text, wider than the ink.
    made->set("box", std::int64_t{document::kSubtitlesBoxBlock});
    if (const auto boxed = referenceAt(expectations, *made, seconds(1), "box")) {
        const auto box = coverage(*boxed);
        expectations.expect(box.left < covered.left && box.right > covered.right &&
                                box.top < covered.top && box.bottom > covered.bottom,
                            "a Block box extends past the text's ink");
        gpu.emplace_back("box", *boxed);
    }
    made->set("fadeIn", 0.5);
    if (const auto fading = referenceAt(expectations, *made, seconds(1, 4), "fade")) {
        gpu.emplace_back("fade", *fading);
        float most = 0.0F;
        const auto faded = coverage(*fading);
        for (auto y = faded.top; y < faded.bottom; ++y)
            for (auto x = faded.left; x < faded.right; ++x)
                most = std::max(most, alphaAt(*fading, x, y));
        // Every layer (box, outline, fill) draws at half its alpha, so no pixel is opaque; the
        // most is where the half-opaque fill, outline and box cover one another.
        const double expectedMost = 1.0 - 0.5 * 0.5 * (1.0 - 0.6 * 0.5);
        expectations.expect(!faded.empty() && most > 0.5F &&
                                static_cast<double>(most) <= expectedMost + 1e-5,
                            "half way through the fade in every layer draws at half opacity");
    }
    made->set("burnIn", false);
    if (const auto off = referenceAt(expectations, *made, seconds(1), "burn in off"))
        expectations.expect(coverage(*off).empty(), "Burn in off draws nothing");

    // Cancellation: a cancelled evaluation publishes nothing.
    {
        const auto result = runtime::CpuCompositionEvaluator{}.evaluate(
            inCue->plan, inCue->request, makeCancelledToken());
        expectations.expect(result.status() == runtime::EvaluationStatus::Cancelled &&
                                result.frame() == nullptr,
                            "a cancelled evaluation is cancelled");
        const auto prepared =
            runtime::CpuGpuSceneBuilder{}.build(inCue->plan, inCue->request, makeCancelledToken());
        expectations.expect(!prepared.hasValue() && prepared.diagnostic.code ==
                                                        runtime::PreparedGpuSceneDiagnosticCode::
                                                            Cancelled,
                            "a cancelled GPU preparation is cancelled");
    }
}

// Pixels a picture covers fully, and pixels on its antialiased edge.
struct EdgeCount final {
    std::size_t solid = 0;
    std::size_t partial = 0;
};
[[nodiscard]] EdgeCount edgeCount(const Reference& reference) {
    EdgeCount count;
    for (const auto& pixel : reference.frame->processImage().pixels()) {
        if (pixel.alpha() >= 1.0F)
            ++count.solid;
        else if (pixel.alpha() > 0.0F)
            ++count.partial;
    }
    return count;
}

// #752: a scaled or a rotated Subtitles layer redraws the cue's paths through the layer matrix
// (the vector leaf), so it stays as crisp as the unscaled render.
void transformTests(Expectations& expectations,
                    std::vector<std::pair<std::string, Reference>>& gpu) {
    auto made = fixture(listOf({cue(1, seconds(0), seconds(2), "Hello <i>world</i>")}));
    made->set("size", 32.0);
    // Centered on the frame, so the layer's pivot (the frame's center) keeps it in view.
    made->set("anchor", std::int64_t{4});
    const auto layer = layerNode(made->document, made->main, lastLayer(made->document, made->main));
    const auto setLayer = [&](const std::string_view role, document::ParameterValue value) {
        execute<commands::SetParameterSource>(made->document, made->commands, made->main,
                                              parameter(made->document, made->main, layer, role),
                                              document::ConstantValueSource{std::move(value)});
    };
    const auto plain = referenceAt(expectations, *made, seconds(1), "unscaled layer");
    setLayer("scale", document::Vec2d{2.0, 2.0});
    const auto scaled = referenceAt(expectations, *made, seconds(1), "scaled layer");
    setLayer("opacity", 0.5);
    const auto faded = referenceAt(expectations, *made, seconds(1), "scaled layer, half opacity");
    setLayer("opacity", 1.0);
    setLayer("scale", document::Vec2d{1.0, 1.0});
    setLayer("rotation", 30.0);
    const auto rotated = referenceAt(expectations, *made, seconds(1), "rotated layer");
    // One layer only (no outline, one paint): its coverage command carries the opacity itself.
    setLayer("rotation", 0.0);
    setLayer("scale", document::Vec2d{2.0, 2.0});
    setLayer("opacity", 0.5);
    made->set("outlineWidth", 0.0);
    const auto single =
        referenceAt(expectations, *made, seconds(1), "scaled layer, fill only, half opacity");
    if (!plain || !scaled || !faded || !single || !rotated)
        return;
    // A cancelled redraw publishes nothing, on either route.
    {
        const auto result = runtime::CpuCompositionEvaluator{}.evaluate(
            rotated->plan, rotated->request, makeCancelledToken());
        expectations.expect(result.status() == runtime::EvaluationStatus::Cancelled &&
                                result.frame() == nullptr,
                            "a cancelled evaluation of a rotated layer is cancelled");
        const auto prepared = runtime::CpuGpuSceneBuilder{}.build(rotated->plan, rotated->request,
                                                                  makeCancelledToken());
        expectations.expect(!prepared.hasValue() && prepared.diagnostic.code ==
                                                        runtime::PreparedGpuSceneDiagnosticCode::
                                                            Cancelled,
                            "a cancelled GPU preparation of a rotated layer is cancelled");
    }
    const auto base = edgeCount(*plain);
    const auto large = edgeCount(*scaled);
    const auto turned = edgeCount(*rotated);
    std::cout << "edges: unscaled solid=" << base.solid << " partial=" << base.partial
              << "; scaled solid=" << large.solid << " partial=" << large.partial
              << "; rotated solid=" << turned.solid << " partial=" << turned.partial << '\n';
    const auto ratio = [](const std::size_t a, const std::size_t b) {
        return b == 0 ? 0.0 : static_cast<double>(a) / static_cast<double>(b);
    };
    expectations.expect(base.solid > 500 && base.partial > 100, "the unscaled cue draws");
    expectations.expect(ratio(large.solid, base.solid) > 3.6 &&
                            ratio(large.solid, base.solid) < 4.4,
                        "a layer scaled two times covers four times the solid area");
    expectations.expect(ratio(large.partial, base.partial) < 2.6,
                        "a layer scaled two times keeps a one-pixel antialiased edge: it is "
                        "redrawn, not resampled");
    expectations.expect(ratio(turned.solid, base.solid) > 0.9 &&
                            ratio(turned.solid, base.solid) < 1.1 &&
                            ratio(turned.partial, base.partial) < 1.4,
                        "a rotated layer keeps the unscaled solid area and edge: it is redrawn, "
                        "not resampled");
    gpu.emplace_back("scaled layer", *scaled);
    gpu.emplace_back("scaled layer, half opacity", *faded);
    gpu.emplace_back("scaled layer, fill only, half opacity", *single);
    gpu.emplace_back("rotated layer", *rotated);
}

void budgetTests(Expectations& expectations) {
    const auto refusal = [&](const std::shared_ptr<const runtime::CompiledCompositionPlan>& plan,
                             const std::string& label) {
        if (!plan)
            return std::string{};
        auto request = requestFor(*plan);
        request.time = seconds(1, 2);
        request.bypassOperationCache = true;
        const auto result = runtime::CpuCompositionEvaluator{}.evaluate(plan, request, {});
        expectations.expect(result.status() == runtime::EvaluationStatus::Failed,
                            label + ": the frame fails");
        const auto prepared = runtime::CpuGpuSceneBuilder{}.build(plan, request);
        expectations.expect(!prepared.hasValue(), label + ": the GPU preparation fails too");
        std::string message;
        for (const auto& diagnostic : result.diagnostics())
            message += diagnostic.summary + ": " + diagnostic.detail + "\n";
        expectations.expect(prepared.hasValue() ||
                                message.find(prepared.diagnostic.message) != std::string::npos,
                            label + ": both routes give the same reason");
        return message;
    };
    // An edit never leaves 17 cues on screen at once, but a list another node supplies is not
    // checked at the edit; the frame refuses it. The compiled list is swapped for such a list.
    {
        const auto made = fixture(listOf({cue(1, seconds(0), seconds(1), "One")}));
        const auto plan = compile(expectations, *made, "17 cues");
        if (plan) {
            std::vector<document::CueHandle> many;
            for (std::uint64_t id = 1; id <= 17; ++id)
                many.push_back(cue(id, seconds(0), seconds(1), "Cue " + std::to_string(id)));
            auto definition = plan->copyDefinition();
            for (auto& operation : definition.operations)
                if (auto* subtitles = std::get_if<runtime::CompiledSubtitles>(&operation))
                    subtitles->cues = runtime::CompiledCueList{listOf(std::move(many))};
            const auto crowded = refusal(
                std::make_shared<const runtime::CompiledCompositionPlan>(std::move(definition)),
                "17 cues");
            expectations.expect(
                crowded.find("At 00:00:00.500 17 cues show at once (cues 1, 2,") !=
                        std::string::npos &&
                    crowded.find("at most 16") != std::string::npos,
                "17 cues at once are refused, naming the time and the cues: " + crowded);
        }
    }
    // Four cues of 2,700 letters each draw more than 8,192 glyphs.
    std::string longText;
    for (int word = 0; word < 300; ++word)
        longText += "abcdefghi ";
    longText.pop_back();
    std::vector<document::CueHandle> wordyCues;
    for (std::uint64_t id = 1; id <= 4; ++id)
        wordyCues.push_back(cue(id, seconds(0), seconds(1), longText));
    const auto wordy = fixture(listOf(std::move(wordyCues)));
    wordy->set("overflow", std::int64_t{document::kSubtitlesOverflowShowAll});
    wordy->set("wrap", std::int64_t{document::kSubtitlesWrapGreedy});
    const auto glyphs = refusal(compile(expectations, *wordy, "glyphs"), "glyphs");
    expectations.expect(glyphs.find("At 00:00:00.500 the subtitles draw ") != std::string::npos &&
                            glyphs.find("glyphs (cues 1, 2, 3, 4); at most 8192") !=
                                std::string::npos,
                        "more than 8,192 glyphs are refused, naming the time and the cues: " +
                            glyphs);
}

void checkTests(Expectations& expectations) {
    // 24 fps; the composition is 10 s long.
    const auto list =
        listOf({cue(1, seconds(0), seconds(1, 2), "Short"),
                cue(2, seconds(13, 24), seconds(3),
                    "A line that is far too long for anyone to read comfortably"),
                cue(3, seconds(4), seconds(6),
                    "\xd9\x85\xd8\xb1\xd8\xad\xd8\xa8\xd8\xa7"), // Arabic
                cue(4, seconds(5), seconds(11), "Overlaps and runs past the end")});
    auto made = fixture(list);
    made->set("maxCharactersPerLine", std::int64_t{20});
    const runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
    const auto compiled = compiler.compile({made->document.snapshot(), made->main}, {});
    expectations.expect(compiled.plan != nullptr, "checks: compiles");
    if (!compiled.plan)
        return;
    bool warned = false;
    for (const auto& diagnostic : compiled.diagnostics)
        warned = warned ||
                 (diagnostic.code == runtime::CompileDiagnosticCode::UnshapedScript &&
                  diagnostic.summary == "Cue 3 contains Arabic text; Bloom lays text out without "
                                        "shaping, so it will not read correctly");
    expectations.expect(warned, "the compile warns of the Arabic cue by number");
    const auto* node = subtitlesOf(*compiled.plan);
    if (node == nullptr)
        return;
    const runtime::SubtitlesCheckSettings settings{
        .maxCharactersPerLine = node->settings.maxCharactersPerLine,
        .maxReadingSpeed = node->settings.maxReadingSpeed,
        .minDuration = node->settings.minDuration,
        .minGapFrames = node->settings.minGap,
        .frameRate = *document::FrameRate::create(24, 1),
        .compositionDuration = core::RationalTime::fromInteger(10)};
    const auto checks = runtime::checkSubtitleCues(*node, *node->cues.list,
                                                   runtime::subtitlesConstantStyle(*node), settings);
    const auto has = [&](const std::size_t index, const std::string_view code) {
        return index < checks.size() &&
               std::ranges::any_of(checks[index],
                                   [&](const auto& check) { return check.code == code; });
    };
    expectations.expect(checks.size() == 4, "one set of checks per cue");
    expectations.expect(has(0, "duration") && has(0, "gap"),
                        "a half-second cue one frame before the next is too short and too close");
    expectations.expect(has(1, "characters-per-line") && has(1, "reading-speed") &&
                            !has(1, "gap"),
                        "a long fast cue has too many characters per line and reads too fast");
    expectations.expect(has(2, "unshaped-script") && has(2, "overlap"),
                        "the Arabic cue is flagged and overlaps the next");
    expectations.expect(has(3, "overlap") && has(3, "outside-composition"),
                        "the last cue overlaps and runs past the composition");
    expectations.expect(!has(0, "unshaped-script") && !has(3, "unshaped-script"),
                        "Latin text is not flagged for shaping");
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
        std::vector<std::pair<std::string, Reference>> references;
        layoutTests(expectations);
        placementTests(expectations);
        valueOutputTests(expectations);
        cpuTests(expectations, references);
        const auto transformedFrom = references.size();
        transformTests(expectations, references);
        budgetTests(expectations);
        checkTests(expectations);
        if (expectations.failures() != 0) {
            std::cerr << expectations.failures() << " subtitles expectation(s) failed\n";
            return 1;
        }

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
        for (const auto& [label, reference] : references)
            gpuParity(expectations, *executor.executor, reference, label);
        // A later frame of the same cue prepares the same scene: it dispatches nothing new.
        if (!references.empty()) {
            auto later = references.front().second;
            later.request.time = seconds(3, 2);
            const auto prepared = runtime::CpuGpuSceneBuilder{}.build(later.plan, later.request);
            expectations.expect(prepared.hasValue(), "a later frame of the cue prepares");
            if (prepared) {
                const auto before = executor.executor->counters();
                const auto pixels = runScene(*executor.executor, prepared.scene);
                expectations.expect(
                    parity(pixels, references.front().second.frame->processImage().pixels()) &&
                        executor.executor->counters().dispatches == before.dispatches,
                    "a later frame of the same cue matches and dispatches nothing new");
            }
        }
        // A later frame of the same cue on a scaled or rotated layer redraws nothing either.
        for (auto index = transformedFrom; index < references.size(); ++index) {
            auto later = references[index].second;
            later.request.time = seconds(3, 2);
            const auto prepared = runtime::CpuGpuSceneBuilder{}.build(later.plan, later.request);
            expectations.expect(prepared.hasValue(),
                                references[index].first + ": a later frame of the cue prepares");
            if (!prepared)
                continue;
            const auto before = executor.executor->counters();
            const auto pixels = runScene(*executor.executor, prepared.scene);
            expectations.expect(
                parity(pixels, references[index].second.frame->processImage().pixels()) &&
                    executor.executor->counters().dispatches == before.dispatches,
                references[index].first +
                    ": a later frame of the same cue matches and dispatches nothing new");
        }
        if (expectations.failures() != 0) {
            std::cerr << expectations.failures() << " subtitles expectation(s) failed\n";
            return 1;
        }
        std::cout << "PASS: Subtitles lay out, render, cache, refuse and check alike on the GPU and "
                     "the CPU reference\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected test exception: " << exception.what() << '\n';
        return 1;
    }
}
