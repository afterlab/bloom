#pragma once

#include <bloom/runtime/channel_inspection.hpp>
#include <bloom/runtime/cancellation.hpp>
#include <bloom/runtime/compiled_plan.hpp>
#include <bloom/runtime/font_fetch.hpp>
#include <bloom/runtime/node_definition_registry.hpp>
#include <bloom/runtime/task_types.hpp>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace bloom::runtime {

namespace fonts {
class FontResolver;
}

class CompiledPlanCache;

enum class SnapshotCompileStatus {
    Compiled,
    Unsupported,
    Cancelled,
    Failed,
};

enum class CompileDiagnosticCode {
    RegistryNotFrozen,
    CompositionNotFound,
    CompositionNestingCycle,
    UnknownNodeType,
    UnsupportedNodeVersion,
    UnsupportedNode,
    InvalidCompositionOutput,
    UnknownPort,
    MissingInput,
    PortTypeMismatch,
    MissingParameter,
    UnexpectedParameter,
    ParameterSchemaMismatch,
    ParameterValueKindMismatch,
    UnsupportedParameterSource,
    InvalidParameterOverride,
    UnsupportedParameterOverride,
    BoundsReadoutDrivesImageOperation,
    // A text font that could not be resolved: the fallback is drawn and the Viewer marks the text.
    FontMissing,
    TopologyInvariant,
    // ADR 0024: too many layer time clocks or scoped operation copies.
    TimeScopeLimit,
    // ADR 0024: a Source content extent names no resolvable media or composition length; the
    // layer compiles with an Unbounded extent.
    LayerExtentUnresolved,
    // #502: an Adjustment's chain reaches more than one Merge entry; the Adjustment draws nothing.
    AdjustmentTargetAmbiguous,
};

[[nodiscard]] std::string_view compileDiagnosticCodeId(CompileDiagnosticCode code) noexcept;

struct CompileSubject {
    document::CompositionId compositionId;
    std::optional<document::NodeId> nodeId;
    std::optional<document::EdgeId> edgeId;
    std::optional<document::ParameterId> parameterId;
    std::optional<document::LayerId> layerId;
    std::optional<document::LayerSlotId> layerSlotId;
    std::string field;

    friend bool operator==(const CompileSubject&, const CompileSubject&) = default;
};

struct CompileDiagnostic {
    CompileDiagnosticCode code = CompileDiagnosticCode::TopologyInvariant;
    DiagnosticSeverity severity = DiagnosticSeverity::Error;
    CompileSubject subject;
    std::string summary;
    std::string detail;

    friend bool operator==(const CompileDiagnostic&, const CompileDiagnostic&) = default;
};

struct SnapshotParameterOverride final {
    document::Revision sourceRevision;
    document::ParameterId parameterId;
    std::variant<double, document::Vec2d, core::Color4d, std::int64_t, std::string, document::Vec3d,
                 bool, document::PathValue, document::GradientValue>
        value;

    friend bool operator==(const SnapshotParameterOverride&,
                           const SnapshotParameterOverride&) = default;
};

struct SnapshotCompileRequest {
    document::Snapshot snapshot;
    document::CompositionId compositionId;
    // Admission limits this request-local channel to eight distinct parameters.
    std::vector<SnapshotParameterOverride> parameterOverrides{};
    // ADR 0026: a viewer-only plan whose output shows these channels raw (see channel_inspection).
    std::optional<ChannelInspection> channelInspection{};
    // #645 isolation: a viewer-only plan while the Viewer is inside these groups (outermost first;
    // document::drillIsolationDimmed names the layers outside them), each of which then draws at
    // kViewerIsolationOpacity of its own opacity. Empty for every export, render and query.
    std::vector<document::LayerId> isolation{};
    // #349: the statistics plan of this Image Statistics node. The compiler sets it, with
    // `channelInspection` on the picture the node reads, to compile that picture followed by the
    // node's reduction (docs/architecture/image-statistics.md). `statisticsStack` holds the
    // Image Statistics nodes whose plans are being compiled around this one, so a node that reads
    // its own result is refused.
    std::optional<document::NodeId> statisticsTap{};
    std::vector<document::NodeId> statisticsStack{};
    // Resolves the text fonts; null uses the process font resolver (docs/architecture/fonts.md).
    std::shared_ptr<fonts::FontResolver> fontResolver{};
    // The Viewer starts a missing Google font downloading and draws the fallback until it arrives;
    // an export waits for it.
    fonts::FontFetch fontFetch = fonts::FontFetch::Background;
    // #627: value nodes a query reads although nothing in the picture reads them (a list output
    // has no image consumer to act as a probe's sink). Each is compiled with what it reads, as a
    // Layer Bounds readout is; a node that does not exist is ignored.
    std::vector<document::NodeId> valueReadouts{};
};

struct SnapshotCompileResult {
    SnapshotCompileStatus status = SnapshotCompileStatus::Failed;
    std::shared_ptr<const CompiledCompositionPlan> plan;
    std::vector<CompileDiagnostic> diagnostics;
};

// A composition that does not compile, as the preview controller, the status bar and scripting
// clients present it: one plain sentence and what it is about. The full diagnostics stay on the
// result for panels that list them all.
struct CompileFailureSummary final {
    // compileDiagnosticCodeId() of the first error.
    std::string code;
    // "Layer Output could not be lowered: ..." -- a sentence, ending in a full stop.
    std::string detail;
    document::CompositionId compositionId;
    std::optional<document::NodeId> nodeId;
    std::optional<document::ParameterId> parameterId;

    friend bool operator==(const CompileFailureSummary&, const CompileFailureSummary&) = default;
};

// The first error of a Failed or Unsupported result; std::nullopt for Compiled and Cancelled.
[[nodiscard]] std::optional<CompileFailureSummary>
compileFailureSummary(const SnapshotCompileResult& result, document::CompositionId composition);

// The same first error as one sentence that names its subject, for text that leaves the compiler
// (an export failure): `Composition "Main", layer "Clip", parameter "timeRemap": <detail>`. Names
// come from `project`; a subject it no longer resolves is named by its id. Empty for Compiled and
// Cancelled.
[[nodiscard]] std::string describeCompileFailure(const SnapshotCompileResult& result,
                                                 const document::Project& project,
                                                 document::CompositionId composition);

class SnapshotCompiler final {
  public:
    explicit SnapshotCompiler(const NodeDefinitionRegistry& registry) noexcept
        : registry_(registry) {}

    [[nodiscard]] SnapshotCompileResult compile(const SnapshotCompileRequest& request,
                                                const CancellationToken& cancellation,
                                                CompiledPlanCache* planCache = nullptr) const;

  private:
    const NodeDefinitionRegistry& registry_;
};

} // namespace bloom::runtime
