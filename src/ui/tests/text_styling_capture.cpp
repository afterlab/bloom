// Review captures of the Text Source styling (#757): the Properties of a styled text (Markup, the
// accents, the outline and the background rows), its node card, and an older Text node with no
// styling bound, whose Properties offer the button that binds them; the button is clicked and the
// rows must then appear. Then a text with Background Off and Outline width 0, whose Properties and
// card hide the rows nothing reads (they come back when the values change). Hard checks: the styling
// rows run Markup, the accents, the outline, the background on Properties, the card and the
// timeline; every color's swatch row on the card sits directly above its R/G/B/A rows; Letter
// Spacing shows the parameter's unit on Properties and the card. A tool, not a test: it writes
// 757-<surface>-dpr<d>.png to the directory given as the first argument and exits non-zero when a
// check fails. The device pixel ratio follows QT_SCALE_FACTOR (run it at 1, 1.5 and 2).
#include "isolated_qt_settings.hpp"

#include "node_editor_items.hpp"

#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/node_definition_registry.hpp>
#include <bloom/document/parameter_presentation.hpp>
#include <bloom/document/text_styling.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/color_chip.hpp>
#include <bloom/ui/kit/value_field.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/node_editor.hpp>
#include <bloom/ui/properties_editor.hpp>
#include <bloom/ui/timeline_editor.hpp>

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
#include <optional>
#include <set>
#include <utility>
#include <string>
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

// The styling button, when one is shown (a rebuilt panel deletes the old one later).
ui::kit::KButton* stylingButton(const QWidget& root) {
    for (auto* button : root.findChildren<ui::kit::KButton*>("propertiesTextShowStyling"))
        if (button->isVisible())
            return button;
    return nullptr;
}

bool hasLabel(const QWidget& root, const QString& text) {
    for (const auto* label : root.findChildren<QLabel*>())
        if (label->isVisible() && label->text() == text)
            return true;
    return false;
}

// The styling rows (and Overflow, the layout row before them) in the order every surface shows
// them: grouped by function.
const std::vector<QString> kStylingOrder{
    "overflow",     "markup",          "accent1",           "accent2",
    "accent3",      "accent4",         "outlineColor",      "outlineWidth",
    "outlineJoin",  "background",      "backgroundColor",   "backgroundPadding",
    "backgroundRadius", "fixedDigits"};
// The rows a text with Background Off and Outline width 0 does not read.
const std::set<QString> kInactiveWhenPlain{"outlineColor", "outlineJoin", "backgroundColor",
                                           "backgroundPadding", "backgroundRadius"};

// `order` without the roles not in `shown`.
std::vector<QString> expectedOrder(const std::set<QString>& hidden) {
    std::vector<QString> order;
    for (const auto& role : kStylingOrder)
        if (!hidden.contains(role))
            order.push_back(role);
    return order;
}

// The roles of `roles` that are styling rows, consecutive repeats folded (a color's channel rows).
std::vector<QString> stylingRoles(const std::vector<QString>& roles) {
    std::vector<QString> out;
    for (const auto& role : roles)
        if (std::ranges::find(kStylingOrder, role) != kStylingOrder.end() &&
            (out.empty() || out.back() != role))
            out.push_back(role);
    return out;
}

std::string joined(const std::vector<QString>& roles) {
    QStringList list;
    for (const auto& role : roles)
        list.push_back(role);
    return list.join(QStringLiteral(", ")).toStdString();
}

int checkOrder(const char* surface, const std::vector<QString>& roles,
               const std::set<QString>& hidden) {
    const auto shown = stylingRoles(roles);
    const auto expected = expectedOrder(hidden);
    if (shown == expected)
        return 0;
    std::cout << surface << " styling rows: " << joined(shown) << "\n  expected: " << joined(expected)
              << '\n';
    return 1;
}

// The visible Properties registry rows' roles, top to bottom.
std::vector<QString> propertiesRoles(const QWidget& root) {
    std::vector<std::pair<int, QString>> rows;
    for (auto* row : root.findChildren<QWidget*>(QStringLiteral("propertiesRegistryRow")))
        if (row->isVisible() && row->property("role").isValid())
            rows.emplace_back(row->mapTo(&root, QPoint()).y(), row->property("role").toString());
    std::ranges::sort(rows);
    std::vector<QString> roles;
    for (const auto& row : rows)
        roles.push_back(row.second);
    return roles;
}

QString propertiesUnit(const QWidget& root, const QString& role) {
    for (auto* row : root.findChildren<QWidget*>(QStringLiteral("propertiesRegistryRow")))
        if (row->property("role").toString() == role)
            if (const auto* field = row->findChild<ui::kit::KValueField*>())
                return field->unit();
    return QStringLiteral("<none>");
}

struct CardRow final {
    qreal y = 0.0;
    QString role;
    bool swatch = false;
    bool component = false;
    QWidget* row = nullptr;
};

// The node card's shown rows, top to bottom.
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

// Every color's swatch row sits directly above its own R, G, B and A rows, and no channel row is
// anywhere else.
int checkColorRows(const ui::node_editor::NodeItem& card) {
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
    if (swatches == 0) {
        ++failures;
        std::cout << "card: no color swatch rows\n";
    }
    return failures;
}

QString cardUnit(const ui::node_editor::NodeItem& card, const QString& role) {
    for (const auto& row : cardRows(card))
        if (row.role == role)
            if (const auto* field = row.row->findChild<ui::kit::KValueField*>())
                return field->unit();
    return QStringLiteral("<none>");
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

// The timeline's parameter rows of `layer`, in order.
std::vector<QString> timelineRoles(const ui::CompositionSession& session,
                                   const document::LayerId layer) {
    ui::TimelineLayerEntry entry;
    entry.layerId = layer;
    std::vector<QString> roles;
    for (const auto& row : ui::timelinePropertyEntries(session, {entry}, {layer}))
        if (row.rowKind == ui::TimelineLayerEntry::Kind::Parameter)
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

// Strips the styling bindings (and their parameters) from a Text Source, leaving the twelve layout
// parameters a node saved before #757 has.
bool makeOlder(document::Document& doc, const document::CompositionId compositionId,
               const document::NodeId nodeId) {
    const auto snapshot = doc.snapshot();
    auto draft = doc.draft(snapshot);
    auto* composition = draft.project().findComposition(compositionId);
    auto* node = composition == nullptr ? nullptr : composition->graph().findNode(nodeId);
    if (node == nullptr)
        return false;
    std::vector<document::ParameterBinding> kept;
    for (const auto& binding : node->parameters) {
        if (document::textStylingParameterSpecForRole(binding.role) == nullptr) {
            kept.push_back(binding);
            continue;
        }
        if (!composition->parameters().erase(binding.parameterId))
            return false;
    }
    node->parameters = std::move(kept);
    if (!draft.validate().ok()) {
        std::cout << "validation refused the older node\n";
        return false;
    }
    return doc.commit(snapshot.revision(), std::move(draft)).committed();
}
} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    bloom::test_support::isolateQtSettings();
    app.setOrganizationName("BloomTextStylingCapture");
    app.setApplicationName("TextStylingCapture");
    QStandardPaths::setTestModeEnabled(true);
    QSettings().clear();
    ui::kit::installAfterlabTheme(app);
    const QString directory = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QStringLiteral(".");
    QDir().mkpath(directory);
    const QString dpr =
        QStringLiteral("dpr") + qEnvironmentVariable("QT_SCALE_FACTOR", QStringLiteral("1"));

    auto seed =
        document::makeNewProject("Text styling", "Main", core::RationalTime::fromInteger(24));
    document::Document doc(std::move(seed.project));
    commands::CommandStack stack(doc);
    ui::CompositionSession session(doc, stack, seed.initialCompositionId);
    commands::TextLayerStyle style;
    style.styling = {{"markup", true},
                     {"accent1", core::Color4d{1.0, 0.55, 0.0, 1.0}},
                     {"outlineWidth", 4.0},
                     {"background", std::int64_t{document::kTextBackgroundPerLine}}};
    if (!session.addTextLayer(QStringLiteral("Caption"),
                              QStringLiteral("Bloom <c.accent1>now</c> <b>live</b>"), 96.0,
                              core::Color4d{1.0, 1.0, 1.0, 1.0}, std::nullopt, style))
        return 1;
    const auto styledLayer = session.selection().contextualLayer;
    std::optional<document::NodeId> textNode;
    for (const auto& node : session.composition()->graph().nodes())
        if (node.typeId == document::kTextSourceNodeType)
            textNode = node.id;
    if (!textNode || !styledLayer)
        return 1;
    // The unit Letter Spacing is stored in, which every surface shows.
    QString spacingUnit;
    if (const auto* definition = document::builtInNodeDefinitions().find(
            document::kTextSourceNodeType, document::kTextSourceNodeSchemaVersion))
        for (const auto& parameter : definition->parameters)
            if (parameter.role == document::kTextLetterSpacingParameterRole)
                spacingUnit = QString::fromStdString(document::parameterPresentation(parameter).unit);
    if (spacingUnit.isEmpty())
        return 1;

    int failures = 0;
    const auto save = [&](QImage image, const QString& surface) {
        const QString name = QStringLiteral("757-%1-%2").arg(surface, dpr);
        if (image.isNull() || !image.save(directory + '/' + name + ".png"))
            ++failures;
        std::cout << name.toStdString() << ' ' << image.width() << 'x' << image.height()
                  << " ratio=" << image.devicePixelRatio() << '\n';
    };

    {
        session.selectNode(*textNode);
        settle();
        ui::PropertiesEditor properties(session);
        properties.resize(380, 2000);
        properties.show();
        settle();
        failures += truncatedLabels(properties);
        for (const auto* label :
             {"Markup", "Accent 1", "Accent 4", "Outline color", "Outline width", "Outline join",
              "Background", "Background color", "Background padding", "Background corner radius",
              "Fixed-width digits"})
            if (!hasLabel(properties, QString::fromLatin1(label))) {
                ++failures;
                std::cout << "missing row: " << label << '\n';
            }
        if (stylingButton(properties) != nullptr) {
            ++failures;
            std::cout << "a new text layer offers the styling button\n";
        }
        failures += checkOrder("Properties", propertiesRoles(properties), {});
        if (const auto unit = propertiesUnit(
                properties, QString::fromLatin1(document::kTextLetterSpacingParameterRole));
            unit != spacingUnit) {
            ++failures;
            std::cout << "Properties shows Letter Spacing in " << unit.toStdString() << ", not "
                      << spacingUnit.toStdString() << '\n';
        }
        save(properties.grab().toImage(), QStringLiteral("properties"));
    }
    {
        // The timeline lists the keyable rows; those it lists run in the same order.
        const auto roles = stylingRoles(timelineRoles(session, *styledLayer));
        std::vector<QString> expected;
        for (const auto& role : kStylingOrder)
            if (std::ranges::find(roles, role) != roles.end())
                expected.push_back(role);
        if (roles != expected || roles.size() < document::text_styling_parameters::kSocketedCount) {
            ++failures;
            std::cout << "timeline styling rows: " << joined(roles) << '\n';
        }
    }
    {
        ui::NodeGraphEditor nodes(session);
        nodes.resize(900, 700);
        nodes.show();
        settle();
        auto* card = findCard(nodes, *textNode);
        if (card == nullptr) {
            ++failures;
        } else {
            failures += checkOrder("card", cardRoles(*card), {});
            failures += checkColorRows(*card);
            if (const auto unit =
                    cardUnit(*card, QString::fromLatin1(document::kTextLetterSpacingParameterRole));
                unit != spacingUnit) {
                ++failures;
                std::cout << "the card shows Letter Spacing in " << unit.toStdString() << ", not "
                          << spacingUnit.toStdString() << '\n';
            }
            save(cardImage(nodes, *card), QStringLiteral("node-card"));
        }
    }
    {
        if (!session.addTextLayer(QStringLiteral("Older title"), QStringLiteral("Older title")))
            return 1;
        std::optional<document::NodeId> older;
        for (const auto& node : session.composition()->graph().nodes())
            if (node.typeId == document::kTextSourceNodeType && node.id != *textNode)
                older = node.id;
        if (!older || !makeOlder(doc, seed.initialCompositionId, *older)) {
            std::cout << "the older text node could not be made\n";
            return 1;
        }
        // A session opened on the edited document, as when the older project is opened.
        commands::CommandStack reopenedStack(doc);
        ui::CompositionSession reopened(doc, reopenedStack, seed.initialCompositionId);
        reopened.selectNode(*older);
        settle();
        ui::PropertiesEditor properties(reopened);
        properties.resize(380, 1500);
        properties.show();
        settle();
        failures += truncatedLabels(properties);
        auto* button = stylingButton(properties);
        if (button == nullptr || hasLabel(properties, QStringLiteral("Accent 1"))) {
            ++failures;
            std::cout << "an older text node does not offer the styling button alone\n";
        }
        save(properties.grab().toImage(), QStringLiteral("properties-older"));
        if (button != nullptr) {
            button->click();
            settle();
            const auto* node = reopened.composition()->graph().findNode(*older);
            if (node == nullptr || node->parameters.size() != 25 ||
                !hasLabel(properties, QStringLiteral("Outline width")) ||
                stylingButton(properties) != nullptr) {
                ++failures;
                std::cout << "the styling button does not bind the rows (bindings "
                          << (node ? node->parameters.size() : 0U) << ", outline row "
                          << hasLabel(properties, QStringLiteral("Outline width")) << ", button "
                          << (stylingButton(properties) != nullptr) << ")\n";
            }
            failures += truncatedLabels(properties);
            save(properties.grab().toImage(), QStringLiteral("properties-older-shown"));
        }
    }
    {
        // A text with Background Off and Outline width 0 (the defaults): the background's color,
        // padding and corner radius and the outline's color and join are not read, so no surface
        // shows them; they come back when Background and Outline width change.
        // A session opened on the document as it now is (the older node was committed beside it).
        commands::CommandStack plainStack(doc);
        ui::CompositionSession plainSession(doc, plainStack, seed.initialCompositionId);
        commands::TextLayerStyle plain;
        plain.styling = {{"markup", true}};
        if (!plainSession.addTextLayer(QStringLiteral("Plain caption"), QStringLiteral("Plain caption"),
                                  96.0, core::Color4d{1.0, 1.0, 1.0, 1.0}, std::nullopt, plain))
            return 1;
        const auto plainLayer = plainSession.selection().contextualLayer;
        std::optional<document::NodeId> plainNode;
        // The newest Text Source.
        for (const auto& node : plainSession.composition()->graph().nodes())
            if (node.typeId == document::kTextSourceNodeType &&
                (!plainNode || node.id.value() > plainNode->value()))
                plainNode = node.id;
        const auto background = plainNode ? parameterOf(plainSession, *plainNode, "background")
                                           : std::nullopt;
        const auto width = plainNode ? parameterOf(plainSession, *plainNode, "outlineWidth")
                                     : std::nullopt;
        if (!plainNode || !plainLayer || !background || !width) {
            std::cout << "the plain text node could not be made\n";
            return 1;
        }
        plainSession.selectNode(*plainNode);
        settle();
        ui::PropertiesEditor properties(plainSession);
        properties.resize(380, 1700);
        properties.show();
        ui::NodeGraphEditor nodes(plainSession);
        nodes.resize(900, 700);
        nodes.show();
        settle();
        failures += truncatedLabels(properties);
        for (const auto* label : {"Outline color", "Outline join", "Background color",
                                  "Background padding", "Background corner radius"})
            if (hasLabel(properties, QString::fromLatin1(label))) {
                ++failures;
                std::cout << "an inactive row shows: " << label << '\n';
            }
        failures += checkOrder("Properties (inactive)", propertiesRoles(properties),
                               kInactiveWhenPlain);
        auto* card = findCard(nodes, *plainNode);
        if (card == nullptr) {
            ++failures;
        } else {
            failures += checkOrder("card (inactive)", cardRoles(*card), kInactiveWhenPlain);
            failures += checkColorRows(*card);
            save(cardImage(nodes, *card), QStringLiteral("node-card-inactive"));
        }
        for (const auto& role : stylingRoles(timelineRoles(plainSession, *plainLayer)))
            if (kInactiveWhenPlain.contains(role)) {
                ++failures;
                std::cout << "the timeline shows an inactive row: " << role.toStdString() << '\n';
            }
        save(properties.grab().toImage(), QStringLiteral("properties-inactive"));
        // The rows come back with the values that read them.
        if (!plainSession.setParameterValue(*background,
                                       std::int64_t{document::kTextBackgroundBlock},
                                       QStringLiteral("Set Background")) ||
            !plainSession.setParameterValue(*width, 3.0, QStringLiteral("Set Outline Width"))) {
            ++failures;
            std::cout << "the plain text's Background and Outline width could not be set\n";
        }
        settle();
        failures += checkOrder("Properties (active again)", propertiesRoles(properties), {});
        card = findCard(nodes, *plainNode);
        if (card != nullptr)
            failures += checkOrder("card (active again)", cardRoles(*card), {});
        const auto timeline = stylingRoles(timelineRoles(plainSession, *plainLayer));
        for (const auto& role : kInactiveWhenPlain)
            if (std::ranges::find(timeline, role) == timeline.end() && role != "outlineJoin") {
                ++failures;
                std::cout << "the timeline does not show " << role.toStdString() << " again\n";
            }
    }
    if (failures != 0)
        std::cout << failures << " check(s) failed\n";
    return failures == 0 ? 0 : 1;
}
