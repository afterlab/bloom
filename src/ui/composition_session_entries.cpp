// #502 Every Merge entry is a layer: the session's entry selection, entry edits and the fold that
// keeps a live gesture on a plain entry one undo step. See composition_session.hpp, "Merge
// entries", and docs/architecture/layer-graph-model.md, "Merge".

#include "entry_layer_session_operations.hpp"

#include <bloom/commands/animation_operations.hpp>
#include <bloom/commands/entry_layer_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/document/merge_entries.hpp>
#include <bloom/document/project.hpp>
#include <bloom/ui/composition_session.hpp>

#include <QThread>

#include <algorithm>
#include <memory>
#include <string>
#include <utility>

namespace bloom::ui {

const document::LayerOutputBoundary*
CompositionSession::layerBoundary(const document::LayerId layerId) const noexcept {
    const auto* current = composition();
    return current == nullptr || !layerId.isValid() ? nullptr : current->graph().findLayer(layerId);
}

bool CompositionSession::isEntryLayer(const document::LayerId layerId) const noexcept {
    const auto* layer = layerBoundary(layerId);
    return layer != nullptr && layer->entryOwned();
}

bool CompositionSession::entryExists(const document::MergeEntryRef& entry) const noexcept {
    const auto* current = composition();
    const auto* merge = current ? current->graph().merge(entry.merge) : nullptr;
    return merge != nullptr && merge->find(entry.slot) != nullptr;
}

std::optional<document::LayerId>
CompositionSession::entryLayer(const document::MergeEntryRef& entry) const noexcept {
    const auto* current = composition();
    if (current == nullptr || !entryExists(entry))
        return std::nullopt;
    const auto* layer = document::mergeEntryLayer(current->graph(), entry.merge, entry.slot);
    return layer ? std::optional(layer->layerId) : std::nullopt;
}

std::vector<document::MergeEntryRef>
CompositionSession::entriesOfLayer(const document::LayerId layerId) const {
    const auto* current = composition();
    if (current == nullptr || layerBoundary(layerId) == nullptr)
        return {};
    return document::mergeEntriesOfLayer(current->graph(), layerId);
}

std::optional<document::MergeEntryRef>
CompositionSession::entryOfSelectedLayer(const document::LayerId layerId) const {
    const auto entries = entriesOfLayer(layerId);
    if (entries.empty())
        return std::nullopt;
    // A shared Layer node is selected as its row in the timeline Merge when it has one.
    const auto* timeline = timelineMerge();
    if (timeline != nullptr)
        for (const auto& entry : entries)
            if (entry.merge == timeline->nodeId())
                return entry;
    return entries.front();
}

const document::ParameterRecord*
CompositionSession::layerParameter(const document::LayerId layerId,
                                   const std::string_view role) const noexcept {
    const auto* current = composition();
    const auto* layer = layerBoundary(layerId);
    const auto* binding =
        layer ? document::layerParameterBinding(current->graph(), *layer, role) : nullptr;
    return binding ? current->parameters().find(binding->parameterId) : nullptr;
}

void CompositionSession::selectEntry(const document::MergeEntryRef& entry, const bool extend) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!entryExists(entry)) {
        reportUnavailable(QStringLiteral("The selected layer is no longer available"));
        return;
    }
    auto nodes = extend ? selectedNodes_ : std::set<document::NodeId>{};
    auto entries = extend ? selectedEntries() : std::vector<document::MergeEntryRef>{};
    CompositionSelection next;
    if (const auto layer = entryLayer(entry)) {
        next = {.primary = *layer, .contextualLayer = *layer};
        if (const auto node = boundaryNodeForLayer(*layer))
            nodes.insert(*node);
    } else {
        next = {.primary = entry, .contextualLayer = std::nullopt};
    }
    std::erase(entries, entry);
    entries.insert(entries.begin(), entry);
    if (selection_ == next && selectedNodes_ == nodes && selectedEntries_ == entries &&
        primaryEntry_ == entry)
        return;
    selection_ = std::move(next);
    selectedNodes_ = std::move(nodes);
    selectedEntries_ = std::move(entries);
    primaryEntry_ = entry;
    emit selectionChanged();
}

void CompositionSession::selectEntries(const std::vector<document::MergeEntryRef>& entries,
                                       const document::MergeEntryRef& primary) {
    Q_ASSERT(QThread::currentThread() == thread());
    std::vector<document::MergeEntryRef> kept;
    for (const auto& entry : entries)
        if (entryExists(entry) && std::ranges::find(kept, entry) == kept.end())
            kept.push_back(entry);
    if (kept.empty() || std::ranges::find(kept, primary) == kept.end()) {
        if (kept.empty())
            clearSelection();
        else
            selectEntries(kept, kept.front());
        return;
    }
    std::erase(kept, primary);
    kept.insert(kept.begin(), primary);
    std::set<document::NodeId> nodes;
    for (const auto& entry : kept)
        if (const auto layer = entryLayer(entry))
            if (const auto node = boundaryNodeForLayer(*layer))
                nodes.insert(*node);
    CompositionSelection next;
    if (const auto layer = entryLayer(primary))
        next = {.primary = *layer, .contextualLayer = *layer};
    else
        next = {.primary = primary, .contextualLayer = std::nullopt};
    if (selection_ == next && selectedNodes_ == nodes && selectedEntries_ == kept &&
        primaryEntry_ == primary)
        return;
    selection_ = std::move(next);
    selectedNodes_ = std::move(nodes);
    selectedEntries_ = std::move(kept);
    primaryEntry_ = primary;
    emit selectionChanged();
}

bool CompositionSession::isEntrySelected(const document::MergeEntryRef& entry) const {
    const bool entrySelection = std::holds_alternative<document::LayerId>(selection_.primary) ||
                                std::holds_alternative<document::MergeEntryRef>(selection_.primary);
    if (const auto* plain = std::get_if<document::MergeEntryRef>(&selection_.primary);
        plain && *plain == entry)
        return true;
    if (entrySelection && std::ranges::find(selectedEntries_, entry) != selectedEntries_.end())
        return true;
    const auto layer = entryLayer(entry);
    if (!layer)
        return false;
    if (const auto node = boundaryNodeForLayer(*layer); node && selectedNodes_.contains(*node))
        return true;
    if (const auto* direct = std::get_if<document::LayerId>(&selection_.primary))
        return *direct == *layer;
    return selection_.contextualLayer == *layer;
}

std::optional<document::MergeEntryRef> CompositionSession::selectedEntry() const {
    if (const auto* plain = std::get_if<document::MergeEntryRef>(&selection_.primary))
        return *plain;
    const auto* layer = std::get_if<document::LayerId>(&selection_.primary);
    if (layer == nullptr)
        return std::nullopt;
    if (primaryEntry_ && entryExists(*primaryEntry_) && entryLayer(*primaryEntry_) == *layer)
        return primaryEntry_;
    return entryOfSelectedLayer(*layer);
}

std::vector<document::MergeEntryRef> CompositionSession::selectedEntries() const {
    std::vector<document::MergeEntryRef> entries;
    const auto add = [&](const document::MergeEntryRef& entry) {
        if (entryExists(entry) && std::ranges::find(entries, entry) == entries.end())
            entries.push_back(entry);
    };
    if (const auto primary = selectedEntry())
        add(*primary);
    const bool entrySelection = std::holds_alternative<document::LayerId>(selection_.primary) ||
                                std::holds_alternative<document::MergeEntryRef>(selection_.primary);
    if (entrySelection)
        for (const auto& entry : selectedEntries_)
            add(entry);
    for (const auto node : selectedNodes_)
        if (const auto layer = layerForNode(node); layer && boundaryNodeForLayer(*layer) == node)
            for (const auto& entry : entriesOfLayer(*layer))
                add(entry);
    if (entries.empty() && selection_.contextualLayer)
        for (const auto& entry : entriesOfLayer(*selection_.contextualLayer))
            add(entry);
    return entries;
}

commands::CommandResult CompositionSession::editEntry(const document::MergeEntryRef& entry,
                                                      const QString& label,
                                                      commands::EntryLayerOperationBuilder build) {
    Q_ASSERT(QThread::currentThread() == thread());
    commands::Transaction transaction(label.toStdString(), snapshot_.revision());
    transaction.emplace<commands::EntryLayerEdit>(compositionId_, entry.merge, entry.slot,
                                                  std::move(build));
    return executeTransaction(std::move(transaction));
}

std::optional<document::ParameterValue>
CompositionSession::entryLayerDefault(const std::string_view role) const {
    const auto* current = composition();
    if (current == nullptr)
        return std::nullopt;
    if (role == document::kPositionParameterRole)
        return document::ParameterValue{
            document::Vec2d{static_cast<double>(current->format().width()) / 2.0,
                            static_cast<double>(current->format().height()) / 2.0}};
    if (role == document::kAnchorParameterRole)
        return document::ParameterValue{document::Vec2d{0.0, 0.0}};
    if (role == document::kScaleParameterRole)
        return document::ParameterValue{document::kDefaultScale};
    if (role == document::kRotationParameterRole)
        return document::ParameterValue{0.0};
    if (role == document::kOpacityParameterRole)
        return document::ParameterValue{1.0};
    if (role == document::kBlendModeParameterRole)
        return document::ParameterValue{std::int64_t{0}};
    return std::nullopt;
}

std::optional<ParameterSample>
CompositionSession::entryParameterValue(const document::MergeEntryRef& entry,
                                        const std::string_view role) const {
    if (const auto layer = entryLayer(entry))
        return effectiveParameterValue(layerParameter(*layer, role));
    const auto value = entryLayerDefault(role);
    if (!value)
        return std::nullopt;
    return std::visit(
        [](const auto& held) -> std::optional<ParameterSample> {
            if constexpr (std::is_constructible_v<ParameterSample, const std::decay_t<decltype(held)>&>)
                return ParameterSample(held);
            else
                return std::nullopt;
        },
        *value);
}

bool CompositionSession::setEntryParameterValue(const document::MergeEntryRef& entry,
                                                const std::string_view role,
                                                document::ParameterValue value,
                                                const QString& label) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (const auto layer = entryLayer(entry)) {
        const auto* parameter = layerParameter(*layer, role);
        if (parameter == nullptr) {
            reportUnavailable(QStringLiteral("That layer does not expose this parameter"));
            return false;
        }
        if (role == document::kBlendModeParameterRole) {
            const auto* mode = std::get_if<std::int64_t>(&value);
            const auto blend = mode ? core::blendModeFromStoredValue(*mode) : std::nullopt;
            return blend && setLayerBlendMode(*layer, *blend);
        }
        return setParameterValue(parameter->id, std::move(value), label);
    }
    if (entryLayerDefault(role) == value)
        return true;
    const auto compositionId = compositionId_;
    auto result = editEntry(entry, label,
                            [compositionId, role = std::string(role),
                             value = std::move(value)](const document::LayerId layer) {
                                return std::make_unique<EntryRoleEdit>(
                                    compositionId, layer, role,
                                    [compositionId, value](const document::ParameterId parameter) {
                                        return std::make_unique<commands::SetParameterSource>(
                                            compositionId, parameter,
                                            document::ConstantValueSource{value});
                                    });
                            });
    return result.succeeded();
}

bool CompositionSession::editSelectedPlainEntry(const std::string_view role,
                                                document::ParameterValue value,
                                                const QString& label) {
    const auto* entry = std::get_if<document::MergeEntryRef>(&selection_.primary);
    if (entry == nullptr || !entryLayerDefault(role)) {
        reportUnavailable(QStringLiteral("The selected object does not expose this parameter"));
        return false;
    }
    return setEntryParameterValue(*entry, role, std::move(value), label);
}

std::optional<document::ParameterId>
CompositionSession::materializeEntryParameter(const document::MergeEntryRef& entry,
                                              const std::string_view role) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!entryExists(entry))
        return std::nullopt;
    if (const auto layer = entryLayer(entry)) {
        const auto* parameter = layerParameter(*layer, role);
        return parameter ? std::optional(parameter->id) : std::nullopt;
    }
    if (!entryLayerDefault(role))
        return std::nullopt;
    withdrawPendingEntryLayer();
    commands::Transaction transaction("Create Layer", snapshot_.revision());
    transaction.emplace<commands::EnsureEntryLayer>(compositionId_, entry.merge, entry.slot);
    const auto result = commandStack_->execute(std::move(transaction));
    if (!handleResult(result) || !result.changed())
        return std::nullopt;
    pendingEntryLayer_ = PendingEntryLayer{snapshot_.revision(), entry};
    const auto layer = entryLayer(entry);
    const auto* parameter = layer ? layerParameter(*layer, role) : nullptr;
    return parameter ? std::optional(parameter->id) : std::nullopt;
}

bool CompositionSession::toggleEntryKeyframe(const document::MergeEntryRef& entry,
                                             const std::string_view role) {
    Q_ASSERT(QThread::currentThread() == thread());
    const bool plain = !entryLayer(entry).has_value();
    const auto parameter = materializeEntryParameter(entry, role);
    const auto* current = composition();
    const auto* record = parameter && current ? current->parameters().find(*parameter) : nullptr;
    if (record == nullptr)
        return false;
    const bool toggled = toggleKeyframeFor(record);
    // A refused toggle leaves no layer behind it.
    if (!toggled && plain)
        withdrawPendingEntryLayer();
    return toggled;
}

void CompositionSession::withdrawPendingEntryLayer() {
    if (!pendingEntryLayer_ || foldingEntryLayer_)
        return;
    const auto pending = *pendingEntryLayer_;
    pendingEntryLayer_.reset();
    if (pending.revision != snapshot_.revision() || !commandStack_->canUndo())
        return;
    (void)withdrawLastChange();
}

commands::CommandResult CompositionSession::runTransaction(commands::Transaction&& transaction) {
    if (!pendingEntryLayer_ || foldingEntryLayer_)
        return commandStack_->execute(std::move(transaction));
    const auto pending = *pendingEntryLayer_;
    pendingEntryLayer_.reset();
    if (pending.revision != snapshot_.revision() || !commandStack_->canUndo())
        return commandStack_->execute(std::move(transaction));
    // Withdraw the gesture's layer creation, then recreate it and replay the gesture's commit in one
    // transaction. The id allocator is part of the snapshot, so the layer and its parameters come
    // back with the ids the commit names.
    foldingEntryLayer_ = true;
    const auto undone = commandStack_->undo();
    if (!undone.changed()) {
        foldingEntryLayer_ = false;
        return commandStack_->execute(std::move(transaction));
    }
    commandStack_->discardRedo();
    commands::Transaction folded(std::string(transaction.label()), snapshot_.revision());
    folded.emplace<MaterializedEdit>(compositionId_, pending.entry.merge, pending.entry.slot,
                                     std::move(transaction));
    auto result = commandStack_->execute(std::move(folded));
    foldingEntryLayer_ = false;
    return result;
}

} // namespace bloom::ui
