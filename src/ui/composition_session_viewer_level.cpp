// #645 The Viewer's drill level: session/UI state beside the selection (ViewerLevel). It is never
// saved and never part of undo; document/drill_levels.hpp decides what a group and a shape are.

#include <bloom/ui/composition_session.hpp>

#include <bloom/document/project.hpp>
#include <bloom/document/value_nodes.hpp>

#include <QScopedValueRollback>
#include <QThread>

#include <algorithm>

namespace bloom::ui {

document::DrillLevelCheck CompositionSession::setViewerLevel(ViewerLevel level) {
    Q_ASSERT(QThread::currentThread() == thread());
    const auto* current = composition();
    if (current == nullptr)
        return {document::DrillLevelRefusal::NotAChain, {}};
    const auto check = document::checkDrillLevel(*current, level.groups, level.points);
    if (!check.ok())
        return check;
    if (viewerLevel_ != level) {
        viewerLevel_ = std::move(level);
        emit viewerLevelChanged();
    }
    return check;
}

const std::vector<document::LayerId>&
CompositionSession::enclosingGroups(const document::LayerId layer) const {
    static const std::vector<document::LayerId> kNone;
    const auto* current = composition();
    if (current == nullptr || !layer.isValid())
        return kNone;
    auto& cache = enclosingGroups_;
    if (cache.project != &snapshot_.project() || cache.revision != snapshot_.revision() ||
        cache.composition != compositionId_) {
        cache.project = &snapshot_.project();
        cache.revision = snapshot_.revision();
        cache.composition = compositionId_;
        cache.chains.clear();
    }
    if (const auto found = cache.chains.find(layer); found != cache.chains.end())
        return found->second;
    // A top-level entry needs no walk over the whole graph.
    std::vector<document::LayerId> chain;
    const auto top = current->graph().outputMergeId();
    const auto topLayers =
        top ? document::mergeEntryLayers(*current, *top) : std::vector<document::LayerId>{};
    if (std::ranges::find(topLayers, layer) == topLayers.end())
        chain =
            document::drillAncestors(*current, layer).value_or(std::vector<document::LayerId>{});
    return cache.chains.emplace(layer, std::move(chain)).first->second;
}

void CompositionSession::selectLayerFromViewer(const document::LayerId layerId, const bool extend) {
    const QScopedValueRollback guard(viewerSelecting_, true);
    selectLayer(layerId, extend);
}

void CompositionSession::clearSelectionFromViewer() {
    const QScopedValueRollback guard(viewerSelecting_, true);
    clearSelection();
}

bool CompositionSession::enterViewerPoints(const document::LayerId shape) {
    const auto* current = composition();
    if (current == nullptr)
        return false;
    const auto info = document::drillShape(*current, shape);
    if (!info || !document::checkDrillLevel(*current, viewerLevel_.groups, shape).ok())
        return false;
    ViewerLevel next{viewerLevel_.groups, shape};
    if (viewerLevel_ != next) {
        viewerLevel_ = std::move(next);
        emit viewerLevelChanged();
    }
    selectLayerFromViewer(info->edited);
    return true;
}

bool CompositionSession::leaveViewerLevel() {
    if (viewerLevel_.points) {
        const auto shape = *viewerLevel_.points;
        viewerLevel_.points.reset();
        emit viewerLevelChanged();
        selectLayerFromViewer(shape);
        return true;
    }
    if (viewerLevel_.groups.empty())
        return false;
    const auto group = viewerLevel_.groups.back();
    viewerLevel_.groups.pop_back();
    emit viewerLevelChanged();
    selectLayerFromViewer(group);
    return true;
}

void CompositionSession::resetViewerLevel() {
    if (viewerLevel_ == ViewerLevel{})
        return;
    viewerLevel_ = {};
    emit viewerLevelChanged();
}

void CompositionSession::normalizeViewerLevel() {
    const auto* current = composition();
    if (current == nullptr) {
        resetViewerLevel();
        return;
    }
    if (viewerLevel_ == ViewerLevel{})
        return;
    auto next = viewerLevel_;
    next.groups.resize(document::validDrillPrefix(*current, next.groups));
    if (next.points && (next.groups.size() != viewerLevel_.groups.size() ||
                        !document::checkDrillLevel(*current, next.groups, next.points).ok()))
        next.points.reset();
    if (next != viewerLevel_) {
        viewerLevel_ = std::move(next);
        emit viewerLevelChanged();
    }
}

void CompositionSession::followSelectionWithViewerLevel() {
    if (viewerSelecting_)
        return;
    const auto* current = composition();
    if (current == nullptr)
        return;
    // Every layer the selection names: the primary layer, the selected rows and the layers of the
    // selected nodes (a Path node names the shape it draws).
    std::vector<document::LayerId> layers;
    const auto add = [&layers](const document::LayerId layer) {
        if (layer.isValid() && std::ranges::find(layers, layer) == layers.end())
            layers.push_back(layer);
    };
    if (const auto* layer = std::get_if<document::LayerId>(&selection_.primary))
        add(*layer);
    else if (selection_.contextualLayer &&
             !std::holds_alternative<document::NodeId>(selection_.primary))
        add(*selection_.contextualLayer);
    for (const auto& entry : selectedEntries_)
        if (const auto layer = entryLayer(entry))
            add(*layer);
    for (const auto node : selectedNodes_) {
        if (const auto layer = layerForNode(node)) {
            add(*layer);
        } else if (const auto* record = current->graph().findNode(node);
                   record != nullptr && record->typeId == document::kPathValueNodeType) {
            if (const auto shape = document::shapeLayerOfPathNode(*current, node))
                add(*shape);
        }
    }
    if (layers.empty()) {
        // A cleared selection keeps the entered groups; point editing needs its shape selected.
        if (viewerLevel_.points) {
            viewerLevel_.points.reset();
            emit viewerLevelChanged();
        }
        return;
    }
    std::optional<std::vector<document::LayerId>> common;
    for (const auto layer : layers) {
        auto chain = std::optional(enclosingGroups(layer));
        // A part of a small-group shape: the level is the shape's group, never the shape itself.
        if (!chain->empty())
            if (const auto shape = document::drillShape(*current, chain->back());
                shape && shape->smallGroup)
                chain->pop_back();
        if (!common) {
            common = std::move(*chain);
            continue;
        }
        std::size_t shared = 0;
        while (shared < common->size() && shared < chain->size() &&
               (*common)[shared] == (*chain)[shared])
            ++shared;
        common->resize(shared);
    }
    if (!common)
        return;
    ViewerLevel next{std::move(*common), std::nullopt};
    // Point editing stays while the selection is still the shape being edited.
    if (viewerLevel_.points && layers.size() == 1 && next.groups == viewerLevel_.groups) {
        const auto shape = document::drillShape(*current, *viewerLevel_.points);
        if (layers.front() == *viewerLevel_.points || (shape && layers.front() == shape->edited))
            next.points = viewerLevel_.points;
    }
    if (next != viewerLevel_) {
        viewerLevel_ = std::move(next);
        emit viewerLevelChanged();
    }
}

} // namespace bloom::ui
