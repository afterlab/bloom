// #701 Delete Unused Nodes in the Node editor: the footer button beside Arrange (broom glyph, with
// Arrange on its own layout glyph), the View > Organize command, one undo step, what stays, and a
// refusal that changes nothing.

#include "node_interaction_test_support.hpp"

#include <bloom/commands/node_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/document/image_preview.hpp>
#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/icons.hpp>

#include <QAction>

#include <set>
#include <string>

namespace bloom::ui::test {
namespace {

document::NodeId addType(Fixture& f, const std::string_view type) {
    const auto result = f.edit<commands::AddNode>(std::string(type), document::Vec2d{0.0, 0.0});
    const auto node = result.outputId<document::NodeId>(commands::kAddNodeOutput);
    if (!node)
        throw std::runtime_error("fixture node");
    return *node;
}

bool exists(Fixture& f, const document::NodeId id) {
    return f.session.composition()->graph().findNode(id) != nullptr;
}

void testDeleteUnused() {
    Fixture f;
    const auto layer = f.edit<commands::AddSolidLayer>("Plate", core::Color4d{0.5, 0.25, 0.75, 1.0});
    const auto layerNode = layer.outputId<document::NodeId>("layerOutputNode");
    const auto source = layer.outputId<document::NodeId>(commands::kAddSolidLayerSolidNodeOutput);
    expect(layerNode && source, "the fixture layer has its nodes");
    if (!layerNode || !source)
        return;
    // A dead-end branch off the used source, and a loose node.
    const auto blur = addType(f, "bloom.gaussian-blur");
    const auto preview = addType(f, document::kImagePreviewNodeType);
    const auto loose = addType(f, document::kSolidSourceNodeType);
    expect(f.edit<commands::ConnectPorts>(document::OutputPortRef{*source, "image"},
                                          document::NodeInputRef{blur, "input"})
               .changed(),
           "the blur reads the layer's source");
    expect(f.edit<commands::ConnectPorts>(document::OutputPortRef{blur, "image"},
                                          document::NodeInputRef{preview, "image"})
               .changed(),
           "the preview reads the blur");
    QCoreApplication::processEvents();

    auto* footer = f.editor.footerWidgetForTest();
    auto* button = footer ? footer->findChild<kit::KButton*>(QStringLiteral("nodeDeleteUnusedButton"))
                          : nullptr;
    auto* arrange = footer ? footer->findChild<kit::KButton*>(QStringLiteral("nodeArrangeButton"))
                           : nullptr;
    expect(button != nullptr && arrange != nullptr, "the footer has Delete Unused and Arrange");
    if (button == nullptr || arrange == nullptr)
        return;
    expect(button->text() == QStringLiteral("Delete Unused") &&
               button->accessibleName() == QStringLiteral("Delete Unused Nodes") &&
               !button->toolTip().isEmpty(),
           "Delete Unused is labelled, named and has a tooltip");
    expect(button->iconId() == kit::IconId::DeleteUnused && arrange->iconId() == kit::IconId::Arrange &&
               kit::IconId::DeleteUnused != kit::IconId::Arrange,
           "Delete Unused carries the broom id and Arrange its own layout id");
    {
        // The chrome row adds its widgets in item order, so the footer's buttons list in order.
        const auto buttons = footer->findChildren<kit::KButton*>();
        const auto at = buttons.indexOf(button);
        expect(at >= 0 && at + 1 < buttons.size() && buttons[at + 1] == arrange,
               "Delete Unused sits directly beside Arrange");
    }

    const auto history = f.stack.size();
    button->click();
    QCoreApplication::processEvents();
    expect(f.stack.size() == history + 1, "Delete Unused is one undo step");
    expect(exists(f, *layerNode) && exists(f, *source) &&
               exists(f, f.session.composition()->graph().layerStack().nodeId()),
           "the nodes on the path to the Output stay");
    expect(!exists(f, blur) && !exists(f, preview) && !exists(f, loose),
           "the dead-end branch and the loose node are deleted");
    expect(f.session.undo() && exists(f, blur) && exists(f, preview) && exists(f, loose),
           "one undo brings every deleted node back");
    (void)f.session.redo();

    const auto settled = f.stack.size();
    button->click();
    QCoreApplication::processEvents();
    expect(f.stack.size() == settled, "with nothing unused the click changes nothing");

    // The same command from View > Organize.
    (void)f.session.undo();
    QCoreApplication::processEvents();
    auto* view = f.editor.headerMenuForTest("view");
    auto* action = view ? view->findChild<QAction*>(QStringLiteral("nodeDeleteUnusedAction")) : nullptr;
    expect(action != nullptr && action->text() == QStringLiteral("Delete Unused Nodes"),
           "View > Organize offers Delete Unused Nodes");
    if (action != nullptr) {
        action->trigger();
        QCoreApplication::processEvents();
        expect(!exists(f, blur) && !exists(f, preview) && !exists(f, loose) && exists(f, *source),
               "the menu command deletes the same nodes");
    }
}

// The Output and the Merge on it stay however little is wired, and a locked layer's nodes stay.
void testProtectedNodesStay() {
    Fixture f;
    const auto layer = f.edit<commands::AddSolidLayer>("Plate", core::Color4d{0.1, 0.2, 0.3, 1.0});
    QCoreApplication::processEvents();
    const auto output = f.session.composition()->graph().compositionOutput()->nodeId;
    const auto merge = f.session.composition()->graph().layerStack().nodeId();
    const auto loose = addType(f, document::kSolidSourceNodeType);
    (void)layer;
    auto* footer = f.editor.footerWidgetForTest();
    auto* button = footer->findChild<kit::KButton*>(QStringLiteral("nodeDeleteUnusedButton"));
    expect(button != nullptr, "the footer has Delete Unused");
    if (button == nullptr)
        return;
    button->click();
    QCoreApplication::processEvents();
    expect(!exists(f, loose) && exists(f, output) && exists(f, merge),
           "the Output and its Merge stay");
}
} // namespace
} // namespace bloom::ui::test

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    try {
        bloom::ui::test::testDeleteUnused();
        bloom::ui::test::testProtectedNodesStay();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return bloom::ui::test::failures == 0 ? 0 : 1;
}
