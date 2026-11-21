// #757: a Text Source's styled runs, outline and background box through real documents
// (docs/architecture/evaluation-primitives.md, "Styled Text Source").
//
// A 512x288 composition gets a Text layer through AddTextLayer and is compiled by the production
// SnapshotCompiler. The CPU half always runs:
//
// - a new text at the styling defaults lowers with no style, so it takes the plain Text path
//   (its pixels and identity are the ones the text goldens hold);
// - markup: two-color runs through an accent and a literal color, bold, the synthesized oblique,
//   underline, an unknown tag drawn as written with its compile warning, and Markup off drawing the
//   tags as text;
// - the outline: drawn outside the glyphs in its color, the three joins differ, the content bounds
//   grow by it;
// - the background box per line and as a block, its corner radius, and the bounds including it;
// - an animated accent and an animated outline width over a few frames;
// - the cache identity: an accent no run uses keeps the frame's identity, a used one changes it;
// - Text Outline reads the glyphs of the styled layout (the tags left out);
// - the budget refusal and cancellation;
// - save and reopen draw the same pixels.
//
// Then every state is prepared by the production CpuGpuSceneBuilder, run by the production
// GpuSceneExecutor, and must match the CPU reference (2e-6) full frame, over an ROI, at a reduced
// render scale and on a scaled and rotated layer, with a warm rerun that dispatches nothing.
// Without a device the GPU half reports an explicit SKIP (exit 77) after the CPU half passed;
// --require-device makes that a failure.

#include "document_render_parity_support.hpp"
#include "text_styled_frame.hpp"

#include <bloom/commands/animation_operations.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/core/color.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/parameter_activity.hpp>
#include <bloom/document/text_styling.hpp>
#include <bloom/document/value_nodes.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/runtime/text_markup.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
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

[[nodiscard]] core::RationalTime frameTime(const std::int64_t frame) {
    return *core::RationalTime::create(frame, 24);
}

struct Fixture final {
    document::Document document;
    commands::CommandStack commands;
    document::CompositionId main;
    document::NodeId node;

    explicit Fixture(document::NewProject initial)
        : document(std::move(initial.project)), commands(document),
          main(initial.initialCompositionId) {}

    [[nodiscard]] document::ParameterId id(const std::string_view role) const {
        return parameter(document, main, node, role);
    }
    void set(const std::string_view role, document::ParameterValue value) {
        try {
            execute<commands::SetParameterSource>(document, commands, main, id(role),
                                                  document::ConstantValueSource{std::move(value)});
        } catch (const std::runtime_error& error) {
            throw std::runtime_error("setting " + std::string(role) +
                                     " was refused: " + error.what());
        }
    }
    // Keys a parameter: `first` at frame 0, `last` at frame 10.
    template <typename Insert, typename Value>
    void key(const std::string_view role, const Value& first, const Value& last) {
        set(role, first);
        execute<commands::CreateAnimationForParameter>(document, commands, main, id(role),
                                                       frameTime(0));
        const auto* record = composition(document, main).parameters().find(id(role));
        const auto* curve = record == nullptr
                                ? nullptr
                                : std::get_if<document::AnimationCurveSource>(&record->source);
        if (curve == nullptr)
            throw std::runtime_error("keying " + std::string(role) + " made no curve");
        execute<Insert>(document, commands, main, curve->curveId, frameTime(10), last);
    }
};

[[nodiscard]] std::unique_ptr<Fixture> fixture(std::string text, const double size = 48.0) {
    auto made = std::make_unique<Fixture>(document::makeNewProject(
        "Styled text", "Main", core::RationalTime::fromInteger(10), format()));
    // Left-aligned from x = 24, so every line used here stays inside the frame.
    execute<commands::AddTextLayer>(
        made->document, made->commands, made->main, "Text", std::move(text),
        document::Vec2d{24.0, static_cast<double>(kHeight) / 2.0}, 1.0, size);
    made->node = lastNode(made->document, made->main, document::kTextSourceNodeType);
    return made;
}

[[nodiscard]] std::shared_ptr<const runtime::CompiledCompositionPlan>
compile(Expectations& expectations, const Fixture& fixture, const std::string& label,
        std::vector<runtime::CompileDiagnostic>* diagnostics = nullptr) {
    const runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
    const auto compiled = compiler.compile({fixture.document.snapshot(), fixture.main}, {});
    expectations.expect(compiled.plan != nullptr,
                        label + ": compiles: " +
                            runtime::describeCompileFailure(
                                compiled, fixture.document.snapshot().project(), fixture.main));
    if (diagnostics != nullptr)
        *diagnostics = compiled.diagnostics;
    return compiled.plan;
}

[[nodiscard]] const runtime::CompiledText* textOf(const runtime::CompiledCompositionPlan& plan) {
    for (const auto& operation : plan.operations())
        if (const auto* found = std::get_if<runtime::CompiledText>(&operation))
            return found;
    return nullptr;
}

[[nodiscard]] std::size_t textIndexOf(const runtime::CompiledCompositionPlan& plan) {
    for (std::size_t index = 0; index < plan.operations().size(); ++index)
        if (std::holds_alternative<runtime::CompiledText>(plan.operations()[index]))
            return index;
    return 0;
}

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

// Opaque pixels of `reference` matching `test`.
template <typename Test>
[[nodiscard]] std::size_t countPixels(const Reference& reference, Test test) {
    std::size_t count = 0;
    for (const auto& pixel : reference.frame->processImage().pixels())
        if (test(pixel))
            ++count;
    return count;
}

[[nodiscard]] bool reddish(const render::Rgba32f& p) {
    return p.alpha() > 0.95F && p.red() > 0.6F && p.green() < 0.2F && p.blue() < 0.2F;
}
[[nodiscard]] bool greenish(const render::Rgba32f& p) {
    return p.alpha() > 0.95F && p.green() > 0.6F && p.red() < 0.2F && p.blue() < 0.2F;
}
[[nodiscard]] bool whitish(const render::Rgba32f& p) {
    return p.alpha() > 0.95F && p.red() > 0.9F && p.green() > 0.9F && p.blue() > 0.9F;
}
[[nodiscard]] bool bluish(const render::Rgba32f& p) {
    return p.alpha() > 0.95F && p.blue() > 0.6F && p.red() < 0.2F && p.green() < 0.2F;
}

[[nodiscard]] render::ImageWindow windowOf(const std::int64_t x, const std::int64_t y,
                                           const std::uint64_t width, const std::uint64_t height) {
    auto made = render::ImageWindow::create(x, y, width, height);
    return *made.value();
}

[[nodiscard]] render::ImageExtent extentOf(const std::uint64_t width, const std::uint64_t height) {
    auto made = render::ImageExtent::create(width, height);
    return *made.value();
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
        TaskRequest("styled text cancel token",
                    {.kind = TaskOwnerKind::Composition, .id = TaskOwnerId::fromRaw(757)}),
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

// The content bounds the CPU reference published for the text leaf.
[[nodiscard]] runtime::ContentBounds textBounds(const Reference& reference) {
    return reference.frame->evaluatedBounds()[textIndexOf(*reference.plan)].local;
}

void defaultsTests(Expectations& expectations) {
    auto made = fixture("Plain");
    const auto plan = compile(expectations, *made, "defaults");
    const auto* text = plan ? textOf(*plan) : nullptr;
    expectations.expect(text != nullptr && !text->style,
                        "a new text at the styling defaults lowers unstyled (the plain Text path)");
    made->set("markup", true);
    const auto marked = compile(expectations, *made, "markup on");
    const auto* styled = marked ? textOf(*marked) : nullptr;
    expectations.expect(styled != nullptr && styled->style && styled->style->markup,
                        "Markup on lowers a styled text");
    made->set("markup", false);
    made->set("outlineWidth", 2.0);
    const auto outlined = compile(expectations, *made, "outline");
    expectations.expect(outlined && textOf(*outlined)->style,
                        "an outline width lowers a styled text");
    made->set("outlineWidth", 0.0);
    made->set("background", std::int64_t{document::kTextBackgroundPerLine});
    const auto boxed = compile(expectations, *made, "background");
    expectations.expect(boxed && textOf(*boxed)->style, "a background lowers a styled text");
}

void markupTests(Expectations& expectations, std::vector<std::pair<std::string, Reference>>& gpu) {
    auto made = fixture("Hello <c.accent1>world</c> <c.#00FF00>go</c>");
    made->set("accent1", core::Color4d{1.0, 0.0, 0.0, 1.0});
    const auto literal = referenceAt(expectations, *made, frameTime(0), "markup off");
    made->set("markup", true);
    const auto runs = referenceAt(expectations, *made, frameTime(0), "two-color runs");
    if (!literal || !runs)
        return;
    expectations.expect(countPixels(*runs, reddish) > 100,
                        "an accent span draws in its accent color");
    expectations.expect(countPixels(*runs, greenish) > 50,
                        "a literal color span draws in its color (sRGB #00FF00)");
    expectations.expect(countPixels(*runs, whitish) > 100,
                        "the text outside the spans draws in the node's color");
    expectations.expect(countPixels(*literal, reddish) == 0 &&
                            coverage(*literal).right - coverage(*literal).left >
                                coverage(*runs).right - coverage(*runs).left,
                        "with Markup off the tags draw as literal text in one color");
    gpu.emplace_back("two-color runs", *runs);
    if (auto roi =
            referenceAt(expectations, *made, frameTime(0), "runs ROI", windowOf(200, 110, 160, 60)))
        gpu.emplace_back("runs ROI", *roi);
    if (auto proxy = referenceAt(expectations, *made, frameTime(0), "runs proxy", std::nullopt,
                                 runtime::ProxyResolution{extentOf(256, 144)})) {
        const auto half = coverage(*proxy);
        const auto full = coverage(*runs);
        expectations.expect(
            !half.empty() && std::abs((half.right - half.left) * 2 - (full.right - full.left)) <= 4,
            "a half-size frame draws the runs at half size");
        gpu.emplace_back("runs proxy", *proxy);
    }

    // Bold, italic (the synthesized oblique of the built-in face) and underline.
    made->set("text", std::string("Lift"));
    const auto upright = referenceAt(expectations, *made, frameTime(0), "upright");
    made->set("text", std::string("<b>Lift</b>"));
    const auto bold = referenceAt(expectations, *made, frameTime(0), "bold");
    made->set("text", std::string("<i>Lift</i>"));
    const auto italic = referenceAt(expectations, *made, frameTime(0), "italic");
    made->set("text", std::string("<u>Lift</u>"));
    const auto underline = referenceAt(expectations, *made, frameTime(0), "underline");
    if (upright && bold && italic && underline) {
        const auto plain = coverage(*upright);
        const auto slanted = coverage(*italic);
        // The built-in face has no bold: bold runs draw at the regular weight.
        expectations.expect(samePixels(*upright, *bold),
                            "a font with no bold draws bold runs at the regular weight");
        expectations.expect(!samePixels(*upright, *italic) && slanted.right > plain.right,
                            "an italic run with no italic face is the slanted oblique");
        expectations.expect(coverage(*underline).bottom > plain.bottom + 2,
                            "an underlined run draws its underline below the baseline");
        gpu.emplace_back("italic oblique", *italic);
        gpu.emplace_back("underline", *underline);
    }
    // A bold run with no bold face carries the font-missing warning, as in Subtitles.
    {
        made->set("text", std::string("<b>Lift</b>"));
        std::vector<runtime::CompileDiagnostic> boldDiagnostics;
        static_cast<void>(compile(expectations, *made, "bold diagnostics", &boldDiagnostics));
        expectations.expect(
            std::ranges::any_of(boldDiagnostics,
                                [](const auto& diagnostic) {
                                    return diagnostic.code ==
                                               runtime::CompileDiagnosticCode::FontMissing &&
                                           diagnostic.detail.find("bold") != std::string::npos;
                                }),
            "a bold run with no bold face warns that the face is missing");
    }

    // An unknown tag is drawn as written, with a warning on the node.
    made->set("text", std::string("a <x>tag</x>"));
    std::vector<runtime::CompileDiagnostic> diagnostics;
    const auto warned = compile(expectations, *made, "unknown tag", &diagnostics);
    const bool warning = std::ranges::any_of(diagnostics, [](const auto& diagnostic) {
        return diagnostic.code == runtime::CompileDiagnosticCode::TextMarkupIgnored &&
               diagnostic.detail.find("unknown tag <x>") != std::string::npos;
    });
    expectations.expect(warning, "an unknown tag warns on the node, naming it");
    const auto faces = runtime::CompiledSubtitlesFaces{};
    const auto parsed = runtime::textMarkupRuns("a <x>tag</x> <b>b</b>", true, faces);
    std::u32string shown;
    for (const auto& run : parsed.runs)
        shown += run.text;
    expectations.expect(shown == U"a <x>tag</x> b" && parsed.usesBold,
                        "the unknown tags draw as written; the known one is read");
    static_cast<void>(warned);
}

void outlineTests(Expectations& expectations, std::vector<std::pair<std::string, Reference>>& gpu) {
    auto made = fixture("VAMW");
    const auto plain = referenceAt(expectations, *made, frameTime(0), "no outline");
    made->set("outlineColor", core::Color4d{0.0, 0.0, 1.0, 1.0});
    made->set("outlineWidth", 4.0);
    const auto round = referenceAt(expectations, *made, frameTime(0), "outline round");
    made->set("outlineJoin", std::int64_t{document::kTextOutlineJoinMiter});
    const auto miter = referenceAt(expectations, *made, frameTime(0), "outline miter");
    made->set("outlineJoin", std::int64_t{document::kTextOutlineJoinBevel});
    const auto bevel = referenceAt(expectations, *made, frameTime(0), "outline bevel");
    if (!plain || !round || !miter || !bevel)
        return;
    const auto inner = coverage(*plain);
    const auto outer = coverage(*round);
    expectations.expect(outer.left <= inner.left - 3 && outer.right >= inner.right + 3 &&
                            outer.top <= inner.top - 3 && outer.bottom >= inner.bottom + 3,
                        "the outline draws outside the glyphs by its width");
    expectations.expect(countPixels(*round, bluish) > 200 && countPixels(*round, whitish) > 200,
                        "the outline draws in its color under the fill");
    expectations.expect(!samePixels(*round, *miter) && !samePixels(*miter, *bevel) &&
                            !samePixels(*round, *bevel),
                        "round, miter and bevel joins draw differently");
    expectations.expect(countPixels(*miter, bluish) > countPixels(*bevel, bluish),
                        "a miter join covers its corners, a bevel cuts them");
    const auto innerBounds = textBounds(*plain);
    const auto outerBounds = textBounds(*round);
    expectations.expect(outerBounds.left <= innerBounds.left - 3.0 &&
                            outerBounds.right >= innerBounds.right + 3.0,
                        "the content bounds include the outline");
    gpu.emplace_back("outline round", *round);
    gpu.emplace_back("outline miter", *miter);
    gpu.emplace_back("outline bevel", *bevel);

    // A scaled and rotated layer redraws the outlined text through its matrix.
    const auto layer = layerNode(made->document, made->main, lastLayer(made->document, made->main));
    execute<commands::SetParameterSource>(made->document, made->commands, made->main,
                                          parameter(made->document, made->main, layer, "scale"),
                                          document::ConstantValueSource{document::Vec2d{1.5, 1.5}});
    execute<commands::SetParameterSource>(made->document, made->commands, made->main,
                                          parameter(made->document, made->main, layer, "rotation"),
                                          document::ConstantValueSource{12.0});
    if (const auto turned = referenceAt(expectations, *made, frameTime(0), "outline transformed"))
        gpu.emplace_back("outline, scaled and rotated layer", *turned);
}

void backgroundTests(Expectations& expectations,
                     std::vector<std::pair<std::string, Reference>>& gpu) {
    auto made = fixture("Short\nA longer line", 40.0);
    made->set("backgroundColor", core::Color4d{1.0, 0.0, 0.0, 1.0});
    const auto plain = referenceAt(expectations, *made, frameTime(0), "no background");
    made->set("background", std::int64_t{document::kTextBackgroundPerLine});
    const auto perLine = referenceAt(expectations, *made, frameTime(0), "per line");
    made->set("background", std::int64_t{document::kTextBackgroundBlock});
    const auto block = referenceAt(expectations, *made, frameTime(0), "block");
    made->set("backgroundRadius", 12.0);
    const auto rounded = referenceAt(expectations, *made, frameTime(0), "block rounded");
    if (!plain || !perLine || !block || !rounded)
        return;
    const auto ink = coverage(*plain);
    const auto boxed = coverage(*block);
    expectations.expect(boxed.left < ink.left && boxed.right > ink.right && boxed.top < ink.top &&
                            boxed.bottom > ink.bottom,
                        "the block box extends past the text by its padding");
    // The first line is shorter: per line, a row through it is covered over fewer columns than a
    // row through the second line; as a block both rows are covered alike.
    const auto rowWidth = [](const Reference& reference, const std::int64_t y) {
        std::int64_t count = 0;
        for (std::int64_t x = 0; x < kWidth; ++x)
            if (pixelAt(reference, x, y).alpha() > 0.99F)
                ++count;
        return count;
    };
    const auto lineStep = (boxed.bottom - boxed.top) / 2;
    const auto firstRow = boxed.top + lineStep / 3;
    const auto secondRow = boxed.bottom - lineStep / 3;
    expectations.expect(rowWidth(*perLine, firstRow) + 40 < rowWidth(*perLine, secondRow),
                        "a per-line box follows each line's width");
    expectations.expect(std::abs(rowWidth(*block, firstRow) - rowWidth(*block, secondRow)) <= 1,
                        "a block box covers every line alike");
    expectations.expect(pixelAt(*block, boxed.left, boxed.top).alpha() > 0.99F &&
                            pixelAt(*rounded, boxed.left, boxed.top).alpha() < 0.01F,
                        "the corner radius rounds the box's corners");
    expectations.expect(countPixels(*block, reddish) > 1000 && countPixels(*block, whitish) > 200,
                        "the box draws in its color under the text");
    const auto bounds = textBounds(*block);
    // The bounds are the layer's local coordinates and the coverage the frame's; at scale 1 the
    // widths and heights compare.
    expectations.expect(
        bounds.right - bounds.left >= static_cast<double>(boxed.right - boxed.left) - 1.0 &&
            bounds.bottom - bounds.top >= static_cast<double>(boxed.bottom - boxed.top) - 1.0,
        "the content bounds include the box");
    gpu.emplace_back("box per line", *perLine);
    gpu.emplace_back("box block", *block);
    gpu.emplace_back("box rounded", *rounded);

    // Overflow Clip in a text box cuts what draws past the box (and its margins).
    made->set("box", document::Vec2d{120.0, 60.0});
    made->set("wrap", true);
    if (const auto clipped = referenceAt(expectations, *made, frameTime(0), "box clipped")) {
        const auto area = coverage(*clipped);
        expectations.expect(!area.empty() && area.right - area.left <= 120 + 2 * 10 + 2,
                            "a clipped text box keeps the box within the box and its padding");
        gpu.emplace_back("text box clipped", *clipped);
    }
}

void animationTests(Expectations& expectations,
                    std::vector<std::pair<std::string, Reference>>& gpu) {
    auto made = fixture("<c.accent2>Glow</c> on");
    made->set("markup", true);
    made->key<commands::InsertColor4Keyframe>("accent2", core::Color4d{1.0, 0.0, 0.0, 1.0},
                                              core::Color4d{0.0, 0.0, 1.0, 1.0});
    made->key<commands::InsertScalarKeyframe>("outlineWidth", 0.5, 5.0);
    std::vector<Reference> frames;
    for (const auto frame : {0, 5, 10})
        if (auto reference = referenceAt(expectations, *made, frameTime(frame),
                                         "animated frame " + std::to_string(frame)))
            frames.push_back(std::move(*reference));
    if (frames.size() != 3)
        return;
    expectations.expect(countPixels(frames[0], reddish) > 100 &&
                            countPixels(frames[2], bluish) > 100,
                        "the animated accent turns the run from red to blue");
    const auto start = coverage(frames[0]);
    const auto middle = coverage(frames[1]);
    const auto end = coverage(frames[2]);
    expectations.expect(middle.right - middle.left > start.right - start.left + 3 &&
                            end.right - end.left > middle.right - middle.left + 3,
                        "the animated outline width grows over the frames");
    for (std::size_t index = 0; index < frames.size(); ++index)
        gpu.emplace_back("animated frame " + std::to_string(index * 5), frames[index]);
}

// TX1b: the activity rule every surface and the compiler read. Background Off leaves the
// background's color, padding and corner radius unread; a constant Outline width of 0 leaves the
// outline's color and join unread. An unread value lowers at its default even when keyed (its curve
// is not compiled, and the text still compiles), and is read again as soon as Background is on or
// the width can change (keyed or driven).
void activityTests(Expectations& expectations) {
    const auto inactive = [](const Fixture& made, const std::string_view role) {
        const auto& owner = composition(made.document, made.main);
        const auto* node = owner.graph().findNode(made.node);
        const auto* record = owner.parameters().find(made.id(role));
        return node != nullptr && record != nullptr &&
               document::isParameterInactive(owner, *node, record->schemaKey, role);
    };
    const std::vector<std::string_view> unread{"backgroundColor", "backgroundPadding",
                                               "backgroundRadius", "outlineColor", "outlineJoin"};
    auto made = fixture("Plain words");
    auto plain = fixture("Plain words");
    expectations.expect(std::ranges::all_of(unread, [&](auto role) { return inactive(*made, role); }) &&
                            !inactive(*made, "background") && !inactive(*made, "outlineWidth") &&
                            !inactive(*made, "markup") && !inactive(*made, "accent1"),
                        "Background Off and Outline width 0 leave exactly their rows unread");
    made->key<commands::InsertColor4Keyframe>("backgroundColor", core::Color4d{1.0, 0.0, 0.0, 1.0},
                                              core::Color4d{0.0, 0.0, 1.0, 1.0});
    made->key<commands::InsertScalarKeyframe>("backgroundPadding", 0.1, 0.6);
    made->key<commands::InsertColor4Keyframe>("outlineColor", core::Color4d{0.0, 1.0, 0.0, 1.0},
                                              core::Color4d{1.0, 0.0, 0.0, 1.0});
    made->set("outlineJoin", std::int64_t{document::kTextOutlineJoinBevel});
    if (const auto plan = compile(expectations, *made, "keyed unread values")) {
        const auto* text = textOf(*plan);
        expectations.expect(text != nullptr && !text->style.has_value(),
                            "keyed unread values compile, and the text lowers plain");
    }
    {
        const auto keyed = referenceAt(expectations, *made, frameTime(5), "keyed unread values");
        const auto reference = referenceAt(expectations, *plain, frameTime(5), "plain");
        expectations.expect(keyed && reference && samePixels(*keyed, *reference),
                            "keyed unread values draw exactly as plain text");
    }
    made->set("background", std::int64_t{document::kTextBackgroundBlock});
    expectations.expect(!inactive(*made, "backgroundColor") && !inactive(*made, "backgroundPadding") &&
                            !inactive(*made, "backgroundRadius") && inactive(*made, "outlineColor"),
                        "a Background reads its color, padding and radius again");
    {
        const auto first = referenceAt(expectations, *made, frameTime(0), "background frame 0");
        const auto last = referenceAt(expectations, *made, frameTime(10), "background frame 10");
        expectations.expect(first && last && countPixels(*first, reddish) > 100 &&
                                countPixels(*last, bluish) > 100,
                            "the keyed background color draws once Background is on");
    }
    made->key<commands::InsertScalarKeyframe>("outlineWidth", 0.5, 4.0);
    expectations.expect(!inactive(*made, "outlineColor") && !inactive(*made, "outlineJoin"),
                        "an animated Outline width reads the outline's color and join");
    expectations.expect(compile(expectations, *made, "animated outline width") != nullptr,
                        "the keyed outline color compiles once it is read");
    // A width driven by a value node can change too, even at 0.
    auto driven = fixture("Driven");
    {
        commands::Transaction transaction("Add value", driven->document.snapshot().revision());
        transaction.emplace<commands::AddNode>(driven->main,
                                               std::string(document::kScalarValueNodeType),
                                               document::Vec2d{});
        auto result = driven->commands.execute(std::move(transaction));
        const auto value =
            result.succeeded() ? result.outputId<document::NodeId>(commands::kAddNodeOutput)
                               : std::nullopt;
        expectations.expect(value.has_value(), "a Scalar node is added");
        if (value) {
            expectations.expect(inactive(*driven, "outlineColor"),
                                "an unconnected width of 0 leaves the outline color unread");
            execute<commands::ConnectPorts>(
                driven->document, driven->commands, driven->main,
                document::OutputPortRef{*value, std::string(document::kValuePortName)},
                document::NodeInputRef{driven->node, "outlineWidth"});
            expectations.expect(!inactive(*driven, "outlineColor") &&
                                    !inactive(*driven, "outlineJoin"),
                                "a connected Outline width reads the outline's color and join");
        }
    }
}

void cacheTests(Expectations& expectations) {
    // One evaluator (one operation cache) across the plans an edit makes: the text's image is
    // evaluated again only when what it draws changes.
    auto made = fixture("Plain <c.accent1>accent</c>");
    made->set("markup", true);
    const runtime::CpuCompositionEvaluator evaluator;
    const auto evaluated = [&](const std::string& label, const core::RationalTime time) {
        const auto plan = compile(expectations, *made, label);
        if (!plan)
            return std::ptrdiff_t{-1};
        auto request = requestFor(*plan);
        request.time = time;
        runtime::OperationCacheStatistics statistics;
        const auto result = evaluator.evaluate(plan, request, {}, {}, nullptr, &statistics);
        expectations.expect(result.frame() != nullptr, label + ": evaluates");
        return std::ranges::count(statistics.evaluatedNodes, made->node);
    };
    expectations.expect(evaluated("first", frameTime(0)) == 1,
                        "the first frame evaluates the text");
    expectations.expect(evaluated("later", frameTime(7)) == 0,
                        "a later frame of an unanimated text reuses its image");
    made->set("accent3", core::Color4d{0.1, 0.2, 0.3, 1.0});
    expectations.expect(evaluated("an unused accent changed", frameTime(0)) == 0,
                        "an accent no run uses keeps the text's identity warm");
    made->set("accent1", core::Color4d{0.4, 0.8, 0.2, 1.0});
    expectations.expect(evaluated("a used accent changed", frameTime(0)) == 1,
                        "an accent a run uses changes the text's identity");
    made->set("outlineColor", core::Color4d{0.4, 0.8, 0.2, 1.0});
    expectations.expect(evaluated("an outline color with no outline", frameTime(0)) == 0,
                        "an outline color with no outline width keeps the identity");
    made->set("outlineWidth", 1.5);
    expectations.expect(evaluated("an outline width", frameTime(0)) == 1,
                        "an outline width changes the identity");
}

void outlineSourceTests(Expectations& expectations) {
    // Text Outline reads the glyphs of the styled layout: the tags draw nothing.
    runtime::CompiledSubtitlesFaces faces;
    const auto laid = [&](const std::string_view content, const bool markup) {
        runtime::detail::StyledTextSourceInput input{
            .content = content, .markup = markup, .faces = &faces, .size = 40.0};
        auto result = runtime::detail::layoutStyledTextSource(input, {});
        std::size_t paths = 0;
        if (const auto* geometry = std::get_if<runtime::detail::StyledTextGeometry>(&result))
            for (const auto& paint : geometry->fills)
                paths += paint.paths.size();
        return paths;
    };
    expectations.expect(laid("<b>A</b> <c.accent1>B</c>", true) == laid("A B", false) &&
                            laid("A B", false) > 0,
                        "the styled glyphs hold the letters only, not the tags");
}

void budgetAndCancelTests(Expectations& expectations) {
    auto made = fixture(std::string(9000, 'a'), 8.0);
    made->set("markup", true);
    const auto plan = compile(expectations, *made, "over budget");
    if (plan) {
        auto request = requestFor(*plan);
        request.bypassOperationCache = true;
        const auto result = runtime::CpuCompositionEvaluator{}.evaluate(plan, request, {});
        const bool refused =
            result.frame() == nullptr &&
            std::ranges::any_of(result.diagnostics(), [](const auto& diagnostic) {
                return diagnostic.detail.find("9000 glyphs; at most 8192") != std::string::npos;
            });
        expectations.expect(refused, "a styled text past the glyph limit is refused, naming it");
        const auto prepared = runtime::CpuGpuSceneBuilder{}.build(plan, request);
        expectations.expect(!prepared.hasValue() && prepared.diagnostic.message.find(
                                                        "at most 8192") != std::string::npos,
                            "the GPU route refuses it with the same message");
    }
    auto small = fixture("Cancel <b>me</b>");
    small->set("outlineWidth", 2.0);
    if (const auto reference = referenceAt(expectations, *small, frameTime(0), "cancel")) {
        const auto result = runtime::CpuCompositionEvaluator{}.evaluate(
            reference->plan, reference->request, makeCancelledToken());
        expectations.expect(result.status() == runtime::EvaluationStatus::Cancelled &&
                                result.frame() == nullptr,
                            "a cancelled evaluation is cancelled");
        const auto prepared = runtime::CpuGpuSceneBuilder{}.build(
            reference->plan, reference->request, makeCancelledToken());
        expectations.expect(!prepared.hasValue() &&
                                prepared.diagnostic.code ==
                                    runtime::PreparedGpuSceneDiagnosticCode::Cancelled,
                            "a cancelled GPU preparation is cancelled");
    }
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
        defaultsTests(expectations);
        markupTests(expectations, references);
        outlineTests(expectations, references);
        backgroundTests(expectations, references);
        animationTests(expectations, references);
        activityTests(expectations);
        cacheTests(expectations);
        outlineSourceTests(expectations);
        budgetAndCancelTests(expectations);
        if (expectations.failures() != 0) {
            std::cerr << expectations.failures() << " styled text expectation(s) failed\n";
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
        if (expectations.failures() != 0) {
            std::cerr << expectations.failures() << " styled text expectation(s) failed\n";
            return 1;
        }
        std::cout << "PASS: styled Text Source runs, outline and background draw alike on the GPU "
                     "and the CPU reference ("
                  << references.size() << " states)\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected test exception: " << exception.what() << '\n';
        return 1;
    }
}
