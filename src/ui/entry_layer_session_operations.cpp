#include "entry_layer_session_operations.hpp"

#include <bloom/commands/entry_layer_operations.hpp>
#include <bloom/document/merge_entries.hpp>
#include <bloom/document/project.hpp>

#include <algorithm>
#include <iterator>
#include <utility>

namespace bloom::ui {

commands::OperationResult EntryRoleEdit::apply(document::Draft& draft) const {
    const auto* composition = std::as_const(draft).project().findComposition(composition_);
    const auto* layer = composition ? composition->graph().findLayer(layer_) : nullptr;
    const auto* binding =
        layer ? document::layerParameterBinding(composition->graph(), *layer, role_) : nullptr;
    if (binding == nullptr || !build_)
        return commands::OperationResult::rejected(commands::OperationIssueCode::InvalidTarget,
                                                   "The layer does not expose " + role_);
    const auto operation = build_(binding->parameterId);
    if (!operation)
        return commands::OperationResult::rejected(commands::OperationIssueCode::InvalidValue,
                                                   "No edit was built for " + role_);
    return operation->apply(draft);
}

commands::OperationResult MaterializedEdit::apply(document::Draft& draft) const {
    auto ensured = commands::EnsureEntryLayer(composition_, merge_, slot_).apply(draft);
    if (ensured.status == commands::OperationStatus::Rejected)
        return ensured;
    bool applied = ensured.status == commands::OperationStatus::Applied;
    commands::OperationResult result = commands::OperationResult::noChange();
    for (const auto& operation : edit_->operations()) {
        auto step = operation->apply(draft);
        if (step.status == commands::OperationStatus::Rejected)
            return step;
        applied = applied || step.status == commands::OperationStatus::Applied;
        result.outputs.insert(result.outputs.end(), std::make_move_iterator(step.outputs.begin()),
                              std::make_move_iterator(step.outputs.end()));
        result.issues.insert(result.issues.end(), step.issues.begin(), step.issues.end());
    }
    // A created layer is a topology change: the whole-render default, no finite footprint.
    result.status = applied ? commands::OperationStatus::Applied : commands::OperationStatus::NoChange;
    return result;
}

void appendLayerEdits(commands::Transaction& transaction, const document::CompositionId composition,
                      const std::vector<LayerTarget>& targets,
                      const commands::EntryLayerOperationBuilder& build) {
    std::vector<document::LayerId> seen;
    for (const auto& target : targets) {
        if (target.layer.isValid()) {
            if (std::ranges::find(seen, target.layer) != seen.end())
                continue;
            seen.push_back(target.layer);
            if (auto operation = build(target.layer))
                (void)transaction.add(std::move(operation));
            continue;
        }
        transaction.emplace<commands::EntryLayerEdit>(composition, target.entry.merge,
                                                      target.entry.slot, build);
    }
}

} // namespace bloom::ui
