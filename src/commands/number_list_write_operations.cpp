#include <bloom/commands/number_list_write_operations.hpp>

#include <bloom/commands/node_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/document/project.hpp>
#include <bloom/document/value_nodes.hpp>

#include <string>
#include <utility>
#include <vector>

namespace bloom::commands {

std::string_view WriteValueToNumberList::typeId() const noexcept {
    return "bloom.node.write-to-number-list";
}

OperationResult WriteValueToNumberList::apply(document::Draft& draft) const {
    auto* composition = draft.project().findComposition(compositionId_);
    if (composition == nullptr)
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "The composition does not exist");
    if (!value_.isValid())
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "The value is not a list a Number List node can hold");
    document::NodeId node;
    if (target_) {
        const auto* existing = composition->graph().findNode(*target_);
        if (existing == nullptr || existing->typeId != document::kNumberListValueNodeType)
            return OperationResult::rejected(
                OperationIssueCode::InvalidValue,
                "Node " + std::to_string(target_->value()) + " is not a Number List node");
        node = *target_;
    } else {
        auto added = AddNode(compositionId_, std::string(document::kNumberListValueNodeType),
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
                                             "The Number List node could not be added");
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
                                         "The Number List node has no value to write");
    // No curve kind carries a list, so the value is a constant or driven by another node.
    if (!std::holds_alternative<document::ConstantValueSource>(stored->source))
        return OperationResult::rejected(
            OperationIssueCode::InvalidValue,
            "Node " + std::to_string(node.value()) +
                " takes its list from another node; disconnect it before writing to it");
    const auto written =
        SetParameterSource(compositionId_, parameter, document::ConstantValueSource{value_})
            .apply(draft);
    if (written.status == OperationStatus::Rejected)
        return written;
    std::vector<OperationOutput> outputs;
    outputs.push_back({std::string(kWriteToNumberListNodeOutput), node});
    return OperationResult::applied(std::move(outputs));
}

} // namespace bloom::commands
