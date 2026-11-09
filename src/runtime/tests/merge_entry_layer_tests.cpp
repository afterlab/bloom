// #502 every Merge entry is a layer, and an Adjustment entry processes the stack below it. Real
// documents compiled by the production snapshot compiler and evaluated by the CPU reference:
//   * a Merge entry's own layer lowers to a Layer Output owned by its Merge, a legacy plain slot
//     lowers exactly as before, and a chain Layer (Layer -> effect -> Merge) gives the entry its
//     blend mode, enable and solo;
//   * an Adjustment's image is the stack below its entry, which the entry replaces: the issue's
//     pixel equivalences hold within the CPU/GPU parity tolerance, a chain reaching two entries is
//     a node-addressed warning that draws nothing, and cache identity covers the stack below;
//   * a Layer node's values equal the same values on the entry, a nested Merge at its defaults
//     equals the flattened stack, and a frame pivot is the identity for off-centre content.

#include "gpu_coverage_native_support.hpp"
#include "merge_entry_test_support.hpp"

#include <bloom/document/value_nodes.hpp>
#include <bloom/runtime/compiled_plan.hpp>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <source_location>
#include <string>
#include <variant>
#include <vector>

namespace {

using namespace bloom;
using merge_entry_test::Builder;
using merge_entry_test::compile;
using merge_entry_test::LayerValues;
using merge_entry_test::pixelsOf;
using merge_entry_test::require;

int failures = 0;

void expect(const bool condition, const std::string& message,
            const std::source_location location = std::source_location::current()) {
    if (condition)
        return;
    ++failures;
    std::cerr << location.line() << ": FAIL: " << message << '\n';
}

// The CPU/GPU parity tolerance the native gates use (gpu_coverage_native_support.hpp).
[[nodiscard]] bool same(const std::optional<std::vector<render::Rgba32f>>& left,
                        const std::optional<std::vector<render::Rgba32f>>& right,
                        std::string& detail) {
    if (!left || !right) {
        detail = " (an image did not evaluate)";
        return false;
    }
    detail = gpu_coverage_native::pixelParityDetail(*left, *right);
    return gpu_coverage_native::pixelsClose(*left, *right);
}

void expectSame(const document::Project& left, const document::Project& right,
                const std::string& message,
                const std::source_location location = std::source_location::current()) {
    std::string detail;
    const auto a = pixelsOf(left);
    const auto b = pixelsOf(right);
    expect(same(a, b, detail), message + detail, location);
}

[[nodiscard]] bool visiblyDifferent(const std::optional<std::vector<render::Rgba32f>>& left,
                                    const std::optional<std::vector<render::Rgba32f>>& right) {
    if (!left || !right || left->size() != right->size())
        return false;
    for (std::size_t index = 0; index < left->size(); ++index)
        if (std::fabs((*left)[index].red() - (*right)[index].red()) > 1e-3F ||
            std::fabs((*left)[index].alpha() - (*right)[index].alpha()) > 1e-3F)
            return true;
    return false;
}

template <typename Operation>
[[nodiscard]] std::vector<const Operation*>
operationsOf(const runtime::CompiledCompositionPlan& plan) {
    std::vector<const Operation*> found;
    for (const auto& operation : plan.operations())
        if (const auto* typed = std::get_if<Operation>(&operation))
            found.push_back(typed);
    return found;
}

[[nodiscard]] const runtime::CompiledMerge* mergeOf(const runtime::CompiledCompositionPlan& plan,
                                                    const document::NodeId node) {
    const runtime::CompiledMerge* found = nullptr;
    for (const auto* merge : operationsOf<runtime::CompiledMerge>(plan))
        if (merge->sourceNodeId == node)
            found = merge;
    return found;
}

constexpr core::Color4d kRed{0.8, 0.1, 0.05, 1.0};
constexpr core::Color4d kBlue{0.1, 0.2, 0.9, 1.0};
constexpr core::Color4d kGreen{0.2, 0.7, 0.3, 0.75};

// Bottom: a full red frame; above it a blue 6x4 block placed off-centre by a Layer node.
struct Stack final {
    document::NodeId red;
    document::NodeId blue;
    document::LayerId redLayer;
    document::LayerId blueLayer;
};

Stack baseStack(Builder& builder, const document::NodeId merge) {
    Stack stack;
    stack.blue = builder.solid(kBlue, 6.0, 4.0);
    const auto [blueNode, blueLayer] = builder.layerNode(
        stack.blue, {.position = {6.0, 5.0}, .pivot = document::LayerPivot::Content});
    stack.blueLayer = blueLayer;
    builder.slot(merge, blueNode, blueLayer);
    stack.red = builder.solid(kRed, 16.0, 12.0);
    const auto [redNode, redLayer] = builder.layerNode(
        stack.red, {.position = {8.0, 6.0}, .pivot = document::LayerPivot::Content});
    stack.redLayer = redLayer;
    builder.slot(merge, redNode, redLayer);
    return stack;
}

// --- compiler ---------------------------------------------------------------------------------

void legacyPlainSlotCompilesAsBefore() {
    Builder builder;
    const auto green = builder.solid(kGreen, 16.0, 12.0);
    const auto slot = builder.slot(builder.root(), green);
    static_cast<void>(baseStack(builder, builder.root()));
    const auto compiled = compile(builder.project());
    expect(compiled.plan != nullptr, "a document with a plain slot compiles");
    if (!compiled.plan)
        return;
    for (const auto* layer : operationsOf<runtime::CompiledLayerOutput>(*compiled.plan))
        expect(layer->sourceNodeId != builder.root(),
               "a plain slot without an entry layer adds no Layer Output");
    const auto* root = mergeOf(*compiled.plan, builder.root());
    expect(root != nullptr && root->entries.size() == 3 && root->entries[0].slotId == slot &&
               !root->entries[0].layerId.isValid() && !root->entries[0].blendMode &&
               std::holds_alternative<runtime::CompiledSolid>(
                   compiled.plan->operations()[root->entries[0].input.value()]),
           "a plain slot composites its source directly with no layer identity");
}

void entryLayerLowersToItsOwnLayerOutput() {
    Builder builder;
    static_cast<void>(baseStack(builder, builder.root()));
    const auto green = builder.solid(kGreen, 6.0, 6.0);
    const auto slot = builder.topSlot(builder.root(), green);
    const auto layer = builder.entryLayer(
        builder.root(), slot,
        {.position = {9.0, 7.0}, .opacity = 0.5, .blendMode = core::BlendMode::Screen});
    const auto compiled = compile(builder.project());
    expect(compiled.plan != nullptr, "a document with an entry layer compiles");
    if (!compiled.plan)
        return;
    const auto* root = mergeOf(*compiled.plan, builder.root());
    expect(root != nullptr && !root->entries.empty() && root->entries[0].layerId == layer,
           "the entry names its own layer");
    if (root == nullptr || root->entries.empty())
        return;
    const auto* lowered = std::get_if<runtime::CompiledLayerOutput>(
        &compiled.plan->operations()[root->entries[0].input.value()]);
    expect(
        lowered != nullptr && lowered->sourceNodeId == builder.root() &&
            lowered->layerId == layer && lowered->framePivot &&
            lowered->blendMode == core::BlendMode::Screen &&
            std::holds_alternative<runtime::CompiledSolid>(
                compiled.plan->operations()[lowered->input.value()]),
        "the entry layer is a frame-pivot Layer Output owned by its Merge over the slot content");
    expect(runtime::mergeEntryLayerIndex(*compiled.plan, root->entries[0]) ==
               root->entries[0].input,
           "the Merge reads the entry layer's blend mode");
}

void chainLayerGivesTheEntryItsBlendEnableAndSolo() {
    const auto build = [](const bool viaCurves, const core::BlendMode mode, const bool enabled,
                          const bool soloOther) {
        Builder builder;
        const auto stack = baseStack(builder, builder.root());
        const auto green = builder.solid(kGreen, 16.0, 12.0);
        const auto [layerNode, layer] =
            builder.layerNode(green, {.position = {8.0, 6.0}, .blendMode = mode});
        builder.boundary(layer).enabled = enabled;
        if (soloOther)
            builder.boundary(stack.redLayer).solo = true;
        auto source = layerNode;
        if (viaCurves) {
            // The default Curves is the identity: the chain changes nothing but its shape.
            source = builder.node(document::kCurvesNodeType, document::kCurvesNodeSchemaVersion);
            builder.connect(layerNode, "image", source, "input");
        }
        builder.topSlot(builder.root(), source, viaCurves ? document::LayerId{} : layer);
        return builder.project();
    };
    for (const auto mode : {core::BlendMode::Multiply, core::BlendMode::Difference}) {
        expectSame(build(true, mode, true, false), build(false, mode, true, false),
                   "Layer -> effect -> Merge composites with the Layer's blend mode");
        expect(visiblyDifferent(pixelsOf(build(true, mode, true, false)),
                                pixelsOf(build(true, core::BlendMode::Normal, true, false))),
               "the chain Layer's blend mode is visible");
    }
    expectSame(build(true, core::BlendMode::Normal, false, false),
               build(false, core::BlendMode::Normal, false, false),
               "a disabled chain Layer leaves its entry out");
    expectSame(build(true, core::BlendMode::Normal, true, true),
               build(false, core::BlendMode::Normal, true, true),
               "a solo elsewhere leaves a non-solo chain Layer's entry out");
    const auto compiled = compile(build(true, core::BlendMode::Multiply, true, false));
    if (compiled.plan) {
        const auto* root = mergeOf(*compiled.plan, document::NodeId::fromRaw(1000));
        const auto index = root ? runtime::mergeEntryLayerIndex(*compiled.plan, root->entries[0])
                                : runtime::OperationIndex::fromRaw(0);
        expect(root != nullptr && root->entries[0].layerId.isValid() &&
                   index != root->entries[0].input &&
                   std::holds_alternative<runtime::CompiledLayerOutput>(
                       compiled.plan->operations()[index.value()]),
               "the Merge walks back through the effect to the chain Layer's Layer Output");
    }
}

void entryLayerParentMatteAndMasksMatchLayerNodes() {
    // The same two layers as Layer nodes and as entry layers: the green one parented to the blue
    // one and matted by the red one, the blue one masked.
    const auto build = [](const bool entryLayers) {
        Builder builder;
        const auto red = builder.solid(kRed, 10.0, 8.0);
        const auto blue = builder.solid(kBlue, 6.0, 4.0);
        const auto green = builder.solid(kGreen, 5.0, 5.0);
        const LayerValues redValues{.position = {7.0, 5.0}, .pivot = document::LayerPivot::Content};
        const LayerValues blueValues{.position = {9.0, 7.0}, .rotation = 30.0};
        const LayerValues greenValues{.position = {7.5, 6.5}, .scale = {1.5, 0.75}};
        document::LayerId redLayer, blueLayer, greenLayer;
        if (entryLayers) {
            const auto greenSlot = builder.slot(builder.root(), green);
            const auto blueSlot = builder.slot(builder.root(), blue);
            const auto redSlot = builder.slot(builder.root(), red);
            greenLayer = builder.entryLayer(builder.root(), greenSlot, greenValues);
            blueLayer = builder.entryLayer(builder.root(), blueSlot, blueValues);
            redLayer = builder.entryLayer(builder.root(), redSlot, redValues);
        } else {
            const auto [greenNode, g] = builder.layerNode(green, greenValues);
            const auto [blueNode, b] = builder.layerNode(blue, blueValues);
            const auto [redNode, r] = builder.layerNode(red, redValues);
            greenLayer = g;
            blueLayer = b;
            redLayer = r;
            builder.slot(builder.root(), greenNode, g);
            builder.slot(builder.root(), blueNode, b);
            builder.slot(builder.root(), redNode, r);
        }
        builder.boundary(greenLayer).parent = blueLayer;
        builder.boundary(greenLayer).trackMatte =
            document::TrackMatte{redLayer, document::TrackMatteChannel::Alpha, false};
        document::LayerMask mask;
        mask.id = builder.id<document::LayerMaskId>();
        mask.name = "Mask";
        mask.mode = document::LayerMaskMode::Add;
        document::PathValue path;
        path.editableContour().closed = true;
        for (const auto& [x, y] :
             {std::pair{1.0, 0.5}, std::pair{5.0, 0.5}, std::pair{5.0, 3.5}, std::pair{1.0, 3.5}})
            path.editableContour().anchors.push_back({{x, y}, std::nullopt, std::nullopt});
        mask.path = builder.constant(document::kLayerMaskPathSchemaKey, path);
        mask.feather = builder.constant(document::kLayerMaskFeatherSchemaKey, 0.0);
        mask.expansion = builder.constant(document::kLayerMaskExpansionSchemaKey, 0.0);
        mask.opacity = builder.constant(document::kLayerMaskOpacitySchemaKey, 1.0);
        builder.boundary(blueLayer).masks.push_back(mask);
        return builder.project();
    };
    expectSame(build(true), build(false),
               "entry layers parent, matte and mask exactly as Layer nodes with the same values");
}

// #434 O7: parenting is a transform-only dependency, so a track matte whose source is a child of
// the matted layer compiles and evaluates, as Layer nodes and as entry layers alike.
void matteSourceMayBeChildOfTheMattedLayer() {
    const auto build = [](const bool entryLayers, const bool parented) {
        Builder builder;
        const auto target = builder.solid(kBlue, 10.0, 8.0);
        const auto matte = builder.solid(kRed, 6.0, 6.0);
        const LayerValues targetValues{.position = {8.0, 6.0},
                                       .pivot = document::LayerPivot::Content};
        const LayerValues matteValues{.position = {3.0, 2.0}, .rotation = 20.0};
        document::LayerId targetLayer, matteLayer;
        if (entryLayers) {
            const auto targetSlot = builder.slot(builder.root(), target);
            const auto matteSlot = builder.slot(builder.root(), matte);
            targetLayer = builder.entryLayer(builder.root(), targetSlot, targetValues);
            matteLayer = builder.entryLayer(builder.root(), matteSlot, matteValues);
        } else {
            const auto [targetNode, t] = builder.layerNode(target, targetValues);
            const auto [matteNode, m] = builder.layerNode(matte, matteValues);
            targetLayer = t;
            matteLayer = m;
            builder.slot(builder.root(), targetNode, t);
            builder.slot(builder.root(), matteNode, m);
        }
        if (parented)
            builder.boundary(matteLayer).parent = targetLayer;
        builder.boundary(targetLayer).trackMatte =
            document::TrackMatte{matteLayer, document::TrackMatteChannel::Alpha, false};
        return builder.project();
    };
    expectSame(build(true, true), build(false, true),
               "a matte from the target's child evaluates the same on entry layers and nodes");
    expect(visiblyDifferent(pixelsOf(build(false, true)), pixelsOf(build(false, false))),
           "the child matte follows its parent's transform");
}

// #622: a layer whose content extent cannot be resolved is reported once, however many times the
// compiler lowers it (its own slot, as a parent, as a matte source).
void unresolvedExtentIsReportedOncePerLayer() {
    Builder builder;
    const auto parentSource = builder.solid(kBlue, 6.0, 4.0);
    const auto childSource = builder.solid(kRed, 4.0, 4.0);
    const auto [parentNode, parentLayer] = builder.layerNode(parentSource, {});
    const auto [childNode, childLayer] = builder.layerNode(childSource, {});
    builder.slot(builder.root(), parentNode, parentLayer);
    builder.slot(builder.root(), childNode, childLayer);
    builder.boundary(childLayer).parent = parentLayer;
    builder.boundary(parentLayer).trackMatte =
        document::TrackMatte{childLayer, document::TrackMatteChannel::Alpha, false};
    for (const auto& [layer, source] : {std::pair{parentLayer, parentSource},
                                        std::pair{childLayer, childSource}}) {
        auto& timing = builder.boundary(layer).timing;
        timing.startTime = core::RationalTime::create(1, 2).value();
        timing.contentExtent = document::SourceLayerExtent{source};
    }
    const auto compiled = compile(builder.project());
    for (const auto layer : {parentLayer, childLayer}) {
        const auto count = std::ranges::count_if(compiled.diagnostics, [&](const auto& diagnostic) {
            return diagnostic.code == runtime::CompileDiagnosticCode::LayerExtentUnresolved &&
                   diagnostic.subject.layerId == layer;
        });
        expect(count == 1, "an unresolved extent is reported once, got " + std::to_string(count));
    }
}

void adjustmentDiagnosticsAndEmptyCases() {
    // An Adjustment whose chain reaches two entries: a node-addressed warning, it draws nothing.
    {
        Builder builder;
        const auto stack = baseStack(builder, builder.root());
        static_cast<void>(stack);
        const auto adjustment = builder.adjustment();
        const auto curves = builder.curves(adjustment);
        builder.topSlot(builder.root(), curves);
        const auto inner = builder.merge();
        const auto innerSolid = builder.solid(kGreen, 16.0, 12.0);
        builder.slot(inner, builder.curves(adjustment));
        builder.slot(inner, innerSolid);
        builder.slot(builder.root(), inner);
        const auto compiled = compile(builder.project());
        expect(compiled.plan != nullptr, "an Adjustment reaching two entries still compiles");
        const auto warning =
            std::ranges::find_if(compiled.diagnostics, [&](const auto& diagnostic) {
                return diagnostic.code ==
                           runtime::CompileDiagnosticCode::AdjustmentTargetAmbiguous &&
                       diagnostic.severity == runtime::DiagnosticSeverity::Warning &&
                       diagnostic.subject.nodeId == adjustment;
            });
        expect(warning != compiled.diagnostics.end(),
               "an Adjustment reaching two entries is a node-addressed warning");
        // It draws nothing: the same document without either adjusted slot.
        Builder reference;
        static_cast<void>(baseStack(reference, reference.root()));
        const auto referenceInner = reference.merge();
        reference.slot(referenceInner, reference.solid(kGreen, 16.0, 12.0));
        reference.slot(reference.root(), referenceInner);
        expectSame(builder.project(), reference.project(),
                   "an Adjustment reaching two entries draws nothing");
    }
    // A disconnected Adjustment, and one whose chain reaches the output but no entry.
    {
        Builder builder;
        static_cast<void>(baseStack(builder, builder.root()));
        static_cast<void>(builder.adjustment());
        Builder reference;
        static_cast<void>(baseStack(reference, reference.root()));
        expectSame(builder.project(), reference.project(),
                   "a disconnected Adjustment draws nothing");
        Builder direct;
        const auto curves = direct.curves(direct.adjustment());
        const auto compiled = compile(direct.project(curves));
        const runtime::CpuCompositionEvaluator evaluator;
        const auto evaluated = merge_entry_test::evaluate(evaluator, compiled.plan);
        expect(evaluated && std::ranges::all_of(evaluated->pixels,
                                                [](const auto& pixel) {
                                                    return pixel.alpha() == 0.0F &&
                                                           pixel.red() == 0.0F;
                                                }),
               "an Adjustment that lands in no Merge entry draws nothing");
    }
}

// --- the issue's pixel equivalences --------------------------------------------------------------

// Adjustment -> Curves at the top of a Merge = Merge -> Curves = Merge -> Curves -> the bottom
// entry of a second Merge holding the remaining layers.
void adjustmentEqualsCurvesAfterTheStack() {
    Builder adjusted;
    static_cast<void>(baseStack(adjusted, adjusted.root()));
    adjusted.topSlot(adjusted.root(), adjusted.curves(adjusted.adjustment()));

    Builder after;
    static_cast<void>(baseStack(after, after.root()));
    const auto afterCurves = after.curves(after.root());

    Builder nested;
    const auto inner = nested.merge();
    static_cast<void>(baseStack(nested, inner));
    nested.slot(nested.root(), nested.curves(inner));

    expectSame(adjusted.project(), after.project(afterCurves),
               "Adjustment -> Curves at the top of a Merge = Merge -> Curves");
    expectSame(adjusted.project(), nested.project(),
               "Adjustment -> Curves at the top = Merge -> Curves -> a second Merge's entry");
    expect(visiblyDifferent(pixelsOf(adjusted.project()),
                            [] {
                                Builder plain;
                                static_cast<void>(baseStack(plain, plain.root()));
                                return pixelsOf(plain.project());
                            }()),
           "the adjustment visibly changes the stack");

    // In the middle: the layers above stay over the adjusted stack below.
    Builder middle;
    const auto top = middle.solid(kGreen, 5.0, 5.0);
    const auto [topNode, topLayer] = middle.layerNode(top, {.position = {11.0, 4.0}});
    middle.slot(middle.root(), topNode, topLayer);
    middle.slot(middle.root(), middle.curves(middle.adjustment()));
    static_cast<void>(baseStack(middle, middle.root()));

    Builder split;
    const auto splitTop = split.solid(kGreen, 5.0, 5.0);
    const auto [splitTopNode, splitTopLayer] = split.layerNode(splitTop, {.position = {11.0, 4.0}});
    split.slot(split.root(), splitTopNode, splitTopLayer);
    const auto below = split.merge();
    static_cast<void>(baseStack(split, below));
    split.slot(split.root(), split.curves(below));
    expectSame(middle.project(), split.project(),
               "an Adjustment in the middle = the remaining layers over Curves of the stack below");
}

// An Adjustment at opacity o = the curved stack mixed over the unadjusted stack at o.
void adjustmentOpacityMixesTheAdjustedStack() {
    for (const double opacity : {0.0, 0.4, 1.0}) {
        Builder adjusted;
        static_cast<void>(baseStack(adjusted, adjusted.root()));
        const auto slot = adjusted.topSlot(adjusted.root(), adjusted.curves(adjusted.adjustment()));
        adjusted.entryLayer(adjusted.root(), slot,
                            {.position = adjusted.centre(), .opacity = opacity});

        Builder mixed;
        const auto stack = mixed.merge();
        static_cast<void>(baseStack(mixed, stack));
        const auto curves = mixed.curves(stack);
        const auto mix = mixed.node("bloom.image-mix", 1, {{"factor", opacity}});
        mixed.connect(stack, "image", mix, "input");
        mixed.connect(curves, "image", mix, "second");
        expectSame(adjusted.project(), mixed.project(mix),
                   "an Adjustment at opacity " + std::to_string(opacity) +
                       " = the curved stack mixed over the unadjusted stack");
    }
    // Disabled, the adjustment entry is absent: the stack below shows unadjusted.
    Builder disabled;
    static_cast<void>(baseStack(disabled, disabled.root()));
    const auto slot = disabled.topSlot(disabled.root(), disabled.curves(disabled.adjustment()));
    const auto layer = disabled.entryLayer(disabled.root(), slot, {.position = disabled.centre()});
    disabled.boundary(layer).enabled = false;
    Builder plain;
    static_cast<void>(baseStack(plain, plain.root()));
    expectSame(disabled.project(), plain.project(),
               "a disabled adjustment entry leaves the stack below unadjusted");
}

// The entry's blend mode blends the adjusted image over the stack below; masks and a moved
// coverage limit it to where the coverage is.
void adjustmentBlendAndCoverage() {
    Builder adjusted;
    static_cast<void>(baseStack(adjusted, adjusted.root()));
    const auto slot = adjusted.topSlot(adjusted.root(), adjusted.curves(adjusted.adjustment()));
    adjusted.entryLayer(adjusted.root(), slot,
                        {.position = adjusted.centre(), .blendMode = core::BlendMode::Screen});

    Builder reference;
    const auto below = reference.merge();
    static_cast<void>(baseStack(reference, below));
    const auto curves = reference.curves(below);
    const auto [screen, screenLayer] = reference.layerNode(
        curves, {.position = reference.centre(), .blendMode = core::BlendMode::Screen});
    reference.slot(reference.root(), screen, screenLayer);
    reference.slot(reference.root(), below);
    expectSame(adjusted.project(), reference.project(),
               "a Screen adjustment entry = the adjusted stack Screened over the stack below");

    // Coverage moved half off the frame: the left half adjusted, the right half not.
    Builder moved;
    static_cast<void>(baseStack(moved, moved.root()));
    const auto movedSlot = moved.topSlot(moved.root(), moved.curves(moved.adjustment()));
    moved.entryLayer(moved.root(), movedSlot, {.position = {0.0, 6.0}});
    Builder whole;
    static_cast<void>(baseStack(whole, whole.root()));
    whole.topSlot(whole.root(), whole.curves(whole.adjustment()));
    Builder none;
    static_cast<void>(baseStack(none, none.root()));
    const auto partial = pixelsOf(moved.project());
    const auto all = pixelsOf(whole.project());
    const auto unadjusted = pixelsOf(none.project());
    bool halves = partial && all && unadjusted;
    for (std::size_t y = 0; halves && y < 12; ++y)
        for (std::size_t x = 0; x < 16; ++x) {
            const auto index = y * 16 + x;
            const auto& expected = x < 8 ? *all : *unadjusted;
            halves = halves && std::fabs((*partial)[index].red() - expected[index].red()) < 1e-5F &&
                     (*partial)[index].alpha() == expected[index].alpha();
        }
    expect(halves, "an adjustment's coverage placed by its transform adjusts only where it lies");
}

// A Layer node's values = the same values set on the entry, when no filter follows the Layer node.
void layerNodeEqualsEntryLayer() {
    for (const auto pivot : {document::LayerPivot::Frame, document::LayerPivot::Content}) {
        const LayerValues values{.position = {9.5, 4.25},
                                 .anchor = {1.0, -0.5},
                                 .scale = {1.25, 0.8},
                                 .rotation = 17.0,
                                 .opacity = 0.7,
                                 .blendMode = core::BlendMode::Add,
                                 .pivot = pivot};
        Builder node;
        static_cast<void>(baseStack(node, node.root()));
        const auto green = node.solid(kGreen, 6.0, 4.0);
        const auto [layerNode, layer] = node.layerNode(green, values);
        node.topSlot(node.root(), layerNode, layer);

        Builder entry;
        static_cast<void>(baseStack(entry, entry.root()));
        const auto entryGreen = entry.solid(kGreen, 6.0, 4.0);
        const auto entrySlot = entry.topSlot(entry.root(), entryGreen);
        entry.entryLayer(entry.root(), entrySlot, values);
        expectSame(node.project(), entry.project(),
                   std::string("a Layer node's values = the same values on the entry (") +
                       (pivot == document::LayerPivot::Frame ? "frame" : "content") + " pivot)");
    }
}

// A nested Merge at default layer controls = the same stack flattened into one Merge.
void nestedMergeAtDefaultsEqualsFlattened() {
    const auto layers = [](Builder& builder, const document::NodeId merge, const int which) {
        const core::Color4d colors[] = {kGreen, kBlue, kRed, {0.9, 0.9, 0.2, 0.5}};
        const document::Vec2d positions[] = {{4.0, 3.0}, {9.0, 6.0}, {8.0, 6.0}, {12.0, 9.0}};
        const auto solid =
            builder.solid(colors[which], which == 2 ? 16.0 : 6.0, which == 2 ? 12.0 : 5.0);
        const auto [node, layer] = builder.layerNode(solid, {.position = positions[which]});
        builder.slot(merge, node, layer);
    };
    for (const bool explicitDefaults : {false, true}) {
        Builder nested;
        layers(nested, nested.root(), 0);
        const auto inner = nested.merge();
        layers(nested, inner, 1);
        layers(nested, inner, 2);
        const auto innerSlot = nested.slot(nested.root(), inner);
        layers(nested, nested.root(), 3);
        if (explicitDefaults)
            nested.entryLayer(nested.root(), innerSlot, {.position = nested.centre()});
        Builder flat;
        for (int which = 0; which < 4; ++which)
            layers(flat, flat.root(), which);
        expectSame(nested.project(), flat.project(),
                   std::string("a nested Merge at ") +
                       (explicitDefaults ? "an explicit default entry layer" : "default controls") +
                       " = the stack flattened");
    }
}

// A frame-pivot entry layer at its defaults is the identity whatever its content's bounds; scale
// and rotation turn about the frame centre.
void framePivotIsTheIdentityForOffCentreContent() {
    Builder plain;
    const auto block = plain.solid(kBlue, 6.0, 4.0);
    plain.slot(plain.root(), block);
    Builder layered;
    const auto layeredBlock = layered.solid(kBlue, 6.0, 4.0);
    const auto slot = layered.slot(layered.root(), layeredBlock);
    layered.entryLayer(layered.root(), slot, {.position = layered.centre()});
    expectSame(plain.project(), layered.project(),
               "a frame-pivot entry layer at its defaults is the identity for off-centre content");

    // Scale 2 about the frame centre (8, 6): the 6x4 block at the origin covers [-8, 4) x [-6, 2),
    // so (3, 1) is inside and (5, 1) is not. About its own centre it would cover [-3, 9) x [-2, 6).
    Builder scaled;
    const auto scaledBlock = scaled.solid(kBlue, 6.0, 4.0);
    const auto scaledSlot = scaled.slot(scaled.root(), scaledBlock);
    scaled.entryLayer(scaled.root(), scaledSlot,
                      {.position = scaled.centre(), .scale = {2.0, 2.0}});
    const auto pixels = pixelsOf(scaled.project());
    expect(pixels && (*pixels)[1 * 16 + 3].alpha() == 1.0F &&
               (*pixels)[1 * 16 + 5].alpha() == 0.0F && (*pixels)[3 * 16 + 1].alpha() == 0.0F,
           "a frame-pivot scale turns about the frame centre");
}

// Cache identity covers the stack below an Adjustment: an edit below re-runs the Adjustment and
// its chain; an edit to a layer above re-runs neither.
void cacheIdentityCoversTheStackBelow() {
    struct Variant final {
        double aboveOpacity = 1.0;
        double belowOpacity = 1.0;
    };
    document::NodeId adjustmentNode;
    document::NodeId curvesNode;
    const auto build = [&](const Variant variant) {
        Builder builder;
        const auto top = builder.solid(kGreen, 5.0, 5.0);
        const auto [topNode, topLayer] =
            builder.layerNode(top, {.position = {11.0, 4.0}, .opacity = variant.aboveOpacity});
        builder.slot(builder.root(), topNode, topLayer);
        adjustmentNode = builder.adjustment();
        curvesNode = builder.curves(adjustmentNode);
        builder.slot(builder.root(), curvesNode);
        const auto stack = baseStack(builder, builder.root());
        builder.set(builder.parameterOf(stack.blueLayer, document::kOpacityParameterRole),
                    variant.belowOpacity);
        return compile(builder.project()).plan;
    };
    const runtime::CpuCompositionEvaluator evaluator;
    const auto cold = merge_entry_test::evaluate(evaluator, build({}));
    const auto warm = merge_entry_test::evaluate(evaluator, build({}));
    expect(cold && warm && warm->statistics.misses == 0, "a warm adjusted frame reuses everything");
    const auto evaluated = [](const merge_entry_test::Evaluated& result,
                              const document::NodeId node) {
        return std::ranges::find(result.statistics.evaluatedNodes, node) !=
               result.statistics.evaluatedNodes.end();
    };
    const auto above = merge_entry_test::evaluate(evaluator, build({.aboveOpacity = 0.5}));
    expect(above && !evaluated(*above, adjustmentNode) && !evaluated(*above, curvesNode),
           "an edit to a layer above an Adjustment re-runs neither it nor its chain");
    const auto below = merge_entry_test::evaluate(evaluator, build({.belowOpacity = 0.5}));
    expect(below && evaluated(*below, adjustmentNode) && evaluated(*below, curvesNode),
           "an edit to a layer below an Adjustment re-runs it and its chain");
    expect(below && cold && visiblyDifferent(below->pixels, cold->pixels),
           "the edit below changes the adjusted picture");
}

// Consecutive raster Layer nodes concatenate into one resample
// (docs/architecture/layer-graph-model.md). Rotating raster content by 37 degrees at 1.25x and back
// by -37 degrees at 0.8x about the frame centre composes to the identity, so the single resample
// reproduces the content; two bilinear resamples would soften every edge of the blue block. The
// opacities multiply.
void consecutiveLayersResampleOnce() {
    const auto build = [](const bool chained, const double opacity) {
        Builder builder;
        const auto inner = builder.merge();
        baseStack(builder, inner);
        if (!chained) {
            const auto slot = builder.slot(builder.root(), inner);
            builder.entryLayer(builder.root(), slot,
                               {.position = builder.centre(), .opacity = opacity * opacity});
            return builder.project();
        }
        const auto [first, firstLayer] = builder.layerNode(inner, {.position = builder.centre(),
                                                                   .scale = {1.25, 1.25},
                                                                   .rotation = 37.0,
                                                                   .opacity = opacity});
        static_cast<void>(firstLayer);
        const auto [second, secondLayer] = builder.layerNode(first, {.position = builder.centre(),
                                                                     .scale = {0.8, 0.8},
                                                                     .rotation = -37.0,
                                                                     .opacity = opacity});
        builder.slot(builder.root(), second, secondLayer);
        return builder.project();
    };
    const auto within = [](const std::optional<std::vector<render::Rgba32f>>& left,
                           const std::optional<std::vector<render::Rgba32f>>& right) {
        if (!left || !right || left->size() != right->size())
            return false;
        for (std::size_t index = 0; index < left->size(); ++index)
            for (const auto [a, b] : {std::pair{(*left)[index].red(), (*right)[index].red()},
                                      std::pair{(*left)[index].green(), (*right)[index].green()},
                                      std::pair{(*left)[index].blue(), (*right)[index].blue()},
                                      std::pair{(*left)[index].alpha(), (*right)[index].alpha()}})
                if (std::fabs(a - b) > 1e-5F)
                    return false;
        return true;
    };
    for (const double opacity : {1.0, 0.5}) {
        const auto chained = pixelsOf(build(true, opacity));
        const auto direct = pixelsOf(build(false, opacity));
        expect(within(chained, direct),
               "two consecutive Layer nodes composing to the identity resample once (opacity " +
                   std::to_string(opacity) + ")");
    }
    // The content has edges a single 37-degree rotation visibly resamples.
    Builder rotated;
    const auto inner = rotated.merge();
    baseStack(rotated, inner);
    const auto [node, layer] =
        rotated.layerNode(inner, {.position = rotated.centre(), .rotation = 37.0});
    rotated.slot(rotated.root(), node, layer);
    expect(visiblyDifferent(pixelsOf(rotated.project()), pixelsOf(build(false, 1.0))),
           "the content changes under one rotation");
}

} // namespace

int main() {
    try {
        legacyPlainSlotCompilesAsBefore();
        entryLayerLowersToItsOwnLayerOutput();
        chainLayerGivesTheEntryItsBlendEnableAndSolo();
        entryLayerParentMatteAndMasksMatchLayerNodes();
        matteSourceMayBeChildOfTheMattedLayer();
        unresolvedExtentIsReportedOncePerLayer();
        adjustmentDiagnosticsAndEmptyCases();
        adjustmentEqualsCurvesAfterTheStack();
        adjustmentOpacityMixesTheAdjustedStack();
        adjustmentBlendAndCoverage();
        layerNodeEqualsEntryLayer();
        nestedMergeAtDefaultsEqualsFlattened();
        framePivotIsTheIdentityForOffCentreContent();
        cacheIdentityCoversTheStackBelow();
        consecutiveLayersResampleOnce();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "PASS: merge entry layers and adjustments\n";
    return 0;
}
