#include "timeline_layer_labels.hpp"

#include "entry_layer_session_operations.hpp"

#include <bloom/commands/layer_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/document/merge_entries.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/project.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/swatch_button.hpp>
#include <bloom/ui/kit/tokens.hpp>

#include <QAction>
#include <QCoreApplication>
#include <QMenu>

namespace bloom::ui::layer_labels {
namespace {

QString tr(const char* text) {
    return QCoreApplication::translate("bloom::ui::TimelineEditor", text);
}

constexpr std::array<kit::Color, kPresetCount> kPresets{
    kit::Color::Label1, kit::Color::Label2, kit::Color::Label3, kit::Color::Label4,
    kit::Color::Label5, kit::Color::Label6, kit::Color::Label7, kit::Color::Label8};

[[nodiscard]] std::array<std::uint8_t, 3> rgb(const QColor& color) {
    return {static_cast<std::uint8_t>(color.red()), static_cast<std::uint8_t>(color.green()),
            static_cast<std::uint8_t>(color.blue())};
}

} // namespace

Label labelOf(const CompositionSession& session, const document::LayerId layer) {
    const auto* composition = session.composition();
    const auto* record = composition ? composition->graph().findLayer(layer) : nullptr;
    return record ? record->labelColor : std::nullopt;
}

QColor presetColor(const int preset) {
    return preset >= 1 && preset <= kPresetCount
               ? kit::color(kPresets[static_cast<std::size_t>(preset - 1)])
               : QColor{};
}

int filterKey(const Label& label) {
    if (!label)
        return kKindDefault;
    for (int preset = 1; preset <= kPresetCount; ++preset)
        if (rgb(presetColor(preset)) == *label)
            return preset;
    return kCustom;
}

QString filterKeyName(const int key) {
    if (key == kKindDefault)
        return tr("Default");
    if (key == kCustom)
        return tr("Custom");
    return tr("Label %1").arg(key);
}

void selectLabelGroup(CompositionSession& session, const document::LayerId layer) {
    const auto entries = session.entriesOfLayer(layer);
    if (!entries.empty()) {
        selectLabelGroup(session, LayerTarget{entries.front(), layer});
        return;
    }
    const auto* composition = session.composition();
    const auto anchor = session.boundaryNodeForLayer(layer);
    if (composition == nullptr || !anchor)
        return;
    const auto label = labelOf(session, layer);
    std::set<document::NodeId> nodes;
    for (const auto& record : composition->graph().layerOutputs())
        if (record.labelColor == label)
            if (const auto boundary = session.boundaryNodeForLayer(record.layerId))
                nodes.insert(*boundary);
    session.selectNodes(nodes, *anchor);
}

void selectLabelGroup(CompositionSession& session, const LayerTarget& anchor) {
    const auto* composition = session.composition();
    if (composition == nullptr)
        return;
    const auto label = anchor.layer.isValid() ? labelOf(session, anchor.layer) : Label{};
    std::vector<document::MergeEntryRef> entries;
    for (const auto& merge : composition->graph().merges())
        for (const auto& slot : merge.entries()) {
            const auto* layer =
                document::mergeEntryLayer(composition->graph(), merge.nodeId(), slot.slotId);
            if ((layer ? layer->labelColor : Label{}) == label)
                entries.push_back({merge.nodeId(), slot.slotId});
        }
    session.selectEntries(entries, anchor.entry);
}

void populateLabelMenu(QMenu& menu, CompositionSession& session,
                       const std::vector<document::LayerId>& layers, const document::LayerId anchor,
                       std::function<void()> custom) {
    std::vector<LayerTarget> targets;
    for (const auto layer : layers) {
        const auto entries = session.entriesOfLayer(layer);
        targets.push_back({entries.empty() ? document::MergeEntryRef{} : entries.front(), layer});
    }
    const auto entries = session.entriesOfLayer(anchor);
    populateLabelMenu(menu, session, targets,
                      {entries.empty() ? document::MergeEntryRef{} : entries.front(), anchor},
                      std::move(custom));
}

void populateLabelMenu(QMenu& menu, CompositionSession& session,
                       const std::vector<LayerTarget>& targets, const LayerTarget& anchor,
                       std::function<void()> custom) {
    const auto compositionId = session.compositionId();
    const auto revision = session.snapshot().revision();
    const auto current = anchor.layer.isValid() ? labelOf(session, anchor.layer) : Label{};
    const auto apply = [&session, compositionId, revision, targets](const Label& label) {
        commands::Transaction transaction("Set Layer Label Color", revision);
        appendLayerEdits(transaction, compositionId, targets,
                         [compositionId, label](const document::LayerId layer) {
                             return std::make_unique<commands::SetLayerLabelColor>(compositionId,
                                                                                   layer, label);
                         });
        (void)session.executeTransaction(std::move(transaction));
    };
    auto* kindDefault = menu.addAction(tr("Default"));
    kindDefault->setCheckable(true);
    kindDefault->setChecked(!current.has_value());
    QObject::connect(kindDefault, &QAction::triggered, &menu, [apply] { apply(std::nullopt); });
    for (int preset = 1; preset <= kPresetCount; ++preset) {
        const auto color = presetColor(preset);
        auto* action = menu.addAction(kit::swatchIcon(color), tr("Label %1").arg(preset));
        action->setCheckable(true);
        action->setChecked(current == Label{rgb(color)});
        QObject::connect(action, &QAction::triggered, &menu,
                         [apply, color] { apply(Label{rgb(color)}); });
    }
    auto* customAction = menu.addAction(tr("Custom…"));
    customAction->setCheckable(true);
    customAction->setChecked(filterKey(current) == kCustom);
    QObject::connect(customAction, &QAction::triggered, &menu, [custom] {
        if (custom)
            custom();
    });
    menu.addSeparator();
    auto* group = menu.addAction(tr("Select Label Group"));
    group->setObjectName(QStringLiteral("timelineSelectLabelGroupAction"));
    QObject::connect(group, &QAction::triggered, &menu, [&session, anchor] {
        if (anchor.entry.merge.isValid())
            selectLabelGroup(session, anchor);
        else
            selectLabelGroup(session, anchor.layer);
    });
}

void populateLabelFilterMenu(QMenu& menu, const std::set<int>& active,
                             std::function<void(std::set<int>)> apply) {
    auto* all = menu.addAction(tr("All Labels"));
    all->setCheckable(true);
    all->setChecked(active.empty());
    QObject::connect(all, &QAction::triggered, &menu, [apply] { apply({}); });
    menu.addSeparator();
    for (int key = kKindDefault; key <= kCustom; ++key) {
        auto* action = key >= 1 && key <= kPresetCount
                           ? menu.addAction(kit::swatchIcon(presetColor(key)), filterKeyName(key))
                           : menu.addAction(filterKeyName(key));
        action->setCheckable(true);
        action->setChecked(active.contains(key));
        QObject::connect(action, &QAction::triggered, &menu, [apply, active, key] {
            auto next = active;
            if (!next.erase(key))
                next.insert(key);
            apply(next);
        });
    }
}

} // namespace bloom::ui::layer_labels
