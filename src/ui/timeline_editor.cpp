#include "asset_drop.hpp"
#include "layer_drop.hpp"
#include "node_editor_items.hpp"
#include <QSignalBlocker>
#include <bloom/ui/asset_controller.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/row.hpp>
#include <bloom/ui/timeline_editor.hpp>
#include <bloom/ui/timeline_graph_view.hpp>
#include <memory>

#include <bloom/ui/viewer_editor.hpp>

#include "composition_editor_support.hpp"
#include "timeline_key_time.hpp"
#include "timeline_layer_fields.hpp"
#include "timeline_layer_labels.hpp"
#include "timeline_layer_timing_gesture.hpp"
#include "timeline_matte_cell.hpp"
#include "timeline_property_rows.hpp"
#include "timeline_reveal.hpp"
#include "entry_layer_session_operations.hpp"

#include <bloom/runtime/layer_time_projection.hpp>
#include <bloom/ui/composition_authoring.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/ram_preview_controller.hpp>
#include <bloom/ui/timeline_frame_math.hpp>
#include <bloom/ui/timeline_ruler.hpp>

#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/icons.hpp>
#include <bloom/ui/kit/painting.hpp>
#include <bloom/ui/kit/split_handle.hpp>
#include <bloom/ui/kit/surfaces.hpp>
#include <bloom/ui/kit/swatch_button.hpp>
#include <bloom/ui/kit/tokens.hpp>

#include <bloom/commands/entry_layer_operations.hpp>
#include <bloom/commands/layer_mask_operations.hpp>
#include <bloom/commands/layer_operations.hpp>
#include <bloom/commands/layer_stack_operations.hpp>
#include <bloom/commands/layer_time_operations.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/core/blend_mode.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/merge_entries.hpp>
#include <bloom/document/project.hpp>

#include <QAction>
#include <QApplication>
#include <QContextMenuEvent>
#include <QCursor>
#include <QEvent>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QHelpEvent>
#include <QIcon>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QResizeEvent>
#include <QScrollBar>
#include <QSettings>
#include <QShowEvent>
#include <QSize>
#include <QToolButton>
#include <QToolTip>
#include <QVBoxLayout>
#include <QVariant>
#include <QWheelEvent>
#include <bloom/ui/kit/color_picker.hpp>

#include <bloom/document/layer_masks.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numeric>
#include <optional>
#include <utility>
#include <variant>

namespace bloom::ui {
namespace {

// ---------------------------------------------------------------------------------------------
// The LEFT column's cell table (task TL-C1).
//
// One table, read by the column-header row AND by every layer row, so a header can never drift out
// of alignment with the cells underneath it. Every width resolves from a kit token -- tokens.hpp
// forbids a raw pixel gap outright -- and the column's own fixed width is their exact sum, which is
// also the x origin of the ruler, of every lane, and of the work-area strip.
constexpr int kCellGap = kit::px(kit::Spacing::XS);
constexpr int kToggleCellWidth = kit::px(kit::Size::ToggleCell);
// Video, Audio, Solo and Lock (a compositor's A/V Features), then the Label square.
constexpr int kToggleCellCount = 4;
constexpr int kLabelCellIndex = kToggleCellCount;
constexpr int kToggleColumnWidth = kit::px(kit::Size::TimelineToggleColumn);
// The switch cells after the name: Shy (a compositor's Switches column).
constexpr int kSwitchColumnWidth = kit::px(kit::Size::TimelineSwitchColumn);
constexpr int kNameCellMinWidth = kit::px(kit::Size::TimelineNameMin);
constexpr int kColumnWidth = kit::px(kit::Size::TimelineColumn);
// Blending, Parent and (ADR 0028) Matte.
constexpr int kRightColumnCount = 3;

constexpr int kToggleStripX = kit::px(kit::Spacing::ChromePadding);
constexpr int kNameCellX = kToggleStripX + kToggleColumnWidth;
constexpr int kLayerColumnWidthPx = kit::px(kit::Size::TimelineLeftColumn);
// First-run / Reset Workspace divider: the layer table occupies 37% of the timeline's usable
// width. This is a ratio, not a pixel default; a persisted pixel width still wins on later opens.
constexpr double kDefaultLayerColumnShare = 0.37;
constexpr auto kLayerColumnWidthSetting = "timeline/layer-column-width";

// The scroll gutter reserved to the right of the lane region: the scrollbar's one constant track
// width (the kit stylesheet never changes it on hover), so the ruler above always agrees with the
// lanes about where a frame is. Reserved only while rows overflow vertically.
constexpr int kScrollGutterWidth = kit::px(kit::Size::ScrollBarTrack);

[[nodiscard]] int toggleCellX(const int index) {
    return kToggleStripX + index * (kToggleCellWidth + kCellGap);
}
[[nodiscard]] int nameCellWidth(const int width) {
    return std::max(kNameCellMinWidth,
                    width - kNameCellX - kSwitchColumnWidth - kRightColumnCount * kColumnWidth);
}
// ---------------------------------------------------------------------------------------------
// Visibility, audio, solo and lock share the same cell geometry in headings and rows.
enum class ToggleCell : int { Visibility = 0, Audio = 1, Solo = 2, Lock = 3 };
kit::IconId toggleIcon(const int index) {
    switch (static_cast<ToggleCell>(index)) {
    case ToggleCell::Visibility:
        return kit::IconId::Visible;
    case ToggleCell::Audio:
        return kit::IconId::AudioOn;
    case ToggleCell::Solo:
        return kit::IconId::Solo;
    case ToggleCell::Lock:
        return kit::IconId::Locked;
    }
    return kit::IconId::Visible;
}
QString toggleToolTip(const int index) {
    switch (static_cast<ToggleCell>(index)) {
    case ToggleCell::Visibility:
        return TimelineEditor::tr("Toggle layer visibility");
    case ToggleCell::Audio:
        return TimelineEditor::tr("Mute or unmute this audio layer");
    case ToggleCell::Solo:
        return TimelineEditor::tr("Solo: render only soloed layers");
    case ToggleCell::Lock:
        return TimelineEditor::tr("Lock layer editing");
    }
    return {};
}

QString labelToolTip() { return TimelineEditor::tr("Label: click for the label menu"); }
QString shyToolTip() {
    return TimelineEditor::tr("Shy: hidden from the layer list while Hide Shy Layers is on");
}

// The tooltip for whatever cell `x` lands in, or an empty string for the cells that need none.
[[nodiscard]] QString cellToolTipAt(const int x) {
    for (int index = 0; index < kToggleCellCount; ++index) {
        const int left = toggleCellX(index);
        if (x >= left && x < left + kToggleCellWidth) {
            return toggleToolTip(index);
        }
    }
    if (x >= toggleCellX(kLabelCellIndex) && x < toggleCellX(kLabelCellIndex) + kToggleCellWidth)
        return labelToolTip();
    return {};
}

// ---------------------------------------------------------------------------------------------

[[nodiscard]] QString sourceKind(const document::NodeRecord* sourceNode) {
    if (isKnownSource(sourceNode, document::kSolidSourceNodeType,
                      document::kSolidSourceNodeSchemaVersion)) {
        return TimelineEditor::tr("Solid");
    }
    if (isKnownSource(sourceNode, document::kTextSourceNodeType,
                      document::kTextSourceNodeSchemaVersion)) {
        return TimelineEditor::tr("Text");
    }
    if (isKnownSource(sourceNode, document::kAudioSourceNodeType,
                      document::kAudioSourceNodeSchemaVersion)) {
        return TimelineEditor::tr("Audio");
    }
    if (sourceNode && sourceNode->typeId == "bloom.video-source")
        return TimelineEditor::tr("Video");
    if (sourceNode && sourceNode->typeId == document::kCompositionSourceNodeType)
        return TimelineEditor::tr("Composition");
    if (sourceNode && sourceNode->typeId == document::kLayerStackNodeType)
        return TimelineEditor::tr("Group");
    if (sourceNode && sourceNode->typeId == document::kAdjustmentNodeType)
        return TimelineEditor::tr("Adjustment");
    return TimelineEditor::tr("Layer");
}


// The clip bar's fill, from the data-type palette (task T1).
//
// Task T1 shipped ONE color for both kinds and disclosed that as a blocked sub-item: with the Kind
// column removed, kind was supposed to be expressed by the clip color, but every role in the
// data-type palette names a kind of REFERENCED MEDIA (docs/ux/visual-language.md: image sequences,
// clips, compositions, still images, audio), and Bloom has no media import pipeline at all -- its
// only kinds are Solid and Text, both generated in-project. Task S3 makes text a real rendering
// layer kind, so the two kinds now need to be distinguishable on the lane, and this is the choice:
//
//   Solid -> DataComposition (#8B5CF6), unchanged, so no existing clip changes color.
//   Text  -> DataClip (#3FBF6B).
//
// Why DataClip, having rejected the others on their own terms:
//   * DataImage (#3AA5F0) is byte-identical to AccentHover and one step from Accent (#0C8CE9), so a
//     clip bar painted with it reads as an accent/selected surface and swallows the 1px Accent
//     playhead crossing it.
//   * DataSequence (#E0554E) is byte-identical to Error, so a text clip would read as a failed one.
//   * DataAudio (#7C5CFF) is a neighbouring purple to DataComposition's #8B5CF6 -- two kinds that
//     are supposed to be told apart at a glance would not be.
//   * DataClip is byte-identical to Ok (#3FBF6B), which is the one remaining collision, and a green
//     clip bar reading as "ready" is a far smaller misstatement than one reading as "error",
//     "selected", or "the same kind as a solid".
// Adding a DataText role to the kit would be the ideal fix and remains a kit-owner decision; it is
// not needed for the two kinds that exist.
//
// Kind is still named in text as well: TimelineLayerStack::toolTipAt() spells it on every row, so
// the color is a second channel rather than the only one. An unrecognized layer kind takes Muted,
// because there is no honest data-type color for "kind unknown". A future media-backed or
// pre-composition layer kind takes its own role here on the day it ships.
[[nodiscard]] kit::Color sourceClipColorToken(const CompositionSession& session,
                                              const document::NodeRecord* sourceNode) {
    if (sourceNode && (sourceNode->typeId == document::kCompositionSourceNodeType ||
                       sourceNode->typeId == document::kLayerStackNodeType))
        return kit::Color::DataComposition;
    if (sourceNode && (sourceNode->typeId == "bloom.image-source" ||
                       sourceNode->typeId == "bloom.video-source")) {
        for (const auto& binding : sourceNode->parameters)
            if (binding.role == "asset") {
                const auto value = session.constantStringValue(binding.parameterId);
                const auto* asset = value ? session.snapshot().project().findAsset(
                                                document::AssetId::fromRaw(value->toULongLong()))
                                          : nullptr;
                return asset && asset->kind == document::AssetKind::Sequence
                           ? kit::Color::DataSequence
                           : kit::Color::DataImage;
            }
    }

    if (isKnownSource(sourceNode, document::kSolidSourceNodeType,
                      document::kSolidSourceNodeSchemaVersion)) {
        return kit::Color::DataComposition;
    }
    if (isKnownSource(sourceNode, document::kTextSourceNodeType,
                      document::kTextSourceNodeSchemaVersion)) {
        return kit::Color::DataClip;
    }
    if (isKnownSource(sourceNode, document::kAudioSourceNodeType,
                      document::kAudioSourceNodeSchemaVersion)) {
        return kit::Color::DataAudio;
    }
    // #502: an Adjustment has no pixels of its own (it processes the stack below), so its bar is
    // the neutral Muted grey; the row's adjustment marker names the kind.
    return kit::Color::Muted;
}


// The layer the session currently points at: the primary selection when it IS a layer, otherwise
// the contextual layer a node/parameter selection carries. Exactly the resolution order the
// QTreeWidget stack used before this task.
[[nodiscard]] std::optional<document::LayerId> selectedLayer(const CompositionSession& session) {
    if (const auto* direct = std::get_if<document::LayerId>(&session.selection().primary)) {
        return *direct;
    }
    return session.selection().contextualLayer;
}

[[nodiscard]] bool isLayerSelected(const CompositionSession& session, document::LayerId layer) {
    const auto boundary = session.boundaryNodeForLayer(layer);
    return (boundary.has_value() && session.selectedNodes().contains(*boundary)) ||
           selectedLayer(session) == layer;
}

// #502: a row is selected when its Merge entry is (every layer row is a Merge entry).
[[nodiscard]] bool isRowSelected(const CompositionSession& session,
                                 const TimelineLayerEntry& entry) {
    if (entry.mergeNodeId.isValid())
        return session.isEntrySelected(entry.entry());
    return entry.layerId.isValid() && isLayerSelected(session, entry.layerId);
}

[[nodiscard]] LayerTarget rowTarget(const TimelineLayerEntry& entry) {
    return {entry.entry(), entry.layerId};
}

// One undoable step applying `build` to every target's layer (a plain entry gains its layer in the
// same step). Returns whether the document changed.
bool editTargets(CompositionSession& session, const char* label,
                 const std::vector<LayerTarget>& targets,
                 const commands::EntryLayerOperationBuilder& build) {
    if (targets.empty())
        return false;
    commands::Transaction transaction(label, session.snapshot().revision());
    appendLayerEdits(transaction, session.compositionId(), targets, build);
    if (transaction.empty())
        return false;
    return session.executeTransaction(std::move(transaction)).changed();
}

// The step one name-cell nesting level indents a nested Merge's rows by.
constexpr int kNestingIndentStep = kit::px(kit::Spacing::M);

// The Blending dropdown: every implemented blend mode, in core::kBlendModes order, named by the one
// shared vocabulary blendModeDisplayName() owns. The item DATA is the mode's stored integer rather
// than its row index, so the control never depends on the order it happened to be filled in.
kit::KDropdown* makeBlendingDropdown(QWidget* parent) {
    auto* dropdown = new kit::KDropdown(parent);
    dropdown->setObjectName(QStringLiteral("layerBlendingDropdown"));
    dropdown->setAccessibleName(TimelineEditor::tr("Blending"));
    dropdown->setControlSize(kit::KDropdown::ControlSize::Dense);
    addBlendModeItems(*dropdown);
    return dropdown;
}

// Which row of a blending dropdown shows `mode`, found by stored value rather than by assuming the
// fill order.
[[nodiscard]] int blendingDropdownIndex(const kit::KDropdown& dropdown,
                                        const core::BlendMode mode) {
    return blendModeDropdownIndex(dropdown, mode);
}

// The hairline that closes every row, in both halves of the grid: rows are FLAT (no striping at all
// -- task T1 replaces task U7's surface ladder), so the separator is the only thing giving the grid
// its rhythm.
void paintRowSeparator(QPainter& painter, const int top, const int widthPixels) {
    kit::paintRowSeparator(painter, QRectF(0.0, top, widthPixels, kTimelineRowHeight));
}

// The selected row's lane half: the kit selected-row wash, continuous across the column/lane
// boundary. The Accent edge belongs to the layer row's leading side (kit::KRow), not to the lane.
void paintSelectedRowFill(QPainter& painter, const int top, const int widthPixels) {
    kit::paintSelectedRow(painter, QRectF(0.0, top, widthPixels, kTimelineRowHeight),
                          kit::Color::Surface, false);
}

} // namespace

// ---------------------------------------------------------------------------------------------
// One pooled row of the left layer-stack column.
//
// Pooled KRow controls project their current binding. Blank-row pointer gestures and actual
// toggle activations share the stack's selection/command path; dropdowns remain independently
// reachable. The class lives here because TimelineLayerStack forward-declares its pool type.
class TimelineLayerRow final : public kit::KRow {
  public:
    TimelineLayerRow(CompositionSession& session, QWidget* parent)
        : kit::KRow(parent), session_(&session) {
        setObjectName(QStringLiteral("timelineLayerRow"));
        setLeadingInset(kit::px(kit::Spacing::ChromePadding));
        setAttribute(Qt::WA_TransparentForMouseEvents, false);
        setFixedHeight(kTimelineRowHeight);
        blending_ = makeBlendingDropdown(this);
        parentDropdown_ = new kit::KDropdown(this);
        parentDropdown_->setObjectName("layerParentDropdown");
        parentDropdown_->setControlSize(kit::KDropdown::ControlSize::Dense);
        parentDropdown_->setAccessibleName(TimelineEditor::tr("Parent"));
        parentDropdown_->setToolTip(TimelineEditor::tr("Transform relative to another layer"));
        connect(parentDropdown_, &kit::KDropdown::activated, this, [this](int index) {
            if (binding_ || !bound_ || index < 0)
                return;
            const auto raw = parentDropdown_->itemData(index).toULongLong();
            const auto parent = raw ? std::optional(document::LayerId::fromRaw(raw)) : std::nullopt;
            const auto compositionId = session_->compositionId();
            const bool changed =
                entry_.layerId.isValid()
                    ? session_->setLayerParent(entry_.layerId, parent)
                    : editTargets(*session_, "Set Layer Parent", {rowTarget(entry_)},
                                  [compositionId, parent](const document::LayerId layer) {
                                      return std::make_unique<commands::SetLayerParent>(
                                          compositionId, layer, parent);
                                  });
            if (!changed) {
                const QSignalBlocker blocker(parentDropdown_);
                const auto current = entry_.layerId.isValid() ? session_->parentOf(entry_.layerId)
                                                              : std::nullopt;
                parentDropdown_->setCurrentIndex(parentDropdown_->findData(
                    QVariant::fromValue(static_cast<qulonglong>(current ? current->value() : 0))));
            }
        });
        matteDropdown_ = new kit::KDropdown(this);
        matteDropdown_->setObjectName("layerTrackMatteDropdown");
        matteDropdown_->setControlSize(kit::KDropdown::ControlSize::Dense);
        matteDropdown_->setAccessibleName(TimelineEditor::tr("Track Matte"));
        connect(matteDropdown_, &kit::KDropdown::activated, this, [this](int index) {
            if (binding_ || !bound_ || index < 0)
                return;
            const auto target = rowTarget(entry_);
            (void)matte_cell::activate(*session_, target,
                                       matteDropdown_->itemData(index).toLongLong());
            const QSignalBlocker blocker(matteDropdown_);
            matte_cell::populate(*matteDropdown_, *session_, target);
        });
        QList<QWidget*> cells;
        for (int index = 0; index < kToggleCellCount; ++index) {
            auto* toggle = new kit::KIconToggle(toggleIcon(index), this);
            toggle->setProperty("layerSwitch", true);
            // The dense switch strip: ToggleCell cells, each exactly one control body.
            toggle->setFixedSize(kToggleCellWidth, kToggleCellWidth);
            auto policy = toggle->sizePolicy();
            policy.setRetainSizeWhenHidden(true);
            toggle->setSizePolicy(policy);
            toggles_[static_cast<std::size_t>(index)] = toggle;
            toggle->setObjectName(QStringLiteral("timelineLayerToggle%1").arg(index));
            toggle->setToolTip(toggleToolTip(index));
            toggle->setAccessibleName(toggleToolTip(index));
            cells.append(toggle);
            connect(toggle, &QToolButton::clicked, this,
                    [this, index] { activateAt(toggleCellX(index) + kToggleCellWidth / 2); });
        }
        // The Label square: the layer's label colour (its kind colour when it has none); a click
        // opens the label menu under it.
        label_ = new kit::KSwatchButton(this);
        label_->setObjectName(QStringLiteral("timelineLayerLabel"));
        // A cell of the dense switch strip (ToggleCell, not Control, tall).
        label_->setProperty("layerSwitch", true);
        label_->setToolTip(labelToolTip());
        label_->setAccessibleName(TimelineEditor::tr("Label"));
        retainSize(*label_);
        cells.append(label_);
        connect(label_, &QToolButton::clicked, this, [this] {
            if (!binding_ && bound_ && openLabelMenu)
                openLabelMenu(entry_, label_);
        });
        shy_ = new kit::KIconToggle(kit::IconId::Shy, this);
        shy_->setProperty("layerSwitch", true);
        shy_->setObjectName(QStringLiteral("timelineLayerShy"));
        shy_->setToolTip(shyToolTip());
        shy_->setAccessibleName(TimelineEditor::tr("Shy"));
        retainSize(*shy_);
        connect(shy_, &QToolButton::clicked, this, [this] {
            if (!binding_ && bound_ && toggleShy)
                toggleShy(entry_);
        });
        setCells(cells, nullptr, {blending_, parentDropdown_, matteDropdown_}, nullptr, kCellGap,
                 {shy_});
        // #502 markers after the name: the adjustment row and the shared Layer node.
        auto* nameLayout = qobject_cast<QHBoxLayout*>(nameLabel()->parentWidget()->layout());
        const auto marker = [&](const QString& name, const kit::IconId icon,
                                const kit::Color tone) {
            auto* label = new kit::KLabel(nameLabel()->parentWidget());
            label->setObjectName(name);
            label->setPixmap(kit::iconPixmap(icon, kit::Size::IconSmall, tone, kit::State::Normal,
                                             kit::IconWeight::Bold));
            label->setFixedSize(kit::px(kit::Size::ToggleCell), kit::px(kit::Size::ToggleCell));
            label->setAlignment(Qt::AlignCenter);
            label->hide();
            if (nameLayout != nullptr)
                nameLayout->addWidget(label, 0, Qt::AlignVCenter);
            return label;
        };
        adjustmentMarker_ = marker(QStringLiteral("timelineAdjustmentMarker"),
                                   kit::IconId::SlidersHorizontal, kit::Color::Muted);
        adjustmentMarker_->setToolTip(
            TimelineEditor::tr("Adjustment layer: processes the layers below it in its Merge"));
        adjustmentMarker_->setAccessibleName(TimelineEditor::tr("Adjustment layer"));
        sharedMarker_ =
            marker(QStringLiteral("timelineSharedMarker"), kit::IconId::Link, kit::Color::Muted);
        sharedMarker_->setAccessibleName(TimelineEditor::tr("Shared Layer node"));
        disclosureButton()->setAccessibleName(TimelineEditor::tr("Expand layer properties"));
        connect(disclosureButton(), &QToolButton::clicked, this,
                [this] { activateAt(chevronX(entry_.nesting) + kCellGap); });
        // The connection is made once, for the life of the pooled row, and reads whichever entry
        // the row is bound to AT THE MOMENT the artist picks a mode -- a pooled row is re-pointed
        // on every scroll step, so capturing a layer id here would author the wrong layer.
        connect(blending_, &kit::KDropdown::activated, this, [this](const int index) {
            if (binding_ || !bound_ || index < 0) {
                return;
            }
            const auto mode = blendModeFromDropdownIndex(*blending_, index);
            if (!mode.has_value()) {
                return;
            }
            if (entry_.layerId.isValid())
                (void)session_->setLayerBlendMode(entry_.layerId, *mode);
            else if (entry_.mergeNodeId.isValid())
                (void)session_->setEntryParameterValue(
                    entry_.entry(), document::kBlendModeParameterRole,
                    document::ParameterValue{
                        static_cast<std::int64_t>(core::blendModeStoredValue(*mode))},
                    TimelineEditor::tr("Set Blend Mode"));
        });
    }

    // The stack's label menu and Shy commands, called with the entry the row is bound to now.
    std::function<void(const TimelineLayerEntry&, QWidget*)> openLabelMenu;
    std::function<void(const TimelineLayerEntry&)> toggleShy;

    // The x where a row's twirl caret starts: the name cell, indented by its Merge's nesting.
    [[nodiscard]] static int chevronX(const int nesting) {
        return kNameCellX + kit::px(kit::Spacing::S) + nesting * kNestingIndentStep;
    }

    // Rebind the same controls when scrolling; signal-blocked projection never authors edits.
    void bind(const TimelineLayerEntry& entry, const bool selected, const int rowIndex) {
        setName(entry.name, entry.expanded ? kit::IconId::CaretDown : kit::IconId::CaretRight);
        setRowState(rowIndex, selected);
        entry_ = entry;
        bound_ = true;
        audioLayer_ = entry.kind == TimelineEditor::tr("Audio");
        if (auto* nameLayout = nameLabel()->parentWidget()->layout()) {
            const int inset = kit::px(kit::Spacing::XS) + entry.nesting * kNestingIndentStep;
            if (nameLayout->contentsMargins().left() != inset)
                nameLayout->setContentsMargins(inset, 0, kit::px(kit::Spacing::XS), 0);
        }
        // `binding_` (not just a QSignalBlocker) because setCurrentIndex() is a projection of
        // document truth, never an edit: a blocked signal would still leave the lambda armed for a
        // nested change, and a row re-pointed during a scroll must author nothing at all.
        binding_ = true;
        const auto* composition = session_->composition();
        const auto* layer = composition && entry.layerId.isValid()
                                ? composition->graph().findLayer(entry.layerId)
                                : nullptr;
        // #502: a plain entry is an entry layer at its defaults: Normal, visible, not solo or
        // locked. Every control is live and creates the entry's layer on its first edit.
        const auto mode = layer ? session_->blendModeForLayer(entry.layerId)
                                : std::optional(core::BlendMode::Normal);
        const int row = mode.has_value() ? blendingDropdownIndex(*blending_, *mode) : -1;
        enabled_ = layer == nullptr || layer->enabled;
        if (layer && !layer->entryOwned()) {
            const auto layout = composition->nodeLayout().find(layer->nodeId);
            enabled_ =
                enabled_ && (layout == composition->nodeLayout().end() || !layout->second.muted);
        }
        if (entry.groupMerge.isValid() && composition) {
            const auto* merge = composition->graph().merge(entry.groupMerge);
            enabled_ = enabled_ && (merge == nullptr || merge->enabled());
        }
        solo_ = layer && layer->solo;
        locked_ = layer && layer->locked;
        const bool hasAudio = entry.audioNodeId.isValid();
        const bool audioConnected = composition && audioConnectedFor(*composition, entry, layer);
        blending_->setEnabled(mode.has_value() && !locked_);
        parentDropdown_->clearItems();
        parentDropdown_->addItem(TimelineEditor::tr("None"), QVariant::fromValue(qulonglong{0}));
        std::vector<document::LayerId> candidates;
        if (layer)
            candidates = session_->candidateParents(entry.layerId);
        else if (composition)
            for (const auto& other : composition->graph().layerOutputs())
                candidates.push_back(other.layerId);
        for (const auto candidate : candidates) {
            const auto boundary = session_->boundaryNodeForLayer(candidate);
            const auto* node =
                composition && boundary ? composition->graph().findNode(*boundary) : nullptr;
            parentDropdown_->addItem(
                node ? node_editor::nodeDisplayName(*composition, *node)
                     : layer_fields::displayName(*session_, candidate),
                QVariant::fromValue(static_cast<qulonglong>(candidate.value())));
        }
        const auto parent = layer ? session_->parentOf(entry.layerId) : std::nullopt;
        parentDropdown_->setCurrentIndex(parentDropdown_->findData(
            QVariant::fromValue(static_cast<qulonglong>(parent ? parent->value() : 0))));
        parentDropdown_->setEnabled(!locked_);
        parentDropdown_->setToolTip(TimelineEditor::tr("Transform relative to another layer"));
        parentDropdown_->show();
        // ADR 0028 Track Matte column.
        matte_cell::populate(*matteDropdown_, *session_, rowTarget(entry));
        matteDropdown_->setEnabled(!locked_);
        blending_->setCurrentIndex(row >= 0 ? row : 0);
        blending_->setToolTip(mode.has_value()
                                  ? TimelineEditor::tr("How this layer combines with the layers "
                                                       "beneath it")
                                  : TimelineEditor::tr("This layer does not expose a blend mode"));
        for (int index = 0; index < kToggleCellCount; ++index) {
            auto* toggle = toggles_[static_cast<std::size_t>(index)];
            const QSignalBlocker blocker(toggle);
            toggle->setVisible(index != 1 || hasAudio);
            toggle->setChecked(index == 0   ? (enabled_ && !audioLayer_)
                               : index == 1 ? (enabled_ && audioConnected)
                               : index == 2 ? solo_
                                            : index == 3 && locked_);
            toggle->setEnabled(index == 1   ? hasAudio && !locked_
                               : index == 0 ? !audioLayer_
                                            : true);
            toggle->setGlyph(toggleIcon(index));
        }
        label_->setVisible(true);
        label_->setSwatch(entry.labelColor.isValid() ? entry.labelColor
                                                     : kit::color(entry.clipColor));
        label_->setEnabled(true);
        {
            const QSignalBlocker blocker(shy_);
            shy_->setVisible(true);
            shy_->setChecked(layer && layer_fields::shy(*session_, entry.layerId));
            shy_->setEnabled(true);
        }
        adjustmentMarker_->setVisible(entry.adjustment);
        sharedMarker_->setVisible(!entry.sharedWith.isEmpty());
        sharedMarker_->setToolTip(
            entry.sharedWith.isEmpty()
                ? QString{}
                : TimelineEditor::tr("Shared Layer node: also in %1").arg(entry.sharedWith));
        binding_ = false;
        update();
    }

    // Whether the entry's audio is connected: its Layer node's audio input, or (an entry layer or a
    // plain entry) its slot's own audio content.
    [[nodiscard]] static bool audioConnectedFor(const document::Composition& composition,
                                                const TimelineLayerEntry& entry,
                                                const document::LayerOutputBoundary* layer) {
        return std::ranges::any_of(composition.graph().edges(), [&](const auto& edge) {
            if (layer && !layer->entryOwned()) {
                const auto* input = std::get_if<document::NodeInputRef>(&edge.destination);
                return input && input->nodeId == layer->nodeId &&
                       input->port == document::kLayerOutputAudioInputPort;
            }
            const auto* slot = std::get_if<document::LayerStackInputRef>(&edge.destination);
            return slot && slot->stackNodeId == entry.mergeNodeId && slot->slotId == entry.slotId &&
                   slot->role == document::kLayerStackAudioInputRole;
        });
    }

  protected:
    void mousePressEvent(QMouseEvent* event) override { forwardMouse(event); }
    void mouseMoveEvent(QMouseEvent* event) override { forwardMouse(event); }
    void mouseReleaseEvent(QMouseEvent* event) override { forwardMouse(event); }
    void mouseDoubleClickEvent(QMouseEvent* event) override { forwardMouse(event); }
    void contextMenuEvent(QContextMenuEvent* event) override {
        QContextMenuEvent mapped(event->reason(), mapToParent(event->pos()), event->globalPos(),
                                 event->modifiers());
        QApplication::sendEvent(parentWidget(), &mapped);
    }
    void forwardMouse(QMouseEvent* event) {
        QMouseEvent mapped(event->type(), mapToParent(event->position().toPoint()),
                           event->globalPosition(), event->button(), event->buttons(),
                           event->modifiers());
        QApplication::sendEvent(parentWidget(), &mapped);
        event->accept();
    }

  private:
    static void retainSize(QWidget& widget) {
        auto policy = widget.sizePolicy();
        policy.setRetainSizeWhenHidden(true);
        widget.setSizePolicy(policy);
    }
    // Reuse the stack's command path for pointer and keyboard activation. Coordinates are
    // resolved on click so a pooled row always acts on its current binding.
    void activateAt(int x) {
        if (binding_)
            return;
        const QPoint local(x, std::midpoint(0, height()));
        QMouseEvent event(QEvent::MouseButtonPress, mapToParent(local), mapToGlobal(local),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(parentWidget(), &event);
    }
    std::array<kit::KIconToggle*, kToggleCellCount> toggles_{};
    kit::KSwatchButton* label_ = nullptr;
    kit::KIconToggle* shy_ = nullptr;
    kit::KLabel* adjustmentMarker_ = nullptr;
    kit::KLabel* sharedMarker_ = nullptr;
    CompositionSession* session_ = nullptr;
    TimelineLayerEntry entry_{};
    bool bound_ = false;
    bool enabled_ = true, solo_ = false, locked_ = false;
    bool audioLayer_ = false;
    bool binding_ = false;
    kit::KDropdown* blending_ = nullptr;
    kit::KDropdown* parentDropdown_ = nullptr;
    kit::KDropdown* matteDropdown_ = nullptr;
};

// ---------------------------------------------------------------------------------------------

TimelineColumnHeaders::TimelineColumnHeaders(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("timelineColumnHeaders"));
    setAccessibleName(tr("Layer columns"));
    setMinimumWidth(kLayerColumnWidthPx);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    setFixedHeight(kit::px(kit::Size::Control));
    auto* row = new kit::KRow(this);
    row->setLeadingInset(kit::px(kit::Spacing::ChromePadding));
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    QList<QWidget*> glyphs;
    for (int index = 0; index < kToggleCellCount; ++index) {
        auto* glyph = new kit::KIconButton(row);
        glyph->setObjectName(QStringLiteral("timelineHeaderToggle%1").arg(index));
        glyph->setIcon(kit::icon(toggleIcon(index), kit::Size::IconMedium, kit::Color::Muted,
                                 kit::IconWeight::Regular));
        glyph->setIconSize(QSize(kit::px(kit::Size::IconMedium), kit::px(kit::Size::IconMedium)));
        glyph->setAttribute(Qt::WA_TransparentForMouseEvents);
        glyphs.append(glyph);
    }
    // Above the Label squares: the label filter (Accent while it hides any label). Above the Shy
    // switches: the master Shy toggle (Accent while shy layers are hidden).
    labelFilter_ = new kit::KIconToggle(kit::IconId::LabelFilter, row);
    labelFilter_->setObjectName(QStringLiteral("timelineLabelFilterButton"));
    labelFilter_->setToolTip(tr("Show only layers with the chosen labels"));
    labelFilter_->setAccessibleName(tr("Label filter"));
    glyphs.append(labelFilter_);
    connect(labelFilter_, &QToolButton::clicked, this, [this] {
        // The button shows the filter's state, not its own click; the menu changes the state.
        labelFilter_->setChecked(labelFilterActive_);
        Q_EMIT labelFilterRequested(labelFilter_->mapToGlobal(QPoint(0, labelFilter_->height())));
    });
    hideShy_ = new kit::KIconToggle(kit::IconId::Shy, row);
    hideShy_->setObjectName(QStringLiteral("timelineHideShyToggle"));
    hideShy_->setToolTip(tr("Hide Shy Layers"));
    hideShy_->setAccessibleName(tr("Hide Shy Layers"));
    connect(hideShy_, &QToolButton::toggled, this,
            [this](const bool checked) { Q_EMIT hideShyToggled(checked); });
    auto* name = new QWidget(row);
    auto* nameLayout = new QHBoxLayout(name);
    nameLayout->setContentsMargins(kit::px(kit::Spacing::XS), 0, kit::px(kit::Spacing::XS), 0);
    nameLayout->setSpacing(0);
    nameLayout->addSpacing(kit::px(kit::Size::ToggleCell) + kit::px(kit::Spacing::XS));
    nameLayout->addWidget(new kit::KLabel(tr("Name"), name));
    auto* blending = makeBlendingDropdown(row);
    auto* blendingLabel = new kit::KLabel(tr("Blending"), row);
    auto* parentLabel = new kit::KLabel(tr("Parent"), row);
    auto* matteLabel = new kit::KLabel(tr("Matte"), row);
    // Each heading starts where its dropdowns' values start (a Dense dropdown's text inset), so
    // "Blending" stands over "Normal" and keeps clear of the Shy toggle before it.
    for (auto* heading : {blendingLabel, parentLabel, matteLabel})
        heading->setIndent(kit::px(kit::Spacing::ControlInset) + kit::px(kit::Spacing::XS));
    row->setCells(glyphs, name, {blendingLabel, parentLabel, matteLabel}, nullptr, kCellGap,
                  {hideShy_});
    blendingLabel->setFixedWidth(
        std::max(kit::px(kit::Size::DropdownWidth), blending->sizeHint().width()));
    delete blending;
    row->setFixedHeight(kit::px(kit::Size::Control));
    row->setProperty("headerRow", true);
    layout->addWidget(row);
}

QString TimelineColumnHeaders::toolTipAtX(const int x) { return cellToolTipAt(x); }

void TimelineColumnHeaders::setLabelFilterActive(const bool active) {
    labelFilterActive_ = active;
    const QSignalBlocker blocker(labelFilter_);
    labelFilter_->setChecked(active);
}

void TimelineColumnHeaders::setHideShy(const bool hide) {
    const QSignalBlocker blocker(hideShy_);
    hideShy_->setChecked(hide);
}

bool TimelineColumnHeaders::event(QEvent* event) {
    if (event->type() == QEvent::ToolTip) {
        const auto* help = static_cast<QHelpEvent*>(event);
        const QString text = toolTipAtX(help->pos().x());
        if (text.isEmpty()) {
            QToolTip::hideText();
        } else {
            QToolTip::showText(help->globalPos(), text, this);
        }
        event->accept();
        return true;
    }
    return QWidget::event(event);
}

// ---------------------------------------------------------------------------------------------

TimelineLayerStack::TimelineLayerStack(CompositionSession& session, QScrollBar& scrollBar,
                                       QWidget* parent)
    : kit::KListSurface(parent), session_(session), scrollBar_(scrollBar) {
    // Unchanged objectName: same role (the composition's layer stack), new primitive.
    setObjectName(QStringLiteral("layerStackView"));
    setAccessibleName(tr("Composition layers"));
    setMinimumWidth(kLayerColumnWidthPx);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    // Accepts focus so row navigation has somewhere to live, and so the frame-step shortcuts can
    // keep yielding to it exactly as they yielded to the QTreeWidget before this task.
    setFocusPolicy(Qt::StrongFocus);
    // This column consumes Up/Down/Home/End for its own row navigation but, unlike a text-entry
    // widget, does not claim ShortcutOverride for them. The marker is how the Viewer's frame-step
    // actions -- Qt::WindowShortcut, and owners of Home/End since task VIEW-1 moved the transport
    // there -- know to stand down while this column holds focus. See kDefersTransportKeysProperty.
    setProperty(kDefersTransportKeysProperty, true);
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    connect(&session_, &CompositionSession::selectionChanged, this,
            &TimelineLayerStack::syncCurrentRowFromSelection);
}

void TimelineLayerStack::syncCurrentRowFromSelection() {
    const auto selected = selectedLayer(session_);
    // #502: the selected row is the selected entry's (a shared Layer node has several rows).
    const auto selectedEntry = session_.selectedEntry();
    int resolved = -1;
    if (selectedEntry.has_value())
        for (int index = 0; index < rowCount(); ++index) {
            const auto& entry = entries_[static_cast<std::size_t>(index)];
            if (entry.rowKind == TimelineLayerEntry::Kind::Layer &&
                entry.entry() == *selectedEntry) {
                resolved = index;
                break;
            }
        }
    if (resolved < 0 && selected.has_value()) {
        for (int index = 0; index < rowCount(); ++index) {
            if (entries_[static_cast<std::size_t>(index)].layerId == *selected) {
                resolved = index;
                break;
            }
        }
    }
    if (resolved == currentRow_) {
        // Still worth repainting: the row whose FILL changed may not be the current row at all (a
        // node selection can move the contextual layer without moving this column's current row).
        relayoutRows();
        update();
        return;
    }
    currentRow_ = resolved;
    relayoutRows();
    update();
}

int TimelineLayerStack::contentHeight() const noexcept { return rowCount() * kTimelineRowHeight; }

int TimelineLayerStack::rowTop(const int row) const noexcept {
    return row * kTimelineRowHeight - scrollOffset_;
}

void TimelineLayerStack::setEntries(std::vector<TimelineLayerEntry> entries) {
    dragRow_ = -1;
    insertionRow_ = -1;
    if (insertion_)
        insertion_->hide();
    entries_ = std::move(entries);
    syncCurrentRowFromSelection();
}

void TimelineLayerStack::setScrollOffset(const int offset) {
    if (offset == scrollOffset_) {
        return;
    }
    scrollOffset_ = offset;
    setGridOffset(offset);
    relayoutRows();
    update();
}

void TimelineLayerStack::setCurrentRow(const int row) {
    if (row < 0 || row >= rowCount()) {
        return;
    }
    currentRow_ = row;
    // Navigation IS selection here (single-selection, exactly like the QTreeWidget before it): the
    // session owns selection truth, and every other editor follows it from selectionChanged.
    const auto& entry = entries_[static_cast<std::size_t>(row)];
    if (entry.mergeNodeId.isValid())
        session_.selectEntry(entry.entry());
    else
        session_.selectLayer(entry.layerId);
    relayoutRows();
    update();
}

void TimelineLayerStack::clearPooledRows() {
    for (auto* row : rowPool_)
        delete row;
    rowPool_.clear();
    for (auto* property : propertyPool_)
        delete property;
    propertyPool_.clear();
    entries_.clear();
    if (renameEditor_ != nullptr)
        renameEditor_->hide();
    renamingLayer_ = {};
    anchorRow_ = -1;
    dragRow_ = -1;
    insertionRow_ = -1;
    currentRow_ = -1;
    hoveredRow_ = -1;
    selectedRow_.reset();
    setRowStates(-1, {});
    updateRenameGeometry();
    update();
}

void TimelineLayerStack::relayoutRows() {
    const int viewportRows = (height() + kTimelineRowHeight - 1) / kTimelineRowHeight + 1;
    const int first =
        std::clamp(scrollOffset_ / kTimelineRowHeight, 0, std::max(0, rowCount() - 1));
    const int needed = std::clamp(rowCount() - first, 0, viewportRows);
    while (static_cast<int>(rowPool_.size()) < needed) {
        auto* row = new TimelineLayerRow(session_, this);
        row->openLabelMenu = [this](const TimelineLayerEntry& entry, QWidget* anchor) {
            openRowLabelMenu(entry, anchor);
        };
        row->toggleShy = [this](const TimelineLayerEntry& entry) { toggleRowShy(entry); };
        row->installEventFilter(this);
        rowPool_.push_back(row);
    }
    while (static_cast<int>(propertyPool_.size()) < needed) {
        propertyPool_.push_back(nullptr);
    }
    for (std::size_t slot = 0; slot < rowPool_.size(); ++slot) {
        auto* row = rowPool_[slot];
        auto* property = propertyPool_[slot];
        if (static_cast<int>(slot) >= needed) {
            row->hide();
            if (property)
                property->hide();
            continue;
        }
        const int index = first + static_cast<int>(slot);
        const auto& entry = entries_[static_cast<std::size_t>(index)];
        if (entry.rowKind == TimelineLayerEntry::Kind::Layer) {
            if (property)
                property->hide();
            ++work_.layerRowBinds;
            row->bind(entry, isRowSelected(session_, entry), index);
            row->setGeometry(0, rowTop(index), width(), kTimelineRowHeight);
            row->show();
        } else {
            row->hide();
            if (!property) {
                property = propertyPool_[slot] = new TimelinePropertyRow(session_, this);
                property->toggleParameter = [this](document::ParameterId parameter) {
                    emit parameterExpansionRequested(parameter);
                };
                property->toggleGroup = [this](document::LayerId layer, const QString& group) {
                    emit groupExpansionRequested(layer, group);
                };
                property->installEventFilter(this);
            }
            property->bind(entry);
            property->setGeometry(0, rowTop(index), width(), kTimelineRowHeight);
            property->show();
        }
    }
    updateRenameGeometry();
    if (underMouse())
        setHoveredRow((mapFromGlobal(QCursor::pos()).y() + scrollOffset_) / kTimelineRowHeight);
    updateRowStates();
}

TimelineLayerStack::RowKey TimelineLayerStack::rowKey(const TimelineLayerEntry& entry) {
    return {entry.layerId,   entry.rowKind, entry.parameterId,
            entry.component, entry.group,   entry.maskId};
}

bool TimelineLayerStack::rowHighlighted(const int row) const {
    if (row < 0 || row >= rowCount() || !selectedRow_)
        return false;
    const auto& entry = entries_[static_cast<std::size_t>(row)];
    return entry.rowKind != TimelineLayerEntry::Kind::Layer && *selectedRow_ == rowKey(entry) &&
           session_.selection() == selectionAtSelect_;
}

void TimelineLayerStack::selectRow(const int row) {
    if (row < 0 || row >= rowCount())
        return;
    const auto entry = entries_[static_cast<std::size_t>(row)];
    if (entry.rowKind == TimelineLayerEntry::Kind::Layer) {
        selectedRow_.reset();
        setCurrentRow(row);
        return;
    }
    if (entry.maskId.isValid())
        session_.selectMask(entry.layerId, entry.maskId);
    if (entry.rowKind == TimelineLayerEntry::Kind::Group) {
        if (!entry.maskId.isValid()) {
            if (entry.mergeNodeId.isValid())
                session_.selectEntry(entry.entry());
            else
                session_.selectLayer(entry.layerId);
        }
    } else if (!entry.parameterId.isValid()) {
        // #502: a plain entry's property row (its layer is not created yet) selects the entry.
        session_.selectEntry(entry.entry());
    } else {
        // A property row selects itself and every key it shows (all of a vector's components on
        // its parameter row, the one component on a component row).
        std::vector<KeyframeSelection> keys;
        for (const auto& key : timeline_reveal::rowKeys(session_, entry))
            keys.push_back(key.key);
        if (!keys.empty())
            session_.selectKeyframes(keys);
        else if (!entry.maskId.isValid())
            session_.selectParameter(entry.parameterId);
    }
    selectedRow_ = rowKey(entry);
    selectionAtSelect_ = session_.selection();
    updateRowStates();
}

void TimelineLayerStack::setHoveredRow(const int row) {
    const int next = row >= 0 && row < rowCount() ? row : -1;
    if (next == hoveredRow_)
        return;
    hoveredRow_ = next;
    updateRowStates();
}

void TimelineLayerStack::leaveEvent(QEvent* event) {
    setHoveredRow(-1);
    kit::KListSurface::leaveEvent(event);
}

// One hover step and one selected-row wash for every row kind: a layer row (KRow) paints its own,
// and the column paints them under the group and property rows, which paint no background.
void TimelineLayerStack::updateRowStates() {
    if (hoveredRow_ >= rowCount())
        hoveredRow_ = -1;
    for (auto* row : rowPool_)
        if (row->isVisible())
            row->setHovered((row->y() + scrollOffset_) / kTimelineRowHeight == hoveredRow_);
    std::vector<int> selected;
    const int first = std::max(0, scrollOffset_ / kTimelineRowHeight);
    const int last = std::min(rowCount() - 1, (scrollOffset_ + height()) / kTimelineRowHeight);
    for (int row = first; row <= last; ++row)
        if (rowHighlighted(row))
            selected.push_back(row);
    const bool changed = selected != selectedRows();
    setRowStates(hoveredRow_ >= 0 && entries_[static_cast<std::size_t>(hoveredRow_)].rowKind !=
                                         TimelineLayerEntry::Kind::Layer
                     ? hoveredRow_
                     : -1,
                 std::move(selected));
    if (changed)
        Q_EMIT rowHighlightsChanged();
}

void TimelineLayerStack::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    relayoutRows();
    Q_EMIT viewportResized();
}

void TimelineLayerStack::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    setFocus(Qt::MouseFocusReason);
    const int index =
        (static_cast<int>(event->position().y()) + scrollOffset_) / kTimelineRowHeight;
    if (index < 0 || index >= rowCount()) {
        session_.clearSelection();
        return;
    }
    const auto entry = entries_[static_cast<std::size_t>(index)];
    const auto id = entry.layerId;
    if (entry.rowKind != TimelineLayerEntry::Kind::Layer) {
        selectRow(index);
        event->accept();
        return;
    }
    selectedRow_.reset();
    const int chevronX = TimelineLayerRow::chevronX(entry.nesting) + kCellGap;
    if (event->position().x() >= chevronX &&
        event->position().x() < chevronX + kit::px(kit::Size::IconMedium)) {
        if (entry.mergeNodeId.isValid())
            Q_EMIT entryExpansionRequested(entry.slotId);
        else
            Q_EMIT expansionRequested(id);
        return;
    }
    const auto* composition = session_.composition();
    if (composition == nullptr)
        return;
    // #502: every row is live. A plain entry (no layer yet) is an entry layer at its defaults; a
    // switch on it creates its layer in the same undo step.
    const auto* layer = id.isValid() ? composition->graph().findLayer(id) : nullptr;
    if (layer == nullptr && !entry.mergeNodeId.isValid())
        return;
    const bool locked = layer && layer->locked;
    const int toggleX = static_cast<int>(event->position().x()) - kToggleStripX;
    const int togglePitch = kToggleCellWidth + kCellGap;
    const int toggle = toggleX / togglePitch;
    const auto compositionId = session_.compositionId();
    if (toggleX >= 0 && toggle < kToggleCellCount && toggleX % togglePitch < kToggleCellWidth) {
        if (toggle == static_cast<int>(ToggleCell::Audio)) {
            if (!entry.audioNodeId.isValid() || locked)
                return;
            if (entry.kind == tr("Audio")) {
                const bool enabled = layer == nullptr || layer->enabled;
                (void)editTargets(session_, "Toggle Audio", {rowTarget(entry)},
                                  [compositionId, enabled](const document::LayerId target) {
                                      return std::make_unique<commands::SetLayerEnabled>(
                                          compositionId, target, !enabled);
                                  });
                return;
            }
            commands::Transaction transaction("Toggle Audio", session_.snapshot().revision());
            const document::InputPortRef input =
                layer && !layer->entryOwned()
                    ? document::InputPortRef{document::NodeInputRef{
                          layer->nodeId, std::string(document::kLayerOutputAudioInputPort)}}
                    : document::InputPortRef{document::LayerStackInputRef{
                          entry.mergeNodeId, entry.slotId,
                          std::string(document::kLayerStackAudioInputRole)}};
            const bool connected =
                std::ranges::any_of(composition->graph().edges(),
                                    [&](const auto& edge) { return edge.destination == input; });
            if (connected)
                transaction.emplace<commands::DisconnectInput>(compositionId, input);
            else
                transaction.emplace<commands::ConnectPorts>(
                    compositionId, document::OutputPortRef{entry.audioNodeId, "audio"}, input);
            (void)session_.executeTransaction(std::move(transaction));
            return;
        }
        if (toggle == 0) {
            bool visible = layer == nullptr || layer->enabled;
            if (layer && !layer->entryOwned()) {
                const auto layout = composition->nodeLayout().find(layer->nodeId);
                visible = visible &&
                          (layout == composition->nodeLayout().end() || !layout->second.muted);
            }
            const auto* group =
                entry.groupMerge.isValid() ? composition->graph().merge(entry.groupMerge) : nullptr;
            const bool groupOff = group != nullptr && !group->enabled();
            visible = visible && !groupOff;
            commands::Transaction transaction("Toggle Layer", session_.snapshot().revision());
            appendLayerEdits(transaction, compositionId, {rowTarget(entry)},
                             [compositionId, visible](const document::LayerId target) {
                                 return std::make_unique<commands::SetLayerEnabled>(
                                     compositionId, target, !visible);
                             });
            // A group row shows its Merge's own enabled flag too: turning the row on turns both on.
            if (!visible && groupOff)
                transaction.emplace<commands::SetMergeEnabled>(compositionId, entry.groupMerge,
                                                               true);
            (void)session_.executeTransaction(std::move(transaction));
            return;
        }
        if (toggle == static_cast<int>(ToggleCell::Solo)) {
            const bool solo = layer && layer->solo;
            (void)editTargets(session_, "Toggle Layer", {rowTarget(entry)},
                              [compositionId, solo](const document::LayerId target) {
                                  return std::make_unique<commands::SetLayerSolo>(compositionId,
                                                                                  target, !solo);
                              });
        }
        if (toggle == static_cast<int>(ToggleCell::Lock))
            (void)editTargets(session_, "Toggle Layer", {rowTarget(entry)},
                              [compositionId, locked](const document::LayerId target) {
                                  return std::make_unique<commands::SetLayerLocked>(
                                      compositionId, target, !locked);
                              });
        return;
    }
    if (event->modifiers().testFlag(Qt::ShiftModifier) && anchorRow_ >= 0) {
        auto entries = session_.selectedEntries();
        for (int row = std::min(anchorRow_, index);
             row <= std::max(anchorRow_, index) && row < rowCount(); ++row) {
            const auto& candidate = entries_[static_cast<std::size_t>(row)];
            if (candidate.rowKind == TimelineLayerEntry::Kind::Layer &&
                candidate.mergeNodeId.isValid() &&
                std::ranges::find(entries, candidate.entry()) == entries.end())
                entries.push_back(candidate.entry());
        }
        session_.selectEntries(entries, entry.entry());
    } else if (event->modifiers().testFlag(Qt::ControlModifier)) {
        auto entries = session_.selectedEntries();
        if (std::ranges::find(entries, entry.entry()) != entries.end()) {
            std::erase(entries, entry.entry());
            if (entries.empty())
                session_.clearSelection();
            else
                session_.selectEntries(entries, entries.front());
        } else {
            entries.push_back(entry.entry());
            session_.selectEntries(entries, entry.entry());
        }
        anchorRow_ = index;
    } else {
        session_.selectEntry(entry.entry());
        anchorRow_ = index;
    }
    dragRow_ = index;
    dragStart_ = event->position().toPoint();
    dragRevision_ = session_.snapshot().revision();
    event->accept();
}

void TimelineLayerStack::mouseMoveEvent(QMouseEvent* event) {
    if (dragRow_ < 0 || !event->buttons().testFlag(Qt::LeftButton) ||
        (event->position().toPoint() - dragStart_).manhattanLength() <
            QApplication::startDragDistance())
        return;
    insertionRow_ = insertionRowAt(static_cast<int>(event->position().y()));
    showInsertionIndicator(insertionRow_);
    event->accept();
}
int TimelineLayerStack::insertionRowAt(const int y) const noexcept {
    auto row = std::clamp((y + scrollOffset_ + kTimelineRowHeight / 2) / kTimelineRowHeight, 0,
                          rowCount());
    while (row < rowCount() &&
           entries_[static_cast<std::size_t>(row)].rowKind != TimelineLayerEntry::Kind::Layer)
        ++row;
    return row;
}
void TimelineLayerStack::showInsertionIndicator(const int row) {
    if (row < 0) {
        if (insertion_)
            insertion_->hide();
        return;
    }
    if (!insertion_) {
        insertion_ = new kit::KSurface(this, kit::Color::Accent);
        insertion_->setObjectName("timelineLayerInsertionIndicator");
        insertion_->setAttribute(Qt::WA_TransparentForMouseEvents);
    }
    insertion_->setGeometry(0, rowTop(row), width(), kit::px(kit::Spacing::XXS));
    insertion_->show();
    insertion_->raise();
}
void TimelineLayerStack::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton)
        return;
    const int from = std::exchange(dragRow_, -1), to = std::exchange(insertionRow_, -1);
    if (insertion_)
        insertion_->hide();
    if (from < 0 || from >= rowCount() || to < 0 || to > rowCount())
        return;
    // #502: a row reorders within its own Merge. The drop lands before the next row of that Merge
    // at or below the line, or at the Merge's bottom once the line leaves its rows.
    const auto& moving = entries_[static_cast<std::size_t>(from)];
    std::optional<document::LayerSlotId> before;
    std::vector<int> block;
    for (int row = 0; row < rowCount(); ++row) {
        const auto& candidate = entries_[static_cast<std::size_t>(row)];
        if (candidate.rowKind == TimelineLayerEntry::Kind::Layer &&
            candidate.mergeNodeId == moving.mergeNodeId && candidate.nesting == moving.nesting)
            block.push_back(row);
    }
    // The first row of the Merge at or below the line (a line above them lands at the top);
    // none, the bottom.
    for (const int row : block)
        if (row >= to) {
            before = entries_[static_cast<std::size_t>(row)].slotId;
            break;
        }
    commands::Transaction transaction("Reorder Layer", dragRevision_);
    transaction.emplace<commands::MoveLayerBefore>(session_.compositionId(), moving.slotId, before);
    (void)session_.executeTransaction(std::move(transaction));
}
bool TimelineLayerStack::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::Enter || event->type() == QEvent::Leave) {
        auto* row = qobject_cast<QWidget*>(watched);
        if (row != nullptr && row->parentWidget() == this && row != renameEditor_ &&
            row != insertion_ && row != labelPickerAnchor_) {
            const int index = (row->y() + scrollOffset_) / kTimelineRowHeight;
            if (event->type() == QEvent::Enter)
                setHoveredRow(index);
            else if (hoveredRow_ == index)
                setHoveredRow(-1);
        }
    }
    if (watched == labelPicker_ && event->type() == QEvent::Hide && labelPickerOpen_) {
        labelPickerOpen_ = false;
        commitLabelColor();
    }
    if (renameEditor_ && event->type() == QEvent::MouseButtonPress) {
        const auto* target = qobject_cast<QWidget*>(watched);
        if (target && target != renameEditor_ && !renameEditor_->isAncestorOf(target))
            renameEditor_->clearFocus();
    }
    return kit::KListSurface::eventFilter(watched, event);
}

void TimelineLayerStack::updateRenameGeometry() {
    if (!renameEditor_)
        return;
    // #502: the row being renamed is its entry's (a shared Layer node has several rows).
    const auto found = std::ranges::find_if(entries_, [this](const auto& entry) {
        return entry.rowKind == TimelineLayerEntry::Kind::Layer &&
               (renamingSlot_.isValid() ? entry.slotId == renamingSlot_
                                        : entry.layerId == renamingLayer_);
    });
    const int first = scrollOffset_ / kTimelineRowHeight;
    const int slot = static_cast<int>(found - entries_.begin()) - first;
    if (found == entries_.end() || slot < 0 || slot >= static_cast<int>(rowPool_.size()) ||
        !rowPool_[static_cast<std::size_t>(slot)]->isVisible()) {
        renameEditor_->clearFocus();
        return;
    }
    auto* row = rowPool_[static_cast<std::size_t>(slot)];
    auto* label = row->nameLabel();
    row->layout()->activate();
    label->parentWidget()->layout()->activate();
    const auto origin = label->mapTo(this, QPoint{});
    renameEditor_->setGeometry(origin.x(), row->y() + (row->height() - renameEditor_->height()) / 2,
                               label->width(), renameEditor_->height());
    renameEditor_->raise();
}

void TimelineLayerStack::renameLayer(const document::LayerId layerId) {
    const auto found = std::ranges::find_if(entries_, [layerId](const auto& entry) {
        return entry.rowKind == TimelineLayerEntry::Kind::Layer && entry.layerId == layerId;
    });
    if (found != entries_.end())
        renameRow(static_cast<int>(found - entries_.begin()));
}

void TimelineLayerStack::renameRow(const int index) {
    if (renameEditor_)
        renameEditor_->clearFocus();
    if (index < 0 || index >= rowCount())
        return;
    const auto entry = entries_[static_cast<std::size_t>(index)];
    const auto* composition = session_.composition();
    const auto* layer = composition && entry.layerId.isValid()
                            ? composition->graph().findLayer(entry.layerId)
                            : nullptr;
    if (!layer && !entry.mergeNodeId.isValid())
        return;
    const auto originalName = layer ? layer_fields::displayName(session_, entry.layerId) : entry.name;
    auto* field = new kit::KLineEdit(originalName, this);
    renameEditor_ = field;
    renamingLayer_ = entry.layerId;
    renamingSlot_ = entry.mergeNodeId.isValid() ? entry.slotId : document::LayerSlotId{};
    field->setObjectName("timelineLayerRenameEditor");
    field->setMinimumWidth(0);
    updateRenameGeometry();
    const auto revision = session_.snapshot().revision();
    const auto compositionId = session_.compositionId();
    const auto target = rowTarget(entry);
    const auto finish = [this, field, target, revision, compositionId, originalName](bool cancel) {
        // Return, focus loss and hiding the field may all finish the same edit. Retire it first
        // so a focus signal or synchronous session refresh cannot submit a second transaction.
        if (renameEditor_ != field)
            return;
        renameEditor_.clear();
        qApp->removeEventFilter(this);
        const auto name = field->text();
        field->hide();
        field->deleteLater();
        if (!cancel && name != originalName) {
            commands::Transaction transaction("Rename Layer", revision);
            appendLayerEdits(transaction, compositionId, {target},
                             [compositionId, name = name.toStdString()](const document::LayerId l) {
                                 return std::make_unique<commands::RenameLayer>(compositionId, l,
                                                                                name);
                             });
            (void)session_.executeTransaction(std::move(transaction));
        }
    };
    // The kit inline-edit policy: Return, Escape, Tab and focus loss all commit (Escape confirms
    // everywhere; Undo is the discard). An empty name is refused: Return keeps the field open with
    // the reason, and Escape or a click away keeps the layer's current name.
    auto& policy = field->useInlineEdit(kit::KInlineEdit::nameValidator());
    connect(&policy, &kit::KInlineEdit::accepted, field,
            [this, finish](const QString&, const kit::KInlineEdit::Ending ending) {
                finish(false);
                if (ending != kit::KInlineEdit::Ending::FocusOut)
                    setFocus();
            });
    connect(&policy, &kit::KInlineEdit::refused, field,
            [this, finish](const QString&, const QString&, const kit::KInlineEdit::Ending ending) {
                finish(true);
                if (ending != kit::KInlineEdit::Ending::FocusOut)
                    setFocus();
            });
    field->show();
    field->raise();
    field->setFocus();
    field->selectAll();
    // A click on a NoFocus surface must finish the edit too, not just clicks that transfer focus.
    qApp->installEventFilter(this);
}
void TimelineLayerStack::mouseDoubleClickEvent(QMouseEvent* event) {
    const int row = (static_cast<int>(event->position().y()) + scrollOffset_) / kTimelineRowHeight;
    if (event->button() == Qt::LeftButton && row >= 0 && row < rowCount() &&
        entries_[static_cast<std::size_t>(row)].rowKind == TimelineLayerEntry::Kind::Layer &&
        event->position().x() >= kNameCellX &&
        event->position().x() < kNameCellX + nameCellWidth(width()))
        renameRow(row);
}
void TimelineLayerStack::openLabelColorPicker(const int row,
                                              std::vector<document::LayerId> layers) {
    std::vector<LayerTarget> targets;
    for (const auto layer : layers) {
        const auto entries = session_.entriesOfLayer(layer);
        targets.push_back({entries.empty() ? document::MergeEntryRef{} : entries.front(), layer});
    }
    openLabelColorPicker(row, std::move(targets));
}

void TimelineLayerStack::openLabelColorPicker(const int row, std::vector<LayerTarget> targets) {
    if (row < 0 || row >= rowCount())
        return;
    const auto& entry = entries_[static_cast<std::size_t>(row)];
    if (labelPicker_ == nullptr) {
        labelPicker_ = new kit::KColorPicker(this);
        labelPicker_->setObjectName(QStringLiteral("timelineLayerLabelColorPicker"));
        labelPicker_->installEventFilter(this);
        connect(labelPicker_, &kit::KColorPicker::editCancelled, this,
                [this] { labelPickerCancelled_ = true; });
        labelPickerAnchor_ = new QWidget(this);
        labelPickerAnchor_->setObjectName(QStringLiteral("timelineLayerLabelColorAnchor"));
        labelPickerAnchor_->setAttribute(Qt::WA_TransparentForMouseEvents);
    }
    labelPickerComposition_ = session_.compositionId();
    labelPickerTargets_ = std::move(targets);
    labelPickerInitial_ =
        entry.labelColor.isValid() ? entry.labelColor : kit::color(entry.clipColor);
    labelPickerCancelled_ = false;
    labelPicker_->setColor(kit::KColor::fromQColor(labelPickerInitial_));
    // The popup opens under the row it colours.
    labelPickerAnchor_->setGeometry(0, rowTop(row), width(), kTimelineRowHeight);
    labelPickerOpen_ = true;
    labelPicker_->openBelow(*labelPickerAnchor_);
}

void TimelineLayerStack::commitLabelColor() {
    // One commit when the popup closes, never one per drag step; Escape keeps the old colour.
    if (labelPickerCancelled_ || labelPicker_ == nullptr ||
        session_.compositionId() != labelPickerComposition_)
        return;
    const auto chosen = labelPicker_->color().toQColor();
    if (chosen.rgb() == labelPickerInitial_.rgb())
        return;
    const auto compositionId = labelPickerComposition_;
    const std::array<std::uint8_t, 3> rgb{static_cast<std::uint8_t>(chosen.red()),
                                          static_cast<std::uint8_t>(chosen.green()),
                                          static_cast<std::uint8_t>(chosen.blue())};
    (void)editTargets(session_, "Set Layer Label Color", labelPickerTargets_,
                      [compositionId, rgb](const document::LayerId layer) {
                          return std::make_unique<commands::SetLayerLabelColor>(compositionId,
                                                                                layer, rgb);
                      });
}

// A switch or label edit on a selected layer applies to the whole selection, as in a
// layer-based compositor; on any other layer it applies to that layer alone.
std::vector<document::LayerId>
TimelineLayerStack::actionLayers(const document::LayerId layer) const {
    if (!isLayerSelected(session_, layer))
        return {layer};
    std::vector<document::LayerId> layers;
    for (const auto& entry : entries_)
        if (entry.rowKind == TimelineLayerEntry::Kind::Layer && entry.layerId.isValid() &&
            isLayerSelected(session_, entry.layerId) &&
            std::ranges::find(layers, entry.layerId) == layers.end())
            layers.push_back(entry.layerId);
    return layers;
}

std::vector<LayerTarget> TimelineLayerStack::actionTargets(const TimelineLayerEntry& row) const {
    if (!isRowSelected(session_, row))
        return {rowTarget(row)};
    std::vector<LayerTarget> targets;
    for (const auto& entry : entries_)
        if (entry.rowKind == TimelineLayerEntry::Kind::Layer && isRowSelected(session_, entry))
            targets.push_back(rowTarget(entry));
    return targets;
}

void TimelineLayerStack::openLabelMenu(const document::LayerId layer, QWidget* anchor) {
    const auto found = std::ranges::find(entries_, layer, &TimelineLayerEntry::layerId);
    if (found == entries_.end())
        return;
    openRowLabelMenu(*found, anchor);
}

void TimelineLayerStack::openRowLabelMenu(const TimelineLayerEntry& entry, QWidget* anchor) {
    const auto found = std::ranges::find_if(entries_, [&](const auto& candidate) {
        return candidate.rowKind == TimelineLayerEntry::Kind::Layer &&
               candidate.slotId == entry.slotId && candidate.layerId == entry.layerId;
    });
    if (found == entries_.end())
        return;
    const int row = static_cast<int>(found - entries_.begin());
    const auto targets = actionTargets(*found);
    auto* menu = kit::makeMenu(this);
    menu->setObjectName(QStringLiteral("timelineLayerLabelMenu"));
    menu->setAttribute(Qt::WA_DeleteOnClose);
    layer_labels::populateLabelMenu(*menu, session_, targets, rowTarget(*found),
                                    [this, row, targets] { openLabelColorPicker(row, targets); });
    QWidget* origin = anchor != nullptr ? anchor : this;
    menu->popup(origin->mapToGlobal(QPoint(0, origin->height())));
}

void TimelineLayerStack::toggleShy(const document::LayerId layer) {
    const auto found = std::ranges::find(entries_, layer, &TimelineLayerEntry::layerId);
    if (found != entries_.end())
        toggleRowShy(*found);
    else
        (void)layer_fields::setShy(session_, {layer}, !layer_fields::shy(session_, layer));
}

void TimelineLayerStack::toggleRowShy(const TimelineLayerEntry& entry) {
    const bool shy = entry.layerId.isValid() && layer_fields::shy(session_, entry.layerId);
    const auto compositionId = session_.compositionId();
    (void)editTargets(session_, "Set Layer Shy", actionTargets(entry),
                      [compositionId, shy](const document::LayerId layer) {
                          return std::make_unique<commands::SetLayerShy>(compositionId, layer,
                                                                         !shy);
                      });
}

void TimelineLayerStack::contextMenuEvent(QContextMenuEvent* event) {
    const int row = (event->pos().y() + scrollOffset_) / kTimelineRowHeight;
    if (row < 0 || row >= rowCount()) {
        emit blankContextMenuRequested(event->globalPos());
        event->accept();
        return;
    }
    const auto entry = entries_[static_cast<std::size_t>(row)];
    if (entry.rowKind != TimelineLayerEntry::Kind::Layer)
        return;
    if (!isRowSelected(session_, entry)) {
        if (entry.mergeNodeId.isValid())
            session_.selectEntry(entry.entry());
        else
            session_.selectLayer(entry.layerId);
    }
    const auto compositionId = session_.compositionId();
    const auto targets = actionTargets(entry);
    auto* menu = kit::makeMenu(this);
    menu->setObjectName("timelineLayerContextMenu");
    menu->setAttribute(Qt::WA_DeleteOnClose);
    // #502: Duplicate and Delete act on the rows' layers (a plain entry gains its layer in the same
    // step), so an entry layer duplicates or leaves with its own Merge entry.
    connect(menu->addAction(tr("Duplicate")), &QAction::triggered, this,
            [this, compositionId, targets] {
                std::vector<document::LayerId> layers;
                std::vector<LayerTarget> plain;
                for (const auto& target : targets)
                    (target.layer.isValid() ? (void)layers.push_back(target.layer)
                                            : (void)plain.push_back(target));
                commands::Transaction transaction("Duplicate Layers",
                                                  session_.snapshot().revision());
                if (!layers.empty())
                    transaction.emplace<commands::DuplicateLayers>(compositionId, layers);
                appendLayerEdits(transaction, compositionId, plain,
                                 [compositionId](const document::LayerId layer) {
                                     return std::make_unique<commands::DuplicateLayers>(
                                         compositionId, std::vector{layer});
                                 });
                (void)session_.executeTransaction(std::move(transaction));
            });
    connect(menu->addAction(tr("Delete")), &QAction::triggered, this,
            [this, compositionId, targets] {
                std::vector<document::LayerId> layers;
                std::vector<LayerTarget> plain;
                for (const auto& target : targets)
                    (target.layer.isValid() ? (void)layers.push_back(target.layer)
                                            : (void)plain.push_back(target));
                commands::Transaction transaction("Delete Layers", session_.snapshot().revision());
                appendLayerEdits(transaction, compositionId, plain,
                                 [compositionId](const document::LayerId layer) {
                                     return std::make_unique<commands::DeleteLayers>(
                                         compositionId, std::vector{layer});
                                 });
                if (!layers.empty())
                    transaction.emplace<commands::DeleteLayers>(compositionId, layers);
                (void)session_.executeTransaction(std::move(transaction));
            });
    connect(menu->addAction(tr("Rename")), &QAction::triggered, this,
            [this, row] { renameRow(row); });
    auto* blending = menu->addMenu(tr("Blending"));
    std::optional<core::BlendModeGroup> previousGroup;
    const auto currentMode = entry.layerId.isValid() ? session_.blendModeForLayer(entry.layerId)
                                                     : std::optional(core::BlendMode::Normal);
    for (const auto mode : core::kBlendModes) {
        // The same groups the Blending dropdowns separate.
        if (previousGroup.has_value() && *previousGroup != core::blendModeGroup(mode))
            blending->addSeparator();
        previousGroup = core::blendModeGroup(mode);
        auto* action = blending->addAction(blendModeDisplayName(mode));
        action->setCheckable(true);
        action->setChecked(currentMode == mode);
        connect(action, &QAction::triggered, this, [this, entry, mode] {
            if (entry.layerId.isValid())
                (void)session_.setLayerBlendMode(entry.layerId, mode);
            else
                (void)session_.setEntryParameterValue(
                    entry.entry(), document::kBlendModeParameterRole,
                    document::ParameterValue{
                        static_cast<std::int64_t>(core::blendModeStoredValue(mode))},
                    tr("Set Blend Mode"));
        });
    }
    auto* colors = menu->addMenu(tr("Label Color"));
    layer_labels::populateLabelMenu(*colors, session_, targets, rowTarget(entry),
                                    [this, row, targets] { openLabelColorPicker(row, targets); });
    connect(menu->addAction(tr("Split at Playhead")), &QAction::triggered, this,
            [this, compositionId, targets] {
                const auto time = session_.currentTime();
                (void)editTargets(session_, "Split Layer at Playhead", targets,
                                  [compositionId, time](const document::LayerId layer) {
                                      return std::make_unique<commands::SplitLayerAtTime>(
                                          compositionId, layer, time);
                                  });
            });
    // #502: the entry's Layer node. Inserting one adopts the entry's values and removing it returns
    // them to the entry, so neither changes the picture.
    if (entry.mergeNodeId.isValid()) {
        menu->addSeparator();
        if (entry.directLayerNode.isValid()) {
            auto* remove = menu->addAction(tr("Remove Layer Node"));
            remove->setObjectName(QStringLiteral("timelineRemoveLayerNodeAction"));
            connect(remove, &QAction::triggered, this, [this, compositionId, entry] {
                commands::Transaction transaction("Remove Layer Node",
                                                  session_.snapshot().revision());
                transaction.emplace<commands::DissolveNode>(compositionId, entry.directLayerNode);
                (void)session_.executeTransaction(std::move(transaction));
            });
        } else {
            auto* insert = menu->addAction(tr("Insert Layer Node"));
            insert->setObjectName(QStringLiteral("timelineInsertLayerNodeAction"));
            const bool hasLayerNode =
                entry.layerId.isValid() && !session_.isEntryLayer(entry.layerId);
            insert->setEnabled(!hasLayerNode && entry.imageNodeId.isValid());
            connect(insert, &QAction::triggered, this, [this, compositionId, entry] {
                commands::Transaction transaction("Insert Layer Node",
                                                  session_.snapshot().revision());
                transaction.emplace<commands::InsertLayerNode>(compositionId, entry.mergeNodeId,
                                                               entry.slotId);
                (void)session_.executeTransaction(std::move(transaction));
            });
        }
    }
    menu->popup(event->globalPos());
}

void TimelineLayerStack::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        dragRow_ = -1;
        insertionRow_ = -1;
        if (insertion_)
            insertion_->hide();
        event->accept();
        return;
    }
    if (rowCount() == 0) {
        QWidget::keyPressEvent(event);
        return;
    }
    // Up/Down/Home/End navigate rows, exactly the keys the QTreeWidget this column replaces claimed
    // for the same purpose -- which is why the frame-step actions still have to yield while this
    // widget holds focus (see TimelineEditor's focusChanged reconciliation).
    const int from = currentRow_ < 0 ? 0 : currentRow_;
    std::optional<int> target;
    switch (event->key()) {
    case Qt::Key_Up:
        target = currentRow_ < 0 ? 0 : std::max(0, from - 1);
        break;
    case Qt::Key_Down:
        target = currentRow_ < 0 ? 0 : std::min(rowCount() - 1, from + 1);
        break;
    case Qt::Key_Home:
        target = 0;
        break;
    case Qt::Key_End:
        target = rowCount() - 1;
        break;
    default:
        break;
    }
    if (!target.has_value()) {
        QWidget::keyPressEvent(event);
        return;
    }
    const int direction = event->key() == Qt::Key_Down ? 1 : -1;
    while (*target >= 0 && *target < rowCount() &&
           entries_[static_cast<std::size_t>(*target)].rowKind != TimelineLayerEntry::Kind::Layer)
        *target += direction;
    if (*target >= 0 && *target < rowCount())
        setCurrentRow(*target);
    event->accept();
}

QString TimelineLayerStack::toolTipAt(const QPoint position) const {
    QString toggle = cellToolTipAt(position.x());
    if (!toggle.isEmpty()) {
        return toggle;
    }
    const int index = (position.y() + scrollOffset_) / kTimelineRowHeight;
    if (index < 0 || index >= rowCount()) {
        return {};
    }
    // The row's kind lives here: task T1 removes the Kind COLUMN, and this is where that
    // information keeps reaching the artist rather than being silently dropped with the column.
    const auto& entry = entries_[static_cast<std::size_t>(index)];
    auto text = tr("%1 · Layer %2 · Slot %3")
                    .arg(entry.kind)
                    .arg(entry.layerId.value())
                    .arg(entry.slotId.value());
    if (!entry.sharedWith.isEmpty())
        text += QLatin1Char('\n') + tr("Shared Layer node: also in %1").arg(entry.sharedWith);
    return text;
}

bool TimelineLayerStack::event(QEvent* event) {
    if (event->type() == QEvent::ToolTip) {
        const auto* help = static_cast<QHelpEvent*>(event);
        const QString text = toolTipAt(help->pos());
        if (text.isEmpty()) {
            QToolTip::hideText();
        } else {
            QToolTip::showText(help->globalPos(), text, this);
        }
        event->accept();
        return true;
    }
    return QWidget::event(event);
}

void TimelineLayerStack::wheelEvent(QWheelEvent* event) {
    const int steps = event->angleDelta().y() / 120;
    if (steps == 0) {
        QWidget::wheelEvent(event);
        return;
    }
    scrollBar_.setValue(scrollBar_.value() - steps * scrollBar_.singleStep());
    event->accept();
}

// ---------------------------------------------------------------------------------------------

TimelineLaneRegion::TimelineLaneRegion(CompositionSession& session, TimelineRuler& ruler,
                                       QScrollBar& scrollBar, QWidget* parent)
    : QWidget(parent), session_(session), ruler_(ruler), scrollBar_(scrollBar) {
    setObjectName(QStringLiteral("timelineLaneRegion"));
    setFocusPolicy(Qt::StrongFocus);
    setAccessibleName(tr("Layer lanes"));
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    connect(&ruler_, &TimelineRuler::axisChanged, this, &TimelineLaneRegion::invalidateAll);
    // #358: a playhead move repaints the two strips it touches, not every lane.
    connect(&session_, &CompositionSession::currentTimeChanged, this,
            &TimelineLaneRegion::invalidatePlayhead);
    connect(&session_, &CompositionSession::selectionChanged, this,
            &TimelineLaneRegion::invalidateAll);
    connect(&session_, &CompositionSession::snapshotChanged, this,
            &TimelineLaneRegion::invalidateAll);
    connect(&session_, &CompositionSession::compositionChanged, this,
            &TimelineLaneRegion::invalidateAll);
    connect(&session_, &CompositionSession::documentRebound, this, [this] {
        keySummaryCache_.clear();
        keySummaryProject_ = nullptr;
    });
}

void TimelineLaneRegion::invalidateAll() {
    ++work_.fullInvalidations;
    update();
}

void TimelineLaneRegion::invalidatePlayhead() {
    const auto axis = ruler_.axisForWidth(width());
    if (!axis || graphEditor_ || !paintedPlayheadX_) {
        invalidateAll();
        return;
    }
    ++work_.playheadInvalidations;
    // The playhead is a one-pixel line; a small margin covers the device-pixel snap either side.
    const int margin = kit::px(kit::Spacing::XXS) + 1;
    const int next = static_cast<int>(std::floor(axis->pixelForTime(session_.currentTime())));
    const int band = margin + kit::px(kit::Size::Hairline) + margin;
    for (const int x : {*paintedPlayheadX_, next})
        update(QRect(x - margin, 0, band, height()));
}

void TimelineLaneRegion::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    if (!keyframeArea_)
        return;
    keyframeArea_->setGeometry(rect());
    keyframePanel_->setGeometry(rect());
    if (keyframePanel_)
        keyframePanel_->setGridEntries(entries_, scrollOffset_);
    syncKeyframeSurfaces();
}

int TimelineLaneRegion::contentHeight() const noexcept {
    return static_cast<int>(entries_.size()) * kTimelineRowHeight;
}

int TimelineLaneRegion::rowTop(const int row) const noexcept {
    return row * kTimelineRowHeight - scrollOffset_;
}

void TimelineLaneRegion::setEntries(std::vector<TimelineLayerEntry> entries) {
    if (!keyframePanel_) {
        keyframeArea_ = new QWidget(this);
        keyframeArea_->setObjectName("timelineKeyframeArea");
        keyframePanel_ = new TimelineKeyframePanel(session_, keyframeArea_);
        keyframePanel_->setRuler(ruler_);
        keyframePanel_->setSnappingEnabled(snapping_);
        keyframePanel_->setKeysVisible(keyframesVisible_);
        keyframePanel_->setGridEntries({}, 0);
        keyframeArea_->setGeometry(rect());
        keyframePanel_->setGeometry(rect());
        graphView_ = new TimelineGraphView(session_, ruler_, this);
        graphView_->setSnappingEnabled(snapping_);
        graphView_->setEaseActions(graphEaseActions_);
        graphView_->setGeometry(rect());
        graphView_->setVisible(graphEditor_);
    }
    drag_.reset();
    guide_.reset();
    entries_ = std::move(entries);
    if (keyframePanel_)
        keyframePanel_->setGridEntries(entries_, scrollOffset_);
    if (graphView_)
        graphView_->setEntries(entries_);
    syncKeyframeSurfaces();
    syncRowHighlights();
}

void TimelineLaneRegion::syncKeyframeSurfaces() {
    if (keyframePanel_ != nullptr)
        keyframePanel_->setKeysVisible(keyframesVisible_ && !graphEditor_);
    if (graphView_ != nullptr) {
        graphView_->setGeometry(rect());
        graphView_->setVisible(graphEditor_);
        if (graphEditor_)
            graphView_->raise();
    }
}

void TimelineLaneRegion::setRowHighlight(std::function<bool(int)> highlighted) {
    rowHighlighted_ = std::move(highlighted);
    syncRowHighlights();
}

// A clicked property row's lane is a key lane (TimelineKeyframeRow), which paints itself; it takes
// the same wash through its "rowSelected" property. Only the rows that change repaint.
void TimelineLaneRegion::syncRowHighlights() {
    if (keyframePanel_ != nullptr)
        for (auto* row :
             keyframePanel_->findChildren<QWidget*>(QStringLiteral("timelineKeyframeRow"))) {
            const bool highlighted =
                rowHighlighted_ && rowHighlighted_((row->y() + scrollOffset_) / kTimelineRowHeight);
            if (row->property("rowSelected").toBool() != highlighted) {
                row->setProperty("rowSelected", highlighted);
                row->update();
            }
        }
    update();
}

void TimelineLaneRegion::setGraphEaseActions(const std::array<QAction*, 3>& actions) {
    graphEaseActions_ = actions;
    if (graphView_ != nullptr)
        graphView_->setEaseActions(actions);
}

void TimelineLaneRegion::setGraphEditorEnabled(const bool enabled) {
    if (graphEditor_ == enabled)
        return;
    graphEditor_ = enabled;
    syncKeyframeSurfaces();
    update();
}

void TimelineLaneRegion::setScrollOffset(const int offset) {
    if (offset == scrollOffset_) {
        return;
    }
    scrollOffset_ = offset;
    if (keyframePanel_)
        keyframePanel_->setGridEntries(entries_, scrollOffset_);
    syncRowHighlights();
}

void TimelineEditor::setKeyframesVisible(const bool visible) {
    if (visible && graphEditor_)
        setGraphEditorEnabled(false);
    keyframesVisible_ = visible;
    keyframesAction_->setChecked(visible);
    QSettings().setValue(QStringLiteral("timeline/keyframes-visible"), visible);
    if (lanes_ != nullptr)
        lanes_->setKeyframesVisible(visible);
}

void TimelineEditor::setGraphEditorEnabled(const bool enabled) {
    if (enabled && keyframesVisible_)
        setKeyframesVisible(false);
    graphEditor_ = enabled;
    graphAction_->setChecked(enabled);
    if (graphPaneAction_ != nullptr)
        graphPaneAction_->setChecked(enabled);
    QSettings().setValue(QStringLiteral("timeline/graph-editor"), enabled);
    // The graph's value gutter is kept clear before frame 0 on the one shared time axis.
    if (ruler_ != nullptr)
        ruler_->setLeadingInset(enabled ? kit::px(kit::Size::GraphValueAxis) : 0);
    if (lanes_ != nullptr)
        lanes_->setGraphEditorEnabled(enabled);
}

void TimelineEditor::setSnappingEnabled(const bool enabled) {
    snapping_ = enabled;
    snapAction_->setChecked(enabled);
    QSettings().setValue(QStringLiteral("timeline/snapping"), enabled);
    if (lanes_ != nullptr)
        lanes_->setSnappingEnabled(enabled);
}

void TimelineEditor::applyApplicationPreferences(const ApplicationPreferences& preferences) {
    setTimecodeFormat(preferences.timelineTimeFormat == TimelineTimeFormat::Timecode);
    setSnappingEnabled(preferences.timelineSnapping);
    // Keyframes and the graph editor are mutually exclusive in this panel; the Preferences window
    // enforces the same rule, so the stored value is already consistent. Apply the graph choice
    // last so a legacy inconsistent pair resolves the way the panel's own menus would.
    setKeyframesVisible(preferences.timelineKeyframesVisible);
    setGraphEditorEnabled(preferences.timelineGraphEditor);
    if (preferences.timelineLayerColumnWidth > 0) {
        setLayerColumnWidth(preferences.timelineLayerColumnWidth, /*persist=*/false);
    }
}

void TimelineLaneRegion::setKeyframesVisible(const bool visible) {
    if (keyframesVisible_ == visible)
        return;
    keyframesVisible_ = visible;
    syncKeyframeSurfaces();
    update();
}

void TimelineLaneRegion::setSnappingEnabled(const bool enabled) {
    snapping_ = enabled;
    if (keyframePanel_)
        keyframePanel_->setSnappingEnabled(enabled);
    if (graphView_)
        graphView_->setSnappingEnabled(enabled);
}

std::optional<QRect> TimelineLaneRegion::clipBarRect(const int row) const {
    if (row < 0 || row >= static_cast<int>(entries_.size())) {
        return std::nullopt;
    }
    const auto* composition = session_.composition();
    if (composition == nullptr) {
        return std::nullopt;
    }
    const auto axis = ruler_.axisForWidth(width());
    if (!axis.has_value()) {
        return std::nullopt;
    }
    const auto& rowEntry = entries_[static_cast<std::size_t>(row)];
    const auto* layer =
        rowEntry.layerId.isValid() ? composition->graph().findLayer(rowEntry.layerId) : nullptr;
    // #502: a plain entry spans the composition, as its default layer does.
    if ((!layer && !rowEntry.mergeNodeId.isValid()) ||
        rowEntry.rowKind != TimelineLayerEntry::Kind::Layer)
        return std::nullopt;
    const bool dragged = drag_ && (layer ? drag_->layer == layer->layerId
                                         : !drag_->layer.isValid() &&
                                               drag_->entry == rowEntry.entry());
    const auto range =
        dragged ? drag_->preview
        : layer ? document::WorkArea{layer->inPoint, layer->endPoint(composition->duration())}
                : document::WorkArea{core::RationalTime{}, composition->duration()};
    const qreal mappedLeft = axis->pixelForTime(range.start);
    const qreal mappedRight = axis->pixelForTime(range.end);
    if (mappedRight < 0 || mappedLeft >= width())
        return std::nullopt;
    const qreal clipMargin = kit::px(kit::Spacing::M);
    const qreal left = std::max(-clipMargin, mappedLeft);
    const qreal right = std::min(static_cast<qreal>(width()) + clipMargin, mappedRight);
    const int top = rowTop(row) + (kTimelineRowHeight - kit::px(kit::Size::TimelineBar)) / 2;
    const int barWidth = std::max(1, static_cast<int>(std::lround(right - left)) + 1);
    return QRect(static_cast<int>(std::lround(left)), top, barWidth,
                 kit::px(kit::Size::TimelineBar));
}

// ADR 0024: the part of a layer's bar outside its content extent (where its before/after mode
// decides what shows) is drawn sunken, so the media extent reads inside the bar.
void TimelineLaneRegion::paintContentExtent(QPainter& painter, const int row,
                                            const QRect& bar) const {
    const auto* composition = session_.composition();
    const auto axis = ruler_.axisForWidth(width());
    if (composition == nullptr || !axis || row < 0 || row >= static_cast<int>(entries_.size()))
        return;
    const auto* layer =
        composition->graph().findLayer(entries_[static_cast<std::size_t>(row)].layerId);
    if (layer == nullptr)
        return;
    auto timing = layer->timing;
    if (drag_ && drag_->layer == layer->layerId) {
        timing.startTime = drag_->previewStart;
        timing.speed = drag_->previewSpeed;
    }
    const auto extent =
        runtime::resolveLayerExtent(session_.snapshot().project(), *composition, timing);
    if (!extent)
        return;
    const auto first = runtime::placementTimeOfLayerTime(timing, {});
    const auto last = runtime::placementTimeOfLayerTime(timing, extent->duration);
    if (!first || !last)
        return;
    const auto from = std::lround(axis->pixelForTime(*first));
    const auto to = std::lround(axis->pixelForTime(*last));
    const QColor shade =
        kit::withOpacity(kit::color(kit::Color::SurfaceSunken), kit::kDimOverlayOpacity);
    const auto hairline = kit::px(kit::Size::Hairline);
    const auto inner = bar.adjusted(0, hairline, 0, -hairline);
    const auto top = inner.top();
    const auto height = inner.height();
    if (from > bar.left())
        painter.fillRect(QRect(bar.left(), top,
                               static_cast<int>(std::min<long>(from, bar.right()) - bar.left()),
                               height),
                         shade);
    if (to < bar.right())
        painter.fillRect(QRect(static_cast<int>(std::max<long>(to, bar.left())), top,
                               static_cast<int>(bar.right() - std::max<long>(to, bar.left())),
                               height),
                         shade);
}

std::vector<core::RationalTime> TimelineLaneRegion::keySummaryTimes(int row) const {
    if (!keyframesVisible_ || row < 0 || row >= static_cast<int>(entries_.size()))
        return {};
    const auto& entry = entries_[static_cast<std::size_t>(row)];
    if (entry.rowKind != TimelineLayerEntry::Kind::Layer || entry.expanded)
        return {};
    // #358: the summary depends only on the document state (keys, drivers, layer clocks), so it is
    // built once per state and layer instead of on every paint.
    const auto& snapshot = session_.snapshot();
    if (keySummaryRevision_ != snapshot.revision() || keySummaryProject_ != &snapshot.project() ||
        keySummaryComposition_ != session_.compositionId()) {
        keySummaryCache_.clear();
        keySummaryRevision_ = snapshot.revision();
        keySummaryProject_ = &snapshot.project();
        keySummaryComposition_ = session_.compositionId();
    }
    // A shared Layer node's rows summarise the same keys; a plain entry has none.
    const auto [found, inserted] = keySummaryCache_.try_emplace(entry.layerId.value());
    if (inserted) {
        ++work_.keySummaryBuilds;
        found->second = computeKeySummaryTimes(row);
    }
    return found->second;
}

std::vector<core::RationalTime> TimelineLaneRegion::computeKeySummaryTimes(int row) const {
    std::set<core::RationalTime> times;
    // Hidden keys are hidden from the HIT TEST too, because this one answer is what both the
    // painter and the summary-glyph click read. An affordance nobody can see must not be one
    // anybody can hit.
    if (!keyframesVisible_ || row < 0 || row >= static_cast<int>(entries_.size()))
        return {};
    const auto& entry = entries_[static_cast<std::size_t>(row)];
    const auto* composition = session_.composition();
    if (!composition || entry.rowKind != TimelineLayerEntry::Kind::Layer || entry.expanded ||
        !entry.layerId.isValid())
        return {};
    std::vector<document::NodeId> nodes;
    for (const auto nodeId : {session_.boundaryNodeForLayer(entry.layerId),
                              session_.directSourceNodeForLayer(entry.layerId)})
        if (nodeId)
            nodes.push_back(*nodeId);
    // Task DRIVE-1: a collapsed layer summarises every key that moves it, and a key on the value
    // node driving one of its parameters moves it exactly as its own keys do. The same walk the
    // twirl-down uses to build its upstream groups, so the summary and the expanded rows can never
    // disagree about which keys belong to this layer.
    for (const auto& upstream : session_.upstreamNodes(nodes, UpstreamTraversal::DriverLinksOnly))
        nodes.push_back(upstream.id);
    // Every key of this layer's own parameters and upstream rows is stored on the layer's clock
    // (ADR 0024) and summarised at its placement time startTime + tau / speed.
    const auto* layerRecord = composition->graph().findLayer(entry.layerId);
    const KeyClock clock = layerRecord ? KeyClock(layerRecord->timing) : std::nullopt;
    // #502: an entry layer's own parameters are bound on the layer, not on a node.
    std::vector<std::span<const document::ParameterBinding>> bindings;
    if (layerRecord && layerRecord->entryOwned())
        bindings.emplace_back(layerRecord->parameters);
    for (const auto nodeId : nodes)
        if (const auto* node = composition->graph().findNode(nodeId))
            bindings.emplace_back(node->parameters);
    for (const auto parameters : bindings) {
        for (const auto& binding : parameters) {
            const auto* parameter = composition->parameters().find(binding.parameterId);
            const auto* source =
                parameter ? std::get_if<document::AnimationCurveSource>(&parameter->source)
                          : nullptr;
            const auto* curve =
                source ? composition->animationCurves().find(source->curveId) : nullptr;
            if (curve)
                std::visit(
                    [&](const auto& record) {
                        using Curve = std::decay_t<decltype(record)>;
                        if constexpr (document::kWholeValueAnimationCurve<Curve>) {
                            for (const auto& key : record.keyframes)
                                times.insert(projectedKeyTime(clock, key.time));
                        } else {
                            for (const auto& component : record.components)
                                for (const auto& key : component.keyframes)
                                    times.insert(projectedKeyTime(clock, key.time));
                        }
                    },
                    *curve);
        }
    }
    // ADR 0028: a layer mask's parameters are keyed outside any node binding but move the layer.
    if (layerRecord)
        for (const auto& mask : layerRecord->masks)
            for (const auto& [parameterId, schemaKey] : document::layerMaskParameters(mask)) {
                (void)schemaKey;
                const auto* parameter = composition->parameters().find(parameterId);
                const auto* source =
                    parameter ? std::get_if<document::AnimationCurveSource>(&parameter->source)
                              : nullptr;
                const auto* curve =
                    source ? composition->animationCurves().find(source->curveId) : nullptr;
                if (curve)
                    std::visit(
                        [&](const auto& record) {
                            using Curve = std::decay_t<decltype(record)>;
                            if constexpr (document::kWholeValueAnimationCurve<Curve>) {
                                for (const auto& key : record.keyframes)
                                    times.insert(projectedKeyTime(clock, key.time));
                            }
                        },
                        *curve);
            }
    return {times.begin(), times.end()};
}

void TimelineLaneRegion::paintEvent(QPaintEvent* event) {
    QPainter painter(this);
    // #401: a playback tick dirties only the playhead's two strips, so per-row work (the content
    // extent, the waveform, the key summary) runs only for bars the dirty region crosses.
    const QRect dirty = event->rect();
    painter.fillRect(rect(), kit::color(kit::Color::SurfaceSunken));
    if (graphEditor_) {
        // In graph mode this region owns nothing but the backdrop: the curve view covers it and
        // paints every row-shaped thing itself. Painting lanes underneath would be ink nobody can
        // see and a second answer about where a key is.
        painter.fillRect(rect(), kit::color(kit::Color::Surface));
        return;
    }

    for (int row = scrollOffset_ / kTimelineRowHeight;
         row <= (scrollOffset_ + height()) / kTimelineRowHeight; ++row) {
        const int top = rowTop(row);
        painter.fillRect(QRect(0, top, width(), kTimelineRowHeight),
                         kit::color(kit::Color::Surface));
        paintRowSeparator(painter, top, width());
    }
    const int firstRow = std::max(0, scrollOffset_ / kTimelineRowHeight);
    const int lastRow = std::min(static_cast<int>(entries_.size()) - 1,
                                 (scrollOffset_ + height()) / kTimelineRowHeight);
    for (int row = firstRow; row <= lastRow; ++row) {
        const auto& entry = entries_[static_cast<std::size_t>(row)];
        const int top = rowTop(row);
        painter.setRenderHint(QPainter::Antialiasing, false);
        painter.fillRect(QRect(0, top, width(), kTimelineRowHeight),
                         kit::color(kit::Color::Surface));
        const bool selected = isRowSelected(session_, entry);
        if ((selected && entry.rowKind == TimelineLayerEntry::Kind::Layer) ||
            (rowHighlighted_ && rowHighlighted_(row))) {
            paintSelectedRowFill(painter, top, width());
        }
        paintRowSeparator(painter, top, width());
        if (const auto bar = clipBarRect(row);
            bar && bar->adjusted(-kit::px(kit::Spacing::S), 0, kit::px(kit::Spacing::S), 0)
                       .intersects(dirty)) {
            painter.setRenderHint(QPainter::Antialiasing, true);
            const auto fill =
                entry.labelColor.isValid() ? entry.labelColor : kit::color(entry.clipColor);
            const auto shadow = kit::shadow(kit::Elevation::TimelineBar);
            kit::fillRoundedSurface(painter,
                                    QRectF(*bar).translated(shadow.offsetX, shadow.offsetY),
                                    shadow.color, QColor(), kit::Radius::Small);
            kit::fillRoundedSurface(painter, QRectF(*bar).adjusted(0.5, 0.5, -0.5, -0.5), fill,
                                    kit::hoverFillFor(fill), kit::Radius::Small);
            const int grip = kit::px(kit::Spacing::XXS);
            const int stripe = kit::px(kit::Size::TimelineWorkAreaHandle) / 2;
            painter.fillRect(QRect(bar->left() + grip, bar->top() + kit::px(kit::Size::Hairline),
                                   stripe, bar->height() - (grip + grip)),
                             fill);
            painter.fillRect(QRect(bar->right() - grip, bar->top() + grip,
                                   kit::px(kit::Size::Hairline), bar->height() - (grip + grip)),
                             kit::hoverFillFor(fill));
            paintContentExtent(painter, row, *bar);
            if (entry.audioNodeId.isValid() && entry.waveform != nullptr &&
                !entry.waveform->buckets.empty()) {
                painter.setPen(QPen(kit::color(kit::Color::DataAudio), kit::kHairlineWidth));
                const auto& buckets = entry.waveform->buckets;
                for (std::size_t bucket = 0; bucket < buckets.size(); ++bucket) {
                    float minimum = 0.0F;
                    float maximum = 0.0F;
                    bool hasRange = false;
                    for (const auto& range : buckets[bucket]) {
                        minimum = hasRange ? std::min(minimum, range.minimum) : range.minimum;
                        maximum = hasRange ? std::max(maximum, range.maximum) : range.maximum;
                        hasRange = true;
                    }
                    if (!hasRange)
                        continue;
                    const auto x = static_cast<qreal>(bar->left()) +
                                   (static_cast<qreal>(bucket) + 0.5) * bar->width() /
                                       static_cast<qreal>(buckets.size());
                    const auto centre = static_cast<qreal>(bar->center().y());
                    const auto halfHeight = static_cast<qreal>(bar->height() - (grip + grip)) * 0.5;
                    painter.drawLine(QPointF(x, centre - static_cast<qreal>(maximum) * halfHeight),
                                     QPointF(x, centre - static_cast<qreal>(minimum) * halfHeight));
                }
            }
        }
        if (const auto axis = ruler_.axisForWidth(width())) {
            painter.setRenderHint(QPainter::Antialiasing, true);
            painter.setPen(kit::color(kit::Color::Keyframe));
            painter.setBrush(kit::color(kit::Color::Keyframe));
            const qreal radius = kit::px(kit::Spacing::XXS) + kit::kHairlineWidth;
            for (const auto time : keySummaryTimes(row)) {
                if (time.toSeconds() < axis->t0 || time.toSeconds() >= axis->t1)
                    continue;
                const qreal x = axis->pixelForTime(time), y = top + kTimelineRowHeight / 2.0;
                painter.drawPolygon(QPolygonF{QPointF(x, y - radius), QPointF(x + radius, y),
                                              QPointF(x, y + radius), QPointF(x - radius, y)});
            }
        }
    }

    if (guide_) {
        if (const auto mapping = ruler_.axisForWidth(width())) {
            painter.setPen(
                QPen(kit::color(kit::Color::Foreground), kit::kHairlineWidth, Qt::DashLine));
            const auto x = mapping->pixelForTime(*guide_);
            painter.drawLine(QPointF(x, 0), QPointF(x, height()));
        }
    }

    const auto* composition = session_.composition();
    if (composition == nullptr) {
        return;
    }
    const auto axis = ruler_.axisForWidth(width());
    if (!axis.has_value()) {
        return;
    }
    // One stroke for the whole region, painted after every lane, so the playhead reads as a single
    // continuous line down all of them and moving it never touches a layout.
    paintPlayheadLine(painter, *axis, session_.currentTime(), height());
    paintedPlayheadX_ = static_cast<int>(std::floor(axis->pixelForTime(session_.currentTime())));
}

void TimelineLaneRegion::setDropMarker(const std::optional<core::RationalTime> time,
                                       const int row) {
    dropTime_ = time;
    dropRow_ = row;
    // Child widgets above the key lanes and graph rows, like the layer stack's insertion line, so
    // expanded property rows never hide them.
    const auto marker = [this](QWidget*& widget, const char* name) {
        if (widget == nullptr) {
            widget = new kit::KSurface(this, kit::Color::Accent);
            widget->setObjectName(QString::fromLatin1(name));
            widget->setAttribute(Qt::WA_TransparentForMouseEvents);
        }
        return widget;
    };
    const auto mapping = time ? ruler_.axisForWidth(width()) : std::nullopt;
    if (mapping) {
        auto* line = marker(dropTimeMarker_, "timelineDropTimeMarker");
        line->setGeometry(static_cast<int>(std::floor(mapping->pixelForTime(*time))), 0,
                          kit::px(kit::Spacing::XXS), height());
        line->show();
        line->raise();
    } else if (dropTimeMarker_ != nullptr) {
        dropTimeMarker_->hide();
    }
    if (row >= 0) {
        auto* line = marker(dropRowLine_, "timelineDropRowLine");
        line->setGeometry(0, rowTop(row), width(), kit::px(kit::Spacing::XXS));
        line->show();
        line->raise();
    } else if (dropRowLine_ != nullptr) {
        dropRowLine_->hide();
    }
}

void TimelineLaneRegion::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    setFocus(Qt::MouseFocusReason);
    const int row = (static_cast<int>(event->position().y()) + scrollOffset_) / kTimelineRowHeight;
    if (const auto axis = ruler_.axisForWidth(width())) {
        const auto center = rowTop(row) + kTimelineRowHeight / 2;
        if (std::abs(event->position().y() - center) <= kit::px(kit::Spacing::S))
            for (const auto time : keySummaryTimes(row)) {
                if (time.toSeconds() < axis->t0 || time.toSeconds() >= axis->t1)
                    continue;
                if (std::abs(axis->pixelForTime(time) - event->position().x()) <=
                    kit::px(kit::Spacing::S)) {
                    const auto& summaryRow = entries_[static_cast<std::size_t>(row)];
                    if (summaryRow.mergeNodeId.isValid())
                        Q_EMIT entryExpansionRequested(summaryRow.slotId);
                    else
                        Q_EMIT expansionRequested(summaryRow.layerId);
                    event->accept();
                    return;
                }
            }
    }
    const auto bar = clipBarRect(row);
    const auto* composition = session_.composition();
    const auto mapping = ruler_.axisForWidth(width());
    if (bar && bar->contains(event->position().toPoint()) && composition && mapping) {
        const auto rowEntry = entries_[static_cast<std::size_t>(row)];
        const auto layerId = rowEntry.layerId;
        const auto* layer = layerId.isValid() ? composition->graph().findLayer(layerId) : nullptr;
        // #502: a plain entry's bar is its default layer's: the whole composition, identity time.
        const auto range =
            layer ? document::WorkArea{layer->inPoint, layer->endPoint(composition->duration())}
                  : document::WorkArea{core::RationalTime{}, composition->duration()};
        const document::LayerTiming timing = layer ? layer->timing : document::LayerTiming{};
        const bool locked = layer && layer->locked;
        const int hit = kit::px(kit::Spacing::S);
        const auto x = event->position().x();
        const int handle = std::abs(x - bar->left()) <= hit    ? -1
                           : std::abs(x - bar->right()) <= hit ? 1
                                                               : 0;
        if (rowEntry.mergeNodeId.isValid())
            session_.selectEntry(rowEntry.entry());
        else
            session_.selectLayer(layerId);
        if (!locked) {
            drag_ = RangeDrag{layerId, rowEntry.entry(), session_.snapshot().revision(), range,
                              range,   mapping->secondsForPixel(x),     handle};
            drag_->alt = event->modifiers().testFlag(Qt::AltModifier);
            drag_->startTime = drag_->previewStart = timing.startTime;
            drag_->speed = drag_->previewSpeed = timing.speed;
            // The content extent in placement time bounds how far a trim may leave the
            // composition (ADR 0024).
            if (layer)
                if (const auto extent = runtime::resolveLayerExtent(session_.snapshot().project(),
                                                                    *composition, timing)) {
                    drag_->extentStart = runtime::placementTimeOfLayerTime(timing, {});
                    drag_->extentEnd = runtime::placementTimeOfLayerTime(timing, extent->duration);
                }
        }
        update();
        event->accept();
        return;
    }
    ruler_.beginScrub(static_cast<int>(event->position().x()));
    event->accept();
}
void TimelineLaneRegion::mouseMoveEvent(QMouseEvent* event) {
    if (!drag_) {
        const int row =
            (static_cast<int>(event->position().y()) + scrollOffset_) / kTimelineRowHeight;
        const auto bar = clipBarRect(row);
        const auto* composition = session_.composition();
        const auto* layer =
            composition && row >= 0 && row < static_cast<int>(entries_.size())
                ? composition->graph().findLayer(entries_[static_cast<std::size_t>(row)].layerId)
                : nullptr;
        const bool plain = row >= 0 && row < static_cast<int>(entries_.size()) && !layer &&
                           entries_[static_cast<std::size_t>(row)].mergeNodeId.isValid();
        const bool edge =
            bar && (plain || (layer && !layer->locked)) &&
            bar->contains(event->position().toPoint()) &&
            (std::abs(event->position().x() - bar->left()) <= kit::px(kit::Spacing::S) ||
             std::abs(event->position().x() - bar->right()) <= kit::px(kit::Spacing::S));
        setCursor(edge ? Qt::SizeHorCursor : Qt::ArrowCursor);
        ruler_.updateScrub(static_cast<int>(event->position().x()));
        event->accept();
        return;
    }
    const auto mapping = ruler_.axisForWidth(width());
    const auto* composition = session_.composition();
    if (!mapping || !composition || session_.snapshot().revision() != drag_->revision) {
        drag_.reset();
        guide_.reset();
        update();
        return;
    }
    double delta = mapping->secondsForPixel(event->position().x()) - drag_->pressSeconds;
    const double start = drag_->original.start.toSeconds();
    const double end = drag_->original.end.toSeconds();
    guide_.reset();
    // Magnetic snapping moves the dragged edge(s) onto a target; a slip moves no edge.
    if (snapping_ && !event->modifiers().testFlag(Qt::ShiftModifier) &&
        !(drag_->alt && drag_->handle == 0)) {
        std::vector<core::RationalTime> targets{session_.currentTime(), session_.workArea().start,
                                                session_.workArea().end};
        for (const auto& layer : composition->graph().layerOutputs())
            if (layer.layerId != drag_->layer) {
                targets.push_back(layer.inPoint);
                targets.push_back(layer.endPoint(composition->duration()));
            }
        double best = kit::px(kit::Spacing::S), adjustment = 0;
        for (const auto target : targets) {
            for (const int edge : {-1, 1}) {
                if (drag_->handle != 0 && drag_->handle != edge)
                    continue;
                const auto seconds = (edge < 0 ? start : end) + delta;
                const auto distance =
                    std::abs(mapping->pixelForTime(target) - mapping->pixelForSeconds(seconds));
                if (distance < best) {
                    best = distance;
                    adjustment = target.toSeconds() - seconds;
                    guide_ = target;
                }
            }
        }
        delta += adjustment;
    }
    const auto proposal = proposeLayerTiming({.start = drag_->startTime,
                                              .in = drag_->original.start,
                                              .out = drag_->original.end,
                                              .speed = drag_->speed,
                                              .rate = composition->format().frameRate(),
                                              .duration = composition->duration(),
                                              .extentStart = drag_->extentStart,
                                              .extentEnd = drag_->extentEnd,
                                              .handle = drag_->handle,
                                              .alt = drag_->alt,
                                              .deltaSeconds = delta});
    if (proposal) {
        drag_->preview = {proposal->in, proposal->out};
        drag_->previewStart = proposal->start;
        drag_->previewSpeed = proposal->speed;
    }
    update();
    event->accept();
}
void TimelineLaneRegion::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    if (drag_) {
        mouseMoveEvent(event);
        if (!drag_)
            return;
        const auto completed = *drag_;
        drag_.reset();
        guide_.reset();
        const auto* composition = session_.composition();
        const auto* boundary = composition && completed.layer.isValid()
                                   ? composition->graph().findLayer(completed.layer)
                                   : nullptr;
        if (boundary == nullptr && !completed.entry.merge.isValid())
            return;
        if (completed.preview == completed.original && completed.previewStart == completed.startTime &&
            completed.previewSpeed == completed.speed)
            return;
        // ADR 0024: every bar gesture commits one SetLayerTiming (move, slip, trim or stretch).
        // #502: a plain entry's first bar edit creates its layer in the same step.
        const char* name = completed.handle == 0 ? (completed.alt ? "Slip Layer" : "Move Layer")
                                                 : (completed.alt ? "Stretch Layer" : "Trim Layer");
        commands::Transaction transaction(name, completed.revision);
        const auto compositionId = session_.compositionId();
        appendLayerEdits(transaction, compositionId, {LayerTarget{completed.entry, completed.layer}},
                         [compositionId, completed](const document::LayerId layer) {
                             return std::make_unique<commands::SetLayerTiming>(
                                 compositionId, layer, completed.previewStart,
                                 completed.preview.start, completed.preview.end,
                                 completed.previewSpeed);
                         });
        (void)session_.executeTransaction(std::move(transaction));
        update();
        event->accept();
        return;
    }
    ruler_.endScrub(static_cast<int>(event->position().x()));
    event->accept();
}
void TimelineLaneRegion::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        drag_.reset();
        guide_.reset();
        update();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void TimelineLaneRegion::wheelEvent(QWheelEvent* event) {
    if (ruler_.handleWheel(event)) {
        return;
    }
    const int steps = event->angleDelta().y() / 120;
    if (steps == 0) {
        QWidget::wheelEvent(event);
        return;
    }
    scrollBar_.setValue(scrollBar_.value() - steps * scrollBar_.singleStep());
    event->accept();
}

// ---------------------------------------------------------------------------------------------

// task TL-FIX2. The property-row hierarchy's depth-1 (group) indent: a spacer width
// TimelinePropertyRow inserts before its own KPropertyRow (see timeline_property_rows.cpp's
// propertyRowIndent(), which adds one more Spacing::M step per depth beyond this). Chosen so that
// once KPropertyRow's own leading-indicator layout (RowPadding, its ToggleCell-wide indicator, and
// the XXS gap the property row's spacing puts before the label) is added on top, a depth-1 label
// lands EXACTLY one Spacing::M step to the right of where the layer row's own name text starts --
// so the full depth series (layer, group, parameter, component) indents by one consistent step
// throughout, not just from group onward.
int TimelineEditor::propertyNameIndent() {
    constexpr int kLayerNameX = kNameCellX + kit::px(kit::Spacing::XS) + kit::px(kit::Spacing::XS) +
                                kit::px(kit::Size::ToggleCell);
    constexpr int kPropertyRowIndicatorPrefix =
        kit::px(kit::Spacing::RowPadding) + kit::px(kit::Size::ToggleCell) +
        kit::px(kit::Spacing::XXS) + kit::px(kit::Spacing::XXS) + kit::px(kit::Spacing::XS);
    return kLayerNameX + kit::px(kit::Spacing::M) - kPropertyRowIndicatorPrefix;
}

int TimelineEditor::layerColumnWidth() { return kLayerColumnWidthPx; }

// task TL-FIX2: the floor a dragged split can shrink the layer table to -- the leading switch
// strip, the name column at its own documented minimum, the Shy switch and the three dropdown
// columns, still all visible. Below this the table would have to start hiding columns, which
// dragging never does.
int TimelineEditor::minLayerColumnWidth() const noexcept {
    return kToggleColumnWidth + kNameCellMinWidth + kSwitchColumnWidth +
           kRightColumnCount * kColumnWidth;
}

TimelineEditor::TimelineEditor(CompositionSession& session,
                               CompositionPreviewController& previewController, QWidget* parent)
    : QWidget(parent), session_(session) {
    setObjectName("timelineEditor");
    // Assets items and files dropped here become layers (#380).
    installLayerDropTarget(
        *this, session_,
        {.hit = [this](const QPoint position) { return layerDropHit(position); },
         .indicate = [this](const std::optional<LayerDropHit>& hit) { indicateLayerDrop(hit); }});
    setAccessibleName(tr("Layers timeline"));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    createHeaderMenus();
    chrome_.splitPosition = [this] {
        return width() > 0 ? std::min(layerColumnWidth_, width()) : layerColumnWidth_;
    };
    chrome_.maximizeInLeadingHeader = true;
    chrome_.hosted = [this] {
        headerFallback_->hide();
        for (auto* action : actions())
            headerRight_->addAction(action);
    };
    for (const auto& [key, start] : {std::pair{Qt::Key_B, true}, std::pair{Qt::Key_N, false}}) {
        auto* action = new QAction(this);
        action->setObjectName(start ? "timelineSetWorkAreaStartAction"
                                    : "timelineSetWorkAreaEndAction");
        action->setShortcut(QKeySequence(key));
        action->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        addAction(action);
        connect(action, &QAction::triggered, this, [this, start] {
            auto area = session_.workArea();
            if (start)
                area.start = session_.currentTime();
            else
                area.end = session_.currentTime();
            commands::Transaction transaction("Set Work Area", session_.snapshot().revision());
            transaction.emplace<commands::SetWorkArea>(session_.compositionId(), area.start,
                                                       area.end);
            (void)session_.executeTransaction(std::move(transaction));
        });
    }
    workArea_ = new TimelineWorkAreaRow(session_, this);
    // The persisted state is read ONCE, here, and then applied through the same setters the click
    // handlers use, so a restored session and a clicked toggle cannot take different paths.
    const QSettings settings;
    keyframesVisible_ = settings.value(QStringLiteral("timeline/keyframes-visible"), true).toBool();
    snapping_ = settings.value(QStringLiteral("timeline/snapping"), true).toBool();
    // task TL-FIX2 / WORKSPACE-1: a persisted width remains authoritative. Without one, the
    // 37%-of-timeline default is applied in showEvent(), when this widget has real geometry; the
    // fixed token width is only the construction-time floor for the not-yet-laid-out panel.
    layerColumnWidth_ = std::max(
        minLayerColumnWidth(),
        settings.value(QLatin1StringView(kLayerColumnWidthSetting), layerColumnWidth()).toInt());
    graphEditor_ = settings.value(QStringLiteral("timeline/graph-editor"), false).toBool();
    if (graphEditor_)
        keyframesVisible_ = false;
    keyframesAction_->setChecked(keyframesVisible_);
    graphAction_->setChecked(graphEditor_);
    snapAction_->setChecked(snapping_);
    ruler_ = new TimelineRuler(session_, previewController, this);
    ruler_->setFixedHeight(kit::px(kit::Size::HeaderRow));
    ruler_->setTimecodeLabels(timecodeFormat_);
    workArea_->setRuler(*ruler_);
    headerMenus_ = EditorArea::buildChromeRow(chrome_.header, this);
    for (auto* action : actions())
        headerMenus_->addAction(action);
    headerRight_ = EditorArea::buildCanvasChrome({"timelineHeaderRow",
                                                  "timelineHeaderScrollGutter",
                                                  nullptr,
                                                  ruler_,
                                                  kScrollGutterWidth,
                                                  0,
                                                  {}},
                                                 this);
    chrome_.headerCanvas = headerRight_;
    headerFallback_ =
        EditorArea::buildSplitChrome(headerMenus_, headerRight_, layerColumnWidth_, this);

    // Column headings start the body. Their right cell continues the header playhead into the
    // lanes, using the same viewport mapping and scrollbar gutter.
    auto* columnHeaderRow = new QWidget(this);
    columnHeaderRow->setObjectName("timelineColumnHeaderRow");
    columnHeaderRow->setFixedHeight(kit::px(kit::Size::Control));
    auto* columnHeaderLayout = new QHBoxLayout(columnHeaderRow);
    columnHeaderLayout->setContentsMargins(0, 0, 0, 0);
    columnHeaderLayout->setSpacing(0);
    columnHeaders_ = new TimelineColumnHeaders(columnHeaderRow);
    loadHideShyLayers();
    connect(columnHeaders_, &TimelineColumnHeaders::hideShyToggled, this,
            &TimelineEditor::setHideShyLayers);
    connect(columnHeaders_, &TimelineColumnHeaders::labelFilterRequested, this,
            [this](const QPoint globalPosition) {
                auto* menu = kit::makeMenu(this);
                menu->setObjectName(QStringLiteral("timelineLabelFilterMenu"));
                menu->setAttribute(Qt::WA_DeleteOnClose);
                layer_labels::populateLabelFilterMenu(
                    *menu, labelFilter_,
                    [this](std::set<int> labels) { setLabelFilter(std::move(labels)); });
                menu->popup(globalPosition);
            });
    scrollBar_ = new QScrollBar(Qt::Vertical, this);
    scrollBar_->setObjectName("timelineVerticalScrollBar");
    scrollBar_->setAccessibleName(tr("Layer stack scroll"));
    auto* rulerGutter = new QWidget(columnHeaderRow);
    rulerGutter->setObjectName("timelineRulerScrollGutter");
    rulerGutter->setFixedWidth(kScrollGutterWidth);
    columnHeaderLayout->addWidget(columnHeaders_);
    // task TL-FIX2: SplitHandle, matching the body divider below so the column headings' own
    // divider continues it in one straight line rather than jogging at the body's taller,
    // draggable handle.
    columnHeaderLayout->addSpacing(kit::px(kit::Size::SplitHandle));
    columnHeaderLayout->addWidget(workArea_, 1);
    columnHeaderLayout->addWidget(rulerGutter);

    // ---- Body: the layer column and the lane region, under ONE scrollbar ------------------------
    auto* body = new QWidget(this);
    body->setObjectName("timelineBody");
    auto* bodyLayout = new QHBoxLayout(body);
    bodyLayout->setContentsMargins(0, 0, 0, 0);
    bodyLayout->setSpacing(0);
    stack_ = new TimelineLayerStack(session_, *scrollBar_, body);
    connect(stack_, &TimelineLayerStack::blankContextMenuRequested, this,
            [this](QPoint globalPosition) { blankLayerContextMenu_->popup(globalPosition); });
    stack_->setMinimumWidth(layerColumnWidth_);
    columnHeaders_->setMinimumWidth(layerColumnWidth_);
    lanes_ = new TimelineLaneRegion(session_, *ruler_, *scrollBar_, body);
    lanes_->setKeyframesVisible(keyframesVisible_);
    lanes_->setSnappingEnabled(snapping_);
    lanes_->setGraphEaseActions(easeActions_);
    setGraphEditorEnabled(graphEditor_);
    const auto toggleExpansion = [this](document::LayerId layer) {
        // A revealed layer's twirl closes it and drops the reveal, as in a layer-based compositor;
        // the next twirl shows every property again.
        const bool open = revealed_.contains(layer) || expandedLayers_.contains(layer) ||
                          std::ranges::any_of(stack_->entries(), [&](const auto& row) {
                              return row.layerId == layer &&
                                     expandedEntries_.contains(row.slotId);
                          });
        revealed_.erase(layer);
        expandedLayers_.erase(layer);
        for (const auto& row : stack_->entries())
            if (row.layerId == layer)
                expandedEntries_.erase(row.slotId);
        if (!open)
            expandedLayers_.insert(layer);
        rebuild();
    };
    // #502: a row's own twirl opens that row (its entry) only: a shared Layer node's other rows and
    // the entries of a plain row's future layer keep their own state.
    const auto toggleEntryExpansion = [this](document::LayerSlotId slot) {
        const auto& rows = stack_->entries();
        const auto found = std::ranges::find_if(rows, [&](const auto& row) {
            return row.rowKind == TimelineLayerEntry::Kind::Layer && row.slotId == slot;
        });
        if (found == rows.end())
            return;
        if (found->expanded) {
            expandedEntries_.erase(slot);
            if (found->layerId.isValid()) {
                expandedLayers_.erase(found->layerId);
                revealed_.erase(found->layerId);
            }
        } else {
            expandedEntries_.insert(slot);
        }
        rebuild();
    };
    connect(stack_, &TimelineLayerStack::expansionRequested, this, toggleExpansion);
    connect(lanes_, &TimelineLaneRegion::expansionRequested, this, toggleExpansion);
    connect(stack_, &TimelineLayerStack::entryExpansionRequested, this, toggleEntryExpansion);
    connect(lanes_, &TimelineLaneRegion::entryExpansionRequested, this, toggleEntryExpansion);
    connect(stack_, &TimelineLayerStack::groupExpansionRequested, this,
            [this](document::LayerId layer, const QString& group) {
                if (!collapsedGroups_.erase({layer, group}))
                    collapsedGroups_.insert({layer, group});
                rebuild();
            });
    connect(stack_, &TimelineLayerStack::parameterExpansionRequested, this,
            [this](document::ParameterId parameter) {
                if (!expandedParameters_.erase(parameter))
                    expandedParameters_.insert(parameter);
                rebuild();
            });
    connect(&session_, &CompositionSession::compositionChanged, this, [this] {
        expandedLayers_.clear();
        expandedEntries_.clear();
        expandedParameters_.clear();
        collapsedGroups_.clear();
        revealed_.clear();
        labelFilter_.clear();
        columnHeaders_->setLabelFilterActive(false);
        loadHideShyLayers();
        rebuild();
    });
    lanes_->setRowHighlight([this](const int row) { return stack_->rowHighlighted(row); });
    connect(stack_, &TimelineLayerStack::rowHighlightsChanged, lanes_,
            &TimelineLaneRegion::syncRowHighlights);
    stack_->addAction(deleteLayerAction_);
    lanes_->addAction(deleteLayerAction_);
    auto* bodyGutter = new QWidget(body);
    bodyGutter->setObjectName("timelineBodyScrollGutter");
    bodyGutter->setFixedWidth(kScrollGutterWidth);
    auto* gutterLayout = new QHBoxLayout(bodyGutter);
    gutterLayout->setContentsMargins(0, 0, 0, 0);
    gutterLayout->setSpacing(0);
    // The scrollbar sits inside a gutter of the scrollbar's own HOVER extent, so the kit
    // stylesheet's hover growth happens INSIDE the reserved width and the lane region's own width
    // -- and therefore the time axis -- never moves under the pointer.
    gutterLayout->addWidget(scrollBar_, 0, Qt::AlignRight);
    // task TL-FIX2: the layer-table/lanes divider becomes a drag handle here, in the one place
    // both halves of the row grid actually meet -- see the class comment on TimelineLayerStack for
    // why this stays two widgets rather than a QSplitter's own children.
    splitHandle_ = new kit::KSplitHandle(Qt::Horizontal, body);
    splitHandle_->setObjectName("timelineLayerColumnSplitHandle");
    splitHandle_->setAccessibleName(tr("Resize the layer table"));
    connect(splitHandle_, &kit::KSplitHandle::dragStarted, this,
            [this] { dragStartColumnWidth_ = layerColumnWidth_; });
    connect(splitHandle_, &kit::KSplitHandle::dragged, this, [this](const int delta) {
        setLayerColumnWidth(dragStartColumnWidth_ + delta, /*persist=*/false);
    });
    connect(splitHandle_, &kit::KSplitHandle::dragFinished, this,
            [this] { setLayerColumnWidth(layerColumnWidth_, /*persist=*/true); });
    connect(splitHandle_, &kit::KSplitHandle::resetRequested, this, [this] {
        const int defaultWidth =
            this->width() > 0
                ? static_cast<int>(std::lround(this->width() * kDefaultLayerColumnShare))
                : layerColumnWidth();
        setLayerColumnWidth(defaultWidth, /*persist=*/true);
    });
    bodyLayout->addWidget(stack_);
    bodyLayout->addWidget(splitHandle_);
    bodyLayout->addWidget(lanes_, 1);
    bodyLayout->addWidget(bodyGutter);

    layout->addWidget(headerFallback_);
    layout->addWidget(columnHeaderRow);
    layout->addWidget(body, 1);
    createFooter();
    layout->addWidget(chrome_.footerCanvas);

    // ONE scrollbar drives both halves of the grid: left/right scroll sync is structural here, not
    // a pair of handlers keeping two scroll areas in step.
    connect(scrollBar_, &QScrollBar::valueChanged, this, [this](const int value) {
        stack_->setScrollOffset(value);
        lanes_->setScrollOffset(value);
    });
    connect(stack_, &TimelineLayerStack::viewportResized, this, &TimelineEditor::updateScrollRange);

    // Session publication order (CompositionSession::rebind): documentRebound() fires first, then
    // liveValueChanged()/snapshotChanged(). Dropping the pooled rows here, before those later
    // signals, means no stale row can dereference the composition the rebind just replaced -- most
    // importantly a blank (composition-less) New/Open. rebuild() then repopulates from the new
    // document.
    connect(&session_, &CompositionSession::documentRebound, stack_,
            &TimelineLayerStack::clearPooledRows);
    connect(&session_, &CompositionSession::snapshotChanged, this, &TimelineEditor::rebuild);
    connect(&session_, &CompositionSession::compositionChanged, this, &TimelineEditor::rebuild);
    connect(&session_, &CompositionSession::selectionChanged, this,
            &TimelineEditor::updateSelection);
    connect(&session_, &CompositionSession::historyChanged, this,
            &TimelineEditor::updateHistoryActions);

    createTimelineKeys();
    rebuild();
    updateHistoryActions();
}

// Hide Shy Layers is per-composition session state (as in a layer-based compositor, where each
// composition has its own switch), persisted with the workspace UI state rather than in the
// project.
QString TimelineEditor::hideShySetting(const document::CompositionId composition) {
    return QStringLiteral("timeline/hide-shy-layers/%1").arg(composition.value());
}

void TimelineEditor::loadHideShyLayers() {
    hideShyLayers_ = QSettings().value(hideShySetting(session_.compositionId()), false).toBool();
    columnHeaders_->setHideShy(hideShyLayers_);
}

void TimelineEditor::setHideShyLayers(const bool hide) {
    hideShyLayers_ = hide;
    columnHeaders_->setHideShy(hide);
    QSettings().setValue(hideShySetting(session_.compositionId()), hide);
    rebuild();
}

void TimelineEditor::setLabelFilter(std::set<int> labels) {
    labelFilter_ = std::move(labels);
    columnHeaders_->setLabelFilterActive(!labelFilter_.empty());
    rebuild();
}

void TimelineEditor::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    if (chrome_.refreshSplit)
        chrome_.refreshSplit();
}

void TimelineEditor::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    // The Timeline's keys also answer when no widget in the window holds focus (see eventFilter).
    if (window() != keyWindow_) {
        if (keyWindow_)
            keyWindow_->removeEventFilter(this);
        keyWindow_ = window();
        keyWindow_->installEventFilter(this);
    }
    refreshHeaderMenus();
    const QSettings settings;
    const int width = settings.contains(QLatin1StringView(kLayerColumnWidthSetting))
                          ? layerColumnWidth_
                          : static_cast<int>(std::lround(this->width() * kDefaultLayerColumnShare));
    // The persisted pixel width is authoritative. A fresh panel gets the ratio-based default only
    // after its real geometry exists, and the shared setter applies the final clamp to both halves.
    setLayerColumnWidth(width, /*persist=*/false);
}

void TimelineEditor::persistLayerColumnWidth() {
    setLayerColumnWidth(layerColumnWidth_, /*persist=*/true);
}

void TimelineEditor::applyDefaultLayerColumnWidth() {
    const int width = this->width() > 0
                          ? static_cast<int>(std::lround(this->width() * kDefaultLayerColumnShare))
                          : layerColumnWidth();
    setLayerColumnWidth(width, /*persist=*/false);
}

// task TL-FIX2. The one place that ever assigns layerColumnWidth_, so every caller -- a live drag
// frame, a completed drag, a reset, and the showEvent() re-clamp -- goes through the same clamp and
// the same set of widgets, and can never leave one of them stale. The maximum (leave
// Size::PanelMinWidth for the lanes) only applies once this widget's own width() is meaningful;
// before the first show it is still 0, and the constructor's own load only floors against
// minLayerColumnWidth() for exactly that reason.
void TimelineEditor::setLayerColumnWidth(const int width, const bool persist) {
    const int minWidth = minLayerColumnWidth();
    const int maxWidth = this->width() > 0
                             ? std::max(minWidth, this->width() - kit::px(kit::Size::PanelMinWidth))
                             : std::numeric_limits<int>::max();
    layerColumnWidth_ = std::clamp(width, minWidth, maxWidth);
    stack_->setFixedWidth(layerColumnWidth_);
    columnHeaders_->setFixedWidth(layerColumnWidth_);
    if (footerLeft_)
        footerLeft_->setFixedWidth(layerColumnWidth_);
    if (chrome_.refreshSplit)
        chrome_.refreshSplit();
    if (auto* fallbackSplit = findChild<QWidget*>("timelineHeaderFallbackSplit"))
        fallbackSplit->setFixedWidth(layerColumnWidth_);
    if (persist) {
        QSettings settings;
        settings.setValue(QLatin1StringView(kLayerColumnWidthSetting), layerColumnWidth_);
    }
}

void TimelineEditor::appendMergeRows(const document::NodeId mergeId, const int nesting,
                                     std::vector<TimelineLayerEntry>& rows,
                                     std::set<document::NodeId>& visiting) {
    const auto* composition = session_.composition();
    const auto* merge = composition ? composition->graph().merge(mergeId) : nullptr;
    if (merge == nullptr || !visiting.insert(mergeId).second)
        return;
    const auto& graph = composition->graph();
    const auto edges = graph.edges();
    for (const auto& slot : merge->entries()) {
        const document::MergeEntryRef ref{mergeId, slot.slotId};
        const auto* imageEdge = document::mergeSlotImageEdge(graph, mergeId, slot.slotId);
        const auto audioEdge = std::ranges::find_if(edges, [&](const auto& candidate) {
            const auto* input = std::get_if<document::LayerStackInputRef>(&candidate.destination);
            return input && input->stackNodeId == mergeId && input->slotId == slot.slotId &&
                   input->role == document::kLayerStackAudioInputRole;
        });
        if (imageEdge == nullptr && audioEdge == edges.end())
            continue;
        const auto* imageSource = imageEdge ? graph.findNode(imageEdge->source.nodeId) : nullptr;
        const auto* audioSource =
            audioEdge != edges.end() ? graph.findNode(audioEdge->source.nodeId) : nullptr;
        const auto* layer = document::mergeEntryLayer(graph, mergeId, slot.slotId);
        const auto layerId = layer ? layer->layerId : document::LayerId{};
        // The master Shy toggle and the label filter take rows out of the list only; the layers
        // still render (layout state, never part of the render).
        if (hideShyLayers_ && layer && layer_fields::shy(session_, layerId))
            continue;
        if (!labelFilter_.empty() &&
            !labelFilter_.contains(layer ? layer_labels::filterKey(layer->labelColor)
                                         : layer_labels::kKindDefault))
            continue;
        // What the entry shows: the content its layer composites (a Layer node's own content, or
        // the slot's source).
        const auto* content = layer ? directSourceNode(session_, layerId) : nullptr;
        if (content == nullptr)
            content = imageSource ? imageSource : audioSource;
        const bool group = imageSource && imageSource->typeId == document::kLayerStackNodeType &&
                           (!layer || layer->entryOwned());
        const auto adjustment = document::mergeEntryAdjustment(graph, mergeId, slot.slotId);
        TimelineLayerEntry row{
            .layerId = layerId,
            .slotId = slot.slotId,
            .name = layer ? layer_fields::displayName(session_, layerId)
                    : imageSource ? node_editor::nodeDisplayName(*composition, *imageSource)
                    : audioSource ? node_editor::nodeDisplayName(*composition, *audioSource)
                                  : QString{},
            .kind = group        ? tr("Group")
                    : adjustment ? tr("Adjustment")
                                 : sourceKind(content),
            .clipColor = group        ? kit::Color::DataComposition
                         : adjustment ? kit::Color::Muted
                                      : sourceClipColorToken(session_, content)};
        row.imageNodeId = imageSource ? imageSource->id : document::NodeId{};
        row.mergeNodeId = mergeId;
        row.nesting = nesting;
        row.groupMerge = group ? imageSource->id : document::NodeId{};
        row.adjustment = adjustment.has_value();
        if (layer && !layer->entryOwned()) {
            if (imageSource && imageSource->id == layer->nodeId)
                row.directLayerNode = layer->nodeId;
            const auto others = document::mergeEntriesOfLayer(graph, layerId);
            if (others.size() > 1) {
                QStringList names;
                for (const auto& other : others) {
                    if (other == ref)
                        continue;
                    const auto* otherMerge = graph.findNode(other.merge);
                    const auto position = [&] {
                        const auto* stack = graph.merge(other.merge);
                        if (stack == nullptr)
                            return 0;
                        const auto items = stack->entries();
                        const auto found = std::ranges::find(items, other.slot,
                                                             &document::LayerStackEntry::slotId);
                        return static_cast<int>(found - items.begin()) + 1;
                    }();
                    names.push_back(
                        tr("%1 entry %2")
                            .arg(otherMerge ? node_editor::nodeDisplayName(*composition, *otherMerge)
                                            : tr("Merge"))
                            .arg(position));
                }
                row.sharedWith = names.join(QStringLiteral(", "));
            }
        }
        if (layer && layer->labelColor) {
            const auto rgb = *layer->labelColor;
            row.labelColor = QColor(rgb[0], rgb[1], rgb[2]);
        }
        // Audio: the entry's audio source (a slot's audio content, or a Layer node's), and its
        // waveform.
        const auto mediaSource = [](const document::NodeRecord* node) {
            return node != nullptr && (node->typeId == document::kAudioSourceNodeType ||
                                       node->typeId == "bloom.video-source");
        };
        const auto* audioNode = mediaSource(audioSource) ? audioSource
                                : mediaSource(content)   ? content
                                                         : nullptr;
        if (audioNode) {
            bool hasAudio = audioNode->typeId == document::kAudioSourceNodeType;
            for (const auto& binding : audioNode->parameters) {
                if (binding.role != "asset")
                    continue;
                const auto value = session_.constantStringValue(binding.parameterId);
                const auto* asset = value ? session_.snapshot().project().findAsset(
                                                document::AssetId::fromRaw(value->toULongLong()))
                                          : nullptr;
                hasAudio = hasAudio ||
                           (asset && (asset->channels > 0 ||
                                      std::ranges::any_of(asset->videoStreams, [](const auto& s) {
                                          return s.kind == 2;
                                      })));
                if (auto* controller = session_.assetController(); controller && value)
                    row.waveform =
                        controller->waveform(document::AssetId::fromRaw(value->toULongLong()));
                break;
            }
            if (hasAudio)
                row.audioNodeId = audioNode->id;
            else
                row.waveform.reset();
        }
        row.expanded = expandedEntries_.contains(slot.slotId) ||
                       (layerId.isValid() && expandedLayers_.contains(layerId)) ||
                       (layerId.isValid() && revealed_.contains(layerId));
        auto block = timelinePropertyEntries(session_, {row}, {}, collapsedGroups_,
                                             expandedParameters_, revealed_);
        if (block.empty())
            continue;
        // A group row's twirl opens its Contents first: the nested Merge's own entries, in place,
        // one level deeper, each just as live (#502).
        if (group && block.front().expanded) {
            auto contents = block.front();
            contents.rowKind = TimelineLayerEntry::Kind::Group;
            contents.depth = 1;
            contents.name = tr("Contents");
            contents.group = timelineGroupKey(contents, QStringLiteral("Contents"));
            contents.parameterId = {};
            contents.role.clear();
            contents.expanded = !collapsedGroups_.contains({contents.layerId, contents.group});
            std::vector<TimelineLayerEntry> nested;
            nested.push_back(contents);
            if (contents.expanded)
                appendMergeRows(row.groupMerge, nesting + 1, nested, visiting);
            block.insert(block.begin() + 1, nested.begin(), nested.end());
        }
        rows.insert(rows.end(), std::make_move_iterator(block.begin()),
                    std::make_move_iterator(block.end()));
    }
    visiting.erase(mergeId);
}

void TimelineEditor::rebuild() {
    std::vector<TimelineLayerEntry> entries;
    if (session_.composition() != nullptr && session_.timelineMerge()) {
        std::set<document::NodeId> visiting;
        appendMergeRows(session_.timelineMerge()->nodeId(), 0, entries, visiting);
    }
    stack_->setEntries(entries);
    lanes_->setEntries(std::move(entries));
    updateScrollRange();
    updateSelection();
}

void TimelineEditor::addAdjustmentLayer() {
    const auto* composition = session_.composition();
    const auto* timeline = session_.timelineMerge();
    if (composition == nullptr || timeline == nullptr)
        return;
    // Into the Merge of the selected row, directly above it; else the top of the timeline Merge.
    auto merge = timeline->nodeId();
    std::optional<document::LayerSlotId> above;
    if (const auto entry = session_.selectedEntry()) {
        merge = entry->merge;
        above = entry->slot;
    }
    commands::Transaction transaction("Add Adjustment Layer", session_.snapshot().revision());
    transaction.emplace<commands::AddAdjustmentLayer>(session_.compositionId(), merge, above);
    const auto result = session_.executeTransaction(std::move(transaction));
    if (const auto slot = result.outputId<document::LayerSlotId>(commands::kEntryLayerSlotOutput))
        session_.selectEntry({merge, *slot});
}

void TimelineEditor::updateScrollRange() {
    const int viewport = stack_->height();
    const int maximum = std::max(0, stack_->contentHeight() - viewport);
    scrollBar_->setRange(0, maximum);
    scrollBar_->setVisible(maximum > 0);
    const int gutterWidth = maximum > 0 ? kScrollGutterWidth : 0;
    for (const auto* name : {"timelineRulerScrollGutter", "timelineBodyScrollGutter"})
        if (auto* gutter = findChild<QWidget*>(QLatin1String(name)))
            gutter->setFixedWidth(gutterWidth);
    if (auto* gutter = headerRight_->findChild<QWidget*>("timelineHeaderScrollGutter"))
        gutter->setFixedWidth(gutterWidth);
    scrollBar_->setPageStep(std::max(1, viewport));
    scrollBar_->setSingleStep(kTimelineRowHeight);
}

void TimelineEditor::updateSelection() {
    // Both halves repaint themselves off selectionChanged (the stack re-binds its rows, the lane
    // region repaints its fills), so the editor's only remaining job is bringing the selected row
    // into view -- exactly what QTreeWidget::scrollToItem() did before this task.
    const int current = stack_->currentRow();
    if (current < 0) {
        return;
    }
    if (std::holds_alternative<KeyframeSelection>(session_.selection().primary))
        return;
    const int top = current * kTimelineRowHeight;
    const int viewport = stack_->height();
    if (top < scrollBar_->value()) {
        scrollBar_->setValue(top);
    } else if (top + kTimelineRowHeight > scrollBar_->value() + viewport) {
        scrollBar_->setValue(top + kTimelineRowHeight - viewport);
    }
}

} // namespace bloom::ui
