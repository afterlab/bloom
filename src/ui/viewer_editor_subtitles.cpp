// #752 Subtitles in the Viewer (docs/ux/interaction-model.md, "Viewer"). The Viewer shows the real
// rendered pixels; these are display-only additions, never in a rendered frame, RAM Preview, an
// export or a headless render:
// - with a Subtitles layer or node selected: the node's safe area as a dashed rectangle, and each
//   shown cue's block outlined with its number (the selected cue in Accent);
// - a click on a block selects that cue (the Properties list and the Timeline follow); dragging the
//   block moves the node's Offset through the session's value edit, one edit on release;
// - a double-click on a block edits that cue's text in place, over the block, as one Edit Cue;
// - a node with Burn in off draws its cues as a guide while Guides > Show Track-Only Subtitles is
//   on (default on).
// Block geometry comes from the session's SubtitlesInspector (laid out off the interface thread);
// the node's frame reaches the composition through its layer's evaluated bounds.

#include "subtitles_editing.hpp"
#include "subtitles_inspector.hpp"

#include <bloom/document/subtitles_nodes.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/grading_controls.hpp>
#include <bloom/ui/kit/text_field.hpp>
#include <bloom/ui/kit/tokens.hpp>
#include <bloom/ui/viewer_editor.hpp>

#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>
#include <variant>

namespace bloom::ui {
namespace {

namespace editing = subtitles_editing;

struct SubtitlesLayer final {
    document::NodeId node;
    document::LayerId layer;
};

// Every layer of the composition that shows a Subtitles node, topmost last (the Merge order).
std::vector<SubtitlesLayer> subtitlesLayers(const CompositionSession& session) {
    std::vector<SubtitlesLayer> layers;
    const auto* composition = session.composition();
    if (composition == nullptr)
        return layers;
    for (const auto& layer : composition->graph().layerOutputs())
        if (const auto node = editing::subtitlesNodeOfLayer(session, layer.layerId))
            layers.push_back({*node, layer.layerId});
    if (const auto* stack = session.timelineMerge()) {
        std::vector<SubtitlesLayer> ordered;
        for (const auto& entry : stack->entries())
            for (const auto& layer : layers)
                if (layer.layer == entry.layerId)
                    ordered.push_back(layer);
        // The Merge lists the bottom layer first.
        if (!ordered.empty())
            return ordered;
    }
    return layers;
}

// The Subtitles node the selection names: a selected Subtitles node, or a selected layer showing
// one.
std::optional<document::NodeId> selectedSubtitles(const CompositionSession& session) {
    if (const auto* node = session.selectedNode();
        node != nullptr && node->typeId == document::kSubtitlesNodeType)
        return node->id;
    if (const auto* layer = std::get_if<document::LayerId>(&session.selection().primary))
        return editing::subtitlesNodeOfLayer(session, *layer);
    return std::nullopt;
}

QPolygonF screenPolygon(const ViewerMapping& mapping, const QTransform& toComposition,
                        const QRectF& rect) {
    QPolygonF polygon;
    for (const auto& corner :
         {rect.topLeft(), rect.topRight(), rect.bottomRight(), rect.bottomLeft()}) {
        const auto composed = toComposition.map(corner);
        polygon << mapping.toScreen({composed.x(), composed.y()});
    }
    return polygon;
}

// The in-place cue editor: the cue's markup in a kit text area over its block. Ctrl+Return,
// Escape and focus loss commit (Escape confirms everywhere); unchanged text writes nothing.
class CueTextEditor final : public kit::KTextArea {
  public:
    CueTextEditor(CompositionSession& session, const document::NodeId node, const std::uint64_t cue,
                  QWidget* parent)
        : kit::KTextArea(parent), session_(session), node_(node), cue_(cue) {
        setObjectName(QStringLiteral("viewerCueTextEditor"));
        setAccessibleName(QObject::tr("Cue text"));
        setToolTip(QObject::tr("WebVTT cue text: <i>, <b>, <u>, <c.yellow>; Ctrl+Return commits"));
        const auto source = editing::cueSource(session_, node_);
        const auto* found = source.list == nullptr ? nullptr : source.list->findCue(cue_);
        original_ = found == nullptr ? QString{} : QString::fromStdString(found->text);
        setPlainText(original_);
        selectAll();
    }
    void commit() {
        if (done_)
            return;
        done_ = true;
        const auto text = toPlainText();
        if (text != original_) {
            commands::CueEdit edit;
            edit.text = text.toStdString();
            (void)editing::editCue(session_, node_, cue_, std::move(edit),
                                   QObject::tr("Edit Cue Text"));
        }
        hide();
        deleteLater();
    }

  protected:
    void keyPressEvent(QKeyEvent* event) override {
        const bool enter = event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter;
        if (event->key() == Qt::Key_Escape || (enter && event->modifiers() & Qt::ControlModifier)) {
            commit();
            event->accept();
            return;
        }
        kit::KTextArea::keyPressEvent(event);
    }
    void focusOutEvent(QFocusEvent* event) override {
        kit::KTextArea::focusOutEvent(event);
        commit();
    }

  private:
    CompositionSession& session_;
    document::NodeId node_;
    std::uint64_t cue_ = 0;
    QString original_;
    bool done_ = false;
};

} // namespace

std::vector<ViewerEditor::SubtitlesScreenBlock>
ViewerEditor::subtitlesScreenBlocks(const ViewerMapping& mapping, const bool selectedOnly) const {
    std::vector<SubtitlesScreenBlock> blocks;
    const auto selected = selectedSubtitles(session_);
    auto& inspector = session_.subtitlesInspector();
    for (const auto& layer : subtitlesLayers(session_)) {
        if (selectedOnly && (!selected || *selected != layer.node))
            continue;
        const auto* inspected = inspector.result(layer.node);
        const auto toComposition = focusFrameToComposition(layer.node);
        if (inspected == nullptr || !toComposition)
            continue;
        for (const auto& block : inspected->blocks)
            blocks.push_back({layer.node, layer.layer, block.cue, block.index,
                              screenPolygon(mapping, *toComposition, block.rect), block.text});
    }
    return blocks;
}

std::vector<std::pair<std::uint64_t, QPolygonF>> ViewerEditor::subtitlesBlocksForTest() const {
    std::vector<std::pair<std::uint64_t, QPolygonF>> blocks;
    if (const auto mapping = motionHandleMapping())
        for (const auto& block : subtitlesScreenBlocks(*mapping, false))
            blocks.emplace_back(block.cue, block.polygon);
    return blocks;
}

void ViewerEditor::paintSubtitles(QPainter& painter,
                                  const std::optional<ViewerMapping>& painted) const {
    if (isReferenceOnly() || session_.composition() == nullptr)
        return;
    const auto mapping = painted ? painted : motionHandleMapping();
    if (!mapping)
        return;
    auto& inspector = session_.subtitlesInspector();
    const auto selectedNode = selectedSubtitles(session_);
    const auto selectedCue = session_.selectedCue();
    painter.save();
    painter.setClipRect(contentRect());
    painter.setRenderHint(QPainter::Antialiasing, true);
    const auto accent = kit::color(kit::Color::Accent);
    for (const auto& layer : subtitlesLayers(session_)) {
        const auto* inspected = inspector.result(layer.node);
        const auto toComposition = focusFrameToComposition(layer.node);
        if (inspected == nullptr || !toComposition)
            continue;
        const bool chosen = selectedNode && *selectedNode == layer.node;
        // The track-only guide: the cues that are not burned in, as plain text on a dark plate.
        if (!inspected->burnIn && overlayOptions_.trackOnlySubtitles)
            for (const auto& block : inspected->blocks) {
                const auto polygon = screenPolygon(*mapping, *toComposition, block.rect);
                const auto bounds = polygon.boundingRect();
                auto plate = kit::color(kit::Color::CanvasBlack);
                plate.setAlphaF(0.6f);
                painter.setPen(Qt::NoPen);
                painter.setBrush(plate);
                painter.drawPolygon(polygon);
                // The interface font, scaled so the lines fill the block as the burned-in cue would.
                const auto lines = std::max<qsizetype>(1, block.text.count(QLatin1Char('\n')) + 1);
                painter.setFont(kit::font(kit::TypeRole::Ui));
                const auto metrics = painter.fontMetrics();
                const double fill = bounds.height() / static_cast<double>(lines) /
                                    std::max(1, metrics.height()) * 0.8;
                if (fill > 0.0) {
                    painter.save();
                    painter.translate(bounds.center());
                    painter.scale(fill, fill);
                    const QRectF local(-bounds.width() / (2.0 * fill), -bounds.height() / (2.0 * fill),
                                       bounds.width() / fill, bounds.height() / fill);
                    painter.setPen(kit::color(kit::Color::CanvasWhite));
                    painter.drawText(local, Qt::AlignCenter, block.text);
                    painter.restore();
                }
                QPen dashed(kit::color(kit::Color::Muted), kit::kHairlineWidth, Qt::DashLine);
                painter.setPen(dashed);
                painter.setBrush(Qt::NoBrush);
                painter.drawPolygon(polygon);
            }
        if (!chosen)
            continue;
        // The node's safe area and its shown cues.
        QPen safe(accent, kit::kHairlineWidth, Qt::DashLine);
        painter.setPen(safe);
        painter.setBrush(Qt::NoBrush);
        if (!inspected->safeArea.isEmpty())
            painter.drawPolygon(screenPolygon(*mapping, *toComposition, inspected->safeArea));
        auto label = kit::font(kit::TypeRole::UiSmallStrong);
        painter.setFont(label);
        const auto metrics = painter.fontMetrics();
        for (const auto& block : inspected->blocks) {
            const auto polygon = screenPolygon(*mapping, *toComposition, block.rect);
            const bool picked =
                selectedCue && selectedCue->node == layer.node && selectedCue->cue == block.cue;
            painter.setPen(QPen(accent, picked ? kit::px(kit::Size::SelectionEdge) : kit::kHairlineWidth));
            painter.setBrush(Qt::NoBrush);
            painter.drawPolygon(polygon);
            const auto number = QString::number(block.index);
            const QRectF tag(polygon.boundingRect().topLeft() -
                                 QPointF(0, metrics.height() + kit::px(kit::Spacing::XXS)),
                             QSizeF(metrics.horizontalAdvance(number) + kit::px(kit::Spacing::S),
                                    metrics.height() + kit::px(kit::Spacing::XXS)));
            painter.setPen(Qt::NoPen);
            painter.setBrush(accent);
            painter.drawRoundedRect(tag, kit::px(kit::Spacing::XXS), kit::px(kit::Spacing::XXS));
            painter.setPen(kit::color(kit::Color::OnAccent));
            painter.drawText(tag, Qt::AlignCenter, number);
        }
    }
    painter.restore();
}

bool ViewerEditor::subtitlesPress(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || tool() != ViewerTool::Select)
        return false;
    const auto mapping = motionHandleMapping();
    if (!mapping)
        return false;
    const auto blocks = subtitlesScreenBlocks(*mapping, false);
    const auto selected = selectedSubtitles(session_);
    // The selected node's blocks first, then the topmost layer's.
    const SubtitlesScreenBlock* hit = nullptr;
    for (const auto& block : blocks)
        if (selected && block.node == *selected &&
            block.polygon.containsPoint(event->position(), Qt::OddEvenFill))
            hit = &block;
    if (hit == nullptr)
        for (const auto& block : blocks)
            if (block.polygon.containsPoint(event->position(), Qt::OddEvenFill))
                hit = &block;
    if (hit == nullptr)
        return false;
    const auto node = hit->node;
    const auto cue = hit->cue;
    if (!selected || *selected != node)
        session_.selectLayer(hit->layer);
    session_.selectCue(node, cue);
    setFocus(Qt::MouseFocusReason);
    event->accept();
    const auto offset = nodeRoleParameter(session_, node, "offset");
    const auto toComposition = focusFrameToComposition(node);
    const auto value = offset ? session_.liveValue(*offset) : std::nullopt;
    const auto* base = value ? std::get_if<document::Vec2d>(&*value) : nullptr;
    if (!offset || !toComposition || !toComposition->isInvertible() || base == nullptr ||
        !scalarEditable(session_, *offset))
        return true;
    const auto composed = mapping->toComposition(event->position());
    subtitlesDrag_ = SubtitlesDrag{node, *offset, *base,
                                   toComposition->inverted().map(QPointF(composed.x, composed.y)),
                                   *toComposition, false};
    update();
    return true;
}

bool ViewerEditor::subtitlesMove(QMouseEvent* event) {
    if (!subtitlesDrag_)
        return false;
    event->accept();
    const auto mapping = motionHandleMapping();
    if (!mapping)
        return true;
    auto& drag = *subtitlesDrag_;
    const auto composed = mapping->toComposition(event->position());
    const auto local = drag.toComposition.inverted().map(QPointF(composed.x, composed.y));
    const auto delta = local - drag.pressLocal;
    if (!drag.moved) {
        // A click that stays put selects the cue and writes nothing.
        if (std::hypot(delta.x(), delta.y()) < 1.0)
            return true;
        if (!session_.beginValueEdit(drag.offset)) {
            subtitlesDrag_.reset();
            return true;
        }
        drag.moved = true;
    }
    // Whole pixels: an offset is a clean number.
    (void)session_.updateValueEdit(document::Vec2d{std::round(drag.base.x + delta.x()),
                                                   std::round(drag.base.y + delta.y())});
    update();
    return true;
}

bool ViewerEditor::subtitlesRelease(QMouseEvent* event) {
    if (!subtitlesDrag_ || event->button() != Qt::LeftButton)
        return false;
    subtitlesMove(event);
    const auto moved = subtitlesDrag_ && subtitlesDrag_->moved;
    subtitlesDrag_.reset();
    if (moved && session_.valueEditActive())
        static_cast<void>(session_.commitValueEdit());
    event->accept();
    update();
    return true;
}

void ViewerEditor::cancelSubtitlesDrag() {
    if (!subtitlesDrag_)
        return;
    if (subtitlesDrag_->moved)
        session_.cancelValueEdit();
    subtitlesDrag_.reset();
    update();
}

bool ViewerEditor::subtitlesDoubleClick(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || tool() != ViewerTool::Select)
        return false;
    const auto mapping = motionHandleMapping();
    if (!mapping)
        return false;
    const auto blocks = subtitlesScreenBlocks(*mapping, false);
    const auto found = std::find_if(blocks.rbegin(), blocks.rend(), [event](const auto& block) {
        return block.polygon.containsPoint(event->position(), Qt::OddEvenFill);
    });
    if (found == blocks.rend())
        return false;
    cancelSubtitlesDrag();
    event->accept();
    session_.selectCue(found->node, found->cue);
    const auto source = editing::cueSource(session_, found->node);
    if (source.kind != editing::CueSourceKind::Typed) {
        emit session_.commandRejected(source.refusal);
        return true;
    }
    if (cueEditor_)
        static_cast<CueTextEditor*>(cueEditor_.data())->commit();
    auto* editor = new CueTextEditor(session_, found->node, found->cue, this);
    const auto bounds = found->polygon.boundingRect().toAlignedRect();
    const int lines = kit::px(kit::Size::ListRow) * 3;
    editor->setGeometry(QRect(bounds.left(), bounds.top(),
                              std::max(bounds.width(), kit::px(kit::Size::NodeCardWidth)),
                              std::max(bounds.height(), lines))
                            .intersected(contentRect().toAlignedRect()));
    editor->show();
    editor->setFocus(Qt::MouseFocusReason);
    cueEditor_ = editor;
    return true;
}

} // namespace bloom::ui
