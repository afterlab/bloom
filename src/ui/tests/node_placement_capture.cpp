// Review captures of the Node editor's framing and Add Node placement (#694, #703). A tool, not a
// test:
//   bloom_node_placement_capture <directory> <1|1.5> <prefix> [new-composition|add-node]
// `new-composition` adds a composition to the window fixture and switches the editor to it, so the
// capture is what a new composition opens at; `add-node` chooses Gaussian Blur from the header Add
// menu with the pointer over empty canvas and over a link.
#include "window_fixture.hpp"

#include "node_editor_items.hpp"

#include <QAction>
#include <QDir>
#include <QImage>
#include <QMenu>
#include <bloom/commands/operations.hpp>
#include <bloom/document/composition_settings.hpp>
#include <bloom/ui/editor_area.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/node_editor.hpp>

#include <iostream>
#include <string>

namespace {
using namespace bloom;
using namespace bloom::ui;

int gFailures = 0;
QString gDirectory;
QString gRatio;
QString gPrefix;

void settle(const int ms = 80) {
    QApplication::processEvents();
    QTest::qWait(ms);
}

void save(const QImage& image, const QString& name) {
    const auto path = gDirectory + QLatin1Char('/') + gPrefix + QLatin1Char('-') + name +
                      QLatin1Char('-') + gRatio + QStringLiteral(".png");
    if (image.isNull() || !image.save(path)) {
        std::cerr << "could not write " << path.toStdString() << '\n';
        ++gFailures;
        return;
    }
    std::cout << path.toStdString() << ' ' << image.width() << 'x' << image.height() << '\n';
}

EditorArea* nodesArea(MainWindow& window) {
    for (auto* area : window.findChildren<EditorArea*>())
        if (area->editorId() == "bloom.nodes")
            return area;
    return nullptr;
}

void sizeArea(MainWindow& window, EditorArea& area, const QSize target) {
    for (int pass = 0; pass < 4; ++pass) {
        QApplication::processEvents();
        const QSize delta = target - area.size();
        if (delta.isNull())
            return;
        window.resize(window.size() + delta);
        QTest::qWait(30);
    }
}

QAction* findAction(QMenu& menu, const QString& text) {
    for (auto* action : menu.actions()) {
        if (action->menu() != nullptr)
            if (auto* found = findAction(*action->menu(), text))
                return found;
        if (action->text() == text)
            return action;
    }
    return nullptr;
}

// The point on screen where the link into the Layer card of the first layer is drawn.
std::optional<QPointF> linkMidpoint(NodeGraphEditor& editor) {
    for (auto* item : editor.graphScene()->items()) {
        const auto* edge = dynamic_cast<node_editor::NodeEdgeItem*>(item);
        if (edge == nullptr || edge->structural || !edge->isVisible())
            continue;
        const auto* destination = std::get_if<document::NodeInputRef>(&edge->edge.destination);
        if (destination == nullptr || destination->port != "image")
            continue;
        return editor.graphView()->mapFromScene(edge->path().pointAtPercent(0.5));
    }
    return std::nullopt;
}

int capture(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setOrganizationName("BloomGrammarTests");
    app.setApplicationName("NodePlacementCapture");
    kit::installAfterlabTheme(app);
    gDirectory = QString::fromLocal8Bit(argv[1]);
    gRatio = QString::fromLatin1(argv[2]) == QStringLiteral("1") ? QStringLiteral("dpr1")
                                                                 : QStringLiteral("dpr15");
    gPrefix = QString::fromLocal8Bit(argv[3]);
    const QString part = argc > 4 ? QString::fromLatin1(argv[4]) : QString();
    QDir().mkpath(gDirectory);
    test::WindowFixture fixture;
    auto& session = fixture.session;
    auto* area = nodesArea(*fixture.window);
    if (area == nullptr) {
        std::cerr << "no Nodes area\n";
        return 1;
    }
    Q_EMIT area->maximizeRequested(area);
    settle();
    sizeArea(*fixture.window, *area, QSize(1400, 820));
    auto* editor = area->findChild<NodeGraphEditor*>();
    if (editor == nullptr)
        return 1;
    fixture.clearInteraction();
    session.clearSelection();
    settle();

    if (part.isEmpty() || part == QStringLiteral("new-composition")) {
        commands::Transaction add("New Composition", session.snapshot().revision());
        add.emplace<commands::AddComposition>("Composition 2", document::CompositionFormat{},
                                              core::RationalTime::fromInteger(10));
        const auto result = session.executeTransaction(std::move(add));
        const auto id = result.outputId<document::CompositionId>(commands::kAddCompositionOutput);
        if (!id || !session.setComposition(*id)) {
            std::cerr << "new composition refused\n";
            return 1;
        }
        settle(200);
        std::cout << "zoom " << editor->graphView()->zoomFactor() << '\n';
        save(area->grab().toImage(), QStringLiteral("new-composition"));
    }
    if (part.isEmpty() || part == QStringLiteral("add-node")) {
        auto* view = editor->graphView();
        // Events straight to the viewport: QTest's synthesized pointer is not delivered under a
        // fractional QT_SCALE_FACTOR offscreen.
        const auto send = [view](const QEvent::Type type, const QPoint at,
                                 const Qt::MouseButton button, const Qt::MouseButtons buttons) {
            QMouseEvent event(type, at, view->viewport()->mapToGlobal(at), button, buttons,
                              Qt::NoModifier);
            QCoreApplication::sendEvent(view->viewport(), &event);
        };
        const auto pointTo = [&](const QPoint at) {
            send(QEvent::MouseMove, at, Qt::NoButton, Qt::NoButton);
        };
        auto* menu = editor->headerMenuForTest("add");
        auto* blur = menu != nullptr ? findAction(*menu, QStringLiteral("Gaussian Blur")) : nullptr;
        if (blur == nullptr) {
            std::cerr << "no Gaussian Blur in the Add menu\n";
            return 1;
        }
        // Over empty canvas, left of the graph.
        const QPoint empty(view->viewport()->width() / 6, view->viewport()->height() / 6);
        pointTo(empty);
        settle(40);
        blur->trigger();
        settle(40);
        pointTo(empty + QPoint(12, 8));
        settle(80);
        save(area->grab().toImage(), QStringLiteral("add-node-empty"));
        // Over a link.
        if (const auto link = linkMidpoint(*editor)) {
            pointTo(link->toPoint());
            settle(80);
            save(area->grab().toImage(), QStringLiteral("add-node-over-link"));
            send(QEvent::MouseButtonPress, link->toPoint(), Qt::LeftButton, Qt::LeftButton);
            send(QEvent::MouseButtonRelease, link->toPoint(), Qt::LeftButton, Qt::NoButton);
            settle(120);
            // The new card keeps clear of the cards under the click, which can take it out of the
            // view; Fit shows the whole result.
            view->frameGraph();
            settle(120);
            save(area->grab().toImage(), QStringLiteral("add-node-inserted"));
        }
    }
    return gFailures == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::cerr << "Usage: bloom_node_placement_capture <directory> <1|1.5> <prefix> "
                     "[new-composition|add-node]\n";
        return 2;
    }
    qputenv("QT_SCALE_FACTOR", argv[2]);
    try {
        return capture(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
