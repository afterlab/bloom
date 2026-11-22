// #760: an export output's background. The snapshot compiler lowers SnapshotCompileRequest's
// exportBackground into a full-frame constant Solid at the bottom of a Normal Merge under the
// Composition Output, so the CPU reference (and the GPU route, which prepares the same plan)
// composites the frame over the composition's Background Color in the working space. These tests
// hold the CPU reference to "the frame over an opaque constant", keep a transparent export's plan
// and pixels unchanged, keep a nested composition's plan free of its own background, and key the
// plan cache on the background.

#include "merge_entry_test_support.hpp"

#include <bloom/document/graph.hpp>
#include <bloom/runtime/compiled_plan_cache.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>

#include <cmath>
#include <cstring>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace {

using namespace bloom;
using merge_entry_test::Builder;

int failures = 0;

void expect(const bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

constexpr core::Color4d kBackground{0.2, 0.4, 0.6, 1.0};

// A translucent layer over a partly covering opaque one, leaving a transparent strip.
[[nodiscard]] document::Project translucentProject() {
    Builder builder;
    const auto blue = builder.solid({0.1, 0.2, 0.9, 0.75}, 6.0, 4.0);
    const auto [blueNode, blueLayer] = builder.layerNode(
        blue, {.position = {6.0, 5.0}, .pivot = document::LayerPivot::Content});
    builder.slot(builder.root(), blueNode, blueLayer);
    const auto red = builder.solid({0.8, 0.1, 0.05, 0.5}, 8.0, 12.0);
    const auto [redNode, redLayer] = builder.layerNode(red, {.position = {4.0, 6.0}});
    builder.slot(builder.root(), redNode, redLayer);
    return builder.project();
}

[[nodiscard]] runtime::SnapshotCompileResult
compileWith(const document::Document& document, const document::CompositionId composition,
            const std::optional<core::Color4d> background) {
    const runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
    runtime::SnapshotCompileRequest request{document.snapshot(), composition};
    request.exportBackground = background;
    return compiler.compile(request, {});
}

[[nodiscard]] std::optional<std::vector<render::Rgba32f>>
pixels(const std::shared_ptr<const runtime::CompiledCompositionPlan>& plan,
       const core::RationalTime time = {}) {
    if (!plan)
        return std::nullopt;
    const runtime::CpuCompositionEvaluator evaluator;
    auto request = merge_entry_test::request(*plan, time);
    request.bypassOperationCache = true;
    const auto result = evaluator.evaluate(plan, request, {});
    if (!result.frame())
        return std::nullopt;
    const auto values = result.frame()->processImage().pixels();
    return std::vector<render::Rgba32f>(values.begin(), values.end());
}

void testCompositesOverTheBackground() {
    const document::Document document(translucentProject());
    const auto plain = compileWith(document, merge_entry_test::kCompositionId, std::nullopt);
    const auto backed = compileWith(document, merge_entry_test::kCompositionId, kBackground);
    expect(plain.plan && backed.plan, "both plans compile");
    if (!plain.plan || !backed.plan)
        return;
    expect(backed.plan->operations().size() == plain.plan->operations().size() + 2,
           "the background adds one Solid and one Merge");
    const auto& output = backed.plan->operations()[backed.plan->output().value()];
    const auto* finished = std::get_if<runtime::CompiledCompositionOutput>(&output);
    expect(finished != nullptr, "the plan still ends in the Composition Output");
    if (finished == nullptr)
        return;
    const auto* merge =
        std::get_if<runtime::CompiledMerge>(&backed.plan->operations()[finished->input.value()]);
    expect(merge != nullptr && merge->entries.size() == 2,
           "the Composition Output reads a two-entry Merge");
    if (merge == nullptr || merge->entries.size() != 2)
        return;
    const auto* solid =
        std::get_if<runtime::CompiledSolid>(&backed.plan->operations()[merge->entries[1].input.value()]);
    const auto* color = solid != nullptr ? std::get_if<core::Color4d>(&solid->color.source) : nullptr;
    const auto* width = solid != nullptr ? std::get_if<double>(&solid->width.source) : nullptr;
    const auto* height = solid != nullptr ? std::get_if<double>(&solid->height.source) : nullptr;
    expect(color != nullptr && *color == kBackground && width != nullptr && *width == 16.0 &&
               height != nullptr && *height == 12.0,
           "the bottom entry is a full-frame Solid of the background");
    const auto before = pixels(plain.plan);
    const auto after = pixels(backed.plan);
    expect(before && after && before->size() == after->size(), "both plans evaluate");
    if (!before || !after || before->size() != after->size())
        return;
    bool over = true;
    bool opaque = true;
    bool someTransparent = false;
    for (std::size_t index = 0; index < before->size(); ++index) {
        const auto& top = (*before)[index];
        const auto& out = (*after)[index];
        someTransparent = someTransparent || top.alpha() == 0.0F;
        const auto cover = 1.0 - static_cast<double>(top.alpha());
        const auto close = [](const float value, const double want) {
            return std::abs(static_cast<double>(value) - want) <= 1e-6;
        };
        const auto wide = [](const float value) { return static_cast<double>(value); };
        over = over && close(out.red(), wide(top.red()) + kBackground.red * cover) &&
               close(out.green(), wide(top.green()) + kBackground.green * cover) &&
               close(out.blue(), wide(top.blue()) + kBackground.blue * cover);
        opaque = opaque && close(out.alpha(), 1.0);
    }
    expect(someTransparent, "the fixture leaves transparent pixels");
    expect(over, "every pixel is the frame over the opaque background (premultiplied over)");
    expect(opaque, "every pixel is opaque");
}

void testTransparentIsUnchanged() {
    const document::Document document(translucentProject());
    const auto plain = compileWith(document, merge_entry_test::kCompositionId, std::nullopt);
    const auto reference = merge_entry_test::compile(translucentProject());
    expect(plain.plan && reference.plan &&
               plain.plan->operations().size() == reference.plan->operations().size(),
           "a transparent export compiles the plan every other caller does");
    const auto a = pixels(plain.plan);
    const auto b = pixels(reference.plan);
    expect(a && b && a->size() == b->size() &&
               std::memcmp(a->data(), b->data(), a->size() * sizeof(render::Rgba32f)) == 0,
           "a transparent export's pixels are byte-identical");
}

// A nested composition: an 8x8 parent reads a 4x4 child holding a 2x2 half-transparent solid.
[[nodiscard]] document::Project nestedProject() {
    using namespace document;
    const auto format = [](const std::uint32_t size) {
        return *CompositionFormat::create(size, size, core::PixelAspectRatio::square(),
                                          *FrameRate::create(24, 1));
    };
    const auto addNode = [](Composition& composition, const std::string_view type,
                            const std::uint64_t raw,
                            const std::vector<std::pair<std::string, ParameterValue>>& values) {
        const auto* definition = builtInNodeDefinitions().find(
            type, std::ranges::find(builtInNodeDefinitions().definitions(), type,
                                    [](const auto& item) { return item.key.typeId; })
                      ->key.schemaVersion);
        NodeRecord record{NodeId::fromRaw(raw), std::string(type), {}, definition->key.schemaVersion};
        std::uint64_t index = 0;
        for (const auto& schema : definition->parameters) {
            if (schema.schemaKey == kTimeRemapParameterSchemaKey)
                continue;
            const auto found = std::ranges::find(
                values, schema.role, &std::pair<std::string, ParameterValue>::first);
            if (!schema.required && found == values.end())
                continue;
            const auto id = ParameterId::fromRaw(raw * 100 + index++);
            merge_entry_test::require(
                composition.parameters().insert(
                    {id, schema.schemaKey,
                     ConstantValueSource{found == values.end() ? schema.defaultValue
                                                               : found->second}}),
                "fixture parameter");
            record.parameters.push_back({schema.role, id});
        }
        merge_entry_test::require(composition.graph().addNode(std::move(record)), "fixture node");
        return NodeId::fromRaw(raw);
    };
    const auto finish = [](Composition& composition, const NodeId source, const NodeId output,
                           const std::uint64_t edge) {
        merge_entry_test::require(
            composition.graph().addEdge(
                {EdgeId::fromRaw(edge), {source, "image"}, NodeInputRef{output, "image"}}),
            "fixture edge");
        composition.graph().setCompositionOutput({output, "image"});
    };
    Composition child(CompositionId::fromRaw(1), "Child", core::RationalTime::fromInteger(2),
                      CanonicalGraph(NodeId{}), format(4));
    child.setBackgroundColor({0.9, 0.1, 0.1, 1.0});
    const auto childOutput = addNode(child, kCompositionOutputNodeType, 101, {});
    const auto childSolid = addNode(child, kSolidSourceNodeType, 102,
                                    {{std::string(kSolidColorParameterRole),
                                      core::Color4d{0.0, 1.0, 0.0, 0.5}},
                                     {std::string(kSolidWidthParameterRole), 2.0},
                                     {std::string(kSolidHeightParameterRole), 2.0}});
    finish(child, childSolid, childOutput, 103);
    Composition parent(CompositionId::fromRaw(2), "Parent", core::RationalTime::fromInteger(2),
                       CanonicalGraph(NodeId{}), format(8));
    const auto parentOutput = addNode(parent, kCompositionOutputNodeType, 201, {});
    const auto source = addNode(parent, kCompositionSourceNodeType, 202,
                                {{"composition", std::int64_t{1}}});
    finish(parent, source, parentOutput, 203);
    Project project(ProjectId::fromRaw(1), "Nested background");
    merge_entry_test::require(project.addComposition(std::move(child)) &&
                                  project.addComposition(std::move(parent)),
                              "fixture compositions");
    const auto validation = project.validate();
    for (const auto& issue : validation.issues())
        std::cerr << "invalid fixture: " << issue.path << ": " << issue.message << '\n';
    merge_entry_test::require(validation.ok(), "nested fixture is valid");
    return project;
}

void testNestedCompositionIgnoresItsBackground() {
    const document::Document document(nestedProject());
    const auto backed = compileWith(document, document::CompositionId::fromRaw(2), kBackground);
    const auto child = compileWith(document, document::CompositionId::fromRaw(1), std::nullopt);
    expect(backed.plan && child.plan, "parent and child compile");
    if (!backed.plan || !child.plan)
        return;
    expect(backed.plan->nestedPlans().size() == 1 &&
               backed.plan->nestedPlans().front()->operations().size() ==
                   child.plan->operations().size(),
           "the nested plan is the child's own plan, without a background");
    const auto values = pixels(backed.plan);
    expect(values && values->size() == 64, "the parent evaluates");
    if (!values || values->size() != 64)
        return;
    bool opaque = true;
    bool parentColor = false;
    bool childColor = false;
    for (const auto& value : *values) {
        opaque = opaque && std::abs(value.alpha() - 1.0F) <= 1e-6F;
        parentColor = parentColor || (std::abs(value.red() - 0.2F) <= 1e-6F &&
                                      std::abs(value.green() - 0.4F) <= 1e-6F &&
                                      std::abs(value.blue() - 0.6F) <= 1e-6F);
        childColor = childColor || value.red() > 0.5F;
    }
    expect(opaque, "the parent's export is opaque");
    expect(parentColor, "empty pixels show the parent's background");
    expect(!childColor, "the child's Background Color is never drawn");
}

void testPlanCacheKeysTheBackground() {
    const document::Document document(translucentProject());
    const runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
    runtime::CompiledPlanCache cache;
    runtime::SnapshotCompileRequest viewer{document.snapshot(), merge_entry_test::kCompositionId};
    auto exported = viewer;
    exported.exportBackground = kBackground;
    auto other = viewer;
    other.exportBackground = core::Color4d{0.0, 0.0, 0.0, 1.0};
    const auto a = cache.compile(compiler, viewer, {});
    const auto b = cache.compile(compiler, exported, {});
    const auto c = cache.compile(compiler, other, {});
    const auto d = cache.compile(compiler, exported, {});
    const auto e = cache.compile(compiler, viewer, {});
    expect(a.plan && b.plan && c.plan && d.plan && e.plan, "every request compiles");
    expect(a.plan != b.plan && b.plan != c.plan && a.plan != c.plan,
           "the Viewer plan and each background have their own plan");
    expect(cache.statistics().compiles == 3, "each distinct request compiles once");
    (void)d;
    (void)e;
}

} // namespace

int main() {
    try {
        testCompositesOverTheBackground();
        testTransparentIsUnchanged();
        testNestedCompositionIgnoresItsBackground();
        testPlanCacheKeysTheBackground();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "PASS: export background lowering\n";
    return 0;
}
