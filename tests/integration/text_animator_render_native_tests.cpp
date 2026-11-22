// #758: the Text Animator and fixed-width digits through real documents
// (docs/architecture/evaluation-primitives.md, "Text Animator").
//
// A 512x288 composition gets a Text layer through AddTextLayer; a Text Animator is added with
// AddNode and wired between the Text Source and the layer with ConnectPorts, and the production
// SnapshotCompiler compiles it. The CPU half always runs:
//
// - the selector arithmetic against an independent reference table: the six shapes, a fractional
//   Square, the ease, the random order, the combine modes, the units and the reveal;
// - a neutral animator draws the plain text's pixels; each property (opacity, position, scale,
//   rotation, skew, fill color, outline color and width, tracking) changes the drawn text;
// - two selectors combined by Add, Subtract and Intersect, and two animators stacked in order;
// - word fade: the faded word draws nothing, the other words the full text's pixels;
// - reveal over a few frames keeps every revealed glyph's pixels and the layer's bounds those of
//   the full text, left-aligned, centered and in a wrapped box; the cursor and its blink;
// - fixed-width digits keep a counter's decimal point in place;
// - Subtitles progress drives a reveal, its text the Text Source and its index a selector;
// - the cache identity: unanimated text keeps its identity, a later frame of a still animator
//   reuses its image, an inactive value keeps the identity and an active one changes it;
// - the budget refusals (glyph and layer limits), cancellation, and save and reopen;
// - an animator fed by something other than a Text Source warns and passes its input through.
//
// Then every state is prepared by the production CpuGpuSceneBuilder, run by the production
// GpuSceneExecutor, and must match the CPU reference (2e-6), cold and with a warm rerun that
// dispatches nothing. Without a device the GPU half reports an explicit SKIP (exit 77) after the
// CPU half passed; --require-device makes that a failure.

#include "document_render_parity_support.hpp"

#include <bloom/commands/animation_operations.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/core/color.hpp>
#include <bloom/document/color_settings.hpp>
#include <bloom/document/cue_list.hpp>
#include <bloom/document/font_id.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/subtitles_nodes.hpp>
#include <bloom/document/text_animator_nodes.hpp>
#include <bloom/document/value_nodes.hpp>
#include <bloom/project/canonical_document.hpp>
#include <bloom/project/canonical_manifest.hpp>
#include <bloom/project/open_archive.hpp>
#include <bloom/project/save_archive.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/runtime/text_animator.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <numeric>
#include <thread>

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

struct Fixture final {
    document::Document document;
    commands::CommandStack commands;
    document::CompositionId main;
    document::NodeId text;
    // The last node of the chain: the Text Source, or the last animator added.
    document::NodeId top;
    std::string topPort = std::string(document::kTextSourceOutputPort);
    std::vector<document::NodeId> animators;

    explicit Fixture(document::NewProject initial)
        : document(std::move(initial.project)), commands(document),
          main(initial.initialCompositionId) {}

    [[nodiscard]] document::ParameterId id(const document::NodeId node,
                                           const std::string_view role) const {
        return parameter(document, main, node, role);
    }
    void set(const document::NodeId node, const std::string_view role,
             document::ParameterValue value) {
        // A value the parameter already holds is no change (the command refuses it).
        if (const auto* held = composition(document, main).parameters().find(id(node, role)))
            if (const auto* constant = std::get_if<document::ConstantValueSource>(&held->source);
                constant != nullptr && constant->value == value)
                return;
        try {
            execute<commands::SetParameterSource>(document, commands, main, id(node, role),
                                                  document::ConstantValueSource{std::move(value)});
        } catch (const std::runtime_error& error) {
            throw std::runtime_error("setting " + std::string(role) + " of node " +
                                     std::to_string(node.value()) + " was refused: " + error.what());
        }
    }
    void setText(const std::string_view role, document::ParameterValue value) {
        set(text, role, std::move(value));
    }
    // The last animator's parameter.
    void setAnimator(const std::string_view role, document::ParameterValue value) {
        set(animators.back(), role, std::move(value));
    }
    // Keys a parameter of the last animator: the first value at frame 0, the last at frame 10.
    template <typename Insert, typename Value>
    void key(const std::string_view role, const Value& first, const Value& last) {
        const auto node = animators.back();
        set(node, role, first);
        execute<commands::CreateAnimationForParameter>(document, commands, main, id(node, role),
                                                       frameTime(0));
        const auto* record = composition(document, main).parameters().find(id(node, role));
        const auto* curve = record == nullptr
                                ? nullptr
                                : std::get_if<document::AnimationCurveSource>(&record->source);
        if (curve == nullptr)
            throw std::runtime_error("keying " + std::string(role) + " made no curve");
        execute<Insert>(document, commands, main, curve->curveId, frameTime(10), last);
    }
    // Centers the layer on the frame, so centered text stays inside it.
    void centerLayer() {
        const auto layer = layerNode(document, main, lastLayer(document, main));
        set(layer, "position",
            document::Vec2d{static_cast<double>(kWidth) / 2.0, static_cast<double>(kHeight) / 2.0});
    }
    // Adds a Text Animator after the chain's last node, between it and what it fed.
    document::NodeId addAnimator() {
        std::optional<document::InputPortRef> fed;
        for (const auto& edge : composition(document, main).graph().edges())
            if (edge.source.nodeId == top)
                fed = edge.destination;
        if (!fed)
            throw std::runtime_error("the chain feeds nothing");
        execute<commands::AddNode>(document, commands, main,
                                   std::string(document::kTextAnimatorNodeType),
                                   document::Vec2d{0.0, 0.0});
        const auto added = lastNode(document, main, document::kTextAnimatorNodeType);
        execute<commands::ConnectPorts>(
            document, commands, main,
            document::OutputPortRef{added, std::string(document::kTextAnimatorImagePort)}, *fed);
        execute<commands::ConnectPorts>(
            document, commands, main, document::OutputPortRef{top, topPort},
            document::NodeInputRef{added, std::string(document::kTextAnimatorTextPort)});
        top = added;
        topPort = std::string(document::kTextAnimatorImagePort);
        animators.push_back(added);
        return added;
    }
};

[[nodiscard]] std::unique_ptr<Fixture> fixture(std::string text, const double size = 48.0) {
    auto made = std::make_unique<Fixture>(document::makeNewProject(
        "Animated text", "Main", core::RationalTime::fromInteger(10), format()));
    // Left-aligned from x = 24, so every line used here stays inside the frame.
    execute<commands::AddTextLayer>(
        made->document, made->commands, made->main, "Text", std::move(text),
        document::Vec2d{24.0, static_cast<double>(kHeight) / 2.0}, 1.0, size);
    made->text = lastNode(made->document, made->main, document::kTextSourceNodeType);
    made->top = made->text;
    return made;
}

[[nodiscard]] std::shared_ptr<const runtime::CompiledCompositionPlan>
compileDocument(Expectations& expectations, const document::Document& document,
                const document::CompositionId main, const std::string& label,
                std::vector<runtime::CompileDiagnostic>* diagnostics = nullptr) {
    const runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
    const auto compiled = compiler.compile({document.snapshot(), main}, {});
    expectations.expect(compiled.plan != nullptr,
                        label + ": compiles: " +
                            runtime::describeCompileFailure(compiled, document.snapshot().project(),
                                                            main));
    if (diagnostics != nullptr)
        *diagnostics = compiled.diagnostics;
    return compiled.plan;
}

[[nodiscard]] std::shared_ptr<const runtime::CompiledCompositionPlan>
compile(Expectations& expectations, const Fixture& fixture, const std::string& label,
        std::vector<runtime::CompileDiagnostic>* diagnostics = nullptr) {
    return compileDocument(expectations, fixture.document, fixture.main, label, diagnostics);
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

[[nodiscard]] std::optional<Reference>
referenceAt(Expectations& expectations, const Fixture& fixture, const core::RationalTime time,
            const std::string& label, const std::optional<render::ImageWindow>& roi = {},
            const std::optional<runtime::EvaluationResolution>& resolution = {}) {
    return referenceOf(expectations, fixture.document, fixture.main, time, label, roi, resolution);
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

[[nodiscard]] bool samePixel(const render::Rgba32f& a, const render::Rgba32f& b) {
    return std::memcmp(&a, &b, sizeof(render::Rgba32f)) == 0;
}

// Whether every pixel of the frame inside [left, right) x [top, bottom) is the same in both.
[[nodiscard]] bool sameRegion(const Reference& a, const Reference& b, const std::int64_t left,
                              const std::int64_t top, const std::int64_t right,
                              const std::int64_t bottom) {
    for (auto y = top; y < bottom; ++y)
        for (auto x = left; x < right; ++x)
            if (!samePixel(pixelAt(a, x, y), pixelAt(b, x, y)))
                return false;
    return true;
}

[[nodiscard]] bool samePicture(const Reference& a, const Reference& b) {
    return sameRegion(a, b, 0, 0, kWidth, kHeight);
}

template <typename Test>
[[nodiscard]] std::size_t countPixels(const Reference& reference, Test test) {
    std::size_t count = 0;
    for (std::int64_t y = 0; y < kHeight; ++y)
        for (std::int64_t x = 0; x < kWidth; ++x)
            if (test(pixelAt(reference, x, y)))
                ++count;
    return count;
}

[[nodiscard]] float mostAlpha(const Reference& reference) {
    float most = 0.0F;
    for (std::int64_t y = 0; y < kHeight; ++y)
        for (std::int64_t x = 0; x < kWidth; ++x)
            most = std::max(most, pixelAt(reference, x, y).alpha());
    return most;
}

[[nodiscard]] bool reddish(const render::Rgba32f& p) {
    return p.alpha() > 0.95F && p.red() > 0.6F && p.green() < 0.2F && p.blue() < 0.2F;
}
[[nodiscard]] bool bluish(const render::Rgba32f& p) {
    return p.alpha() > 0.95F && p.blue() > 0.6F && p.red() < 0.2F && p.green() < 0.2F;
}
[[nodiscard]] bool whitish(const render::Rgba32f& p) {
    return p.alpha() > 0.95F && p.red() > 0.9F && p.green() > 0.9F && p.blue() > 0.9F;
}

[[nodiscard]] runtime::ContentBounds textBounds(const Reference& reference) {
    return reference.frame->evaluatedBounds()[textIndexOf(*reference.plan)].local;
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
        TaskRequest("text animator cancel token",
                    {.kind = TaskOwnerKind::Composition, .id = TaskOwnerId::fromRaw(758)}),
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

using References = std::vector<std::pair<std::string, Reference>>;

// The selector arithmetic against values computed independently of runtime::text_animator (by
// hand from the shape formulas, at the centers of ten units with the range 20 % to 80 %).
void arithmeticTests(Expectations& expectations) {
    using runtime::TextSelectorValues;
    constexpr std::array<std::array<double, 10>, 6> table{{
        {0, 0, 1, 1, 1, 1, 1, 1, 0, 0},
        {0, 0, 1.0 / 12, 0.25, 5.0 / 12, 7.0 / 12, 0.75, 11.0 / 12, 1, 1},
        {1, 1, 11.0 / 12, 0.75, 7.0 / 12, 5.0 / 12, 0.25, 1.0 / 12, 0, 0},
        {0, 0, 1.0 / 6, 0.5, 5.0 / 6, 5.0 / 6, 0.5, 1.0 / 6, 0, 0},
        {0, 0, 0.55277079839256660, 0.86602540378443860, 0.98601329718326924,
         0.98601329718326935, 0.86602540378443860, 0.55277079839256671, 0, 0},
        {0, 0, 0.066987298107780660, 0.5, 0.93301270189221941, 0.93301270189221941, 0.5,
         0.066987298107780813, 0, 0},
    }};
    bool shapes = true;
    for (std::int64_t shape = 0; shape < 6; ++shape)
        for (std::size_t unit = 0; unit < 10; ++unit) {
            TextSelectorValues selector;
            selector.shape = shape;
            selector.start = 20.0;
            selector.end = 80.0;
            const auto value =
                runtime::textSelectorShape(selector, static_cast<double>(unit), 10);
            if (std::abs(value - table[static_cast<std::size_t>(shape)][unit]) > 1e-12) {
                shapes = false;
                std::cerr << "shape " << shape << " unit " << unit << ": " << value << '\n';
            }
        }
    expectations.expect(shapes, "the six selector shapes match the reference table");
    {
        // A fractional Square covers the part of each unit inside [2.5, 5.5).
        constexpr std::array<double, 10> square{0, 0, 0.5, 1, 1, 0.5, 0, 0, 0, 0};
        bool fractional = true;
        TextSelectorValues selector;
        selector.start = 15.0;
        selector.end = 45.0;
        selector.offset = 10.0;
        for (std::size_t unit = 0; unit < 10; ++unit)
            fractional = fractional &&
                         std::abs(runtime::textSelectorShape(selector, static_cast<double>(unit),
                                                             10) -
                                  square[unit]) < 1e-12;
        selector.rangeBy = document::kTextRangeIndex;
        selector.start = 4.5;
        selector.end = 1.5;
        selector.offset = 1.0;
        for (std::size_t unit = 0; unit < 10; ++unit)
            fractional = fractional &&
                         std::abs(runtime::textSelectorShape(selector, static_cast<double>(unit),
                                                             10) -
                                  square[unit]) < 1e-12;
        expectations.expect(fractional,
                            "a Square selector covers fractional units, by percent with an offset "
                            "and by index with the ends reversed");
    }
    {
        // The ease in Bezier form: control values 0, m0 / 3, 1 - m1 / 3, 1.
        constexpr std::array<std::array<double, 4>, 6> ease{{{0.25, 0, 100, 0.109375},
                                                             {0.5, 0, 100, 0.375},
                                                             {0.75, 0, 100, 0.703125},
                                                             {0.25, 100, 100, 0.15625},
                                                             {0.5, 100, 100, 0.5},
                                                             {0.8, -100, 50, 0.656}}};
        bool eased = std::abs(runtime::textSelectorEase(0.3, 0, 0) - 0.3) < 1e-12;
        for (const auto& [value, high, low, expected] : ease)
            eased = eased &&
                    std::abs(runtime::textSelectorEase(value, high, low) - expected) < 1e-12;
        expectations.expect(eased, "the ease matches the reference values; 0 % is linear");
    }
    {
        const auto identity = runtime::textSelectorOrder(12, false, 3.0);
        const auto first = runtime::textSelectorOrder(12, true, 3.0);
        const auto again = runtime::textSelectorOrder(12, true, 3.0);
        const auto other = runtime::textSelectorOrder(12, true, 4.0);
        std::vector<std::size_t> sorted = first;
        std::ranges::sort(sorted);
        std::vector<std::size_t> expected(12);
        std::iota(expected.begin(), expected.end(), std::size_t{0});
        expectations.expect(identity == expected && sorted == expected && first == again &&
                                first != expected && other != first,
                            "Randomize order is a permutation fixed by its seed");
    }
    {
        std::array<double, 3> total{0.0, 0.0, 0.0};
        runtime::combineTextSelector(total, std::array<double, 3>{1.0, 0.5, 0.0},
                                     document::kTextSelectorAdd);
        runtime::combineTextSelector(total, std::array<double, 3>{0.5, 0.0, 0.0},
                                     document::kTextSelectorAdd);
        const auto added = total;
        runtime::combineTextSelector(total, std::array<double, 3>{0.25, 0.0, 0.5},
                                     document::kTextSelectorSubtract);
        const auto subtracted = total;
        runtime::combineTextSelector(total, std::array<double, 3>{0.5, 1.0, 1.0},
                                     document::kTextSelectorIntersect);
        expectations.expect(added == std::array<double, 3>{1.0, 0.5, 0.0} &&
                                subtracted == std::array<double, 3>{0.75, 0.5, -0.5} &&
                                total == std::array<double, 3>{0.375, 0.5, -0.5},
                            "selectors combine by Add, Subtract and Intersect, clamped to [-1, 1]");
    }
    {
        // "ab cd" on line 0, "e" on line 1 (the line break is a cluster of line 0).
        const std::u32string text = U"ab cd\ne";
        const std::array<std::size_t, 7> lines{0, 0, 0, 0, 0, 0, 1};
        const auto units = runtime::textClusterUnits(text, lines, 2);
        expectations.expect(
            units.count == std::array<std::size_t, 4>{6, 5, 3, 2} &&
                units.unitOf[static_cast<std::size_t>(document::kTextUnitWords)] ==
                    std::vector<std::size_t>{0, 0, 0, 1, 1, 1, 2} &&
                units.unitOf[static_cast<std::size_t>(
                    document::kTextUnitCharactersExcludingSpaces)] ==
                    std::vector<std::size_t>{0, 1, 1, 2, 3, 3, 4},
            "the units count characters, characters excluding spaces, words and lines");
        const auto visible = runtime::textRevealVisible(units, document::kTextUnitWords,
                                                        document::kTextRevealCount, 2.0);
        const auto half = runtime::textRevealVisible(units, document::kTextUnitCharacters,
                                                     document::kTextRevealPercent, 50.0);
        expectations.expect(
            visible == std::vector<bool>{true, true, true, true, true, true, false} &&
                half == std::vector<bool>{true, true, true, false, false, false, false},
            "the reveal shows the first units by count or percent");
        expectations.expect(runtime::textCursorShown(2.0, 0.0) &&
                                !runtime::textCursorShown(2.0, 0.25) &&
                                runtime::textCursorShown(2.0, 0.5) &&
                                runtime::textCursorShown(0.0, 0.3),
                            "the cursor blinks on for the first half of each period");
    }
}

void propertyTests(Expectations& expectations, References& gpu) {
    auto made = fixture("Animate me");
    const auto unstyled = referenceAt(expectations, *made, frameTime(0), "unstyled");
    // The still styled text (Fixed-width digits on a text with no digits draws the same glyphs
    // through the styled path an animator draws through).
    made->setText("fixedDigits", true);
    const auto plain = referenceAt(expectations, *made, frameTime(0), "plain");
    made->addAnimator();
    const auto neutral = referenceAt(expectations, *made, frameTime(0), "neutral animator");
    if (!unstyled || !plain || !neutral)
        return;
    expectations.expect(textBounds(*unstyled) == textBounds(*plain) &&
                            coverage(*unstyled).top == coverage(*plain).top &&
                            coverage(*unstyled).left == coverage(*plain).left,
                        "the styled path lays the text out where the plain path does");
    const auto* compiled = textOf(*neutral->plan);
    expectations.expect(compiled != nullptr && compiled->style &&
                            compiled->style->animators.size() == 1 &&
                            compiled->style->animators.front().selectors.size() == 1,
                        "the animator lowers onto the Text Source's text, with its one selector");
    expectations.expect(samePicture(*plain, *neutral) &&
                            textBounds(*plain) == textBounds(*neutral),
                        "a neutral animator draws the text's own pixels and bounds");
    gpu.emplace_back("neutral", *neutral);
    const auto whole = coverage(*plain);

    // Opacity over the whole text: half.
    made->setAnimator("opacity", 0.5);
    if (const auto half = referenceAt(expectations, *made, frameTime(0), "opacity")) {
        const auto most = mostAlpha(*half);
        expectations.expect(most > 0.49F && most < 0.51F,
                            "Opacity 50 % draws every glyph at half");
        gpu.emplace_back("opacity", *half);
    }
    made->setAnimator("opacity", 1.0);
    // Position over the first half: those glyphs move up 30 px, the rest stay.
    made->setAnimator("selector1End", 50.0);
    made->setAnimator("position", document::Vec2d{0.0, -30.0});
    if (const auto moved = referenceAt(expectations, *made, frameTime(0), "position")) {
        const auto box = coverage(*moved);
        expectations.expect(box.top <= whole.top - 29 && box.bottom == whole.bottom &&
                                box.left == whole.left && box.right == whole.right,
                            "Position moves the selected glyphs only");
        expectations.expect(sameRegion(*plain, *moved, whole.right - 60, whole.top, whole.right,
                                       whole.bottom),
                            "the unselected glyphs keep their pixels");
        gpu.emplace_back("position", *moved);
    }
    made->setAnimator("position", document::Vec2d{0.0, 0.0});
    made->setAnimator("selector1End", 100.0);
    made->setAnimator("scale", document::Vec2d{1.5, 1.5});
    if (const auto scaled = referenceAt(expectations, *made, frameTime(0), "scale")) {
        const auto box = coverage(*scaled);
        expectations.expect(box.top < whole.top - 8 && box.right > whole.right + 4 &&
                                std::abs(box.bottom - whole.bottom) <= 2,
                            "Scale grows each glyph about its baseline center");
        expectations.expect(textBounds(*scaled) == textBounds(*plain),
                            "an animated text keeps the still text's bounds");
        gpu.emplace_back("scale", *scaled);
    }
    made->setAnimator("scale", document::Vec2d{1.0, 1.0});
    made->setAnimator("rotation", 30.0);
    if (const auto turned = referenceAt(expectations, *made, frameTime(0), "rotation")) {
        expectations.expect(!samePicture(*turned, *plain) &&
                                coverage(*turned).bottom > whole.bottom,
                            "Rotation turns each glyph about its baseline center");
        gpu.emplace_back("rotation", *turned);
    }
    made->setAnimator("rotation", 0.0);
    made->setAnimator("skew", 20.0);
    if (const auto skewed = referenceAt(expectations, *made, frameTime(0), "skew")) {
        const auto box = coverage(*skewed);
        expectations.expect(!samePicture(*skewed, *plain) && box.right > whole.right &&
                                box.bottom == whole.bottom,
                            "Skew leans each glyph's top to the right");
        gpu.emplace_back("skew", *skewed);
    }
    made->setAnimator("skew", 0.0);
    made->setAnimator("fillColor", core::Color4d{1.0, 0.0, 0.0, 1.0});
    if (const auto off = referenceAt(expectations, *made, frameTime(0), "fill color off"))
        expectations.expect(samePicture(*off, *plain),
                            "a fill color with Change fill off draws nothing different");
    made->setAnimator("fill", true);
    if (const auto red = referenceAt(expectations, *made, frameTime(0), "fill color")) {
        expectations.expect(countPixels(*red, reddish) > 200 && countPixels(*red, whitish) == 0,
                            "Fill color paints the selected glyphs");
        gpu.emplace_back("fill color", *red);
    }
    made->setAnimator("fill", false);
    made->setAnimator("outline", true);
    made->setAnimator("outlineColor", core::Color4d{0.0, 0.0, 1.0, 1.0});
    made->setAnimator("outlineWidth", 4.0);
    if (const auto outlined = referenceAt(expectations, *made, frameTime(0), "outline")) {
        const auto box = coverage(*outlined);
        expectations.expect(countPixels(*outlined, bluish) > 200 && box.left <= whole.left - 3 &&
                                box.right >= whole.right + 3,
                            "Outline color and width draw an outline outside the glyphs");
        gpu.emplace_back("outline", *outlined);
    }
    made->setAnimator("outlineWidth", 0.0);
    made->setAnimator("outline", false);
    made->setAnimator("tracking", 6.0);
    if (const auto tracked = referenceAt(expectations, *made, frameTime(0), "tracking")) {
        const auto box = coverage(*tracked);
        // Ten clusters: nine gaps of 6 px after the first glyph, the line left-aligned.
        expectations.expect(box.left == whole.left &&
                                std::abs((box.right - whole.right) - 54) <= 1,
                            "Tracking widens the line by its gaps, left-aligned");
        gpu.emplace_back("tracking", *tracked);
    }
    made->centerLayer();
    made->setText("alignment", std::int64_t{1});
    made->setAnimator("tracking", 0.0);
    const auto centered = referenceAt(expectations, *made, frameTime(0), "centered");
    made->setAnimator("tracking", 6.0);
    if (const auto tracked = referenceAt(expectations, *made, frameTime(0), "centered tracking");
        tracked && centered) {
        const auto box = coverage(*tracked);
        const auto still = coverage(*centered);
        expectations.expect(std::abs((still.left - box.left) - 27) <= 1 &&
                                std::abs((box.right - still.right) - 27) <= 1,
                            "a centered line grows by its tracking on both sides");
        gpu.emplace_back("centered tracking", *tracked);
    }
}

// The edges of the reference semantics: an animated text over an ROI and at a reduced render
// scale (a proxy), a muted animator, an HDR fill color and an empty text, each on both routes.
void edgeTests(Expectations& expectations, References& gpu) {
    auto made = fixture("Animate me");
    made->setText("fixedDigits", true);
    const auto plain = referenceAt(expectations, *made, frameTime(0), "edge plain");
    made->addAnimator();
    made->setAnimator("selector1End", 50.0);
    made->setAnimator("rotation", 20.0);
    made->setAnimator("position", document::Vec2d{0.0, -20.0});
    const auto full = referenceAt(expectations, *made, frameTime(0), "edge animated");
    if (!plain || !full)
        return;
    const auto box = coverage(*full);
    // An ROI over the turned half draws the full frame's pixels there.
    if (auto roi = referenceAt(expectations, *made, frameTime(0), "animated ROI",
                               windowOf(box.left, box.top, 120, 60))) {
        expectations.expect(sameRegion(*full, *roi, box.left, box.top, box.left + 120, box.top + 60),
                            "an ROI draws the animated pixels the full frame draws there");
        gpu.emplace_back("animated ROI", *roi);
    }
    // A half-size frame draws the animated text at half size.
    if (auto proxy = referenceAt(expectations, *made, frameTime(0), "animated proxy", std::nullopt,
                                 runtime::ProxyResolution{extentOf(256, 144)})) {
        const auto half = coverage(*proxy);
        expectations.expect(!half.empty() &&
                                std::abs((half.right - half.left) * 2 - (box.right - box.left)) <= 4,
                            "a half-size frame draws the animated text at half size");
        gpu.emplace_back("animated proxy", *proxy);
    }
    // A muted animator passes the text through: its own pixels and bounds.
    execute<commands::SetNodeMuted>(made->document, made->commands, made->main,
                                    made->animators.back(), true);
    if (auto muted = referenceAt(expectations, *made, frameTime(0), "muted animator")) {
        expectations.expect(samePicture(*plain, *muted) && textBounds(*plain) == textBounds(*muted),
                            "a muted animator passes the text through untouched");
        gpu.emplace_back("muted animator", *muted);
    }
    execute<commands::SetNodeMuted>(made->document, made->commands, made->main,
                                    made->animators.back(), false);
    // A fill color above 1 stays above 1 (HDR working values are not clamped).
    made->setAnimator("fill", true);
    made->setAnimator("fillColor", core::Color4d{4.0, 0.25, 0.0, 1.0});
    if (auto hdr = referenceAt(expectations, *made, frameTime(0), "HDR fill")) {
        float most = 0.0F;
        for (std::int64_t y = 0; y < kHeight; ++y)
            for (std::int64_t x = 0; x < kWidth; ++x)
                most = std::max(most, pixelAt(*hdr, x, y).red());
        expectations.expect(most > 3.9F, "an HDR fill color keeps its value above 1 (" +
                                              std::to_string(most) + ")");
        gpu.emplace_back("HDR fill", *hdr);
    }
    // An empty text with an animator draws nothing and evaluates without error.
    auto empty = fixture("");
    empty->addAnimator();
    empty->setAnimator("opacity", 0.5);
    if (auto none = referenceAt(expectations, *empty, frameTime(0), "empty text")) {
        expectations.expect(coverage(*none).empty() && mostAlpha(*none) == 0.0F,
                            "an animator on an empty text draws nothing");
        gpu.emplace_back("empty text", *none);
    }
}

void selectorTests(Expectations& expectations, References& gpu) {
    auto made = fixture("ABCDEFGHIJ");
    const auto plain = referenceAt(expectations, *made, frameTime(0), "selectors plain");
    made->addAnimator();
    made->setAnimator("fill", true);
    made->setAnimator("fillColor", core::Color4d{1.0, 0.0, 0.0, 1.0});
    // Selector 1: the first 60 %; selector 2 subtracts the first 20 %: C to F turn red.
    made->setAnimator("selector1End", 60.0);
    made->setAnimator("selector2", true);
    made->setAnimator("selector2Mode", std::int64_t{document::kTextSelectorSubtract});
    made->setAnimator("selector2End", 20.0);
    const auto subtracted = referenceAt(expectations, *made, frameTime(0), "subtract");
    made->setAnimator("selector2Mode", std::int64_t{document::kTextSelectorIntersect});
    const auto intersected = referenceAt(expectations, *made, frameTime(0), "intersect");
    made->setAnimator("selector2Mode", std::int64_t{document::kTextSelectorAdd});
    made->setAnimator("selector2Start", 80.0);
    made->setAnimator("selector2End", 100.0);
    const auto added = referenceAt(expectations, *made, frameTime(0), "add");
    if (!plain || !subtracted || !intersected || !added)
        return;
    const auto red = [](const Reference& reference) { return countPixels(reference, reddish); };
    const auto white = [](const Reference& reference) { return countPixels(reference, whitish); };
    expectations.expect(red(*subtracted) > 100 && white(*subtracted) > red(*subtracted) / 2,
                        "Subtract removes the second selector's range from the first");
    expectations.expect(red(*intersected) > 50 && red(*intersected) < red(*subtracted),
                        "Intersect keeps only where both select");
    expectations.expect(red(*added) > red(*subtracted), "Add selects both ranges");
    gpu.emplace_back("subtract", *subtracted);
    gpu.emplace_back("intersect", *intersected);
    gpu.emplace_back("add", *added);

    // A second animator stacked after the first: blue over the last half, drawn over the red.
    made->addAnimator();
    made->setAnimator("fill", true);
    made->setAnimator("fillColor", core::Color4d{0.0, 0.0, 1.0, 1.0});
    made->setAnimator("selector1Start", 50.0);
    if (const auto stacked = referenceAt(expectations, *made, frameTime(0), "stacked")) {
        const auto* compiled = textOf(*stacked->plan);
        expectations.expect(compiled != nullptr && compiled->style &&
                                compiled->style->animators.size() == 2 &&
                                compiled->style->animators[0].selectors.size() == 2,
                            "two animators stack in order, nearest the text first");
        expectations.expect(countPixels(*stacked, bluish) > 100 && red(*stacked) > 50 &&
                                red(*stacked) < red(*added),
                            "the later animator's color is drawn over the earlier one's");
        gpu.emplace_back("stacked animators", *stacked);
    }
    // With no selector on, an animator changes every cluster.
    made->setAnimator("selector1", false);
    if (const auto none = referenceAt(expectations, *made, frameTime(0), "no selector"))
        expectations.expect(red(*none) == 0 && countPixels(*none, bluish) > 200,
                            "an animator with no selector on changes every cluster");
}

void wordFadeTests(Expectations& expectations, References& gpu) {
    auto made = fixture("one two three");
    made->setText("fixedDigits", true);
    const auto plain = referenceAt(expectations, *made, frameTime(0), "word fade plain");
    made->addAnimator();
    made->setAnimator("selector1Units", std::int64_t{document::kTextUnitWords});
    made->setAnimator("selector1RangeBy", std::int64_t{document::kTextRangeIndex});
    made->setAnimator("selector1End", 1.0);
    made->setAnimator("opacity", 0.0);
    const auto faded = referenceAt(expectations, *made, frameTime(0), "word fade");
    if (!plain || !faded)
        return;
    const auto whole = coverage(*plain);
    const auto rest = coverage(*faded);
    expectations.expect(rest.left > whole.left + 40 && rest.right == whole.right,
                        "the first word fades out");
    expectations.expect(sameRegion(*plain, *faded, rest.left, 0, kWidth, kHeight),
                        "the other words draw the full text's pixels");
    gpu.emplace_back("word fade", *faded);
    // The word-fade recipe: a ramp keyed across the words, half way.
    made->setAnimator("selector1RangeBy", std::int64_t{document::kTextRangePercent});
    made->setAnimator("selector1End", 100.0);
    made->setAnimator("selector1Shape", std::int64_t{document::kTextShapeRampDown});
    made->key<commands::InsertScalarKeyframe>("selector1Offset", -100.0, 100.0);
    if (const auto middle = referenceAt(expectations, *made, frameTime(5), "word fade keyed")) {
        expectations.expect(!samePicture(*middle, *plain) && !coverage(*middle).empty(),
                            "a keyed range fades the words in turn");
        gpu.emplace_back("word fade keyed", *middle);
    }
}

void revealTests(Expectations& expectations, References& gpu) {
    // Left-aligned, centered, and in a wrapped box.
    for (const auto layout : {0, 1, 2}) {
        const std::string name = layout == 0 ? "left" : layout == 1 ? "centered" : "wrapped";
        auto made = fixture(layout == 2 ? "Type on text in a box" : "Type on text", 40.0);
        made->setText("fixedDigits", true);
        if (layout != 0)
            made->centerLayer();
        if (layout == 1)
            made->setText("alignment", std::int64_t{1});
        if (layout == 2) {
            made->setText("box", document::Vec2d{220.0, 200.0});
            made->setText("wrap", true);
            made->setText("alignment", std::int64_t{1});
        }
        const auto full = referenceAt(expectations, *made, frameTime(0), name + " full");
        made->addAnimator();
        made->key<commands::InsertScalarKeyframe>("reveal", 0.0, 100.0);
        if (!full)
            continue;
        const auto fullBox = coverage(*full);
        std::int64_t last = -1;
        bool kept = true;
        bool grows = true;
        bool bounds = true;
        for (const std::int64_t frame : {0, 3, 6, 10}) {
            const auto partial =
                referenceAt(expectations, *made, frameTime(frame), name + " reveal");
            if (!partial)
                return;
            const auto box = coverage(*partial);
            bounds = bounds && textBounds(*partial) == textBounds(*full);
            if (frame == 0) {
                grows = grows && box.empty();
                continue;
            }
            const auto covered =
                countPixels(*partial, [](const auto& pixel) { return pixel.alpha() > 0.0F; });
            grows = grows && static_cast<std::int64_t>(covered) > last;
            last = static_cast<std::int64_t>(covered);
            // Every pixel the partial text draws is the full text's, except at the right edge of
            // its ink, where the last revealed glyph may share a pixel with the next.
            for (std::int64_t y = 0; y < kHeight && kept; ++y)
                for (std::int64_t x = 0; x < kWidth && kept; ++x) {
                    const auto pixel = pixelAt(*partial, x, y);
                    if (pixel.alpha() <= 0.0F)
                        continue;
                    const auto edge = pixelAt(*partial, x + 1, y).alpha() <= 0.0F &&
                                      pixelAt(*partial, x + 2, y).alpha() <= 0.0F;
                    if (!samePixel(pixel, pixelAt(*full, x, y)) && !edge)
                        kept = false;
                }
            if (frame == 6)
                gpu.emplace_back(name + " reveal", *partial);
        }
        expectations.expect(grows, name + ": the reveal draws nothing at 0 % and more each frame");
        expectations.expect(kept, name + ": revealed glyphs keep the full text's pixels");
        expectations.expect(bounds, name + ": the reveal keeps the full text's bounds");
        if (layout == 2) {
            // Reveal by lines: exactly the first line.
            made->setAnimator("reveal", 1.0);
            made->setAnimator("revealBy", std::int64_t{document::kTextRevealCount});
            made->setAnimator("revealUnits", std::int64_t{document::kTextUnitLines});
            if (const auto line = referenceAt(expectations, *made, frameTime(0), "one line")) {
                const auto box = coverage(*line);
                expectations.expect(!box.empty() && box.bottom < fullBox.bottom - 20 &&
                                        sameRegion(*line, *full, 0, 0, kWidth, box.bottom - 2),
                                    "Reveal 1 line draws the first line as the full text does");
                gpu.emplace_back("one line", *line);
            }
        }
    }
    // The cursor: after the revealed text, blinking twice a second.
    auto made = fixture("Typing", 40.0);
    made->setText("fixedDigits", true);
    made->addAnimator();
    made->setAnimator("reveal", 50.0);
    const auto bare = referenceAt(expectations, *made, frameTime(0), "no cursor");
    made->setAnimator("cursor", true);
    const auto on = referenceAt(expectations, *made, frameTime(0), "cursor on");
    const auto off = referenceAt(expectations, *made, frameTime(6), "cursor off");
    if (bare && on && off) {
        expectations.expect(coverage(*on).right > coverage(*bare).right + 4 &&
                                sameRegion(*on, *bare, 0, 0, coverage(*bare).right - 1, kHeight),
                            "the cursor draws after the last revealed glyph");
        expectations.expect(samePicture(*off, *bare),
                            "a quarter second later the blinking cursor is off");
        gpu.emplace_back("cursor", *on);
    }
}

void fixedDigitTests(Expectations& expectations, References& gpu) {
    // Inter's digits are proportional, so the counter moves its decimal point unless digits are
    // fixed-width. The right edge of the trailing "00" follows the point.
    auto made = fixture("0");
    document::FontId inter;
    inter.family = "Inter";
    inter.style = "Regular";
    made->setText("font", document::encodeFontId(inter));
    const auto drawn = [&](const std::string& text, const std::string& label) {
        made->setText("text", text);
        return referenceAt(expectations, *made, frameTime(0), label);
    };
    const auto one = drawn("1.00", "proportional 1");
    const auto eight = drawn("8.00", "proportional 8");
    made->setText("fixedDigits", true);
    const auto fixedOne = drawn("1.00", "fixed 1");
    const auto fixedEight = drawn("8.00", "fixed 8");
    const auto fixedEleven = drawn("11.00", "fixed 11");
    const auto fixedEightyEight = drawn("88.00", "fixed 88");
    if (!one || !eight || !fixedOne || !fixedEight || !fixedEleven || !fixedEightyEight)
        return;
    const auto right = [](const Reference& reference) { return coverage(reference).right; };
    expectations.expect(right(*one) != right(*eight),
                        "proportional digits move the counter's decimal point");
    expectations.expect(right(*fixedOne) == right(*fixedEight) &&
                            right(*fixedEleven) == right(*fixedEightyEight),
                        "fixed-width digits keep the counter's decimal point in place");
    const auto* compiled = textOf(*fixedEight->plan);
    expectations.expect(compiled != nullptr && compiled->style && compiled->style->fixedDigits,
                        "Fixed-width digits lowers a styled text");
    gpu.emplace_back("fixed digits", *fixedEight);
}

// Subtitles drive the animated text: its text the Text Source, its progress (mapped to percent)
// the reveal, its index the first selector's shape.
void subtitlesTests(Expectations& expectations, References& gpu) {
    auto made = fixture("unused", 40.0);
    made->addAnimator();
    auto list = document::CueListValue{};
    list.language = "en";
    auto first = std::make_shared<document::Cue>();
    first->id = 1;
    first->start = frameTime(0);
    first->end = frameTime(24);
    first->text = "Spoken words";
    list.cues = {first};
    list.nextCueId = 2;
    document::canonicalizeCueList(list);
    execute<commands::AddNode>(made->document, made->commands, made->main,
                               std::string(document::kSubtitlesNodeType),
                               document::Vec2d{0.0, 200.0});
    const auto subtitles = lastNode(made->document, made->main, document::kSubtitlesNodeType);
    made->set(subtitles, document::kSubtitlesCuesRole, list);
    execute<commands::AddNode>(made->document, made->commands, made->main,
                               std::string(document::kMapRangeNodeType),
                               document::Vec2d{0.0, 300.0});
    const auto map = lastNode(made->document, made->main, document::kMapRangeNodeType);
    made->set(map, document::kMapRangeToMaximumPortName, 100.0);
    const auto connect = [&](const document::NodeId from, const std::string_view out,
                             const document::NodeId to, const std::string_view in) {
        execute<commands::ConnectPorts>(made->document, made->commands, made->main,
                                        document::OutputPortRef{from, std::string(out)},
                                        document::NodeInputRef{to, std::string(in)});
    };
    connect(subtitles, document::kSubtitlesProgressPort, map, document::kMapRangeValuePortName);
    connect(map, document::kResultPortName, made->animators.back(), "reveal");
    connect(subtitles, document::kSubtitlesTextPort, made->text, document::kTextParameterRole);
    connect(subtitles, document::kSubtitlesIndexPort, made->animators.back(), "selector1Shape");
    auto full = fixture("Spoken words", 40.0);
    full->setText("fixedDigits", true);
    const auto whole = referenceAt(expectations, *full, frameTime(0), "subtitle text");
    const auto start = referenceAt(expectations, *made, frameTime(0), "progress 0");
    const auto middle = referenceAt(expectations, *made, frameTime(12), "progress half");
    const auto late = referenceAt(expectations, *made, frameTime(23), "progress late");
    if (!whole || !start || !middle || !late)
        return;
    const auto half = coverage(*middle);
    expectations.expect(coverage(*start).empty(), "progress 0 reveals nothing");
    expectations.expect(!half.empty() && half.left == coverage(*whole).left &&
                            half.right < coverage(*whole).right - 40 &&
                            coverage(*late).right > half.right,
                        "Subtitles progress drives the reveal of the cue's text");
    gpu.emplace_back("subtitles progress", *middle);
}

void cacheTests(Expectations& expectations) {
    auto made = fixture("Cache me");
    const runtime::CpuCompositionEvaluator evaluator;
    const auto evaluated = [&](const std::string& label, const core::RationalTime time,
                               const document::NodeId node) {
        const auto plan = compile(expectations, *made, label);
        if (!plan)
            return std::ptrdiff_t{-1};
        auto request = requestFor(*plan);
        request.time = time;
        runtime::OperationCacheStatistics statistics;
        const auto result = evaluator.evaluate(plan, request, {}, {}, nullptr, &statistics);
        expectations.expect(result.frame() != nullptr, label + ": evaluates");
        return std::ranges::count(statistics.evaluatedNodes, node);
    };
    expectations.expect(evaluated("unanimated", frameTime(0), made->text) == 1,
                        "the text evaluates once");
    const auto plainPlan = compile(expectations, *made, "plain plan");
    expectations.expect(plainPlan && textOf(*plainPlan) && !textOf(*plainPlan)->style,
                        "unanimated text keeps the plain Text path and its identity");
    expectations.expect(evaluated("unanimated later", frameTime(7), made->text) == 0,
                        "a later frame of unanimated text reuses its image");
    const auto animator = made->addAnimator();
    made->setAnimator("opacity", 0.5);
    expectations.expect(evaluated("animator", frameTime(0), animator) == 1,
                        "the animator evaluates");
    expectations.expect(evaluated("animator later", frameTime(7), animator) == 0,
                        "a later frame of a still animator reuses its image");
    made->setAnimator("selector2Start", 40.0);
    expectations.expect(evaluated("inactive selector", frameTime(0), animator) == 0,
                        "a value of a selector that is off keeps the identity");
    made->setAnimator("fillColor", core::Color4d{0.0, 1.0, 0.0, 1.0});
    expectations.expect(evaluated("fill off", frameTime(0), animator) == 0,
                        "a fill color with Change fill off keeps the identity");
    made->setAnimator("selector1Start", 40.0);
    expectations.expect(evaluated("selector start", frameTime(0), animator) == 1,
                        "a selector value changes the identity");
    made->setAnimator("rotation", 10.0);
    expectations.expect(evaluated("rotation", frameTime(0), animator) == 1,
                        "a property value changes the identity");
    made->key<commands::InsertScalarKeyframe>("reveal", 0.0, 100.0);
    expectations.expect(evaluated("keyed 2", frameTime(2), animator) == 1 &&
                            evaluated("keyed 3", frameTime(3), animator) == 1,
                        "a keyed reveal draws each frame");
}

void refusedOnBothRoutes(Expectations& expectations, const Fixture& made, const std::string& label,
                         const std::string& message) {
    const auto plan = compile(expectations, made, label);
    if (!plan)
        return;
    auto request = requestFor(*plan);
    request.bypassOperationCache = true;
    const auto result = runtime::CpuCompositionEvaluator{}.evaluate(plan, request, {});
    expectations.expect(result.frame() == nullptr &&
                            std::ranges::any_of(result.diagnostics(),
                                                [&](const auto& diagnostic) {
                                                    return diagnostic.detail.find(message) !=
                                                           std::string::npos;
                                                }),
                        label + ": refused, naming the limit");
    const auto prepared = runtime::CpuGpuSceneBuilder{}.build(plan, request);
    expectations.expect(!prepared.hasValue() &&
                            prepared.diagnostic.message.find(message) != std::string::npos,
                        label + ": the GPU route refuses it with the same message");
}

void budgetAndCancelTests(Expectations& expectations) {
    {
        auto made = fixture(std::string(9000, 'a'), 8.0);
        made->addAnimator();
        refusedOnBothRoutes(expectations, *made, "past the glyph limit", "at most 8192");
    }
    {
        // A ramp of fill colors over 300 glyphs: a color per glyph, past the layer limit.
        auto made = fixture(std::string(300, 'i'), 8.0);
        made->setText("box", document::Vec2d{480.0, 260.0});
        made->setText("wrap", true);
        made->addAnimator();
        made->setAnimator("fill", true);
        made->setAnimator("fillColor", core::Color4d{1.0, 0.0, 0.0, 1.0});
        made->setAnimator("selector1Shape", std::int64_t{document::kTextShapeRampUp});
        refusedOnBothRoutes(expectations, *made, "past the layer limit", "at most 256");
    }
    auto small = fixture("Cancel me");
    small->addAnimator();
    small->setAnimator("rotation", 15.0);
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
    auto made = fixture("Saved and opened");
    made->setText("fixedDigits", true);
    made->addAnimator();
    made->setAnimator("selector1Units", std::int64_t{document::kTextUnitWords});
    made->setAnimator("selector1Shape", std::int64_t{document::kTextShapeTriangle});
    made->setAnimator("position", document::Vec2d{0.0, -12.0});
    made->setAnimator("cursor", true);
    made->setAnimator("cursorCharacter", std::string("_"));
    made->key<commands::InsertScalarKeyframe>("reveal", 0.0, 100.0);
    const auto before = referenceAt(expectations, *made, frameTime(5), "before save");
    const auto settings = document::makeBloomNeutralColorSettingsV1({});
    const auto snapshot = made->document.snapshot();
    const project::CanonicalManifestV1 manifest;
    const project::CanonicalDocumentV1 input{.snapshot = &snapshot, .colorSettings = &settings};
    auto saved = project::buildVerifiedSaveArchive(manifest, input, {}, memory());
    expectations.expect(saved && saved.archive(), "the animated project saves");
    if (!saved || !saved.archive() || !before)
        return;
    const auto bytes = saved.archive()->bytes();
    auto opened = project::openProjectArchive(bytes, {}, memory());
    expectations.expect(opened.outcome() == project::OpenArchiveOutcome::Opened,
                        "the animated project opens");
    if (opened.outcome() != project::OpenArchiveOutcome::Opened)
        return;
    auto reopened = std::move(opened).takeOpened();
    expectations.expect(!reopened.roundTrip, "nothing unknown is retained");
    const auto after =
        referenceOf(expectations, *reopened.document, made->main, frameTime(5), "after reopen");
    expectations.expect(after && samePicture(*before, *after),
                        "save and reopen draw the same animated pixels");
    gpu.emplace_back("reopened", *before);
}

void diagnosticTests(Expectations& expectations) {
    // An animator fed by something that is not a Text Source passes its input through, warned.
    auto made = std::make_unique<Fixture>(document::makeNewProject(
        "Not text", "Main", core::RationalTime::fromInteger(10), format()));
    execute<commands::AddSolidLayer>(made->document, made->commands, made->main, "Solid",
                                     core::Color4d{0.2, 0.4, 0.6, 1.0});
    made->text = lastNode(made->document, made->main, document::kSolidSourceNodeType);
    made->top = made->text;
    made->topPort = std::string(document::kSolidSourceOutputPort);
    const auto solid = referenceAt(expectations, *made, frameTime(0), "solid");
    made->addAnimator();
    std::vector<runtime::CompileDiagnostic> diagnostics;
    static_cast<void>(compile(expectations, *made, "not text", &diagnostics));
    expectations.expect(std::ranges::any_of(diagnostics,
                                            [](const auto& diagnostic) {
                                                return diagnostic.code ==
                                                       runtime::CompileDiagnosticCode::
                                                           TextAnimatorNoText;
                                            }),
                        "an animator without a Text Source warns on the node");
    const auto passed = referenceAt(expectations, *made, frameTime(0), "passed through");
    expectations.expect(solid && passed && samePicture(*solid, *passed),
                        "and passes its input through unchanged");
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
        References references;
        arithmeticTests(expectations);
        propertyTests(expectations, references);
        edgeTests(expectations, references);
        selectorTests(expectations, references);
        wordFadeTests(expectations, references);
        revealTests(expectations, references);
        fixedDigitTests(expectations, references);
        subtitlesTests(expectations, references);
        cacheTests(expectations);
        budgetAndCancelTests(expectations);
        persistenceTests(expectations, references);
        diagnosticTests(expectations);
        if (expectations.failures() != 0) {
            std::cerr << expectations.failures() << " text animator expectation(s) failed\n";
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
            std::cerr << expectations.failures() << " text animator expectation(s) failed\n";
            return 1;
        }
        std::cout << "PASS: Text Animator properties, selectors, reveal and fixed digits draw "
                     "alike on the GPU and the CPU reference ("
                  << references.size() << " states)\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected test exception: " << exception.what() << '\n';
        return 1;
    }
}
