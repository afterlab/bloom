#include <bloom/runtime/snapshot_compiler.hpp>

#include <algorithm>
#include <cctype>
#include <string>

namespace bloom::runtime {

std::string_view compileDiagnosticCodeId(const CompileDiagnosticCode code) noexcept {
    switch (code) {
    case CompileDiagnosticCode::RegistryNotFrozen:
        return "bloom.runtime.compile.registry-not-frozen";
    case CompileDiagnosticCode::CompositionNestingCycle:
        return "bloom.runtime.compile.composition-nesting-cycle";
    case CompileDiagnosticCode::CompositionNotFound:
        return "bloom.runtime.compile.composition-not-found";
    case CompileDiagnosticCode::UnknownNodeType:
        return "bloom.runtime.compile.unknown-node-type";
    case CompileDiagnosticCode::UnsupportedNodeVersion:
        return "bloom.runtime.compile.unsupported-node-version";
    case CompileDiagnosticCode::UnsupportedNode:
        return "bloom.runtime.compile.unsupported-node";
    case CompileDiagnosticCode::InvalidCompositionOutput:
        return "bloom.runtime.compile.invalid-composition-output";
    case CompileDiagnosticCode::UnknownPort:
        return "bloom.runtime.compile.unknown-port";
    case CompileDiagnosticCode::MissingInput:
        return "bloom.runtime.compile.missing-input";
    case CompileDiagnosticCode::PortTypeMismatch:
        return "bloom.runtime.compile.port-type-mismatch";
    case CompileDiagnosticCode::MissingParameter:
        return "bloom.runtime.compile.missing-parameter";
    case CompileDiagnosticCode::UnexpectedParameter:
        return "bloom.runtime.compile.unexpected-parameter";
    case CompileDiagnosticCode::ParameterSchemaMismatch:
        return "bloom.runtime.compile.parameter-schema-mismatch";
    case CompileDiagnosticCode::ParameterValueKindMismatch:
        return "bloom.runtime.compile.parameter-value-kind-mismatch";
    case CompileDiagnosticCode::UnsupportedParameterSource:
        return "bloom.runtime.compile.unsupported-parameter-source";
    case CompileDiagnosticCode::InvalidParameterOverride:
        return "bloom.runtime.compile.invalid-parameter-override";
    case CompileDiagnosticCode::UnsupportedParameterOverride:
        return "bloom.runtime.compile.unsupported-parameter-override";
    case CompileDiagnosticCode::BoundsReadoutDrivesImageOperation:
        return "bloom.runtime.compile.bounds-readout-drives-image-operation";
    case CompileDiagnosticCode::FontAssetUnavailable:
        return "bloom.runtime.compile.font-asset-unavailable";
    case CompileDiagnosticCode::TopologyInvariant:
        return "bloom.runtime.compile.topology-invariant";
    case CompileDiagnosticCode::TimeScopeLimit:
        return "bloom.runtime.compile.time-scope-limit";
    case CompileDiagnosticCode::LayerExtentUnresolved:
        return "bloom.runtime.compile.layer-extent-unresolved";
    }
    return "bloom.runtime.compile.unknown-diagnostic";
}

std::optional<CompileFailureSummary>
compileFailureSummary(const SnapshotCompileResult& result,
                      const document::CompositionId composition) {
    if (result.status == SnapshotCompileStatus::Compiled ||
        result.status == SnapshotCompileStatus::Cancelled)
        return std::nullopt;
    const auto error = std::ranges::find(result.diagnostics, DiagnosticSeverity::Error,
                                         &CompileDiagnostic::severity);
    CompileFailureSummary summary{.code = {},
                                  .detail = "The composition cannot be rendered.",
                                  .compositionId = composition,
                                  .nodeId = std::nullopt,
                                  .parameterId = std::nullopt};
    if (error == result.diagnostics.end())
        return summary;
    summary.code = std::string(compileDiagnosticCodeId(error->code));
    auto text = error->summary;
    if (!error->detail.empty())
        text += (text.empty() ? "" : ": ") + error->detail;
    if (text.empty())
        text = "The composition cannot be rendered";
    if (text.back() != '.')
        text.push_back('.');
    summary.detail = std::move(text);
    if (error->subject.compositionId.isValid())
        summary.compositionId = error->subject.compositionId;
    summary.nodeId = error->subject.nodeId;
    summary.parameterId = error->subject.parameterId;
    return summary;
}

std::string describeCompileFailure(const SnapshotCompileResult& result,
                                   const document::Project& project,
                                   const document::CompositionId composition) {
    const auto summary = compileFailureSummary(result, composition);
    if (!summary)
        return {};
    const auto error = std::ranges::find(result.diagnostics, DiagnosticSeverity::Error,
                                         &CompileDiagnostic::severity);
    const auto quoted = [](const std::string& name) { return '"' + name + '"'; };
    std::string subject = "composition ";
    const auto* owner = project.findComposition(summary->compositionId);
    subject += owner != nullptr && !owner->name().empty()
                   ? quoted(owner->name())
                   : std::to_string(summary->compositionId.value());
    if (owner != nullptr && error != result.diagnostics.end()) {
        const auto& graph = owner->graph();
        const auto& named = error->subject;
        // A parameter-only subject names the node that binds the parameter.
        const document::NodeRecord* node = named.nodeId ? graph.findNode(*named.nodeId) : nullptr;
        const document::ParameterBinding* binding = nullptr;
        for (const auto& candidate : graph.nodes()) {
            if (!named.parameterId || (node != nullptr && &candidate != node))
                continue;
            const auto found = std::ranges::find(candidate.parameters, *named.parameterId,
                                                 &document::ParameterBinding::parameterId);
            if (found != candidate.parameters.end()) {
                node = &candidate;
                binding = &*found;
                break;
            }
        }
        const auto nodeId = node != nullptr ? std::optional(node->id) : named.nodeId;
        // A Layer Output's diagnostics name its node; the layer is what the artist sees.
        const auto boundary =
            std::ranges::find_if(graph.layerOutputs(), [&](const auto& candidate) {
                return named.layerId ? candidate.layerId == *named.layerId
                                     : nodeId && candidate.nodeId == *nodeId;
            });
        if (boundary != graph.layerOutputs().end())
            subject += ", layer " + quoted(boundary->name);
        if (nodeId && (boundary == graph.layerOutputs().end() || boundary->nodeId != *nodeId))
            subject += ", node " + std::to_string(nodeId->value()) +
                       (node != nullptr ? " (" + node->typeId + ")" : std::string{});
        if (named.parameterId)
            subject +=
                ", parameter " + (binding != nullptr ? quoted(binding->role)
                                                     : std::to_string(named.parameterId->value()));
    }
    subject.front() = static_cast<char>(std::toupper(static_cast<unsigned char>(subject.front())));
    return subject + ": " + summary->detail;
}

} // namespace bloom::runtime
