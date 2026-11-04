#pragma once

// #502 test documents: real compositions built node by node (Solids, Layer nodes, Merges, a Merge
// entry's own layer, Adjustment and Curves nodes) so the snapshot compiler, the CPU reference and
// the GPU scene preparation are exercised through their production paths. Every parameter a node
// requires is bound at its registered default unless a test names a value.

#include <bloom/core/blend_mode.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/color_grade_nodes.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/grade_curves.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/merge_entries.hpp>
#include <bloom/document/node_definition_registry.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/document/project.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>

#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bloom::merge_entry_test {

inline constexpr auto kProjectId = document::ProjectId::fromRaw(1);
inline constexpr auto kCompositionId = document::CompositionId::fromRaw(2);

// One layer's values; the defaults are an entry layer's identity in a 16x12 frame.
struct LayerValues final {
    document::Vec2d position{8.0, 6.0};
    document::Vec2d anchor{0.0, 0.0};
    document::Vec2d scale{1.0, 1.0};
    double rotation = 0.0;
    double opacity = 1.0;
    core::BlendMode blendMode = core::BlendMode::Normal;
    document::LayerPivot pivot = document::LayerPivot::Frame;
};

inline void require(const bool condition, const std::string_view message) {
    if (!condition)
        throw std::logic_error(std::string(message));
}

class Builder final {
  public:
    explicit Builder(const std::uint32_t width = 16, const std::uint32_t height = 12,
                     const std::uint64_t idBase = 1000)
        : next_(idBase), root_(document::NodeId::fromRaw(idBase)), graph_(root_), width_(width),
          height_(height) {
        ++next_;
        require(graph_.addNode({root_,
                                std::string(document::kLayerStackNodeType),
                                {},
                                document::kLayerStackNodeSchemaVersion}),
                "root Merge");
        output_ = id<document::NodeId>();
        require(graph_.addNode({output_,
                                std::string(document::kCompositionOutputNodeType),
                                {},
                                document::kCompositionOutputNodeSchemaVersion}),
                "Composition Output");
    }

    [[nodiscard]] document::NodeId root() const noexcept { return root_; }
    [[nodiscard]] document::Vec2d centre() const noexcept {
        return {static_cast<double>(width_) / 2.0, static_cast<double>(height_) / 2.0};
    }

    template <typename Id> [[nodiscard]] Id id() { return Id::fromRaw(next_++); }

    document::ParameterId constant(const std::string_view schemaKey,
                                   document::ParameterValue value) {
        const auto parameter = id<document::ParameterId>();
        parameters_.push_back(
            {parameter, std::string(schemaKey), document::ConstantValueSource{std::move(value)}});
        return parameter;
    }

    // A node with every required parameter bound at its registered default, `values` overriding
    // (or adding) roles.
    document::NodeId
    node(const std::string_view type, const std::uint32_t version,
         const std::vector<std::pair<std::string, document::ParameterValue>>& values = {}) {
        const auto* definition = document::builtInNodeDefinitions().find(type, version);
        require(definition != nullptr, "registered node definition");
        document::NodeRecord record{id<document::NodeId>(), std::string(type), {}, version};
        for (const auto& parameter : definition->parameters) {
            const auto value = std::ranges::find(
                values, parameter.role, &std::pair<std::string, document::ParameterValue>::first);
            if (!parameter.required && value == values.end())
                continue;
            record.parameters.push_back(
                {parameter.role,
                 constant(parameter.schemaKey,
                          value == values.end() ? parameter.defaultValue : value->second)});
        }
        const auto nodeId = record.id;
        require(graph_.addNode(std::move(record)), "node accepted");
        return nodeId;
    }

    document::NodeId solid(const core::Color4d color, const double width, const double height) {
        return node(document::kSolidSourceNodeType, document::kSolidSourceNodeSchemaVersion,
                    {{std::string(document::kSolidColorParameterRole), color},
                     {std::string(document::kSolidWidthParameterRole), width},
                     {std::string(document::kSolidHeightParameterRole), height}});
    }

    // Curves lifting the master curve's middle, so every channel visibly changes.
    document::NodeId curves(const document::NodeId input) {
        const auto effect =
            node(document::kCurvesNodeType, document::kCurvesNodeSchemaVersion,
                 {{std::string(document::kGradeCurveRoles[0]),
                   document::gradeCurvePath({{0.0, 0.1}, {0.5, 0.75}, {1.0, 0.95}})}});
        connect(input, "image", effect, "input");
        return effect;
    }

    document::NodeId adjustment() {
        return node(document::kAdjustmentNodeType, document::kAdjustmentNodeSchemaVersion);
    }

    document::NodeId merge() {
        return node(document::kLayerStackNodeType, document::kLayerStackNodeSchemaVersion);
    }

    // A Layer node over `content` with `values`.
    std::pair<document::NodeId, document::LayerId> layerNode(const document::NodeId content,
                                                             const LayerValues& values) {
        using namespace document;
        const auto layerNode =
            node(kLayerOutputNodeType, kLayerOutputNodeSchemaVersion,
                 {{std::string(kPositionParameterRole), values.position},
                  {std::string(kAnchorParameterRole), values.anchor},
                  {std::string(kScaleParameterRole), values.scale},
                  {std::string(kRotationParameterRole), values.rotation},
                  {std::string(kOpacityParameterRole), values.opacity},
                  {std::string(kBlendModeParameterRole),
                   static_cast<std::int64_t>(core::blendModeStoredValue(values.blendMode))}});
        const auto layer = id<LayerId>();
        LayerOutputBoundary boundary{layerNode, layer, "Layer " + std::to_string(layer.value()),
                                     std::string(kLayerOutputOutputPort)};
        boundary.pivot = values.pivot;
        require(graph_.addLayerOutput(std::move(boundary)), "Layer boundary accepted");
        connect(content, "image", layerNode, kLayerOutputContentInputPort);
        return {layerNode, layer};
    }

    void connect(const document::NodeId from, const std::string_view fromPort,
                 const document::NodeId to, const std::string_view toPort) {
        require(graph_.addEdge({id<document::EdgeId>(),
                                {from, std::string(fromPort)},
                                document::NodeInputRef{to, std::string(toPort)}}),
                "edge accepted");
    }

    // A slot at the bottom of `merge` fed by `source`; a Layer node's slot names its layer.
    document::LayerSlotId slot(const document::NodeId merge, const document::NodeId source,
                               const document::LayerId layer = {}) {
        const auto slotId = id<document::LayerSlotId>();
        require(graph_.merge(merge) != nullptr && graph_.merge(merge)->append({slotId, layer}),
                "slot accepted");
        require(graph_.addEdge(
                    {id<document::EdgeId>(),
                     {source, "image"},
                     document::LayerStackInputRef{
                         merge, slotId, std::string(document::kLayerStackContentInputRole)}}),
                "slot edge accepted");
        return slotId;
    }

    // A slot at the TOP of `merge`.
    document::LayerSlotId topSlot(const document::NodeId merge, const document::NodeId source,
                                  const document::LayerId layer = {}) {
        const auto top = graph_.merge(merge) != nullptr && !graph_.merge(merge)->entries().empty()
                             ? std::optional(graph_.merge(merge)->entries().front().slotId)
                             : std::nullopt;
        const auto slotId = slot(merge, source, layer);
        if (top)
            require(graph_.merge(merge)->moveBefore(slotId, top), "slot moves to the top");
        return slotId;
    }

    // The entry's own layer (#502) with `values` and the identity's remaining defaults.
    document::LayerId entryLayer(const document::NodeId merge, const document::LayerSlotId slot,
                                 const LayerValues& values) {
        using namespace document;
        const auto layer = id<LayerId>();
        LayerOutputBoundary boundary{merge, layer, "Entry " + std::to_string(layer.value()),
                                     std::string(kLayerStackOutputPort)};
        boundary.entrySlot = slot;
        boundary.pivot = values.pivot;
        boundary.parameters = {
            {std::string(kPositionParameterRole),
             constant(kPositionParameterSchemaKey, values.position)},
            {std::string(kAnchorParameterRole), constant(kAnchorParameterSchemaKey, values.anchor)},
            {std::string(kScaleParameterRole), constant(kScaleParameterSchemaKey, values.scale)},
            {std::string(kRotationParameterRole),
             constant(kRotationParameterSchemaKey, values.rotation)},
            {std::string(kOpacityParameterRole),
             constant(kOpacityParameterSchemaKey, values.opacity)},
            {std::string(kBlendModeParameterRole),
             constant(kBlendModeParameterSchemaKey,
                      static_cast<std::int64_t>(core::blendModeStoredValue(values.blendMode)))}};
        require(graph_.addLayerOutput(std::move(boundary)), "entry layer accepted");
        require(graph_.merge(merge)->setLayerId(slot, layer), "slot names its entry layer");
        return layer;
    }

    document::LayerOutputBoundary& boundary(const document::LayerId layer) {
        auto* found = graph_.findLayer(layer);
        require(found != nullptr, "layer exists");
        return *found;
    }

    document::LayerStack& stack(const document::NodeId merge) {
        auto* found = graph_.merge(merge);
        require(found != nullptr, "merge exists");
        return *found;
    }

    // The parameter a layer binds for `role`.
    document::ParameterId parameterOf(const document::LayerId layer, const std::string_view role) {
        const auto* binding = document::layerParameterBinding(graph_, boundary(layer), role);
        require(binding != nullptr, "layer binds the role");
        return binding->parameterId;
    }

    void set(const document::ParameterId parameter, document::ParameterValue value) {
        for (auto& record : parameters_)
            if (record.id == parameter)
                record.source = document::ConstantValueSource{std::move(value)};
    }

    // The finished project: `source` (the root Merge by default) feeds Composition Output.
    [[nodiscard]] document::Project project(std::optional<document::NodeId> source = {},
                                            const bool requireValid = true) const {
        auto graph = graph_;
        require(graph.addEdge({document::EdgeId::fromRaw(next_ + 1000),
                               {source.value_or(root_), "image"},
                               document::NodeInputRef{
                                   output_, std::string(document::kCompositionOutputInputPort)}}),
                "output edge accepted");
        graph.setCompositionOutput({output_, std::string(document::kCompositionOutputOutputPort)});
        document::Composition composition(
            kCompositionId, "Main", core::RationalTime::fromInteger(4), std::move(graph),
            *document::CompositionFormat::create(width_, height_, core::PixelAspectRatio::square(),
                                                 *document::FrameRate::create(24, 1)));
        for (const auto& parameter : parameters_)
            require(composition.parameters().insert(parameter), "parameter accepted");
        document::Project project(kProjectId, "Project");
        require(project.addComposition(std::move(composition)), "composition accepted");
        if (requireValid) {
            const auto validation = project.validate();
            for (const auto& issue : validation.issues())
                std::cerr << "invalid fixture: " << issue.path << ": " << issue.message << '\n';
            require(validation.ok(), "fixture is valid document truth");
        }
        return project;
    }

  private:
    std::uint64_t next_;
    document::NodeId root_;
    document::NodeId output_{};
    document::CanonicalGraph graph_;
    std::vector<document::ParameterRecord> parameters_;
    std::uint32_t width_;
    std::uint32_t height_;
};

struct Compiled final {
    std::shared_ptr<const runtime::CompiledCompositionPlan> plan;
    std::vector<runtime::CompileDiagnostic> diagnostics;
};

[[nodiscard]] inline Compiled compile(document::Project project) {
    const document::Document document(std::move(project));
    const runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
    auto result = compiler.compile({document.snapshot(), kCompositionId}, {});
    if (!result.plan)
        for (const auto& diagnostic : result.diagnostics)
            std::cerr << "compile: " << diagnostic.summary << ": " << diagnostic.detail << '\n';
    return {result.plan, std::move(result.diagnostics)};
}

[[nodiscard]] inline runtime::EvaluationRequest
request(const runtime::CompiledCompositionPlan& plan, const core::RationalTime time = {}) {
    return {time,
            plan.output(),
            runtime::CompositionFormatResolution{},
            runtime::EvaluationQuality::Reference,
            runtime::EvaluationColorIntent::LinearRec709Scene,
            1U << 24U};
}

struct Evaluated final {
    std::vector<render::Rgba32f> pixels;
    runtime::OperationCacheStatistics statistics;
};

[[nodiscard]] inline std::optional<Evaluated>
evaluate(const runtime::CpuCompositionEvaluator& evaluator,
         const std::shared_ptr<const runtime::CompiledCompositionPlan>& plan,
         const core::RationalTime time = {}) {
    if (!plan)
        return std::nullopt;
    const auto result = evaluator.evaluate(plan, request(*plan, time), {});
    if (!result.frame()) {
        for (const auto& diagnostic : result.diagnostics())
            std::cerr << "evaluate: " << diagnostic.summary << ": " << diagnostic.detail << '\n';
        return std::nullopt;
    }
    const auto pixels = result.frame()->processImage().pixels();
    return Evaluated{{pixels.begin(), pixels.end()}, result.frame()->operationCacheStatistics()};
}

[[nodiscard]] inline std::optional<std::vector<render::Rgba32f>>
pixelsOf(const document::Project& project, const core::RationalTime time = {}) {
    const runtime::CpuCompositionEvaluator evaluator;
    const auto compiled = compile(project);
    auto evaluated = evaluate(evaluator, compiled.plan, time);
    if (!evaluated)
        return std::nullopt;
    return std::move(evaluated->pixels);
}

} // namespace bloom::merge_entry_test
