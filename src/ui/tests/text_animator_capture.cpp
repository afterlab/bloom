// Review captures of the Text Animator (#758): the Properties of an animator on a Text Source, its
// node card and the timeline, at its defaults (Selector 1 on, Change fill color, Change outline
// color and Cursor off) and with Selector 2, Change fill color and Cursor turned on. Hard checks:
// the rows run in the node's table order (four selectors, the properties, the reveal) on
// Properties, the card and the timeline; the rows nothing reads are hidden and come back when the
// switch that reads them is on; every number shows its unit (percent, pixels, degrees, per second,
// never a raw factor); every color's swatch row on the card heads its R/G/B/A rows; no label is cut.
// A tool, not a test: it writes 758-<surface>-dpr<d>.png to the directory given as the first
// argument and exits non-zero when a check fails. The device pixel ratio follows QT_SCALE_FACTOR
// (run it at 1, 1.5 and 2).
#include "isolated_qt_settings.hpp"

#include "node_editor_items.hpp"

#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/node_definition_registry.hpp>
#include <bloom/document/parameter_presentation.hpp>
#include <bloom/document/text_animator_nodes.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/color_chip.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/kit/value_field.hpp>
#include <bloom/ui/node_editor.hpp>
#include <bloom/ui/properties_editor.hpp>

#include "timeline_property_rows.hpp"

#include <QApplication>
#include <QDir>
#include <QEvent>
#include <QGraphicsProxyWidget>
#include <QGraphicsView>
#include <QImage>
#include <QLabel>
#include <QPainter>
#include <QSettings>
#include <QStandardPaths>

#include <algorithm>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace bloom;

void settle() {
    for (int pass = 0; pass < 3; ++pass) {
        QCoreApplication::sendPostedEvents();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCoreApplication::processEvents();
    }
}

int truncatedLabels(const QWidget& root) {
    int cut = 0;
    for (const auto* label : root.findChildren<QLabel*>()) {
        if (!label->isVisible() || label->text().isEmpty() || label->wordWrap())
            continue;
        if (label->fontMetrics().horizontalAdvance(label->text()) >
            label->contentsRect().width() + 1) {
            ++cut;
            std::cout << "cut label: " << label->text().toStdString() << '\n';
        }
    }
    return cut;
}

QString roleName(const std::string_view role) {
    return QString::fromLatin1(role.data(), static_cast<qsizetype>(role.size()));
}

// The animator's roles in table order.
std::vector<QString> tableOrder() {
    std::vector<QString> order;
    for (const auto& spec : document::textAnimatorParameters())
        order.push_back(roleName(spec.role));
    return order;
}

// The roles nothing reads at the defaults: Selectors 2 to 4 but their switches, Seed while
// Randomize order is off, the colors while their switches are off, and the cursor's rows.
std::set<QString> hiddenAtDefaults() {
    std::set<QString> hidden{"fillColor", "outlineColor", "cursorCharacter", "cursorBlink"};
    for (const auto& spec : document::textAnimatorParameters()) {
        const QString role = roleName(spec.role);
        const auto selector = document::textAnimatorSelectorOf(spec.role);
        if (role.endsWith(QStringLiteral("Seed")))
            hidden.insert(role);
        if (selector >= 2 && role != QStringLiteral("selector%1").arg(selector))
            hidden.insert(role);
    }
    return hidden;
}

// The animator rows among the given roles, consecutive repeats folded (a color's channels).
std::vector<QString> animatorRoles(const std::vector<QString>& roles) {
    const auto order = tableOrder();
    std::vector<QString> out;
    for (const auto& role : roles)
        if (std::ranges::find(order, role) != order.end() && (out.empty() || out.back() != role))
            out.push_back(role);
    return out;
}

std::string joined(const std::vector<QString>& roles) {
    QStringList list;
    for (const auto& role : roles)
        list.push_back(role);
    return list.join(QStringLiteral(", ")).toStdString();
}

// The surface shows exactly the table's rows not hidden, in table order. A subset surface may
// leave rows out (the timeline lists only the keyable rows) but never shows a hidden one.
int checkOrder(const char* surface, const std::vector<QString>& roles,
               const std::set<QString>& hidden, const bool subset = false) {
    const auto shown = animatorRoles(roles);
    std::vector<QString> expected;
    for (const auto& role : tableOrder())
        if (!hidden.contains(role) &&
            (!subset || std::ranges::find(shown, role) != shown.end()))
            expected.push_back(role);
    for (const auto& role : shown)
        if (hidden.contains(role)) {
            std::cout << surface << " shows a row nothing reads: " << role.toStdString() << '\n';
            return 1;
        }
    if (shown == expected && !shown.empty())
        return 0;
    std::cout << surface << " rows: " << joined(shown) << "\n  expected: " << joined(expected)
              << '\n';
    return 1;
}

// The visible rows of the Text Animator's section (between its header and the Text Source's,
// which has rows of the same names), top to bottom.
std::vector<QString> propertiesRoles(const QWidget& root) {
    int top = -1;
    int bottom = 1 << 30;
    for (const auto* label : root.findChildren<QLabel*>()) {
        if (!label->isVisible())
            continue;
        const int y = label->mapTo(&root, QPoint()).y();
        if (label->text() == QStringLiteral("Text Animator"))
            top = y;
        else if (label->text() == QStringLiteral("Text Source"))
            bottom = y;
    }
    std::vector<std::pair<int, QString>> rows;
    for (auto* row : root.findChildren<QWidget*>(QStringLiteral("propertiesRegistryRow")))
        if (const int y = row->mapTo(&root, QPoint()).y();
            row->isVisible() && row->property("role").isValid() && y > top && y < bottom)
            rows.emplace_back(y, row->property("role").toString());
    std::ranges::sort(rows);
    std::vector<QString> roles;
    for (const auto& row : rows)
        roles.push_back(row.second);
    return roles;
}

std::optional<QString> propertiesUnit(const QWidget& root, const QString& role) {
    for (auto* row : root.findChildren<QWidget*>(QStringLiteral("propertiesRegistryRow")))
        if (row->isVisible() && row->property("role").toString() == role)
            if (const auto* field = row->findChild<ui::kit::KValueField*>())
                return field->unit();
    return std::nullopt;
}

struct CardRow final {
    qreal y = 0.0;
    QString role;
    bool swatch = false;
    bool component = false;
    QWidget* row = nullptr;
};

std::vector<CardRow> cardRows(const ui::node_editor::NodeItem& card) {
    std::vector<CardRow> rows;
    for (auto* child : card.childItems()) {
        auto* proxy = qgraphicsitem_cast<QGraphicsProxyWidget*>(child);
        if (proxy == nullptr || !proxy->isVisible() || proxy->widget() == nullptr ||
            proxy->widget()->objectName() != QStringLiteral("nodePropertyRow"))
            continue;
        auto* row = proxy->widget();
        rows.push_back({proxy->pos().y(), row->property("nodeParameterRole").toString(),
                        row->findChild<ui::kit::KColorChip*>() != nullptr,
                        row->findChild<QWidget*>(QStringLiteral("nodeColorComponentEditor")) !=
                            nullptr,
                        row});
    }
    std::ranges::sort(rows, {}, &CardRow::y);
    return rows;
}

std::vector<QString> cardRoles(const ui::node_editor::NodeItem& card) {
    std::vector<QString> roles;
    for (const auto& row : cardRows(card))
        roles.push_back(row.role);
    return roles;
}

std::optional<QString> cardUnit(const ui::node_editor::NodeItem& card, const QString& role) {
    for (const auto& row : cardRows(card))
        if (row.role == role)
            if (const auto* field = row.row->findChild<ui::kit::KValueField*>())
                return field->unit();
    return std::nullopt;
}

int checkColorRows(const ui::node_editor::NodeItem& card, const bool expectSwatch) {
    const auto rows = cardRows(card);
    int failures = 0;
    std::size_t swatches = 0;
    for (std::size_t index = 0; index < rows.size(); ++index) {
        if (rows[index].component) {
            const bool headed = index > 0 && rows[index - 1].role == rows[index].role &&
                                (rows[index - 1].swatch || rows[index - 1].component);
            if (!headed) {
                ++failures;
                std::cout << "card: a " << rows[index].role.toStdString()
                          << " channel row is not under its swatch row\n";
            }
            continue;
        }
        if (!rows[index].swatch)
            continue;
        ++swatches;
        int channels = 0;
        for (std::size_t next = index + 1;
             next < rows.size() && rows[next].component && rows[next].role == rows[index].role;
             ++next)
            ++channels;
        if (channels != 4) {
            ++failures;
            std::cout << "card: the " << rows[index].role.toStdString() << " swatch row heads "
                      << channels << " channel rows, not 4\n";
        }
    }
    if (expectSwatch && swatches == 0) {
        ++failures;
        std::cout << "card: no color swatch rows\n";
    }
    return failures;
}

ui::node_editor::NodeItem* findCard(ui::NodeGraphEditor& nodes, const document::NodeId id) {
    auto* view = nodes.findChild<QGraphicsView*>();
    if (view == nullptr)
        return nullptr;
    for (auto* item : view->scene()->items())
        if (auto* candidate = dynamic_cast<ui::node_editor::NodeItem*>(item);
            candidate && candidate->id() == id)
            return candidate;
    return nullptr;
}

QImage cardImage(ui::NodeGraphEditor& nodes, const ui::node_editor::NodeItem& card) {
    auto* view = nodes.findChild<QGraphicsView*>();
    const auto source = card.sceneBoundingRect().adjusted(-8, -8, 8, 8);
    const qreal ratio = nodes.devicePixelRatioF();
    QImage image((source.size() * ratio).toSize(), QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(ratio);
    image.fill(nodes.palette().window().color());
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    view->scene()->render(&painter, QRectF(QPointF(), source.size()), source);
    painter.end();
    return image;
}

std::vector<QString> timelineRoles(const ui::CompositionSession& session,
                                   const document::LayerId layer, const document::NodeId node) {
    ui::TimelineLayerEntry entry;
    entry.layerId = layer;
    // The animator's own rows: the layer's and the Text Source's share role names.
    std::set<std::uint64_t> owned;
    if (const auto* record = session.composition()->graph().findNode(node))
        for (const auto& binding : record->parameters)
            owned.insert(binding.parameterId.value());
    std::vector<QString> roles;
    for (const auto& row : ui::timelinePropertyEntries(session, {entry}, {layer}))
        if (row.rowKind == ui::TimelineLayerEntry::Kind::Parameter &&
            owned.contains(row.parameterId.value()))
            roles.push_back(QString::fromStdString(row.role));
    return roles;
}

std::optional<document::ParameterId> parameterOf(const ui::CompositionSession& session,
                                                 const document::NodeId nodeId,
                                                 const std::string_view role) {
    const auto* node = session.composition()->graph().findNode(nodeId);
    if (node != nullptr)
        for (const auto& binding : node->parameters)
            if (binding.role == role)
                return binding.parameterId;
    return std::nullopt;
}

// The unit each surface must show for a number row: the presentation's unit.
std::map<QString, QString> expectedUnits() {
    std::map<QString, QString> units;
    const auto* definition = document::builtInNodeDefinitions().find(
        document::kTextAnimatorNodeType, document::kTextAnimatorNodeSchemaVersion);
    if (definition != nullptr)
        for (const auto& parameter : definition->parameters) {
            const auto presentation = document::parameterPresentation(parameter);
            if (presentation.kind == document::ParameterPresentationKind::Number)
                units[QString::fromStdString(parameter.role)] =
                    QString::fromStdString(presentation.unit);
        }
    return units;
}
} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    bloom::test_support::isolateQtSettings();
    app.setOrganizationName("BloomTextAnimatorCapture");
    app.setApplicationName("TextAnimatorCapture");
    QStandardPaths::setTestModeEnabled(true);
    QSettings().clear();
    ui::kit::installAfterlabTheme(app);
    const QString directory = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QStringLiteral(".");
    QDir().mkpath(directory);
    const QString dpr =
        QStringLiteral("dpr") + qEnvironmentVariable("QT_SCALE_FACTOR", QStringLiteral("1"));

    auto seed =
        document::makeNewProject("Text animator", "Main", core::RationalTime::fromInteger(24));
    document::Document doc(std::move(seed.project));
    commands::CommandStack stack(doc);
    ui::CompositionSession session(doc, stack, seed.initialCompositionId);
    if (!session.addTextLayer(QStringLiteral("Caption"), QStringLiteral("Typed on, word by word")))
        return 1;
    const auto layer = session.selection().contextualLayer;
    std::optional<document::NodeId> textNode;
    std::optional<document::InputPortRef> fed;
    for (const auto& node : session.composition()->graph().nodes())
        if (node.typeId == document::kTextSourceNodeType)
            textNode = node.id;
    if (!textNode || !layer)
        return 1;
    for (const auto& edge : session.composition()->graph().edges())
        if (edge.source.nodeId == *textNode)
            fed = edge.destination;
    if (!fed)
        return 1;
    // The animator, on the link from the Text Source to its layer.
    {
        commands::Transaction add("Add Text Animator", doc.snapshot().revision());
        add.emplace<commands::AddNode>(seed.initialCompositionId,
                                       std::string(document::kTextAnimatorNodeType),
                                       document::Vec2d{240.0, 0.0});
        if (!stack.execute(std::move(add)).changed())
            return 1;
    }
    std::optional<document::NodeId> animator;
    for (const auto& node : session.composition()->graph().nodes())
        if (node.typeId == document::kTextAnimatorNodeType)
            animator = node.id;
    if (!animator)
        return 1;
    {
        commands::Transaction wire("Wire Text Animator", doc.snapshot().revision());
        wire.emplace<commands::ConnectPorts>(
            seed.initialCompositionId,
            document::OutputPortRef{*animator, std::string(document::kTextAnimatorImagePort)}, *fed);
        wire.emplace<commands::ConnectPorts>(
            seed.initialCompositionId,
            document::OutputPortRef{*textNode, std::string(document::kTextSourceOutputPort)},
            document::NodeInputRef{*animator, std::string(document::kTextAnimatorTextPort)});
        if (!stack.execute(std::move(wire)).changed())
            return 1;
    }
    const auto units = expectedUnits();
    // Start, End and Offset count in the units Range by picks (percent or index), so they carry
    // no fixed unit; every other number does.
    for (const auto* role : {"opacity", "scale", "position", "rotation", "skew", "tracking",
                             "outlineWidth", "selector1Amount", "selector1EaseHigh", "cursorBlink"})
        if (!units.contains(QString::fromLatin1(role)) ||
            units.at(QString::fromLatin1(role)).isEmpty()) {
            std::cout << "the presentation gives " << role << " no unit\n";
            return 1;
        }

    int failures = 0;
    const auto save = [&](QImage image, const QString& surface) {
        const QString name = QStringLiteral("758-%1-%2").arg(surface, dpr);
        if (image.isNull() || !image.save(directory + '/' + name + ".png"))
            ++failures;
        std::cout << name.toStdString() << ' ' << image.width() << 'x' << image.height()
                  << " ratio=" << image.devicePixelRatio() << '\n';
    };
    const auto checkUnits = [&](const char* surface, const auto& unitOf) {
        for (const auto& [role, unit] : units) {
            const auto shown = unitOf(role);
            if (shown && *shown != unit) {
                ++failures;
                std::cout << surface << " shows " << role.toStdString() << " in "
                          << shown->toStdString() << ", not " << unit.toStdString() << '\n';
            }
        }
    };
    const auto capture = [&](const std::set<QString>& hidden, const QString& suffix) {
        session.selectNode(*animator);
        settle();
        {
            ui::PropertiesEditor properties(session);
            properties.resize(400, 3200);
            properties.show();
            settle();
            failures += truncatedLabels(properties);
            failures += checkOrder("Properties", propertiesRoles(properties), hidden);
            checkUnits("Properties",
                       [&](const QString& role) { return propertiesUnit(properties, role); });
            const auto labels = properties.findChildren<QLabel*>();
            for (const auto* label : {"Selector 1", "Units", "Shape", "Opacity", "Tracking",
                                      "Reveal", "Reveal units", "Cursor"})
                if (std::ranges::none_of(labels, [&](const QLabel* shown) {
                        return shown->isVisible() && shown->text() == QString::fromLatin1(label);
                    })) {
                    ++failures;
                    std::cout << "missing row: " << label << '\n';
                }
            save(properties.grab().toImage(), QStringLiteral("properties") + suffix);
        }
        {
            ui::NodeGraphEditor nodes(session);
            nodes.resize(1200, 900);
            nodes.show();
            settle();
            auto* card = findCard(nodes, *animator);
            if (card == nullptr) {
                ++failures;
                std::cout << "no card for the animator\n";
            } else {
                failures += checkOrder("card", cardRoles(*card), hidden);
                failures += checkColorRows(*card, !hidden.contains(QStringLiteral("fillColor")));
                checkUnits("card", [&](const QString& role) { return cardUnit(*card, role); });
                save(cardImage(nodes, *card), QStringLiteral("node-card") + suffix);
            }
        }
        failures += checkOrder("timeline", timelineRoles(session, *layer, *animator), hidden, true);
    };

    // At the defaults.
    capture(hiddenAtDefaults(), QString());
    // With Selector 2, Change fill color and Cursor on, their rows come back.
    for (const auto* role : {"selector2", "fill", "cursor"}) {
        const auto parameter = parameterOf(session, *animator, role);
        if (!parameter || !session.setParameterValue(*parameter, true, QStringLiteral("Turn On"))) {
            ++failures;
            std::cout << "could not turn on " << role << '\n';
        }
    }
    settle();
    auto shownHidden = hiddenAtDefaults();
    for (const auto& role : tableOrder())
        if (document::textAnimatorSelectorOf(role.toStdString()) == 2 &&
            !role.endsWith(QStringLiteral("Seed")))
            shownHidden.erase(role);
    for (const auto* role : {"fillColor", "cursorCharacter", "cursorBlink"})
        shownHidden.erase(QString::fromLatin1(role));
    capture(shownHidden, QStringLiteral("-on"));
    if (failures != 0)
        std::cout << failures << " check(s) failed\n";
    return failures == 0 ? 0 : 1;
}
