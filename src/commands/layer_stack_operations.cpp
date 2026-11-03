#include <bloom/commands/layer_stack_operations.hpp>

#include <bloom/commands/node_operations.hpp>
#include <bloom/document/project.hpp>

#include <algorithm>
#include <map>
#include <set>
#include <string>

namespace bloom::commands {
namespace {

// The Layer Output boundary node of every layer, or the rejection that names the first one the
// composition does not hold.
[[nodiscard]] std::optional<OperationResult>
boundaryNodes(const document::Draft& draft, const document::CompositionId compositionId,
              const std::vector<document::LayerId>& layers, std::set<document::NodeId>& nodes) {
    const auto* composition = draft.project().findComposition(compositionId);
    if (composition == nullptr)
        return OperationResult::rejected(OperationIssueCode::InvalidTarget,
                                         "The composition does not exist");
    if (layers.empty())
        return OperationResult::rejected(OperationIssueCode::InvalidValue, "No layers were named");
    for (const auto layer : layers) {
        const auto* boundary = composition->graph().findLayer(layer);
        if (boundary == nullptr)
            return OperationResult::rejected(OperationIssueCode::MissingReference,
                                             "Layer " + std::to_string(layer.value()) +
                                                 " is not in the composition");
        nodes.insert(boundary->nodeId);
    }
    return std::nullopt;
}

} // namespace

OperationResult DeleteLayers::apply(document::Draft& draft) const {
    std::set<document::NodeId> nodes;
    if (auto failure = boundaryNodes(draft, composition_, layers_, nodes))
        return std::move(*failure);
    return RemoveNodes(composition_, std::move(nodes)).apply(draft);
}

OperationResult DuplicateLayers::apply(document::Draft& draft) const {
    std::set<document::NodeId> nodes;
    if (auto failure = boundaryNodes(draft, composition_, layers_, nodes))
        return std::move(*failure);
    // A layer named N times is copied N times, as running Duplicate N times would: pass k copies
    // every layer named more than k times, so layers copied in the same pass keep their parent and
    // matte links among those copies.
    std::map<document::LayerId, std::size_t> named;
    for (const auto layer : layers_)
        ++named[layer];
    std::size_t passes = 0;
    for (const auto& [layer, count] : named)
        passes = std::max(passes, count);
    OperationResult result = OperationResult::applied();
    std::vector<std::vector<OperationOutput>> passOutputs;
    for (std::size_t pass = 0; pass < passes; ++pass) {
        std::vector<document::LayerId> copied;
        for (const auto& [layer, count] : named)
            if (count > pass)
                copied.push_back(layer);
        std::set<document::NodeId> passNodes;
        if (auto failure = boundaryNodes(draft, composition_, copied, passNodes))
            return std::move(*failure);
        auto duplicated =
            DuplicateNodes(composition_, std::move(passNodes), document::Vec2d{0, 0}).apply(draft);
        if (duplicated.status == OperationStatus::Rejected)
            return duplicated;
        if (pass == 0) {
            result = duplicated;
        } else {
            result.outputs.insert(result.outputs.end(), duplicated.outputs.begin(),
                                  duplicated.outputs.end());
            // A footprint only one pass proved is not the whole edit's.
            result.affectedTimes =
                result.affectedTimes && duplicated.affectedTimes
                    ? mergeAffectedTimeFootprints(result.affectedTimes, *duplicated.affectedTimes)
                    : std::nullopt;
            result.layerIdentityRemaps.reset();
        }
        passOutputs.push_back(std::move(duplicated.outputs));
    }
    // One "layer" output per copy, in argument order: the k-th mention of a layer is its k-th copy.
    std::vector<OperationOutput> copies;
    std::map<document::LayerId, std::size_t> seen;
    for (const auto layer : layers_) {
        const auto name = "layer." + std::to_string(layer.value());
        for (const auto& output : passOutputs[seen[layer]++])
            if (output.name == name)
                copies.push_back({"layer", output.id});
    }
    result.outputs.insert(result.outputs.end(), copies.begin(), copies.end());
    return result;
}

} // namespace bloom::commands
