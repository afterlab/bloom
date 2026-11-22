// Review captures of Path Stroke's dash pattern rows (#759): the seven rows (Dash, Gap, Dash 2,
// Gap 2, Dash 3, Gap 3, Dash Offset) on Properties and on the node card, with the pattern's
// values set. Hard checks: the rows are shown in that order on both surfaces, each reads in px, the
// card's rows have slider tracks, and no label is cut. A tool, not a test: it writes
// 759-<surface>-dpr<d>.png to the directory given as the first argument and exits non-zero when a
// check fails. The device pixel ratio follows QT_SCALE_FACTOR (run it at 1, 1.5 and 2).
#include "isolated_qt_settings.hpp"

#include "node_editor_items.hpp"

#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/path_value.hpp>
#include <bloom/document/shape.hpp>
#include <bloom/document/value_nodes.hpp>
#include <bloom/document/value_utility_nodes.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/kit/value_field.hpp>
#include <bloom/ui/node_controls_widget.hpp>
#include <bloom/ui/node_editor.hpp>
#include <bloom/ui/properties_editor.hpp>
#include <bloom/ui/scalar_slider_widget.hpp>

#include <QApplication>
#include <QDir>
#include <QEvent>
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

// The dash pattern's roles in the order every surface shows them.
const std::vector<QString> kDashOrder{"dashLength", "dashGap", "dash2",     "gap2",
                                      "dash3",      "gap3",    "dashOffset"};

std::string joined(const std::vector<QString>& roles) {
    QStringList list;
    for (const auto& role : roles)
        list.push_back(role);
    return list.join(QStringLiteral(", ")).toStdString();
}

// The dash roles among the given roles, in the order given.
std::vector<QString> dashRoles(const std::vector<QString>& roles) {
    std::vector<QString> out;
    for (const auto& role : roles)
        if (std::ranges::find(kDashOrder, role) != kDashOrder.end())
            out.push_back(role);
    return out;
}

int checkOrder(const char* surface, const std::vector<QString>& roles) {
    const auto shown = dashRoles(roles);
    if (shown == kDashOrder)
        return 0;
    std::cout << surface << " dash rows: " << joined(shown)
              << "\n  expected: " << joined(kDashOrder) << '\n';
    return 1;
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

document::NodeId addNode(ui::CompositionSession& session, const std::string_view type) {
    commands::Transaction transaction("Add", session.snapshot().revision());
    transaction.emplace<commands::AddNode>(session.compositionId(), std::string(type),
                                           document::Vec2d{});
    const auto result = session.executeNodeTransaction(std::move(transaction));
    return result.outputId<document::NodeId>(commands::kAddNodeOutput).value_or(document::NodeId{});
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
} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    bloom::test_support::isolateQtSettings();
    app.setOrganizationName("BloomPathStrokeDashCapture");
    app.setApplicationName("PathStrokeDashCapture");
    QStandardPaths::setTestModeEnabled(true);
    QSettings().clear();
    ui::kit::installAfterlabTheme(app);
    const QString directory = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QStringLiteral(".");
    QDir().mkpath(directory);
    const QString dpr =
        QStringLiteral("dpr") + qEnvironmentVariable("QT_SCALE_FACTOR", QStringLiteral("1"));

    auto seed = document::makeNewProject("Dashes", "Main", core::RationalTime::fromInteger(24));
    document::Document doc(std::move(seed.project));
    commands::CommandStack stack(doc);
    ui::CompositionSession session(doc, stack, seed.initialCompositionId);

    const auto pathNode = addNode(session, document::kPathValueNodeType);
    const auto strokeNode = addNode(session, document::kPathStrokeNodeType);
    const auto shapeNode = addNode(session, document::kShapeSourceNodeType);
    if (!pathNode.isValid() || !strokeNode.isValid() || !shapeNode.isValid()) {
        std::cout << "the nodes could not be added\n";
        return 1;
    }
    int failures = 0;
    const auto expect = [&](const bool ok, const char* what) {
        if (!ok) {
            ++failures;
            std::cout << "failed: " << what << '\n';
        }
    };

    document::PathContour line;
    for (const auto& point : {document::Vec2d{-200, 0}, document::Vec2d{-60, -80},
                              document::Vec2d{60, 80}, document::Vec2d{200, 0}})
        line.anchors.push_back(
            {point, std::nullopt, std::nullopt, document::PathAnchorMode::Corner});
    const auto path = document::PathValue::fromContour(line);
    if (const auto id = parameterOf(session, pathNode, document::kValueParameterRole))
        expect(session.setParameterValue(*id, path, QStringLiteral("Path")), "the path is set");
    else
        expect(false, "the Path node has a value parameter");
    if (const auto id = parameterOf(session, shapeNode, "kind"))
        expect(session.setParameterValue(*id, static_cast<std::int64_t>(document::ShapeKind::Path),
                                         QStringLiteral("Kind")),
               "the shape kind is set to Path");

    const std::vector<std::pair<const char*, double>> values{
        {"dashLength", 12}, {"dashGap", 6}, {"dash2", 0},     {"gap2", 6},
        {"dash3", 3},       {"gap3", 6},    {"dashOffset", 2}};
    for (const auto& [role, value] : values) {
        const auto id = parameterOf(session, strokeNode, role);
        expect(id && session.setParameterValue(*id, value, QStringLiteral("Dash")),
               "a dash parameter is set");
    }
    if (const auto id = parameterOf(session, strokeNode, "cap"))
        expect(session.setParameterValue(*id, std::int64_t{1}, QStringLiteral("Cap")),
               "the cap is set");

    const auto connect = [&](const document::NodeId from, const std::string& output,
                             const document::NodeId to) {
        commands::Transaction transaction("Connect", session.snapshot().revision());
        transaction.emplace<commands::ConnectPorts>(session.compositionId(),
                                                    document::OutputPortRef{from, output},
                                                    document::NodeInputRef{to, "path"});
        expect(session.executeNodeTransaction(std::move(transaction)).succeeded(),
               "a path connection is made");
    };
    connect(pathNode, std::string(document::kValuePortName), strokeNode);
    connect(strokeNode, "path", shapeNode);

    const auto save = [&](QImage image, const QString& surface) {
        const QString name = QStringLiteral("759-%1-%2").arg(surface, dpr);
        if (image.isNull() || !image.save(directory + '/' + name + ".png"))
            ++failures;
        std::cout << name.toStdString() << ' ' << image.width() << 'x' << image.height()
                  << " ratio=" << image.devicePixelRatio() << '\n';
    };

    session.selectNode(strokeNode);
    settle();
    {
        ui::PropertiesEditor properties(session);
        properties.resize(380, 2000);
        properties.show();
        settle();
        failures += truncatedLabels(properties);
        // Path Stroke's Properties section is the node's controls widget (slider rows), not
        // registry rows, so the rows are read from it as on the card.
        auto* controls = properties.findChild<ui::NodeControlsCardWidget*>();
        if (controls == nullptr) {
            ++failures;
            std::cout << "Properties has no Path Stroke controls widget\n";
        }
        std::vector<std::pair<int, QString>> shownRows;
        if (controls != nullptr)
            for (auto* row : controls->sliderRows()) {
                const QString role = QString::fromStdString(row->role());
                if (std::ranges::find(kDashOrder, role) == kDashOrder.end())
                    continue;
                const QString unit = row->field() ? row->field()->unit() : QStringLiteral("<none>");
                std::cout << "Properties " << role.toStdString() << ": label \""
                          << row->name().toStdString() << "\" visible " << row->isVisible()
                          << " track " << (row->slider() != nullptr) << " unit "
                          << unit.toStdString() << '\n';
                // The labels read as the artist names them: "Dash 2", not "Dash2".
                const std::map<QString, QString> kLabels{
                    {"dashLength", "Dash Length"}, {"dashGap", "Dash Gap"},
                    {"dash2", "Dash 2"},           {"gap2", "Gap 2"},
                    {"dash3", "Dash 3"},           {"gap3", "Gap 3"},
                    {"dashOffset", "Dash Offset"}};
                if (row->name() != kLabels.at(role)) {
                    ++failures;
                    std::cout << "failed: Properties row " << role.toStdString() << " reads \""
                              << row->name().toStdString() << "\", not \""
                              << kLabels.at(role).toStdString() << "\"\n";
                }
                if (!row->isVisible() || row->slider() == nullptr || unit != QStringLiteral("px")) {
                    ++failures;
                    std::cout << "failed: Properties row " << role.toStdString() << '\n';
                }
                shownRows.emplace_back(row->mapTo(&properties, QPoint()).y(), role);
            }
        std::ranges::sort(shownRows);
        std::vector<QString> roles;
        for (const auto& row : shownRows)
            roles.push_back(row.second);
        failures += checkOrder("Properties", roles);
        save(properties.grab().toImage(), QStringLiteral("properties"));
    }
    {
        ui::NodeGraphEditor nodes(session);
        nodes.resize(1100, 900);
        nodes.show();
        settle();
        auto* card = findCard(nodes, strokeNode);
        auto* controls = card == nullptr
                             ? nullptr
                             : qobject_cast<ui::NodeControlsCardWidget*>(card->cardWidget());
        if (card == nullptr || controls == nullptr) {
            ++failures;
            std::cout << "the Path Stroke card has no controls widget\n";
        } else {
            std::vector<std::pair<int, QString>> rows;
            for (auto* row : controls->sliderRows()) {
                const QString role = QString::fromStdString(std::string(row->role()));
                if (std::ranges::find(kDashOrder, role) == kDashOrder.end())
                    continue;
                rows.emplace_back(row->mapTo(controls, QPoint()).y(), role);
                const QString unit = row->field() ? row->field()->unit() : QStringLiteral("<none>");
                std::cout << "card " << role.toStdString() << ": track "
                          << (row->slider() != nullptr) << " unit " << unit.toStdString() << '\n';
                if (row->slider() == nullptr || unit != QStringLiteral("px")) {
                    ++failures;
                    std::cout << "failed: card row " << role.toStdString()
                              << " has no track or px\n";
                }
            }
            std::ranges::sort(rows);
            std::vector<QString> roles;
            for (const auto& row : rows)
                roles.push_back(row.second);
            failures += checkOrder("card", roles);
            failures += truncatedLabels(*controls);
            save(cardImage(nodes, *card), QStringLiteral("node-card"));
        }
    }
    if (failures != 0)
        std::cout << failures << " check(s) failed\n";
    return failures == 0 ? 0 : 1;
}
