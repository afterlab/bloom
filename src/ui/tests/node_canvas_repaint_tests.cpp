// #721: the node canvas leaves nothing behind. An item that moves, changes size or goes away has
// to repaint everything it covered, and the view only repaints what its items declare
// (MinimalViewportUpdate), so an item that paints outside its bounding rectangle leaves pieces of
// itself on the canvas until something else repaints that area.
//
// The check is the screen itself: what the window shows after the canvas's own incremental
// updates, against what it shows after a full repaint of the same state. Any difference is a
// pixel the incremental path left stale. It runs at 50%, 100% and 200% canvas zoom, and the test
// is registered at display scales 1, 1.5 and 2.

#include "node_interaction_test_support.hpp"

#include <QBackingStore>
#include <QImage>
#include <QWindow>
#include <bloom/commands/node_operations.hpp>
#include <cmath>
#include <string>

namespace bloom::ui::test {
namespace {

void settle() {
    for (int pass = 0; pass < 4; ++pass) {
        QCoreApplication::processEvents();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
}

// What the window shows now, without asking anything to repaint: the window's own backing store,
// every device pixel of it, exactly as the incremental paints left it.
QImage shown(Fixture& f) {
    settle();
    auto* store = f.editor.window()->backingStore();
    const auto* image = store != nullptr ? dynamic_cast<QImage*>(store->paintDevice()) : nullptr;
    return image != nullptr ? image->copy() : QImage{};
}

// The number of pixels of the canvas that a full repaint changes: the stale ones.
int stalePixels(Fixture& f, const char* captureName = nullptr) {
    const QImage before = shown(f);
    f.view()->viewport()->update();
    const QImage after = shown(f);
    if (before.isNull() || before.size() != after.size())
        return -1;
    const qreal ratio = f.editor.devicePixelRatioF();
    const QPoint origin = f.view()->viewport()->mapTo(f.editor.window(), QPoint(0, 0));
    const QRect canvas(QPoint(static_cast<int>(std::ceil(origin.x() * ratio)),
                              static_cast<int>(std::ceil(origin.y() * ratio))),
                       QSize(static_cast<int>(std::floor(f.view()->viewport()->width() * ratio)),
                             static_cast<int>(std::floor(f.view()->viewport()->height() * ratio))));
    int stale = 0;
    for (int y = canvas.top(); y <= canvas.bottom() && y < before.height(); ++y)
        for (int x = canvas.left(); x <= canvas.right() && x < before.width(); ++x)
            stale += before.pixel(x, y) != after.pixel(x, y) ? 1 : 0;
    const auto directory = qEnvironmentVariable("BLOOM_NODE_REPAINT_CAPTURE_DIR");
    if (captureName != nullptr && !directory.isEmpty()) {
        (void)before.save(directory + QLatin1Char('/') + QLatin1String(captureName) +
                          QStringLiteral("-shown.png"));
        (void)after.save(directory + QLatin1Char('/') + QLatin1String(captureName) +
                         QStringLiteral("-repainted.png"));
    }
    return stale;
}

void setZoom(Fixture& f, const double zoom) {
    f.view()->setTransform(QTransform::fromScale(zoom, zoom) * QTransform::fromTranslate(40, 40));
    f.view()->viewport()->update();
    settle();
}

std::string at(const double zoom, const char* what) {
    return std::string(what) + " at " + std::to_string(static_cast<int>(zoom * 100.0)) + "%";
}

// A marquee that grows and then shrinks, and is then released: nothing of any of its rectangles
// stays on the canvas, during the drag or after it.
void testMarqueeLeavesNothingBehind() {
    for (const double zoom : {0.5, 1.0, 2.0}) {
        Fixture f;
        const auto solid = f.add(document::kSolidSourceNodeType, {200, 120});
        (void)solid;
        setZoom(f, zoom);
        const auto scene = [&](const QPoint viewport) {
            return f.view()->sceneFromViewport(QPointF(viewport));
        };
        const QPoint origin(700, 420);
        expect(stalePixels(f) == 0, at(zoom, "the canvas is current before the marquee").c_str());
        f.press(scene(origin));
        // Grow down and right, past the card, in uneven steps.
        for (const QPoint step : {QPoint(60, 40), QPoint(173, 95), QPoint(330, 211), QPoint(400, 300)})
            f.move(scene(origin + step));
        expect(stalePixels(f) == 0, at(zoom, "a growing marquee leaves no trail").c_str());
        // Shrink, cross the origin, and grow up and left over the card.
        for (const QPoint step : {QPoint(220, 130), QPoint(41, 37), QPoint(-90, -60),
                                  QPoint(-420, -260), QPoint(-610, -390), QPoint(-300, -150)})
            f.move(scene(origin + step));
        const int during = stalePixels(f, zoom == 0.5 ? "721-marquee-shrunk" : nullptr);
        expect(during == 0, (at(zoom, "a shrinking marquee leaves no trail") + " (" +
                             std::to_string(during) + " stale pixels)")
                                .c_str());
        f.release(scene(origin + QPoint(-300, -150)));
        const int after = stalePixels(f, zoom == 0.5 ? "721-marquee-released" : nullptr);
        expect(after == 0, (at(zoom, "a released marquee leaves nothing on the canvas") + " (" +
                            std::to_string(after) + " stale pixels)")
                               .c_str());
    }
}

// A node added from the Add placement with the pointer over an existing card, then undone. The
// preview is drawn over the card while it follows the pointer; the click lands the new card on
// free canvas beside it (the canvas never leaves two cards overlapping), and the undo takes it
// away. Nothing of the preview or the new card stays, and the card beneath is fully drawn.
void testUndonePlacementLeavesTheCardBeneath() {
    for (const double zoom : {0.5, 1.0, 2.0}) {
        Fixture f;
        const auto beneath = f.add(document::kCurvesNodeType, {200, 120});
        setZoom(f, zoom);
        const QRectF card = f.card(beneath)->sceneBoundingRect();
        const QPointF over = card.center() - QPointF(40, 30);
        const auto nodes = f.session.composition()->graph().nodes().size();
        f.scene()->startPlacement(std::string(document::kSolidSourceNodeType), over - QPointF(300, 0));
        // The preview follows the pointer onto the card.
        for (const QPointF step : {QPointF(-200, 10), QPointF(-90, -20), QPointF(0, 0)})
            f.move(over + step, Qt::NoButton);
        bool previewOverCard = false;
        for (const auto* item : f.scene()->items(card))
            previewOverCard = previewOverCard ||
                              item->data(kNodeItemKindRole).toString() ==
                                  QStringLiteral("placement-preview");
        expect(previewOverCard, at(zoom, "the placement preview lies over the card").c_str());
        expect(f.scene()->placementActive() &&
                   stalePixels(f, zoom == 1.0 ? "721-placement-preview" : nullptr) == 0,
               at(zoom, "the placement preview leaves no trail").c_str());
        f.click(over);
        settle();
        expect(!f.scene()->placementActive() &&
                   f.session.composition()->graph().nodes().size() == nodes + 1,
               at(zoom, "the click over the card places the node").c_str());
        expect(stalePixels(f, zoom == 0.5 ? "721-placed" : nullptr) == 0,
               at(zoom, "the placed card is drawn whole").c_str());
        expect(f.session.undo() && f.session.composition()->graph().nodes().size() == nodes,
               at(zoom, "undo takes the placed node away").c_str());
        const int stale = stalePixels(f, zoom == 1.0 ? "721-placement-undone" : nullptr);
        expect(stale == 0, (at(zoom, "after the undo the card beneath is fully drawn") + " (" +
                            std::to_string(stale) + " stale pixels)")
                               .c_str());
    }
}

} // namespace
} // namespace bloom::ui::test

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    try {
        bloom::ui::test::testMarqueeLeavesNothingBehind();
        bloom::ui::test::testUndonePlacementLeavesTheCardBeneath();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return bloom::ui::test::failures == 0 ? 0 : 1;
}
