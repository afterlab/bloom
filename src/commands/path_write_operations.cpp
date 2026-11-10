#include <bloom/commands/path_write_operations.hpp>

#include <bloom/commands/animation_operations.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/document/project.hpp>
#include <bloom/document/value_nodes.hpp>

#include <string>
#include <utility>
#include <vector>

namespace bloom::commands {

std::string_view WriteValueToPath::typeId() const noexcept { return "bloom.node.write-to-path"; }

OperationResult WriteValueToPath::apply(document::Draft& draft) const {
    auto* composition = draft.project().findComposition(compositionId_);
    if (composition == nullptr)
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "The composition does not exist");
    if (!value_.isValid())
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "The value is not a path a Path node can hold");
    std::vector<OperationOutput> outputs;
    document::NodeId node;
    if (target_) {
        const auto* existing = composition->graph().findNode(*target_);
        if (existing == nullptr || existing->typeId != document::kPathValueNodeType)
            return OperationResult::rejected(
                OperationIssueCode::InvalidValue,
                "Node " + std::to_string(target_->value()) + " is not a Path node");
        node = *target_;
    } else {
        auto added = AddNode(compositionId_, std::string(document::kPathValueNodeType),
                             layoutPosition_)
                         .apply(draft);
        if (added.status == OperationStatus::Rejected)
            return added;
        for (const auto& output : added.outputs)
            if (output.name == kAddNodeOutput)
                if (const auto* id = std::get_if<document::NodeId>(&output.id))
                    node = *id;
        if (!node.isValid())
            return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                             "The Path node could not be added");
        composition = draft.project().findComposition(compositionId_);
    }
    const auto* record = composition->graph().findNode(node);
    document::ParameterId parameter;
    for (const auto& binding : record->parameters)
        if (binding.role == document::kValueParameterRole)
            parameter = binding.parameterId;
    const auto* stored = parameter.isValid() ? composition->parameters().find(parameter) : nullptr;
    if (stored == nullptr)
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "The Path node has no value to write");
    OperationResult written;
    if (std::holds_alternative<document::ConstantValueSource>(stored->source))
        written = SetParameterSource(compositionId_, parameter,
                                     document::ConstantValueSource{value_})
                      .apply(draft);
    else if (std::holds_alternative<document::AnimationCurveSource>(stored->source))
        written = SetKeyframeAtTimeForParameter(compositionId_, parameter, time_, value_).apply(draft);
    else
        return OperationResult::rejected(
            OperationIssueCode::InvalidValue,
            "The Path node's value is driven by another node; disconnect it first");
    if (written.status == OperationStatus::Rejected)
        return written;
    outputs.push_back({std::string(kWriteToPathNodeOutput), node});
    return OperationResult::applied(std::move(outputs));
}

} // namespace bloom::commands
