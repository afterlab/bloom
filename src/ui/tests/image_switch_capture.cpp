// Review captures of the Image Switch and the Channel Set Switch (#531): the Properties of an
// Image Switch over three inputs (a checker, a gradient and a text), its node card, and the card of
// a Channel Set Switch over two channel sets. Hard checks: Properties shows the Show input and Wrap
// around rows, each card is drawn, and no label is cut. A tool, not a test: it writes
// 531-<surface>-dpr<d>.png to the directory given as the first argument and exits non-zero when a
// check fails. The device pixel ratio follows QT_SCALE_FACTOR (run it at 1, 1.5 and 2).
#include "isolated_qt_settings.hpp"

#include "node_editor_items.hpp"

#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/document/channel_nodes.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/image_switch_nodes.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/node_editor.hpp>
#include <bloom/ui/properties_editor.hpp>

#include <QApplication>
#include <QDir>
#include <QEvent>
#include <QGraphicsView>
#include <QImage>
#include <QLabel>
#include <QPainter>
#include <QSettings>
#include <QStandardPaths>

#include <iostream>
#include <string>

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

bool showsLabel(const QWidget& root, const QString& text) {
    for (const auto* label : root.findChildren<QLabel*>())
        if (label->isVisible() && label->text() == text)
            return true;
    return false;
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

document::NodeId addNode(ui::CompositionSession& session, const std::string_view type,
                         const document::Vec2d at) {
    commands::Transaction transaction("Add", session.snapshot().revision());
    transaction.emplace<commands::AddNode>(session.compositionId(), std::string(type), at);
    const auto result = session.executeNodeTransaction(std::move(transaction));
    return result.outputId<document::NodeId>(commands::kAddNodeOutput).value_or(document::NodeId{});
}
} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    bloom::test_support::isolateQtSettings();
    app.setOrganizationName("BloomImageSwitchCapture");
    app.setApplicationName("ImageSwitchCapture");
    QStandardPaths::setTestModeEnabled(true);
    QSettings().clear();
    ui::kit::installAfterlabTheme(app);
    const QString directory = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QStringLiteral(".");
    QDir().mkpath(directory);
    const QString dpr =
        QStringLiteral("dpr") + qEnvironmentVariable("QT_SCALE_FACTOR", QStringLiteral("1"));

    auto seed = document::makeNewProject("Switch", "Main", core::RationalTime::fromInteger(24));
    document::Document doc(std::move(seed.project));
    commands::CommandStack stack(doc);
    ui::CompositionSession session(doc, stack, seed.initialCompositionId);

    int failures = 0;
    const auto expect = [&](const bool ok, const char* what) {
        if (!ok) {
            ++failures;
            std::cout << "failed: " << what << '\n';
        }
    };
    const auto connect = [&](const document::NodeId from, const std::string_view output,
                             const document::NodeId to, const std::string_view input) {
        commands::Transaction transaction("Connect", session.snapshot().revision());
        transaction.emplace<commands::ConnectPorts>(
            session.compositionId(), document::OutputPortRef{from, std::string(output)},
            document::NodeInputRef{to, std::string(input)});
        expect(session.executeNodeTransaction(std::move(transaction)).succeeded(),
               "a connection is made");
    };

    const auto checker = addNode(session, document::kCheckerSourceNodeType, {-420, -160});
    const auto gradient = addNode(session, "bloom.four-color-gradient", {-420, 0});
    const auto text = addNode(session, document::kTextSourceNodeType, {-420, 160});
    const auto imageSwitch = addNode(session, document::kImageSwitchNodeType, {0, 0});
    const auto packA = addNode(session, document::kImageToChannelSetNodeType, {-420, 420});
    const auto packB = addNode(session, document::kImageToChannelSetNodeType, {-420, 560});
    const auto channelSwitch = addNode(session, document::kChannelSetSwitchNodeType, {0, 480});
    expect(checker.isValid() && gradient.isValid() && text.isValid() && imageSwitch.isValid() &&
               packA.isValid() && packB.isValid() && channelSwitch.isValid(),
           "the nodes are added");
    connect(checker, "image", imageSwitch, document::kImageSwitchInputPorts[0]);
    connect(gradient, "image", imageSwitch, document::kImageSwitchInputPorts[1]);
    connect(text, document::kTextSourceOutputPort, imageSwitch, document::kImageSwitchInputPorts[2]);
    connect(checker, "image", packA, document::kImageToChannelSetImagePort);
    connect(gradient, "image", packB, document::kImageToChannelSetImagePort);
    connect(packA, document::kChannelSetPort, channelSwitch, document::kChannelSetSwitchInputPorts[0]);
    connect(packB, document::kChannelSetPort, channelSwitch, document::kChannelSetSwitchInputPorts[1]);

    const auto save = [&](QImage image, const QString& surface) {
        const QString name = QStringLiteral("531-%1-%2").arg(surface, dpr);
        if (image.isNull() || !image.save(directory + '/' + name + ".png"))
            ++failures;
        std::cout << name.toStdString() << ' ' << image.width() << 'x' << image.height()
                  << " ratio=" << image.devicePixelRatio() << '\n';
    };

    session.selectNode(imageSwitch);
    settle();
    {
        ui::PropertiesEditor properties(session);
        properties.resize(380, 900);
        properties.show();
        settle();
        failures += truncatedLabels(properties);
        expect(showsLabel(properties, QStringLiteral("Show input")),
               "Properties shows the Show input row");
        expect(showsLabel(properties, QStringLiteral("Wrap around")),
               "Properties shows the Wrap around row");
        save(properties.grab().toImage(), QStringLiteral("properties"));
    }
    {
        ui::NodeGraphEditor nodes(session);
        nodes.resize(1400, 1100);
        nodes.show();
        settle();
        for (const auto& [id, surface] :
             {std::pair{imageSwitch, QStringLiteral("image-switch-card")},
              std::pair{channelSwitch, QStringLiteral("channel-set-switch-card")}}) {
            auto* card = findCard(nodes, id);
            expect(card != nullptr, "the switch's card is drawn");
            if (card == nullptr)
                continue;
            if (auto* widget = card->cardWidget())
                failures += truncatedLabels(*widget);
            save(cardImage(nodes, *card), surface);
        }
        save(nodes.grab().toImage(), QStringLiteral("node-editor"));
    }
    if (failures != 0)
        std::cout << failures << " check(s) failed\n";
    return failures == 0 ? 0 : 1;
}
