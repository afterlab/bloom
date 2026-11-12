// #703: Add Node's placement on the canvas. Choosing a node from an Add surface does not create it
// at once: a translucent card follows the pointer until a left click places it, into the link under
// the pointer when the node can take it, and Escape or a right-click cancels. The insertion rule is
// the commands module's (linkInsertionSockets), shared with bloom.node.insert-on-link, so a
// highlighted link is exactly the link the click inserts into.
#include "node_editor_interactions.hpp"

#include <bloom/commands/node_editor_operations.hpp>
#include <bloom/commands/node_link_insertion.hpp>
#include <bloom/document/node_groups.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/painting.hpp>

namespace bloom::ui {
using namespace node_editor;
namespace {

// The translucent card that follows the pointer: the node's title band, its category, its sockets
// in their own colors and its parameters' names, at the card width a new card opens at. It is a
// picture of what the click adds, never a document item, so it takes no input.
class NodePlacementPreview final : public QGraphicsItem {
  public:
    explicit NodePlacementPreview(const std::string_view type) {
        setAcceptedMouseButtons(Qt::NoButton);
        setAcceptHoverEvents(false);
        setZValue(20);
        setOpacity(0.6);
        setData(kNodeItemKindRole, QStringLiteral("placement-preview"));
        title_ = nodeTypeDisplayName(type);
        if (const auto group = document::nodeGroupOf(type))
            category_ = nodeGroupTitle(*group);
        const document::NodeDefinition* definition = nullptr;
        for (const auto& candidate : document::builtInNodeDefinitions().definitions())
            if (candidate.key.typeId == type &&
                (definition == nullptr ||
                 candidate.key.schemaVersion > definition->key.schemaVersion))
                definition = &candidate;
        if (definition == nullptr)
            return;
        const auto parameter = [&](const std::string_view name) {
            return std::ranges::any_of(definition->parameters,
                                       [&](const auto& item) { return item.role == name; });
        };
        if (definition->layerSlotInput)
            rows_.push_back({displayTypeName(definition->layerSlotInput->role),
                             definition->layerSlotInput->valueKind, Row::Input});
        for (const auto& port : definition->inputs)
            if (!parameter(port.name))
                rows_.push_back({displayTypeName(port.name), port.valueKind, Row::Input});
        for (const auto& port : definition->outputs)
            rows_.push_back({displayTypeName(port.name), port.valueKind, Row::Output});
        for (const auto& item : definition->parameters) {
            const auto port = std::ranges::find(definition->inputs, item.role,
                                                &document::InputPortDefinition::name);
            rows_.push_back({displayTypeName(item.role),
                             port == definition->inputs.end() ? std::nullopt
                                                              : std::optional(port->valueKind),
                             Row::Parameter});
        }
    }

    [[nodiscard]] QRectF boundingRect() const override {
        // The sockets sit on the card's edges, half outside it.
        return cardRect().adjusted(-kSocketDiameter, 0.0, kSocketDiameter, 0.0);
    }

    void setTargeted(const bool targeted) {
        if (targeted_ == targeted)
            return;
        targeted_ = targeted;
        update();
    }

    void paint(QPainter* painter, const QStyleOptionGraphicsItem*, QWidget*) override {
        const QRectF card = cardRect();
        painter->setRenderHint(QPainter::Antialiasing, true);
        kit::fillRoundedSurface(*painter, card, kit::color(kit::Color::SurfaceRaised),
                                kit::color(kit::Color::Border), kCardRadius);
        const auto radius = static_cast<qreal>(
            kit::radiusPx(kCardRadius, static_cast<int>(std::min(card.width(), kCardHeaderHeight))));
        QPainterPath header;
        header.setFillRule(Qt::WindingFill);
        header.addRoundedRect(QRectF(0.0, 0.0, card.width(), kCardHeaderHeight), radius, radius);
        header.addRect(QRectF(0.0, kCardHeaderHeight - radius, card.width(), radius));
        painter->setPen(Qt::NoPen);
        painter->fillPath(header, kit::color(kit::Color::Surface));
        kit::applyHairlinePen(*painter, kit::color(kit::Color::Border));
        painter->drawLine(QPointF(0.0, kCardHeaderHeight), QPointF(card.width(), kCardHeaderHeight));

        const qreal inner = card.width() - kCardPadding - kCardPadding;
        const QRectF band(kCardPadding, 0.0, inner, kCardHeaderHeight);
        painter->setFont(kit::font(kit::TypeRole::UiSmall));
        painter->setPen(kit::color(kit::Color::Muted));
        painter->drawText(band, Qt::AlignRight | Qt::AlignVCenter, category_);
        painter->setFont(kit::font(kit::TypeRole::Ui));
        painter->setPen(kit::color(kit::Color::Foreground));
        painter->drawText(band, Qt::AlignLeft | Qt::AlignVCenter,
                          QFontMetricsF(painter->font())
                              .elidedText(title_, Qt::ElideRight, band.width() * 0.6));

        painter->setFont(kit::font(kit::TypeRole::UiSmall));
        for (std::size_t index = 0; index < rows_.size(); ++index) {
            const auto& row = rows_[index];
            const qreal y = kCardHeaderHeight + (static_cast<qreal>(index) + 0.5) * kSocketRowHeight;
            const qreal socketRadius = kSocketDiameter / 2.0;
            const qreal rowTop = y - kSocketRowHeight / 2.0;
            const QRectF text(kCardPadding + socketRadius, rowTop, inner - kSocketDiameter,
                              kSocketRowHeight);
            painter->setPen(kit::color(kit::Color::Muted));
            painter->drawText(text,
                              static_cast<int>(Qt::AlignVCenter | (row.side == Row::Output
                                                                       ? Qt::AlignRight
                                                                       : Qt::AlignLeft)),
                              row.name);
            if (!row.kind)
                continue;
            painter->setPen(Qt::NoPen);
            painter->setBrush(kit::color(socketColorToken(*row.kind)));
            const qreal x = row.side == Row::Output ? card.width() : 0.0;
            painter->drawEllipse(QPointF(x, y), kSocketDiameter / 2.0, kSocketDiameter / 2.0);
        }
        if (targeted_) {
            const qreal inset = kSelectionEdgeWidth / 2.0;
            painter->setBrush(Qt::NoBrush);
            painter->setPen(QPen(kit::color(kit::Color::Accent), kSelectionEdgeWidth));
            painter->drawRoundedRect(card.adjusted(inset, inset, -inset, -inset), radius, radius);
        }
    }

  private:
    struct Row final {
        enum Side { Input, Output, Parameter };
        QString name;
        std::optional<document::SocketValueKind> kind;
        Side side = Input;
    };

    [[nodiscard]] QRectF cardRect() const {
        return {0.0, 0.0, kCardMinimumWidth,
                kCardHeaderHeight + static_cast<qreal>(rows_.size()) * kSocketRowHeight +
                    kCardPadding};
    }

    QString title_;
    QString category_;
    std::vector<Row> rows_;
    bool targeted_ = false;
};

// A new card lands with its title band centered under the pointer (task N1, item 3).
[[nodiscard]] QPointF cardOrigin(const QPointF cursor) {
    return {cursor.x() - kCardMinimumWidth / 2.0, cursor.y() - kCardHeaderHeight / 2.0};
}

} // namespace

void NodeGraphicsScene::startPlacement(std::string type, const QPointF scenePosition) {
    if (!session_ || !session_->composition() || !submit_)
        return;
    cancelGesture();
    auto& gesture = *interaction_;
    gesture.mode = NodeInteraction::Mode::Place;
    gesture.revision = session_->snapshot().revision();
    gesture.placeType = std::move(type);
    gesture.placement = new NodePlacementPreview(gesture.placeType);
    addItem(gesture.placement);
    updatePlacement(scenePosition);
}

bool NodeGraphicsScene::placementActive() const {
    return interaction_->mode == NodeInteraction::Mode::Place;
}

QGraphicsItem* NodeGraphicsScene::placementPreviewForTest() const {
    return interaction_->placement;
}

std::optional<document::InputPortRef> NodeGraphicsScene::placementTargetForTest() const {
    if (interaction_->mode != NodeInteraction::Mode::Place || !interaction_->insertEdge)
        return std::nullopt;
    return interaction_->insertEdge->edge.destination;
}

bool NodeGraphicsScene::takeSuppressedContextMenu() noexcept {
    return std::exchange(suppressContextMenu_, false);
}

void NodeGraphicsScene::releasePlacementTarget() {
    auto& gesture = *interaction_;
    if (gesture.insertEdge)
        gesture.insertEdge->emphasize(false);
    gesture.insertEdge = nullptr;
    if (gesture.placement != nullptr)
        static_cast<NodePlacementPreview*>(gesture.placement)->setTargeted(false);
}

void NodeGraphicsScene::updatePlacement(const QPointF cursor) {
    auto& gesture = *interaction_;
    if (gesture.mode != NodeInteraction::Mode::Place || gesture.placement == nullptr)
        return;
    // The pointer, kept so a projection can re-target the link under it.
    gesture.origin = cursor;
    gesture.placement->setPos(cardOrigin(cursor));
    releasePlacementTarget();
    // The link under the pointer, when the node can go into it: the same rule the click applies,
    // so the highlight never promises an insertion the command would refuse.
    if (const auto* composition = session_ ? session_->composition() : nullptr)
        for (auto* item : items(cursor)) {
            auto* edge = dynamic_cast<NodeEdgeItem*>(item);
            if (edge == nullptr || edge->structural || !edge->isVisible())
                continue;
            if (!commands::linkInsertionSockets(*composition, gesture.placeType,
                                                edge->edge.destination))
                continue;
            gesture.insertEdge = edge;
            edge->emphasize(true);
            break;
        }
    static_cast<NodePlacementPreview*>(gesture.placement)->setTargeted(gesture.insertEdge != nullptr);
}

void NodeGraphicsScene::commitPlacement() {
    auto& gesture = *interaction_;
    if (gesture.mode != NodeInteraction::Mode::Place || gesture.placement == nullptr || !session_)
        return;
    const QPointF origin = gesture.placement->pos();
    const document::Vec2d position{origin.x(), origin.y()};
    const auto type = gesture.placeType;
    const std::optional<document::InputPortRef> target =
        gesture.insertEdge ? std::optional(gesture.insertEdge->edge.destination) : std::nullopt;
    document::NodeCardSizes sizes;
    for (const auto* card : nodeItems()) {
        const auto rect = card->cardRect();
        sizes.emplace(card->id(), document::NodeCardSize{rect.width(), rect.height()});
    }
    cancelGesture();
    commands::Transaction transaction(target ? "Insert Node" : "Add Node",
                                      session_->snapshot().revision());
    if (target)
        transaction.emplace<commands::InsertNodeOnLink>(session_->compositionId(), type, *target,
                                                        position, std::move(sizes));
    else
        transaction.emplace<commands::AddEditorNode>(session_->compositionId(), type, position,
                                                     std::nullopt, std::nullopt, std::move(sizes));
    const auto result = submit(std::move(transaction));
    if (const auto id = result.outputId<document::NodeId>("editorNode"); result.succeeded() && id)
        session_->selectNode(*id);
}

} // namespace bloom::ui
