// Review captures of the coordinate space nodes (#591): the Properties of a Coordinate Space in each
// mode, of Space Readout, To Space, From Space and Place in Space, of Function Plot, Axes and Axis
// Tick with a space connected (no Origin or Pixels Per Unit rows) and without one, and the cards of
// the space nodes wired into a plot beside an Axis Tick with no space. A tool, not a test: it writes
// 591-<surface>-dpr<d>.png to the directory given as the first argument. The device pixel ratio
// follows QT_SCALE_FACTOR (run it at 1, 1.5 and 2). It fails when a surface is missing, when a
// Properties label is cut short, or when a placement row shows while a space is connected.
#include "isolated_qt_settings.hpp"

#include "node_editor_items.hpp"

#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/node_group_nodes.hpp>
#include <bloom/document/value_utility_nodes.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/grading_controls.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/node_editor.hpp>
#include <bloom/ui/properties_editor.hpp>

#include <QApplication>
#include <QDir>
#include <QGraphicsView>
#include <QImage>
#include <QLabel>
#include <QPainter>
#include <QSettings>
#include <QStandardPaths>

#include <iostream>
#include <string>
#include <vector>

namespace {
using namespace bloom;

void settle() {
    for (int pass = 0; pass < 3; ++pass) {
        QCoreApplication::sendPostedEvents();
        QCoreApplication::processEvents();
    }
}

document::NodeId add(ui::CompositionSession& session, const std::string_view type,
                     const document::Vec2d at) {
    commands::Transaction transaction("Add", session.snapshot().revision());
    transaction.emplace<commands::AddNode>(session.compositionId(), std::string(type), at);
    return session.executeNodeTransaction(std::move(transaction))
        .outputId<document::NodeId>(commands::kAddNodeOutput)
        .value_or(document::NodeId{});
}

bool connect(ui::CompositionSession& session, const document::NodeId from, const std::string& output,
             const document::NodeId to, const std::string& input) {
    commands::Transaction transaction("Connect", session.snapshot().revision());
    transaction.emplace<commands::ConnectPorts>(session.compositionId(),
                                                document::OutputPortRef{from, output},
                                                document::NodeInputRef{to, input});
    return session.executeNodeTransaction(std::move(transaction)).changed();
}

// A label whose text is wider than the room it was given is cut short.
// Whether Properties shows the row of `node`'s `role` (a registry row names its parameter; the
// panel also lists the nodes upstream, such as the space with its own Origin).
bool showsRow(const QWidget& root, ui::CompositionSession& session, const document::NodeId node,
              const char* role) {
    const auto parameter = ui::nodeRoleParameter(session, node, role);
    if (!parameter)
        return false;
    for (const auto* child : root.findChildren<QWidget*>())
        if (child->property("parameterId").toULongLong() == parameter->value() &&
            child->isVisibleTo(&root))
            return true;
    return false;
}

int truncatedLabels(const QWidget& root) {
    int cut = 0;
    for (const auto* label : root.findChildren<QLabel*>()) {
        if (!label->isVisible() || label->text().isEmpty() || label->wordWrap())
            continue;
        if (label->fontMetrics().horizontalAdvance(label->text()) > label->contentsRect().width() + 1) {
            ++cut;
            std::cout << "cut label: " << label->text().toStdString() << '\n';
        }
    }
    return cut;
}
} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    bloom::test_support::isolateQtSettings();
    app.setOrganizationName("BloomCoordinateSpaceCapture");
    app.setApplicationName("CoordinateSpaceCapture");
    QStandardPaths::setTestModeEnabled(true);
    QSettings().clear();
    ui::kit::installAfterlabTheme(app);
    const QString directory = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QStringLiteral(".");
    QDir().mkpath(directory);
    const QString dpr =
        QStringLiteral("dpr") + qEnvironmentVariable("QT_SCALE_FACTOR", QStringLiteral("1"));

    auto seed = document::makeNewProject("Coordinate Spaces", "Main",
                                         core::RationalTime::fromInteger(24));
    document::Document doc(std::move(seed.project));
    commands::CommandStack stack(doc);
    ui::CompositionSession session(doc, stack, seed.initialCompositionId);
    const auto space = add(session, document::kCoordinateSpaceNodeType, {0, 0});
    const auto readout = add(session, document::kSpaceReadoutNodeType, {320, -200});
    const auto toSpace = add(session, document::kToSpaceNodeType, {320, 120});
    const auto fromSpace = add(session, document::kFromSpaceNodeType, {320, 300});
    const auto place = add(session, document::kPlaceInSpaceNodeType, {320, 480});
    const auto axes = add(session, document::kAxesNodeType, {640, 0});
    const auto tick = add(session, document::kAxisTickNodeType, {640, 520});
    const auto plot = add(session, document::kFunctionPlotNodeType, {960, 0});
    const auto loose = add(session, document::kAxisTickNodeType, {960, 520});
    int failures = 0;
    for (const auto consumer : {readout, toSpace, fromSpace, place, axes, tick, plot})
        if (!connect(session, space, "space", consumer, "space"))
            ++failures;
    settle();

    const auto save = [&](QImage image, const QString& surface) {
        const QString name = QStringLiteral("591-%1-%2").arg(surface, dpr);
        if (image.isNull() || !image.save(directory + '/' + name + ".png"))
            ++failures;
        std::cout << name.toStdString() << ' ' << image.width() << 'x' << image.height()
                  << " ratio=" << image.devicePixelRatio() << '\n';
    };

    // Properties of the space in each mode: only what the mode reads.
    for (const auto& [mode, surface] :
         std::vector<std::pair<std::int64_t, QString>>{{0, QStringLiteral("properties-origin")},
                                                       {1, QStringLiteral("properties-rectangle")},
                                                       {2, QStringLiteral("properties-corners")}}) {
        const auto id = ui::nodeRoleParameter(session, space, "mode");
        if (!id || !session.setParameterValue(*id, mode, QStringLiteral("Mode")))
            ++failures;
        session.selectNode(space);
        settle();
        ui::PropertiesEditor properties(session);
        properties.resize(380, 1300);
        properties.show();
        settle();
        failures += truncatedLabels(properties);
        save(properties.grab().toImage(), surface);
    }
    // The other space nodes, and the three that draw in a space: with one connected, their own
    // Origin and Pixels Per Unit rows are not shown (an Axis Tick with no space keeps them).
    struct Surface final {
        document::NodeId node;
        const char* name;
        bool spaced;
    };
    for (const auto& surface : std::vector<Surface>{{readout, "properties-space-readout", false},
                                                    {toSpace, "properties-to-space", false},
                                                    {fromSpace, "properties-from-space", false},
                                                    {place, "properties-place-in-space", false},
                                                    {plot, "properties-function-plot-in-space", true},
                                                    {axes, "properties-axes-in-space", true},
                                                    {tick, "properties-axis-tick-in-space", true},
                                                    {loose, "properties-axis-tick-own-origin", false}}) {
        session.selectNode(surface.node);
        settle();
        ui::PropertiesEditor properties(session);
        properties.resize(380, 1300);
        properties.show();
        settle();
        failures += truncatedLabels(properties);
        if (surface.spaced && (showsRow(properties, session, surface.node, "origin") ||
                                showsRow(properties, session, surface.node, "pixelsPerUnit"))) {
            ++failures;
            std::cout << surface.name << ": a placement row shows while a space is connected\n";
        }
        if (surface.node == loose &&
            (!showsRow(properties, session, surface.node, "origin") ||
             !showsRow(properties, session, surface.node, "pixelsPerUnit"))) {
            ++failures;
            std::cout << surface.name << ": the placement rows are missing with no space\n";
        }
        save(properties.grab().toImage(), QString::fromLatin1(surface.name));
    }
    {
        ui::NodeGraphEditor nodes(session);
        nodes.resize(1200, 900);
        nodes.show();
        settle();
        auto* view = nodes.findChild<QGraphicsView*>();
        if (view == nullptr) {
            ++failures;
        } else {
            QRectF bounds;
            int cards = 0;
            for (auto* item : view->scene()->items())
                if (auto* card = dynamic_cast<ui::node_editor::NodeItem*>(item)) {
                    bounds = bounds.united(card->sceneBoundingRect());
                    ++cards;
                }
            if (cards < 9)
                ++failures;
            const auto source = bounds.adjusted(-16, -16, 16, 16);
            const qreal ratio = nodes.devicePixelRatioF();
            QImage image((source.size() * ratio).toSize(), QImage::Format_ARGB32_Premultiplied);
            image.setDevicePixelRatio(ratio);
            image.fill(nodes.palette().window().color());
            QPainter painter(&image);
            painter.setRenderHint(QPainter::Antialiasing);
            view->scene()->render(&painter, QRectF(QPointF(), source.size()), source);
            painter.end();
            save(image, QStringLiteral("node-cards"));
        }
    }
    return failures == 0 ? 0 : 1;
}
