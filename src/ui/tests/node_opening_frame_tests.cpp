// #694: a new composition opens in the node editor at 100%, centered on its starting nodes, and
// those nodes keep Arrange's spacing. The canvas never zooms in past 100% by itself; a graph too big
// for the panel opens zoomed out just enough to fit; an explicit Fit is followed until the artist
// moves the view; another composition goes back to the opening frame.
#include "node_production_harness.hpp"

#include <bloom/commands/operations.hpp>
#include <bloom/document/composition_settings.hpp>
#include <bloom/document/node_layout.hpp>

using namespace bloom;
using namespace bloom::ui;
using namespace bloom::ui::production_test;

namespace {
[[nodiscard]] std::optional<document::CompositionId> addComposition(App& a, const char* name) {
    commands::Transaction add("New Composition", a.session.snapshot().revision());
    add.emplace<commands::AddComposition>(name, document::CompositionFormat{},
                                          core::RationalTime::fromInteger(10));
    const auto result = a.session.executeTransaction(std::move(add));
    return result.outputId<document::CompositionId>(commands::kAddCompositionOutput);
}

[[nodiscard]] QRectF sceneRect(node_editor::NodeItem& card) {
    return card.mapRectToScene(card.cardRect());
}

// Zoom factors compare exactly enough to tell 100% from any other step.
[[nodiscard]] bool near(const double left, const double right) {
    return std::abs(left - right) < 1e-6;
}
} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    try {
        App a;
        auto* view = a.editor.graphView();
        // The artist moved the view of the first composition.
        view->zoomStep(3);
        expect(view->viewAdjusted(), "a zoom step is the artist taking the view over");

        const auto created = addComposition(a, "Second");
        expect(created.has_value() && a.session.setComposition(*created),
               "a new composition is created and shown");
        QCoreApplication::processEvents();
        expect(near(view->zoomFactor(), 1.0), "the new composition opens at 100%");
        expect(!view->viewAdjusted(), "the opening frame is the canvas's own, not the artist's");
        const auto merge = a.session.composition()->graph().outputMergeId();
        const auto output = a.session.composition()->graph().compositionOutput();
        auto* mergeCard = merge ? a.card(*merge) : nullptr;
        auto* outputCard = output ? a.card(output->nodeId) : nullptr;
        expect(mergeCard != nullptr && outputCard != nullptr,
               "the new composition shows its Merge and Output");
        if (mergeCard == nullptr || outputCard == nullptr)
            return 1;
        // Centered on everything the canvas draws (the cards with their sockets and the link), so
        // the two cards' own center is off by no more than a socket's overhang.
        const QPointF viewportCenter = QRectF(view->viewport()->rect()).center();
        const QPointF drawn =
            view->mapFromScene(a.editor.graphScene()->itemsBoundingRect().center());
        expect(std::abs(drawn.x() - viewportCenter.x()) <= 1.0 &&
                   std::abs(drawn.y() - viewportCenter.y()) <= 1.0,
               "the view is centered on the starting nodes");
        const QRectF both = sceneRect(*mergeCard).united(sceneRect(*outputCard));
        const QPointF cards = view->mapFromScene(both.center());
        expect(std::abs(cards.x() - viewportCenter.x()) <= document::kNodeLayoutGrid &&
                   std::abs(cards.y() - viewportCenter.y()) <= document::kNodeLayoutGrid,
               "the two cards sit in the middle of the panel");
        const double gap = sceneRect(*outputCard).left() - sceneRect(*mergeCard).right();
        expect(gap >= document::kNodeLayoutSpacing,
               "the starting nodes keep at least Arrange's gap between them");
        expect(!sceneRect(*mergeCard).intersects(sceneRect(*outputCard)),
               "the starting nodes never overlap");
        const double rowOffset =
            std::abs(sceneRect(*mergeCard).center().y() - sceneRect(*outputCard).center().y());
        expect(rowOffset <= document::kNodeLayoutGrid,
               "the starting nodes share a row, centered as Arrange centers them");

        // A resize keeps the opening frame while the artist has not moved the view.
        a.editor.resize(900, 600);
        QCoreApplication::processEvents();
        expect(near(view->zoomFactor(), 1.0), "a resize keeps the opening 100% frame");

        // A graph bigger than the panel opens zoomed out just enough to fit, never zoomed in.
        for (int index = 0; index < 6; ++index)
            static_cast<void>(a.addByType("bloom.gaussian-blur",
                                          {-3000.0 + 900.0 * index, -1500.0 + 500.0 * index}));
        view->reframe();
        QCoreApplication::processEvents();
        const QRectF bounds = a.editor.graphScene()->itemsBoundingRect();
        const QRectF visible = view->mapToScene(view->viewport()->rect()).boundingRect();
        expect(view->zoomFactor() < 1.0 && visible.adjusted(-1, -1, 1, 1).contains(bounds),
               "a graph larger than the panel opens zoomed out to fit");

        // An explicit Fit is followed instead, until the artist moves the view.
        view->frameGraph();
        const double fitted = view->zoomFactor();
        a.editor.resize(1400, 900);
        QCoreApplication::processEvents();
        expect(!near(view->zoomFactor(), fitted) && !view->viewAdjusted(),
               "after Fit a resize fits the graph again");

        // Back to a composition: the opening frame again.
        view->zoomStep(-2);
        const auto third = addComposition(a, "Third");
        expect(third.has_value() && a.session.setComposition(*third),
               "a third composition is shown");
        QCoreApplication::processEvents();
        expect(near(view->zoomFactor(), 1.0) && !view->viewAdjusted(),
               "switching composition resets the view to the opening 100% frame");
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    if (failures != 0) {
        std::cerr << failures << " opening frame check(s) failed\n";
        return 1;
    }
    std::cout << "PASS: node opening frame\n";
    return 0;
}
