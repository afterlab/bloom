#include <bloom/commands/keyframe_bake_operations.hpp>

#include <bloom/commands/animation_operations.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/document/project.hpp>
#include <bloom/document/value_nodes.hpp>

#include "animation_curve_edits.hpp"

#include <cmath>
#include <string>

namespace bloom::commands {

std::string_view BakeValueToKeyframes::typeId() const noexcept {
    return "bloom.node.bake-to-keyframes";
}

OperationResult BakeValueToKeyframes::apply(document::Draft& draft) const {
    if (draft.project().findComposition(compositionId_) == nullptr)
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "The composition does not exist");
    if (keys_.empty())
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "There are no values to bake");
    if (keys_.size() > kMaximumBakedKeys)
        return OperationResult::rejected(
            OperationIssueCode::InvalidValue,
            "A bake writes at most " + std::to_string(kMaximumBakedKeys) + " keyframes");
    for (std::size_t index = 0; index < keys_.size(); ++index) {
        if (!std::isfinite(keys_[index].value))
            return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                             "A baked value is not a number");
        if (index != 0 && !(keys_[index - 1].time < keys_[index].time))
            return OperationResult::rejected(OperationIssueCode::InvalidOrder,
                                             "The baked values are not in order of time");
    }
    auto added =
        AddNode(compositionId_, std::string(document::kScalarValueNodeType), layoutPosition_)
            .apply(draft);
    if (added.status == OperationStatus::Rejected)
        return added;
    document::NodeId node;
    for (const auto& output : added.outputs)
        if (output.name == kAddNodeOutput)
            if (const auto* id = std::get_if<document::NodeId>(&output.id))
                node = *id;
    const auto* composition = draft.project().findComposition(compositionId_);
    const auto* record = node.isValid() ? composition->graph().findNode(node) : nullptr;
    document::ParameterId parameter;
    if (record != nullptr)
        for (const auto& binding : record->parameters)
            if (binding.role == document::kValueParameterRole)
                parameter = binding.parameterId;
    if (!parameter.isValid())
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "The Number node could not be added");
    // The curve is written whole: a bake is thousands of keys, and adding them one at a time
    // would look every earlier key up again for each.
    const auto created =
        CreateAnimationForParameter(compositionId_, parameter, keys_.front().time).apply(draft);
    if (created.status == OperationStatus::Rejected)
        return created;
    auto* edited = draft.project().findComposition(compositionId_);
    const auto* stored = edited->parameters().find(parameter);
    const auto* source =
        stored != nullptr ? std::get_if<document::AnimationCurveSource>(&stored->source) : nullptr;
    const auto* curve =
        source != nullptr ? edited->animationCurves().findScalar(source->curveId) : nullptr;
    if (curve == nullptr)
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "The Number node could not be keyed");
    document::ScalarAnimationCurve baked{curve->id, {}};
    baked.keyframes.reserve(keys_.size());
    for (const auto& key : keys_) {
        const auto id = draft.ids().allocateKeyframe();
        if (!id)
            return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                             "The project has no keyframe ids left");
        baked.keyframes.push_back({*id, key.time, key.value});
    }
    detail::CurveEdits edits;
    edits.emplace(baked.id, document::AnimationCurveRecord{std::move(baked)});
    const auto published = detail::publishCurves(*edited, std::move(edits));
    if (published.status == OperationStatus::Rejected)
        return published;
    std::vector<OperationOutput> outputs;
    outputs.push_back({std::string(kBakeToKeyframesNodeOutput), node});
    return OperationResult::applied(std::move(outputs));
}

} // namespace bloom::commands
