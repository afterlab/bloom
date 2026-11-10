// #509 Mask Path and Text Outline through the real compiler and the CPU reference evaluator. Each
// result is checked against an independent reference in the same frame: the very path the node
// should answer, authored on a Path node (for a keyed mask, keyed the same way; for text, the glyph
// outlines the render library gives for the text's own size and layout), drawn through the same
// Path Stroke or fill. The two frames must be bit-identical, and they must change when the mask,
// the key time, the text's size or its driven content does.
//
// Covered: a layer's masks (all, one, a keyed one between and at its keys), a Mask node, a stroked
// mask, a filled and a stroked text outline, a keyed text size, a driven text content, nothing to
// read (the empty path, not a failure), and an edit of the source undoing and redoing.

#include <bloom/commands/animation_operations.hpp>
#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/layer_mask_operations.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/layer_masks.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/node_definition_registry.hpp>
#include <bloom/document/path_value.hpp>
#include <bloom/document/shape.hpp>
#include <bloom/document/value_nodes.hpp>
#include <bloom/document/value_utility_nodes.hpp>
#include <bloom/render/text_raster.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>

#include <algorithm>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace bloom;
using document::PathAnchor;
using document::PathContour;
using document::PathValue;
using document::Vec2d;

int failures = 0;
void expect(const bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

PathContour quad(const double left, const double top, const double size, const double bulge) {
    PathContour contour;
    contour.closed = true;
    contour.anchors = {PathAnchor{Vec2d{left, top}, std::nullopt, std::nullopt},
                       PathAnchor{Vec2d{left + size, top}, std::nullopt,
                                  Vec2d{left + size + bulge, top + size * 0.5}},
                       PathAnchor{Vec2d{left + size, top + size}, std::nullopt, std::nullopt},
                       PathAnchor{Vec2d{left, top + size}, std::nullopt, std::nullopt}};
    return contour;
}

struct Fixture {
    document::NewProject initial = document::makeNewProject(
        "Path sources", "Main", core::RationalTime::fromInteger(2),
        *document::CompositionFormat::create(160, 120, core::PixelAspectRatio::square(),
                                             document::FrameRate::framesPerSecond24()));
    document::Document document{std::move(initial.project)};
    commands::CommandStack commands{document};
    document::CompositionId composition = initial.initialCompositionId;

    template <typename Operation, typename... Arguments>
    commands::CommandResult run(Arguments&&... arguments) {
        commands::Transaction transaction("Path sources", document.snapshot().revision());
        transaction.emplace<Operation>(std::forward<Arguments>(arguments)...);
        auto result = commands.execute(std::move(transaction));
        if (!result.succeeded()) {
            std::string why = "fixture command refused";
            for (const auto& failure : result.operationFailures)
                why += ": " + failure.issue.message;
            throw std::runtime_error(why);
        }
        return result;
    }
    [[nodiscard]] const document::Composition& current() const {
        return *document.snapshot().project().findComposition(composition);
    }
    document::NodeId add(const std::string_view type) {
        return *run<commands::AddNode>(composition, std::string(type), Vec2d{})
                    .outputId<document::NodeId>(commands::kAddNodeOutput);
    }
    document::ParameterId parameter(const document::NodeId id, const std::string_view role) const {
        for (const auto& binding : current().graph().findNode(id)->parameters)
            if (binding.role == role)
                return binding.parameterId;
        throw std::runtime_error("binding missing: " + std::string(role));
    }
    template <typename Value>
    Value constant(const document::NodeId id, const std::string_view role) {
        const auto* record = current().parameters().find(parameter(id, role));
        return std::get<Value>(std::get<document::ConstantValueSource>(record->source).value);
    }
    void set(const document::NodeId id, const std::string_view role,
             document::ParameterValue value) {
        run<commands::SetParameterSource>(composition, parameter(id, role),
                                          document::ConstantValueSource{std::move(value)});
    }
    void connect(const document::NodeId from, const std::string_view output,
                 const document::NodeId to, const std::string_view input) {
        const bool linked = std::ranges::any_of(current().graph().edges(), [&](const auto& edge) {
            const auto* target = std::get_if<document::NodeInputRef>(&edge.destination);
            return target != nullptr && target->nodeId == to && target->port == input;
        });
        if (linked)
            run<commands::DisconnectInput>(composition,
                                           document::NodeInputRef{to, std::string(input)});
        run<commands::ConnectPorts>(composition, document::OutputPortRef{from, std::string(output)},
                                    document::NodeInputRef{to, std::string(input)});
    }
    // A Shape layer whose Shape Source draws `from`'s `output`; answers the Shape Source.
    document::NodeId shape(const document::NodeId from, const std::string_view output,
                           const core::Color4d fill) {
        commands::ShapeLayerGeometry geometry;
        geometry.path = PathValue::fromContour(quad(1, 1, 2, 0));
        commands::ShapeLayerStyle style;
        style.fillColor = fill;
        run<commands::AddShapeLayer>(composition, document::ShapeKind::Path, geometry, style);
        document::NodeId node;
        for (const auto& record : current().graph().nodes())
            if (record.typeId == document::kShapeSourceNodeType)
                node = record.id;
        connect(from, output, node, "path");
        return node;
    }
    document::NodeId pathNode(PathValue value) {
        const auto node = add(document::kPathValueNodeType);
        set(node, document::kValueParameterRole, std::move(value));
        return node;
    }
};

struct Frame final {
    bool ok = false;
    std::vector<render::Rgba32f> pixels;
};

Frame render(const Fixture& fixture, const core::RationalTime time) {
    const runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
    const auto snapshot = fixture.document.snapshot();
    const auto compiled = compiler.compile({snapshot, fixture.composition}, {});
    if (!compiled.plan) {
        std::cerr << "compile: "
                  << runtime::describeCompileFailure(compiled, snapshot.project(),
                                                     fixture.composition)
                  << '\n';
        return {};
    }
    auto request =
        runtime::EvaluationRequest{.time = time,
                                   .output = compiled.plan->output(),
                                   .resolution = runtime::CompositionFormatResolution{},
                                   .quality = runtime::EvaluationQuality::Reference,
                                   .colorIntent = runtime::EvaluationColorIntent::LinearRec709Scene,
                                   .pixelStorageByteLimit = 64U << 20U};
    request.bypassOperationCache = true;
    const auto result = runtime::CpuCompositionEvaluator{}.evaluate(compiled.plan, request, {});
    if (!result.frame())
        return {};
    const auto pixels = result.frame()->processImage().pixels();
    return {true, {pixels.begin(), pixels.end()}};
}

bool sameBits(const Frame& a, const Frame& b) {
    return a.ok && b.ok && a.pixels.size() == b.pixels.size() &&
           std::memcmp(a.pixels.data(), b.pixels.data(),
                       a.pixels.size() * sizeof(render::Rgba32f)) == 0;
}
bool lit(const Frame& frame) {
    return frame.ok && std::ranges::any_of(frame.pixels, [](const render::Rgba32f& pixel) {
               return pixel.alpha() > 0.0F;
           });
}

core::RationalTime at(const std::int64_t frame) { return *core::RationalTime::create(frame, 24); }

void layerMasks() {
    Fixture f;
    const auto solid =
        f.run<commands::AddSolidLayer>(f.composition, "Plate", core::Color4d{0.2, 0.3, 0.9, 1.0});
    const auto layer = *solid.outputId<document::LayerId>(commands::kAddSolidLayerLayerOutput);
    const auto layerNode =
        *solid.outputId<document::NodeId>(commands::kAddSolidLayerLayerOutputNodeOutput);
    // Two masks, each one contour (a mask path is simple): A is keyed from one quad to another of
    // the same anchors.
    const PathValue a0 = PathValue::fromContour(quad(10, 10, 40, 12));
    const PathValue a1 = PathValue::fromContour(quad(30, 20, 60, -8));
    const PathValue b = PathValue::fromContour(quad(90, 60, 30, 6));
    // Keying a value equal to the constant it replaces is no edit, so the mask starts elsewhere.
    f.run<commands::AddLayerMask>(f.composition, layer, PathValue::fromContour(quad(0, 0, 8, 0)));
    f.run<commands::AddLayerMask>(f.composition, layer, b);
    document::ParameterId aPath;
    for (const auto& boundary : f.current().graph().layerOutputs())
        if (boundary.layerId == layer)
            aPath = boundary.masks.front().path;
    f.run<commands::SetKeyframeAtTimeForParameter>(f.composition, aPath, at(0), a0);
    f.run<commands::SetKeyframeAtTimeForParameter>(f.composition, aPath, at(12), a1);

    const auto maskPath = f.add(document::kMaskPathNodeType);
    f.set(maskPath, document::kPathSourceParameterRole,
          static_cast<std::int64_t>(layerNode.value()));
    const auto stroke = f.add(document::kPathStrokeNodeType);
    f.set(stroke, "width", 5.0);
    f.connect(maskPath, "path", stroke, "path");
    const auto shape = f.shape(stroke, "path", {1.0, 0.8, 0.2, 1.0});

    // The reference: the same paths on a Path node, keyed the same way.
    const auto concatenate = [&](const PathValue& first) {
        PathValue both = first;
        both.contours.insert(both.contours.end(), b.contours.begin(), b.contours.end());
        return both;
    };
    const auto reference = f.add(document::kPathValueNodeType);
    const auto referencePath = f.parameter(reference, document::kValueParameterRole);
    f.run<commands::SetKeyframeAtTimeForParameter>(f.composition, referencePath, at(0),
                                                   concatenate(a0));
    f.run<commands::SetKeyframeAtTimeForParameter>(f.composition, referencePath, at(12),
                                                   concatenate(a1));
    const auto compare = [&](const std::string& label, const std::int64_t frame) {
        f.connect(maskPath, "path", stroke, "path");
        const auto subject = render(f, at(frame));
        f.connect(reference, document::kValuePortName, stroke, "path");
        const auto expected = render(f, at(frame));
        f.connect(maskPath, "path", stroke, "path");
        expect(lit(subject) && sameBits(subject, expected),
               label + ": the stroked Mask Path is bit-identical to the same path on a Path node");
        return subject;
    };
    const auto start = compare("every mask, at the first key", 0);
    const auto middle = compare("every mask, between the keys", 6);
    const auto end = compare("every mask, at the last key", 12);
    expect(!sameBits(start, middle) && !sameBits(middle, end),
           "a keyed mask moves the Mask Path from frame to frame");

    // One mask: the second.
    f.set(maskPath, "mask", std::int64_t{2});
    f.connect(maskPath, "path", stroke, "path");
    const auto onlyB = render(f, at(6));
    const auto bNode = f.pathNode(b);
    f.connect(bNode, document::kValuePortName, stroke, "path");
    expect(lit(onlyB) && sameBits(onlyB, render(f, at(6))),
           "mask 2 is exactly the second mask's path");
    f.connect(maskPath, "path", stroke, "path");

    // An edit of the mask moves the Mask Path; undo puts it back.
    const auto before = render(f, at(6));
    const PathValue moved = PathValue::fromContour(quad(20, 70, 30, 6));
    document::ParameterId bPath;
    for (const auto& boundary : f.current().graph().layerOutputs())
        if (boundary.layerId == layer)
            bPath = boundary.masks.back().path;
    f.run<commands::SetParameterSource>(f.composition, bPath, document::ConstantValueSource{moved});
    expect(!sameBits(before, render(f, at(6))), "editing the mask moves the Mask Path");
    expect(f.commands.undo().succeeded() && sameBits(before, render(f, at(6))),
           "undoing the mask edit restores the Mask Path");

    // The source selector edits, undoes and redoes; a source that is gone reads nothing.
    // The empty path's frame, as the reference for a source that is gone.
    const auto emptyNode = f.pathNode(PathValue{});
    f.connect(emptyNode, document::kValuePortName, stroke, "path");
    const auto nothing = render(f, at(6));
    f.connect(maskPath, "path", stroke, "path");
    f.set(maskPath, document::kPathSourceParameterRole, std::int64_t{999999});
    const auto gone = render(f, at(6));
    expect(gone.ok && sameBits(gone, nothing) && !sameBits(gone, before),
           "a source that is gone reads the empty path, and the frame still renders");
    expect(f.commands.undo().succeeded() && sameBits(before, render(f, at(6))),
           "undoing the source edit reads the layer's masks again");
    expect(f.commands.redo().succeeded() && sameBits(gone, render(f, at(6))),
           "redoing the source edit reads nothing again");
    static_cast<void>(shape);
}

// A Mask node's path, filled.
void maskNode() {
    Fixture f;
    PathValue path = PathValue::fromContour(quad(20, 20, 70, 20));
    const auto mask = f.add(document::kMaskNodeType);
    f.set(mask, "path", path);
    const auto maskPath = f.add(document::kMaskPathNodeType);
    f.set(maskPath, document::kPathSourceParameterRole, static_cast<std::int64_t>(mask.value()));
    const auto shape = f.shape(maskPath, "path", {0.3, 0.9, 0.4, 1.0});
    const auto subject = render(f, at(0));
    f.connect(f.pathNode(path), document::kValuePortName, shape, "path");
    expect(lit(subject) && sameBits(subject, render(f, at(0))),
           "a Mask node's path, filled, is bit-identical to the same path on a Path node");
    // 0 reads the first node it can: the Mask node is the only one.
    f.set(maskPath, document::kPathSourceParameterRole, std::int64_t{0});
    f.connect(maskPath, "path", shape, "path");
    expect(sameBits(subject, render(f, at(0))), "source 0 reads the composition's first mask");
}

// Text Outline against the render library's outlines of the same text, laid out as it draws.
void textOutline() {
    Fixture f;
    const auto added =
        f.run<commands::AddTextLayer>(f.composition, "Title", "Ag", Vec2d{80, 60}, 1.0, 30.0);
    const auto keyFrom = 24.0; // the size before it is keyed (a key equal to it would be no edit)
    const auto text = *added.outputId<document::NodeId>(commands::kAddTextLayerTextNodeOutput);
    const auto outline = f.add(document::kTextOutlineNodeType);
    // Nothing chosen yet: 0 reads the composition's first text.
    const auto shape = f.shape(outline, "path", {0.9, 0.2, 0.6, 1.0});
    const auto stroke = f.add(document::kPathStrokeNodeType);
    f.set(stroke, "width", 2.0);
    f.connect(outline, "path", stroke, "path");

    const auto outlinesOf = [&](const std::string& content, const double size) {
        render::TextLayoutOptions layout;
        layout.alignment = static_cast<render::TextAlignment>(
            f.constant<std::int64_t>(text, document::kTextAlignmentParameterRole));
        layout.lineHeight = f.constant<double>(text, document::kTextLineHeightParameterRole);
        layout.letterSpacing = f.constant<double>(text, document::kTextLetterSpacingParameterRole);
        layout.multiline = true;
        layout.anchorMode = render::resolveTextAnchorMode(
            f.constant<std::int64_t>(text, document::kTextAnchorModeParameterRole),
            layout.alignment);
        const auto parameters = render::TextRasterParameters::create(size, size);
        const auto paths = render::textOutlines(render::EmbeddedFace::DejaVuSans, content,
                                                *parameters.value(), layout);
        PathValue value;
        for (const auto& path : *paths.value()) {
            PathContour contour;
            contour.closed = path.closed;
            for (const auto& anchor : path.anchors) {
                PathAnchor each{{anchor.point.x, anchor.point.y}, std::nullopt, std::nullopt};
                if (anchor.inHandle)
                    each.inHandle = Vec2d{anchor.inHandle->x, anchor.inHandle->y};
                if (anchor.outHandle)
                    each.outHandle = Vec2d{anchor.outHandle->x, anchor.outHandle->y};
                each.mode = document::inferPathAnchorMode(each);
                contour.anchors.push_back(each);
            }
            value.contours.push_back(std::move(contour));
        }
        return value;
    };
    const auto reference = f.add(document::kPathValueNodeType);
    const auto compare = [&](const std::string& label, const std::string& content,
                             const double size, const std::int64_t frame,
                             const std::string_view output, const document::NodeId into,
                             const std::string_view input) {
        f.set(reference, document::kValueParameterRole, outlinesOf(content, size));
        f.connect(outline, "path", into, input);
        const auto subject = render(f, at(frame));
        f.connect(reference, document::kValuePortName, into, input);
        const auto expected = render(f, at(frame));
        f.connect(outline, "path", into, input);
        static_cast<void>(output);
        expect(lit(subject) && sameBits(subject, expected),
               label + ": bit-identical to the render library's outlines on a Path node");
        return subject;
    };
    const auto filled = compare("filled text outline", "Ag", 30.0, 0, "path", shape, "path");
    // The stroked outline.
    f.connect(stroke, "path", shape, "path");
    f.set(reference, document::kValueParameterRole, outlinesOf("Ag", 30.0));
    f.connect(outline, "path", stroke, "path");
    const auto stroked = render(f, at(0));
    f.connect(reference, document::kValuePortName, stroke, "path");
    expect(lit(stroked) && sameBits(stroked, render(f, at(0))) && !sameBits(stroked, filled),
           "stroked text outline: bit-identical to stroking the render library's outlines");
    f.connect(outline, "path", shape, "path");

    // A keyed size moves the outline at each key.
    const auto size = f.parameter(text, document::kTextSizeParameterRole);
    f.set(text, document::kTextSizeParameterRole, keyFrom);
    f.run<commands::SetKeyframeAtTimeForParameter>(f.composition, size, at(0), 30.0);
    f.run<commands::SetKeyframeAtTimeForParameter>(f.composition, size, at(12), 48.0);
    const auto small = compare("keyed size, first key", "Ag", 30.0, 0, "path", shape, "path");
    const auto large = compare("keyed size, last key", "Ag", 48.0, 12, "path", shape, "path");
    expect(!sameBits(small, large), "a keyed text size moves the outline");

    // Driven content: a String node drives the text, and the outline follows it.
    const auto word = f.add(document::kStringValueNodeType);
    f.set(word, document::kValueParameterRole, std::string("Hi"));
    f.connect(word, document::kValuePortName, text, document::kTextParameterRole);
    const auto driven = compare("driven content", "Hi", 30.0, 0, "path", shape, "path");
    expect(!sameBits(driven, small), "driven text content changes the outline");

    // Nothing to read: a Text Outline in a composition with no text is the empty path.
    Fixture empty;
    const auto lone = empty.add(document::kTextOutlineNodeType);
    const auto lonely = empty.shape(lone, "path", {1, 1, 1, 1});
    static_cast<void>(lonely);
    const auto none = render(empty, at(0));
    expect(none.ok && !lit(none),
           "with no text to read Text Outline draws nothing and fails nothing");
}

// The outline lies on the letters. A red "Ag" and a Shape filling its Text Outline in green are the
// two entries of one Merge, each entry at its defaults (the identity), so both draw in the text's
// own space, and the Merge is the text layer's content. Whichever entry is on top must hide the
// other completely: a pixel showing only the entry underneath would be ink of one that the other
// lacks. Edge pixels, where both are partly covered, never show one colour alone.
struct Exposure final {
    bool ok = false;
    int red = 0;   // pixels showing only the text
    int green = 0; // pixels showing only the outline's fill
};

Exposure mergedTextAndOutline(const bool outlineConnectedFirst) {
    Fixture f;
    const auto added = f.run<commands::AddTextLayer>(f.composition, "Title", "Ag", Vec2d{80, 60},
                                                     1.0, 40.0, core::Color4d{1.0, 0.0, 0.0, 1.0});
    const auto text = *added.outputId<document::NodeId>(commands::kAddTextLayerTextNodeOutput);
    const auto layer =
        *added.outputId<document::NodeId>(commands::kAddTextLayerLayerOutputNodeOutput);
    const auto outline = f.add(document::kTextOutlineNodeType);
    const auto fill = f.add(document::kShapeSourceNodeType);
    f.set(fill, "kind", static_cast<std::int64_t>(document::ShapeKind::Path));
    f.set(fill, "fillColor", core::Color4d{0.0, 1.0, 0.0, 1.0});
    f.connect(outline, "path", fill, "path");
    const auto merge = f.add(document::kLayerStackNodeType);
    const auto enter = [&](const document::NodeId source) {
        f.run<commands::ConnectPorts>(
            f.composition, document::OutputPortRef{source, "image"},
            document::InputPortRef{document::LayerStackInputRef{merge, {}, "content"}});
    };
    if (outlineConnectedFirst) {
        enter(fill);
        enter(text);
    } else {
        enter(text);
        enter(fill);
    }
    f.connect(merge, "image", layer, std::string(document::kLayerOutputContentInputPort));
    const auto frame = render(f, at(0));
    Exposure exposure{frame.ok};
    for (const auto& pixel : frame.pixels) {
        if (pixel.red() > 0.6F && pixel.green() < 0.4F)
            ++exposure.red;
        if (pixel.green() > 0.6F && pixel.red() < 0.4F)
            ++exposure.green;
    }
    return exposure;
}

void textOutlineOnTheLetters() {
    const auto first = mergedTextAndOutline(true);
    const auto second = mergedTextAndOutline(false);
    expect(first.ok && second.ok, "the text and its outline render as one Merge");
    // In each order one entry covers the frame's ink and the other shows nowhere on its own.
    expect(std::min(first.red, first.green) == 0 && std::min(second.red, second.green) == 0,
           "the outline's fill covers exactly the text's letters (exposed: " +
               std::to_string(first.red) + "/" + std::to_string(first.green) + ", " +
               std::to_string(second.red) + "/" + std::to_string(second.green) + ")");
    expect(std::max(first.red, first.green) > 100 && std::max(second.red, second.green) > 100 &&
               (first.red > first.green) != (second.red > second.green),
           "each order puts a different entry on top, over the whole of the letters");
}

} // namespace

int main() {
    try {
        layerMasks();
        maskNode();
        textOutline();
        textOutlineOnTheLetters();
    } catch (const std::exception& error) {
        std::cerr << "Unexpected exception: " << error.what() << '\n';
        return 1;
    }
    if (failures != 0)
        return 1;
    std::cout << "PASS: Mask Path and Text Outline match their independent references\n";
    return 0;
}
