// #344 #530 #509: the path processing nodes on the production GPU route (docs/architecture/
// path-processing.md).
//
// Transform Path, Trim Path, Offset Path, Round Corners, Boolean Paths (all four operations),
// Merge Paths and Path Stroke (solid, dashed, dots, tapered, wiggle, core) are built into real
// documents through commands: a Path node, the processing node and a Shape Source whose `path` it
// drives. Each state is compiled by the production SnapshotCompiler, rendered by the CPU reference,
// prepared by the production CpuGpuSceneBuilder, run on the production GpuSceneExecutor and held to
// the CPU reference at 2e-6, alpha exact, cold then warm. Editing one operand of a node changes
// the pixels and is dispatched, an unrelated edit is not; an operand animated over time gives a
// frame per time and a held key dispatches nothing new. A fill with alpha one half and a colour
// above one, a path partly outside the frame, a region of interest and the chain
// `Path Stroke core -> Shape Source -> Glow` run the same way, as do image effects and compositing
// recipes (Gaussian Blur, Exposure, Drop Shadow, Glow) over a curved Shape Source and over a Text
// Source read as an image. Preparation and the native job cancel, and the budget refuses.
//
// The CPU assertions always run. Without a device the GPU half reports an explicit SKIP (exit 77)
// after the CPU half passed; --require-device makes that a failure.

#include "document_render_parity_support.hpp"

#include <bloom/commands/animation_operations.hpp>
#include <bloom/commands/layer_mask_operations.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/commands/path_glow_presets.hpp>
#include <bloom/document/compositing_recipe_nodes.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/node_group_nodes.hpp>
#include <bloom/document/path_value.hpp>
#include <bloom/document/shape.hpp>
#include <bloom/document/value_nodes.hpp>
#include <bloom/document/value_utility_nodes.hpp>
#include <bloom/runtime/gpu_ocio_preparation.hpp>
#include <bloom/runtime/task_scheduler.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <mutex>
#include <span>
#include <thread>

namespace {

using namespace bloom;
using namespace bloom::test::document_render;
using document::PathContour;
using document::PathValue;
using document::Vec2d;
using render::composite_proof::Options;
using render::composite_proof::parseOptions;

PathContour square(const double left, const double top, const double size, const bool curved) {
    PathContour contour;
    contour.closed = true;
    contour.anchors = {{{left, top}, {}, {}},
                       {{left + size, top}, {}, {}},
                       {{left + size, top + size},
                        curved ? std::optional<Vec2d>{Vec2d{left + size + 6.0, top + size - 6.0}}
                               : std::nullopt,
                        {}},
                       {{left, top + size}, {}, {}}};
    return contour;
}

// Two nested squares, one corner curved: a compound path with a hole.
PathValue framed(const double inner = 20.0) {
    PathValue value;
    value.contours = {square(8, 8, 60, true), square(inner, inner, 24, false)};
    value.fillRule = document::PathFillRule::EvenOdd;
    return value;
}
PathValue openLine() {
    PathContour contour;
    contour.closed = false;
    contour.anchors = {{{10, 40}, {}, Vec2d{30, 10}}, {{50, 40}, Vec2d{40, 70}, {}},
                       {{86, 20}, {}, {}}};
    return PathValue::fromContour(contour);
}

struct Chain final {
    document::NodeId producer;
    document::NodeId processor;
    document::NodeId shape;
};

struct Fixture final {
    document::NewProject initial = document::makeNewProject(
        "Path processing", "Main", core::RationalTime::fromInteger(4),
        *document::CompositionFormat::create(96, 80, core::PixelAspectRatio::square(),
                                             *document::FrameRate::create(24, 1)));
    document::CompositionId main = initial.initialCompositionId;
    document::Document document{std::move(initial.project)};
    commands::CommandStack commands{document};

    document::NodeId addNode(const std::string_view type) {
        execute<commands::AddNode>(document, commands, main, std::string(type), Vec2d{0, 0});
        return lastNode(document, main, type);
    }
    document::NodeId addShape(const core::Color4d fill) {
        commands::ShapeLayerGeometry geometry;
        geometry.path = PathValue::fromContour(square(1, 1, 2, false));
        commands::ShapeLayerStyle style;
        style.fillColor = fill;
        execute<commands::AddShapeLayer>(document, commands, main, document::ShapeKind::Path,
                                         geometry, style);
        return lastNode(document, main, document::kShapeSourceNodeType);
    }
    // Connects, taking an existing link into the input out first.
    void connect(const document::NodeId from, const std::string_view output,
                 const document::NodeId to, const std::string_view input) {
        const auto* record = composition(document, main).graph().findNode(to);
        const bool linked = std::ranges::any_of(
            composition(document, main).graph().edges(), [&](const auto& edge) {
                const auto* target = std::get_if<document::NodeInputRef>(&edge.destination);
                return target != nullptr && target->nodeId == to && target->port == input;
            });
        static_cast<void>(record);
        if (linked)
            execute<commands::DisconnectInput>(document, commands, main,
                                               document::NodeInputRef{to, std::string(input)});
        execute<commands::ConnectPorts>(document, commands, main,
                                        document::OutputPortRef{from, std::string(output)},
                                        document::NodeInputRef{to, std::string(input)});
    }
    // Adds `source`'s image as a new entry of `merge`, at the entry's defaults: at the bottom of
    // its stack, or on top.
    void enter(const document::NodeId source, const document::NodeId merge,
               const bool onTop = false) {
        std::optional<document::LayerSlotId> top;
        if (const auto* stack = composition(document, main).graph().merge(merge);
            onTop && stack != nullptr && !stack->entries().empty())
            top = stack->entries().front().slotId;
        execute<commands::ConnectPorts>(
            document, commands, main, document::OutputPortRef{source, "image"},
            document::InputPortRef{document::LayerStackInputRef{merge, {}, "content"}},
            document::builtInNodeDefinitions(), top);
    }
    // The Merge `layer`'s image is an entry of.
    [[nodiscard]] document::NodeId mergeFeeding(const document::NodeId layer) const {
        for (const auto& edge : composition(document, main).graph().edges())
            if (const auto* entry = std::get_if<document::LayerStackInputRef>(&edge.destination);
                entry != nullptr && edge.source.nodeId == layer)
                return entry->stackNodeId;
        throw std::runtime_error("fixture layer feeds no Merge");
    }
    // A value equal to the parameter's current one is not an edit; that is fine here.
    void set(const document::NodeId node, const std::string_view role,
             document::ParameterValue value) {
        commands::Transaction transaction("Fixture", document.snapshot().revision());
        transaction.emplace<commands::SetParameterSource>(
            main, parameter(document, main, node, role),
            document::ConstantValueSource{std::move(value)});
        const auto outcome = commands.execute(std::move(transaction));
        if (!outcome.succeeded())
            throw std::runtime_error("document render fixture command refused");
    }
    void key(const document::NodeId node, const std::string_view role, const std::int64_t quarter,
             const double value) {
        commands::Transaction transaction("Fixture", document.snapshot().revision());
        transaction.emplace<commands::SetKeyframeAtTimeForParameter>(
            main, parameter(document, main, node, role), *core::RationalTime::create(quarter, 4),
            value);
        const auto outcome = commands.execute(std::move(transaction));
        if (!outcome.changed()) {
            std::string why = "keyframe refused";
            for (const auto& failure : outcome.operationFailures)
                why += ": " + failure.issue.message;
            for (const auto& issue : outcome.validation.issues())
                why += "; " + issue.message;
            throw std::runtime_error(why);
        }
    }
    // A Path node holding `path`, `type` fed by it, and a Shape Source drawing the node's `path`.
    Chain chain(const std::string_view type, PathValue path,
                const core::Color4d fill = {0.2, 0.5, 0.9, 1.0},
                const std::string_view operand = "path") {
        Chain result;
        result.producer = addNode(document::kPathValueNodeType);
        set(result.producer, document::kValueParameterRole, std::move(path));
        result.processor = addNode(type);
        connect(result.producer, document::kValuePortName, result.processor, operand);
        result.shape = addShape(fill);
        connect(result.processor, "path", result.shape, "path");
        return result;
    }
};

// A CPU reference at a quarter second, optionally over a region of interest.
[[nodiscard]] std::optional<Reference>
referenceAt(Expectations& expectations, const Fixture& fixture, const int quarter,
            const std::string& label, const std::optional<render::ImageWindow>& roi = {}) {
    auto reference = cpuReference(expectations, fixture.document, fixture.main, label);
    if (!reference)
        return std::nullopt;
    reference->request.time = *core::RationalTime::create(quarter, 4);
    reference->request.roi = roi;
    auto oracle = reference->request;
    oracle.bypassOperationCache = true;
    const auto result = runtime::CpuCompositionEvaluator{}.evaluate(reference->plan, oracle, {});
    expectations.expect(result.frame() != nullptr, label + ": the CPU reference evaluates");
    if (result.frame() == nullptr)
        return std::nullopt;
    reference->frame = result.frame();
    return reference;
}

[[nodiscard]] bool samePixels(const Reference& a, const Reference& b) {
    const auto left = a.frame->processImage().pixels();
    const auto right = b.frame->processImage().pixels();
    return left.size_bytes() == right.size_bytes() &&
           std::memcmp(left.data(), right.data(), left.size_bytes()) == 0;
}

// A scenario: how to build it, the edit of one operand, and what it is called.
struct Scenario final {
    std::string label;
    std::function<document::NodeId(Fixture&)> build; // returns the node whose operand is edited
    std::function<void(Fixture&, document::NodeId)> edit;
};

[[nodiscard]] std::vector<Scenario> scenarios() {
    std::vector<Scenario> result;
    const auto add = [&](std::string label, auto build, auto edit) {
        result.push_back({std::move(label), std::move(build), std::move(edit)});
    };
    add("transform path",
        [](Fixture& f) {
            const auto c = f.chain(document::kTransformPathNodeType, framed());
            f.set(c.processor, "position", Vec2d{12, 6});
            f.set(c.processor, "anchor", Vec2d{8, 8});
            f.set(c.processor, "scale", Vec2d{1.2, 0.8});
            f.set(c.processor, "rotation", 15.0);
            return c.processor;
        },
        [](Fixture& f, const document::NodeId n) { f.set(n, "rotation", 50.0); });
    for (const auto mode : {std::int64_t{0}, std::int64_t{1}})
        add(mode == 0 ? "trim path (simultaneously)" : "trim path (individually)",
            [mode](Fixture& f) {
                const auto c = f.chain(document::kTrimPathNodeType, framed());
                f.set(c.processor, "start", 10.0);
                f.set(c.processor, "end", 70.0);
                f.set(c.processor, "offset", 5.0);
                f.set(c.processor, "mode", mode);
                return c.processor;
            },
            [](Fixture& f, const document::NodeId n) { f.set(n, "end", 45.0); });
    for (const auto join : {std::int64_t{0}, std::int64_t{1}, std::int64_t{2}})
        add(join == 0 ? "offset path (miter)"
                      : join == 1 ? "offset path (round)" : "offset path (bevel)",
            [join](Fixture& f) {
                const auto c = f.chain(document::kOffsetPathNodeType, framed());
                f.set(c.processor, "amount", 4.0);
                f.set(c.processor, "join", join);
                return c.processor;
            },
            [](Fixture& f, const document::NodeId n) { f.set(n, "amount", 9.0); });
    add("round corners",
        [](Fixture& f) {
            const auto c = f.chain(document::kRoundCornersNodeType, framed());
            f.set(c.processor, "radius", 8.0);
            return c.processor;
        },
        [](Fixture& f, const document::NodeId n) { f.set(n, "radius", 3.0); });
    for (const auto operation : {std::int64_t{0}, std::int64_t{1}, std::int64_t{2}, std::int64_t{3}})
        add("boolean paths (operation " + std::to_string(operation) + ")",
            [operation](Fixture& f) {
                const auto c = f.chain(document::kBooleanPathsNodeType, framed(), {0.9, 0.5, 0.2, 1.0},
                                       "first");
                const auto second = f.addNode(document::kPathValueNodeType);
                PathValue other = PathValue::fromContour(square(34, 24, 50, true));
                f.set(second, document::kValueParameterRole, other);
                f.connect(second, document::kValuePortName, c.processor, "second");
                f.set(c.processor, "operation", operation);
                return c.processor;
            },
            [](Fixture& f, const document::NodeId n) { f.set(n, "tolerance", 2.0); });
    for (const auto operation : {std::int64_t{0}, std::int64_t{1}})
        add("merge paths (operation " + std::to_string(operation) + ")",
            [operation](Fixture& f) {
                PathValue pair;
                pair.contours = {square(8, 8, 50, false), square(30, 20, 50, true)};
                const auto c = f.chain(document::kMergePathsNodeType, pair);
                f.set(c.processor, "operation", operation);
                return c.processor;
            },
            [operation](Fixture& f, const document::NodeId n) {
                f.set(n, "operation", operation == 0 ? std::int64_t{2} : std::int64_t{3});
            });
    struct StrokeCase final {
        const char* label;
        std::function<void(Fixture&, document::NodeId)> style;
    };
    const std::vector<StrokeCase> strokes{
        {"path stroke (solid)", [](Fixture& f, document::NodeId n) { f.set(n, "width", 8.0); }},
        {"path stroke (dashed)",
         [](Fixture& f, document::NodeId n) {
             // Butt caps, so each gap shows (a round cap would close a gap narrower than the
             // stroke).
             f.set(n, "cap", document::selectorStoredValue(document::StrokeCap::Butt));
             f.set(n, "dashLength", 9.0);
             f.set(n, "dashGap", 5.0);
         }},
        {"path stroke (dots)",
         [](Fixture& f, document::NodeId n) {
             f.set(n, "spacing", 10.0);
             f.set(n, "width", 7.0);
         }},
        // #509 stamps: each shape, a dash following the path, and a fixed one turned.
        {"path stroke (square stamps)",
         [](Fixture& f, document::NodeId n) {
             f.set(n, "spacing", 10.0);
             f.set(n, "stamp", std::int64_t{1});
         }},
        {"path stroke (diamond stamps, follow path)",
         [](Fixture& f, document::NodeId n) {
             f.set(n, "spacing", 10.0);
             f.set(n, "stamp", std::int64_t{2});
             f.set(n, "stampAlign", std::int64_t{1});
         }},
        {"path stroke (dash stamps)",
         [](Fixture& f, document::NodeId n) {
             f.set(n, "spacing", 9.0);
             f.set(n, "stamp", std::int64_t{3});
         }},
        {"path stroke (dash stamps, fixed, turned)",
         [](Fixture& f, document::NodeId n) {
             f.set(n, "spacing", 9.0);
             f.set(n, "stamp", std::int64_t{3});
             f.set(n, "stampAlign", std::int64_t{2});
             f.set(n, "stampRotation", 35.0);
         }},
        {"path stroke (tapered)",
         [](Fixture& f, document::NodeId n) {
             f.set(n, "taperStart", 30.0);
             f.set(n, "taperEnd", 40.0);
             f.set(n, "width", 12.0);
         }},
        {"path stroke (wiggle)",
         [](Fixture& f, document::NodeId n) {
             f.set(n, "wiggle", 3.0);
             f.set(n, "wiggleFrequency", 4.0);
             f.set(n, "seed", std::int64_t{7});
         }},
        {"path stroke (core)", [](Fixture& f, document::NodeId n) {
             f.set(n, "coreWidth", 45.0);
             // The shape draws the glowing core, not the body.
             const auto shape = lastNode(f.document, f.main, document::kShapeSourceNodeType);
             f.connect(n, "core", shape, "path");
         }}};
    for (const auto& stroke : strokes)
        add(stroke.label,
            [style = stroke.style](Fixture& f) {
                const auto c = f.chain(document::kPathStrokeNodeType, openLine());
                f.set(c.processor, "width", 8.0);
                style(f, c.processor);
                return c.processor;
            },
            [](Fixture& f, const document::NodeId n) { f.set(n, "width", 14.0); });
    // #509 Mask Path and Text Outline: a layer's masks and a text's glyph outlines, stroked where
    // they are. The masks are in the plate's own space, which for a frame-sized plate at its
    // default place is the frame's, so the stroke's Shape is an entry of the composition's Merge at
    // its defaults (the identity). The glyph outlines are in the text's own space, so the stroke's
    // Shape is merged with the text before the text's layer and moves with it.
    add(
        "mask path (layer masks, stroked)",
        [](Fixture& f) {
            execute<commands::AddSolidLayer>(f.document, f.commands, f.main, "Plate",
                                             core::Color4d{0.1, 0.1, 0.1, 1.0});
            const auto boundary = composition(f.document, f.main).graph().layerOutputs().back();
            const auto layer = boundary.layerId;
            const auto layerNode = boundary.nodeId;
            execute<commands::AddLayerMask>(f.document, f.commands, f.main, layer,
                                            PathValue::fromContour(square(10, 10, 40, true)));
            execute<commands::AddLayerMask>(f.document, f.commands, f.main, layer,
                                            PathValue::fromContour(square(50, 30, 30, false)));
            const auto source = f.addNode(document::kMaskPathNodeType);
            f.set(source, document::kPathSourceParameterRole,
                  static_cast<std::int64_t>(layerNode.value()));
            const auto stroke = f.addNode(document::kPathStrokeNodeType);
            f.set(stroke, "width", 5.0);
            f.connect(source, "path", stroke, "path");
            const auto shape = f.addNode(document::kShapeSourceNodeType);
            f.set(shape, "kind", static_cast<std::int64_t>(document::ShapeKind::Path));
            f.set(shape, "fillColor", core::Color4d{0.9, 0.8, 0.2, 1.0});
            f.connect(stroke, "path", shape, "path");
            f.enter(shape, f.mergeFeeding(layerNode), true);
            return stroke;
        },
        [](Fixture& f, const document::NodeId n) { f.set(n, "width", 9.0); });
    add(
        "text outline (stroked)",
        [](Fixture& f) {
            execute<commands::AddTextLayer>(f.document, f.commands, f.main, "Title", "Ag",
                                            Vec2d{48, 40}, 1.0, 30.0);
            const auto text = lastNode(f.document, f.main, document::kTextSourceNodeType);
            const auto textLayer = layerNode(f.document, f.main, lastLayer(f.document, f.main));
            const auto outline = f.addNode(document::kTextOutlineNodeType);
            const auto stroke = f.addNode(document::kPathStrokeNodeType);
            f.set(stroke, "width", 2.0);
            f.connect(outline, "path", stroke, "path");
            const auto shape = f.addNode(document::kShapeSourceNodeType);
            f.set(shape, "kind", static_cast<std::int64_t>(document::ShapeKind::Path));
            f.set(shape, "fillColor", core::Color4d{0.9, 0.2, 0.6, 1.0});
            f.connect(stroke, "path", shape, "path");
            // The text with its stroke over it, merged as the text layer's content.
            const auto merge = f.addNode(document::kLayerStackNodeType);
            f.enter(text, merge);
            f.enter(shape, merge, true);
            f.connect(merge, "image", textLayer, "image");
            return stroke;
        },
        [](Fixture& f, const document::NodeId n) { f.set(n, "width", 3.5); });
    return result;
}

// --montage <dir>: every case's CPU reference and GPU frame written side by side as 8-bit PPMs
// (over a dark grey, sRGB-encoded) for a reviewer's contact sheet. Off by default; the files are
// evidence for a person, never compared by the test.
std::filesystem::path& montageDirectory() {
    static std::filesystem::path directory;
    return directory;
}

void writePpm(const std::filesystem::path& file, const std::span<const render::Rgba32f> pixels,
              const std::size_t width, const std::size_t height) {
    std::ofstream out(file, std::ios::binary);
    out << "P6\n" << width << ' ' << height << "\n255\n";
    const auto encode = [](const float linear) {
        const float v = std::clamp(linear, 0.0F, 1.0F);
        const float srgb =
            v <= 0.0031308F ? 12.92F * v : 1.055F * std::pow(v, 1.0F / 2.4F) - 0.055F;
        return static_cast<char>(static_cast<unsigned char>(std::lround(srgb * 255.0F)));
    };
    for (std::size_t i = 0; i < width * height && i < pixels.size(); ++i) {
        const auto& p = pixels[i];
        const float under = 0.05F * (1.0F - std::clamp(p.alpha(), 0.0F, 1.0F));
        out.put(encode(p.red() + under));
        out.put(encode(p.green() + under));
        out.put(encode(p.blue() + under));
    }
}

void montagePair(const std::string& label, const Reference& reference,
                 const std::vector<render::Rgba32f>& gpu) {
    if (montageDirectory().empty())
        return;
    std::string name;
    for (const char c : label)
        name += std::isalnum(static_cast<unsigned char>(c)) ? c : '-';
    const auto window = reference.frame->processImage().descriptor()->dataWindow();
    const auto width = static_cast<std::size_t>(window.extent().width());
    const auto height = static_cast<std::size_t>(window.extent().height());
    writePpm(montageDirectory() / (name + "-cpu.ppm"), reference.frame->processImage().pixels(),
             width, height);
    writePpm(montageDirectory() / (name + "-gpu.ppm"), gpu, width, height);
}

// The GPU frame of a reference, for the montage only (gpuParity already proved it).
void montageCapture(runtime::GpuSceneExecutor& executor, const runtime::CpuGpuSceneBuilder& builder,
                    const Reference& reference, const std::string& label) {
    if (montageDirectory().empty())
        return;
    const auto prepared = builder.build(reference.plan, reference.request);
    if (prepared)
        montagePair(label, reference, runScene(executor, prepared.scene));
}

#ifdef BLOOM_GPUSHADER_TOOLS_DIR
// The pinned shader tools prepare Glow's working-space programs.
[[nodiscard]] runtime::GpuSceneOcioContext ocioContext() {
    runtime::GpuOcioCompileOptions options;
    options.glslangValidatorPath = std::string(BLOOM_GPUSHADER_TOOLS_DIR) + "/glslangValidator";
    options.spirvValPath = std::string(BLOOM_GPUSHADER_TOOLS_DIR) + "/spirv-val";
    runtime::GpuSceneOcioContext context;
    context.preparer = std::make_shared<runtime::GpuOcioProgramPreparer>();
    context.compileOptions = options;
    return context;
}

// gpuParity with the OCIO preparer an image effect needs.
void gpuParityWithOcio(Expectations& expectations, runtime::GpuSceneExecutor& executor,
                       const Reference& reference, const std::string& label) {
    const runtime::CpuGpuSceneBuilder builder(nullptr, {}, ocioContext());
    const auto prepared = builder.build(reference.plan, reference.request);
    expectations.expect(prepared.hasValue(), label + ": prepares: " + prepared.diagnostic.message);
    if (!prepared)
        return;
    const auto expected = reference.frame->processImage().pixels();
    const auto cold = runScene(executor, prepared.scene);
    expectations.expect(!cold.empty(),
                        label + ": the native run completes: " + executor.diagnostic().message);
    if (!parity(cold, expected) && cold.size() == expected.size()) {
        float worst = 0.0F;
        std::size_t at = 0;
        for (std::size_t i = 0; i < cold.size(); ++i)
            for (std::size_t c = 0; c < 4; ++c) {
                const float d = std::abs(cold[i].components()[c] - expected[i].components()[c]);
                if (d > worst) {
                    worst = d;
                    at = i;
                }
            }
        std::cerr << label << ": largest component difference " << worst << " at pixel " << at
                  << '\n';
        std::size_t wrong = 0;
        for (std::size_t i = 0; i < cold.size(); ++i)
            for (std::size_t c = 0; c < 4; ++c)
                if (std::abs(cold[i].components()[c] - expected[i].components()[c]) > 1e-4F) {
                    if (wrong++ < 6)
                        std::cerr << "  pixel " << i << " (" << i % 96 << ',' << i / 96 << ") c" << c
                                  << " gpu " << cold[i].components()[c] << " cpu "
                                  << expected[i].components()[c] << '\n';
                }
        std::cerr << "  components differing by more than 1e-4: " << wrong << '\n';
    }
    expectations.expect(parity(cold, expected), label + ": GPU pixels match the CPU oracle (2e-6)");
    const auto before = executor.counters();
    const auto warm = runScene(executor, prepared.scene);
    expectations.expect(parity(warm, expected) &&
                            executor.counters().dispatches == before.dispatches,
                        label + ": a warm rerun matches and dispatches nothing new");
}
#endif

// A genuinely cancelled token, taken from a scheduler task that is cancelled while it runs.
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
        TaskRequest("gpu path processing cancel token",
                    {.kind = TaskOwnerKind::Composition, .id = TaskOwnerId::fromRaw(98)}),
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

void cancellationAndBudget(Expectations& expectations, runtime::GpuSceneExecutor& executor,
                           const runtime::CpuGpuSceneBuilder& builder, const Reference& reference,
                           const std::string& label) {
    const auto cancelled = builder.build(reference.plan, reference.request, cancelledToken());
    expectations.expect(!cancelled.hasValue() &&
                            cancelled.diagnostic.code ==
                                runtime::PreparedGpuSceneDiagnosticCode::Cancelled,
                        label + ": a cancelled preparation reports Cancelled");
    auto tiny = reference.request;
    tiny.pixelStorageByteLimit = 64;
    expectations.expect(!builder.build(reference.plan, tiny).hasValue(),
                        label + ": a request below the frame's storage is refused");

    auto fresh = reference.request;
    fresh.time = core::RationalTime::fromInteger(3);
    const auto prepared = builder.build(reference.plan, fresh);
    expectations.expect(prepared.hasValue(), label + ": native cancel prepares");
    if (!prepared)
        return;
    const auto begun = executor.begin(prepared.scene, kSceneBudget);
    expectations.expect(begun.code == runtime::GpuSceneExecutorDiagnosticCode::None,
                        label + ": native cancel begins");
    executor.cancel();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (executor.state() == runtime::GpuSceneExecutorJobState::Pending &&
           std::chrono::steady_clock::now() < deadline) {
        (void)executor.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    expectations.expect(executor.state() != runtime::GpuSceneExecutorJobState::Ready &&
                            executor.image() == nullptr,
                        label + ": no image is published after cancel");
    while (executor.hasUnretiredSubmission() && std::chrono::steady_clock::now() < deadline) {
        (void)executor.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto after = runScene(executor, prepared.scene);
    auto oracle = fresh;
    oracle.bypassOperationCache = true;
    const auto expected = runtime::CpuCompositionEvaluator{}.evaluate(reference.plan, oracle, {});
    expectations.expect(expected.frame() != nullptr &&
                            parity(after, expected.frame()->processImage().pixels()),
                        label + ": the same executor completes the scene afterwards");
    const auto refused = executor.begin(prepared.scene, 64);
    expectations.expect(refused.code != runtime::GpuSceneExecutorDiagnosticCode::None,
                        label + ": the executor refuses a scene over its byte budget");
    if (refused.code == runtime::GpuSceneExecutorDiagnosticCode::None) {
        auto settle = executor.poll();
        while (settle == runtime::GpuSceneExecutorPollResult::Pending)
            settle = executor.poll();
        static_cast<void>(executor.takeImage());
    }
}

// What the CPU half holds for every scenario, and what the GPU half then runs.
struct Prepared final {
    std::string label;
    Reference base;
    std::optional<Reference> edited;   // after editing one operand
    std::optional<Reference> unrelated; // after an edit that reaches no node of the chain
    bool cancels = false;
};

} // namespace

int main(int argc, char** argv) {
    try {
        // --montage <dir> is this test's own; the shared options never see it.
        std::vector<char*> arguments;
        for (int i = 0; i < argc; ++i) {
            if (std::string_view(argv[i]) == "--montage" && i + 1 < argc) {
                montageDirectory() = argv[++i];
                std::filesystem::create_directories(montageDirectory());
                continue;
            }
            arguments.push_back(argv[i]);
        }
        argc = static_cast<int>(arguments.size());
        argv = arguments.data();
        const Options options = parseOptions(argc, argv);
        if (!options.valid) {
            std::cerr << "invalid arguments\n";
            return 2;
        }
        Expectations expectations;
        std::vector<Prepared> prepared;
        std::string lastFamily;

        // 1. Every node, every mode: the CPU reference, an edit of one operand and an unrelated one.
        for (const auto& scenario : scenarios()) {
            Fixture fixture;
            const auto node = scenario.build(fixture);
            auto base = referenceAt(expectations, fixture, 0, scenario.label);
            if (!base)
                continue;
            expectations.expect(!coverage(*base).empty(), scenario.label + ": draws something");
            Prepared entry{scenario.label, std::move(*base), {}, {}, false};
            const auto family = scenario.label.substr(0, scenario.label.find(" ("));
            entry.cancels = family != lastFamily;
            lastFamily = family;
            // An edit of a node's own operand changes the picture.
            scenario.edit(fixture, node);
            entry.edited = referenceAt(expectations, fixture, 0, scenario.label + " edited");
            expectations.expect(entry.edited && !samePixels(*entry.edited, entry.base),
                                scenario.label + ": editing one operand changes the CPU reference");
            // An unrelated edit (a value node nothing reads) changes no pixel.
            const auto unrelated = fixture.addNode(document::kScalarValueNodeType);
            fixture.set(unrelated, document::kValueParameterRole, 42.0);
            entry.unrelated = referenceAt(expectations, fixture, 0, scenario.label + " unrelated");
            expectations.expect(entry.unrelated && entry.edited &&
                                    samePixels(*entry.unrelated, *entry.edited),
                                scenario.label + ": an unrelated edit changes no pixel");
            prepared.push_back(std::move(entry));
        }

        // 2. HDR and alpha: a fill at alpha one half and a colour above one, through Offset Path.
        Fixture hdr;
        hdr.chain(document::kOffsetPathNodeType, framed(), {2.5, 0.4, 0.1, 0.5});
        hdr.set(lastNode(hdr.document, hdr.main, document::kOffsetPathNodeType), "amount", 5.0);
        auto hdrReference = referenceAt(expectations, hdr, 0, "hdr alpha");
        // 3. Data window: Transform Path moves the shape partly outside the frame.
        Fixture outside;
        const auto outsideChain = outside.chain(document::kTransformPathNodeType, framed());
        outside.set(outsideChain.processor, "rotation", 20.0);
        // The layer carrying the shape is moved so the path lies partly outside the frame.
        outside.set(layerNode(outside.document, outside.main, lastLayer(outside.document, outside.main)),
                    "position", Vec2d{-34, -26});
        auto outsideReference = referenceAt(expectations, outside, 0, "data window outside frame");
        // 4. Region of interest: Round Corners over a crop of the frame.
        Fixture region;
        const auto regionChain = region.chain(document::kRoundCornersNodeType, framed());
        region.set(regionChain.processor, "radius", 8.0);
        const auto window = render::ImageWindow::create(20, 16, 40, 34);
        auto regionReference = referenceAt(expectations, region, 0, "region of interest",
                                           window ? std::optional(*window.value()) : std::nullopt);
        auto regionFull = referenceAt(expectations, region, 0, "region of interest (full frame)");
        if (regionReference && regionFull) {
            // The region is the matching crop of the full frame, pixel for pixel.
            const auto& crop = regionReference->frame->processImage();
            const auto& full = regionFull->frame->processImage();
            const auto cropWindow = crop.descriptor()->dataWindow();
            const auto fullWindow = full.descriptor()->dataWindow();
            bool identical = cropWindow.extent().width() > 0;
            for (std::int64_t y = cropWindow.originY(); identical && y < cropWindow.maxYExclusive(); ++y)
                for (std::int64_t x = cropWindow.originX(); identical && x < cropWindow.maxXExclusive(); ++x) {
                    const auto a = crop.pixels()[static_cast<std::size_t>(
                        (y - cropWindow.originY()) *
                            static_cast<std::int64_t>(cropWindow.extent().width()) +
                        (x - cropWindow.originX()))];
                    const auto b = full.pixels()[static_cast<std::size_t>(
                        (y - fullWindow.originY()) *
                            static_cast<std::int64_t>(fullWindow.extent().width()) +
                        (x - fullWindow.originX()))];
                    identical = std::memcmp(a.components().data(), b.components().data(),
                                            4 * sizeof(float)) == 0;
                }
            expectations.expect(identical, "region of interest: the CPU region is the full frame's crop");
        }
        // 5. Path Stroke core -> Shape Source -> Glow.
        Fixture glow;
        const auto stroke = glow.chain(document::kPathStrokeNodeType, openLine());
        glow.set(stroke.processor, "width", 10.0);
        glow.set(stroke.processor, "coreWidth", 40.0);
        // The core, not the body, is what the shape draws; Glow sits between it and the layer.
        glow.connect(stroke.processor, "core", stroke.shape, "path");
        const auto glowNode = glow.addNode(document::kGlowNodeType);
        glow.connect(stroke.shape, "image", glowNode, "input");
        const auto layer = lastLayer(glow.document, glow.main);
        glow.connect(glowNode, "image", layerNode(glow.document, glow.main, layer), "image");
        auto glowReference = referenceAt(expectations, glow, 0, "path stroke core into glow");

        // 5b. An image effect or a compositing recipe over a vector leaf read as an image: a curved
        // Shape Source (its leaf raster, not a layer's), and a Text Source. The recipes' Merge
        // takes the leaf itself as an entry (Glow's Add, Drop Shadow's Normal), and Exposure keeps
        // the leaf's window, so a downstream Layer pivots where the CPU reference does.
        struct LeafEffect final {
            std::string label;
            Reference reference;
        };
        std::vector<LeafEffect> leafEffects;
        {
            struct Effect final {
                const char* type;
                const char* name;
                std::function<void(Fixture&, document::NodeId)> style;
            };
            const std::vector<Effect> effects{
                {"bloom.gaussian-blur", "gaussian blur",
                 [](Fixture& f, document::NodeId n) { f.set(n, "radius", 3.0); }},
                {"bloom.exposure", "exposure",
                 [](Fixture& f, document::NodeId n) { f.set(n, "amount", 1.0); }},
                {"bloom.drop-shadow", "drop shadow", [](Fixture&, document::NodeId) {}},
                {"bloom.glow", "glow (default threshold)", [](Fixture&, document::NodeId) {}},
                {"bloom.glow", "glow (threshold 0)",
                 [](Fixture& f, document::NodeId n) { f.set(n, "threshold", 0.0); }},
                {"bloom.glow", "glow only",
                 [](Fixture& f, document::NodeId n) {
                     f.set(n, "threshold", 0.0);
                     f.set(n, "glowOnly", true);
                 }},
                // #509 Alpha Contour: a band outside the edge over the leaf, and one alone.
                {"bloom.alpha-contour", "alpha contour",
                 [](Fixture& f, document::NodeId n) {
                     f.set(n, "width", 3.0);
                     f.set(n, "offset", 2.0);
                     f.set(n, "color", core::Color4d{2.0, 0.6, 0.2, 1.0});
                 }},
                {"bloom.alpha-contour", "alpha contour alone, soft, inward",
                 [](Fixture& f, document::NodeId n) {
                     f.set(n, "width", 2.0);
                     f.set(n, "softness", 2.0);
                     f.set(n, "offset", -2.0);
                     f.set(n, "composite", std::int64_t{2});
                 }}};
            for (const bool text : {false, true})
                for (const auto& effect : effects) {
                    Fixture f;
                    document::NodeId leaf;
                    if (text) {
                        execute<commands::AddTextLayer>(f.document, f.commands, f.main, "Label",
                                                        "Gly");
                        leaf = lastNode(f.document, f.main, document::kTextSourceNodeType);
                    } else {
                        const auto path = f.addNode(document::kPathValueNodeType);
                        f.set(path, document::kValueParameterRole, framed());
                        leaf = f.addShape({0.2, 0.5, 0.9, 1.0});
                        f.connect(path, document::kValuePortName, leaf, "path");
                    }
                    const auto node = f.addNode(effect.type);
                    f.connect(leaf, "image", node, "input");
                    effect.style(f, node);
                    f.connect(node, "image",
                              layerNode(f.document, f.main, lastLayer(f.document, f.main)),
                              "image");
                    const std::string label =
                        std::string(text ? "text" : "curved shape") + " -> " + effect.name;
                    if (auto reference = referenceAt(expectations, f, 0, label))
                        leafEffects.push_back({label, std::move(*reference)});
                    // #509: Alpha Contour over a region of interest, on the device as on the CPU.
                    if (!text && std::string_view(effect.name) == "alpha contour")
                        if (const auto crop = render::ImageWindow::create(20, 16, 40, 34))
                            if (auto cropped =
                                    referenceAt(expectations, f, 0, label + " (region of interest)",
                                                *crop.value()))
                                leafEffects.push_back(
                                    {label + " (region of interest)", std::move(*cropped)});
                }
        }

        // 5c. #509 the glowing path presets: each ready-made Node Group (Path Stroke, Shape
        // Source, Glow inside a group) drawn by a layer, at a moment Write-on and Vegas are moving.
        std::vector<LeafEffect> presets;
        for (const auto preset : commands::kPathGlowPresets) {
            Fixture f;
            execute<commands::CreatePathGlowPreset>(f.document, f.commands, f.main, preset,
                                                    Vec2d{0, 0});
            const auto instance = lastNode(f.document, f.main, document::kNodeGroupNodeType);
            execute<commands::AddSolidLayer>(f.document, f.commands, f.main, "Glow",
                                             core::Color4d{0.0, 0.0, 0.0, 1.0});
            f.connect(instance, "image1",
                      layerNode(f.document, f.main, lastLayer(f.document, f.main)), "image");
            const std::string label =
                "glowing path preset " + std::string(commands::pathGlowPresetName(preset));
            if (auto reference = referenceAt(expectations, f, 3, label)) {
                expectations.expect(!coverage(*reference).empty(), label + ": draws something");
                presets.push_back({label, std::move(*reference)});
            }
        }

        // 6. Keyed: Trim Path's end animates, then holds.
        Fixture keyed;
        const auto keyedChain = keyed.chain(document::kTrimPathNodeType, framed());
        // The operand is driven by a Scalar value node whose value is keyed: 10 at the start, 90 at
        // one second, held after.
        const auto driver = keyed.addNode(document::kScalarValueNodeType);
        keyed.key(driver, document::kValueParameterRole, 0, 10.0);
        keyed.key(driver, document::kValueParameterRole, 4, 90.0);
        keyed.connect(driver, document::kValuePortName, keyedChain.processor, "end");
        auto keyedStart = referenceAt(expectations, keyed, 0, "keyed start");
        auto keyedMiddle = referenceAt(expectations, keyed, 2, "keyed middle");
        auto keyedEnd = referenceAt(expectations, keyed, 4, "keyed end");
        auto keyedHolding = referenceAt(expectations, keyed, 6, "keyed holding");
        if (keyedStart && keyedMiddle && keyedEnd && keyedHolding)
            expectations.expect(!samePixels(*keyedStart, *keyedMiddle) &&
                                    !samePixels(*keyedMiddle, *keyedEnd) &&
                                    samePixels(*keyedEnd, *keyedHolding),
                                "a keyed Trim Path moves between its keys and holds after the last");

        if (expectations.failures() != 0) {
            std::cerr << expectations.failures() << " path processing expectation(s) failed\n";
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
            std::cout << "SKIP: CPU half passed; no compatible Vulkan device available: "
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
        const runtime::CpuGpuSceneBuilder builder;
#ifdef BLOOM_GPUSHADER_TOOLS_DIR
        // A layer's masks reach the frame through the image-effect route, whose working-space
        // programs the OCIO preparer makes, as the application always configures it.
        const runtime::CpuGpuSceneBuilder sceneBuilder(nullptr, {}, ocioContext());
#else
        const runtime::CpuGpuSceneBuilder& sceneBuilder = builder;
#endif

        for (auto& entry : prepared) {
#ifndef BLOOM_GPUSHADER_TOOLS_DIR
            if (entry.label.starts_with("mask path")) {
                std::cout << "NOTRUN: " << entry.label << " (shader tools unavailable)\n";
                expectations.expect(!options.require_device,
                                    "the layer-mask cases need the pinned shader tools");
                continue;
            }
#endif
            gpuParity(expectations, *executor.executor, entry.base, entry.label, sceneBuilder);
            // Cache identity: the edited state is a different scene and is dispatched; the
            // unrelated edit prepares the same scene and dispatches nothing new.
            if (entry.edited && entry.unrelated) {
                const auto scene = sceneBuilder.build(entry.edited->plan, entry.edited->request);
                expectations.expect(scene.hasValue(), entry.label + ": the edited state prepares");
                if (scene) {
                    const auto before = executor.executor->counters();
                    const auto pixels = runScene(*executor.executor, scene.scene);
                    expectations.expect(
                        parity(pixels, entry.edited->frame->processImage().pixels()),
                        entry.label + ": the edited frame matches the CPU reference");
                    expectations.expect(executor.executor->counters().dispatches > before.dispatches,
                                        entry.label + ": the edited frame is dispatched, not served stale");
                    const auto same =
                        sceneBuilder.build(entry.unrelated->plan, entry.unrelated->request);
                    expectations.expect(same.hasValue(),
                                        entry.label + ": the unrelated-edit state prepares");
                    if (same) {
                        const auto beforeSame = executor.executor->counters();
                        const auto samePixelsGpu = runScene(*executor.executor, same.scene);
                        expectations.expect(
                            parity(samePixelsGpu, entry.unrelated->frame->processImage().pixels()) &&
                                executor.executor->counters().dispatches == beforeSame.dispatches,
                            entry.label + ": an unrelated edit matches and dispatches nothing new");
                    }
                }
            }
            if (entry.cancels)
                cancellationAndBudget(expectations, *executor.executor, sceneBuilder, entry.base,
                                      entry.label);
        }
        if (hdrReference)
            gpuParity(expectations, *executor.executor, *hdrReference, "hdr alpha (colour above one, alpha one half)");
        if (outsideReference)
            gpuParity(expectations, *executor.executor, *outsideReference,
                      "data window: the path lies partly outside the frame");
        if (regionReference)
            gpuParity(expectations, *executor.executor, *regionReference, "region of interest");
#ifdef BLOOM_GPUSHADER_TOOLS_DIR
        // 6. Image effects and compositing recipes over a vector leaf read as an image.
        for (const auto& leaf : leafEffects)
            gpuParityWithOcio(expectations, *executor.executor, leaf.reference, leaf.label);
#else
        if (!leafEffects.empty()) {
            std::cout << "NOTRUN: image effects over vector leaves (shader tools unavailable)\n";
            expectations.expect(!options.require_device,
                                "the leaf effect cases need the pinned shader tools");
        }
#endif
        if (glowReference) {
#ifdef BLOOM_GPUSHADER_TOOLS_DIR
            gpuParityWithOcio(expectations, *executor.executor, *glowReference,
                              "path stroke core -> shape source -> glow");
#else
            std::cout
                << "NOTRUN: path stroke core -> shape source -> glow (shader tools unavailable)\n";
            expectations.expect(!options.require_device,
                                "the Glow chain needs the pinned shader tools");
#endif
        }
#ifdef BLOOM_GPUSHADER_TOOLS_DIR
        for (const auto& preset : presets)
            gpuParityWithOcio(expectations, *executor.executor, preset.reference, preset.label);
#else
        if (!presets.empty()) {
            std::cout << "NOTRUN: glowing path presets (shader tools unavailable)\n";
            expectations.expect(!options.require_device,
                                "the glowing path presets need the pinned shader tools");
        }
#endif
        // The reviewer's contact sheet: every case's CPU and GPU frames (only with --montage).
        if (!montageDirectory().empty()) {
            for (const auto& entry : prepared)
                montageCapture(*executor.executor, sceneBuilder, entry.base, entry.label);
#ifdef BLOOM_GPUSHADER_TOOLS_DIR
            const runtime::CpuGpuSceneBuilder ocioBuilder(nullptr, {}, ocioContext());
            for (const auto& leaf : leafEffects)
                montageCapture(*executor.executor, ocioBuilder, leaf.reference, leaf.label);
            for (const auto& preset : presets)
                montageCapture(*executor.executor, ocioBuilder, preset.reference, preset.label);
            if (glowReference)
                montageCapture(*executor.executor, ocioBuilder, *glowReference,
                               "path stroke core -> shape source -> glow");
#endif
        }
        if (keyedStart && keyedMiddle && keyedEnd && keyedHolding) {
            auto& run = *executor.executor;
            gpuParity(expectations, run, *keyedStart, "keyed start");
            gpuParity(expectations, run, *keyedMiddle, "keyed middle");
            gpuParity(expectations, run, *keyedEnd, "keyed end");
            const auto holding = builder.build(keyedHolding->plan, keyedHolding->request);
            expectations.expect(holding.hasValue(), "keyed holding prepares");
            if (holding) {
                const auto before = run.counters();
                const auto held = runScene(run, holding.scene);
                expectations.expect(parity(held, keyedHolding->frame->processImage().pixels()) &&
                                        run.counters().dispatches == before.dispatches,
                                    "a frame past the last key matches and dispatches nothing new");
            }
            // Different times give different frames, equal times equal frames.
            const auto middle = builder.build(keyedMiddle->plan, keyedMiddle->request);
            if (middle) {
                const auto before = run.counters();
                const auto moved = runScene(run, middle.scene);
                expectations.expect(run.counters().dispatches == before.dispatches ||
                                        parity(moved, keyedMiddle->frame->processImage().pixels()),
                                    "an animated Trim Path frame matches its CPU frame");
            }
        }

        if (expectations.failures() != 0) {
            std::cerr << expectations.failures() << " path processing expectation(s) failed\n";
            return 1;
        }
        std::cout << "PASS: Transform, Trim, Offset, Round Corners, Boolean, Merge and Path Stroke "
                     "match the CPU reference on the device, cold and warm, with cache identity, "
                     "invalidation, animation, HDR and alpha, data window, region of interest, "
                     "cancellation and budget, every brush stamp, a stroked Mask Path and Text "
                     "Outline, the Path Stroke core through Glow, image effects and Alpha Contour "
                     "over curved shapes and text, and every glowing path preset\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected test exception: " << exception.what() << '\n';
        return 1;
    }
}
