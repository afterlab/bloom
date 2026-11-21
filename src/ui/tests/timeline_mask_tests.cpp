// ADR 0028 Timeline authoring: the Masks group rows and the Track Matte column.
#include "surface_harness.hpp"

#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/dropdown.hpp>

#include "layer_mask_support.hpp"
#include "timeline_mask_rows.hpp"
#include "timeline_property_rows.hpp"

#include <algorithm>

using namespace bloom;
using namespace bloom::ui;
using namespace bloom::ui::surface_test;

namespace {

document::PathValue square() {
    document::PathValue path;
    path.editableContour().closed = true;
    path.editableContour().anchors = {
        {{0, 0}, {}, {}}, {{10, 0}, {}, {}}, {{10, 10}, {}, {}}, {{0, 10}, {}, {}}};
    return path;
}

} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    Surfaces surfaces;
    expect(addDefaultSolidLayer(surfaces.session), "first layer");
    expect(addDefaultSolidLayer(surfaces.session), "second layer");
    QCoreApplication::processEvents();
    auto* stack = surfaces.layerStack();
    expect(stack != nullptr && stack->rowCount() == 2, "two layer rows");
    if (stack == nullptr || stack->rowCount() != 2)
        return 1;
    const auto top = stack->entries()[0].layerId;
    const auto bottom = stack->entries()[1].layerId;

    // Masks group rows.
    expect(surfaces.session.addLayerMask(top, square()), "mask added");
    expect(surfaces.session.addLayerMask(top, square(), document::LayerMaskMode::Subtract),
           "second mask added");
    const auto* layer = surfaces.session.composition()->graph().findLayer(top);
    const auto firstMask = layer->masks.at(0);
    auto rows = timelinePropertyEntries(surfaces.session, {stack->entries()[0]}, {top});
    const auto group = std::ranges::find_if(rows, [](const auto& e) {
        return e.rowKind == TimelineLayerEntry::Kind::Group && e.group == "Masks";
    });
    expect(group != rows.end() && group->expanded, "an expanded layer lists a Masks group");
    const auto pathRows =
        std::ranges::count_if(rows, [](const auto& e) { return e.role == kMaskPathRowRole; });
    expect(pathRows == 2, "one path row per mask");
    const auto named = std::ranges::find_if(rows, [&](const auto& e) {
        return e.maskId == firstMask.id && e.role == kMaskPathRowRole;
    });
    expect(named != rows.end() && named->name == "Mask 1 · Add" &&
               named->parameterId == firstMask.path && named->depth == 2,
           "the mask row is labelled with name and mode and keys the path parameter");
    const auto scalarRows = std::ranges::count_if(rows, [&](const auto& e) {
        return e.maskId == firstMask.id && e.depth == 3 &&
               (e.parameterId == firstMask.feather || e.parameterId == firstMask.expansion ||
                e.parameterId == firstMask.opacity);
    });
    expect(scalarRows == 3, "Feather, Expansion and Opacity rows under each mask");
    const auto bottomRows =
        timelinePropertyEntries(surfaces.session, {stack->entries()[1]}, {bottom});
    expect(std::ranges::none_of(bottomRows, [](const auto& e) { return e.group == "Masks"; }),
           "a layer without masks has no Masks group");
    const auto collapsed = timelinePropertyEntries(surfaces.session, {stack->entries()[0]}, {top},
                                                   {{top, QStringLiteral("Masks")}});
    expect(
        std::ranges::none_of(collapsed, [](const auto& e) { return e.role == kMaskPathRowRole; }),
        "a collapsed Masks group hides its rows");

    // The path diamond keys the mask path through the standard keyframe gesture.
    expect(surfaces.session.toggleMaskPathKey(firstMask.path), "animate the path");
    layer = surfaces.session.composition()->graph().findLayer(top);
    expect(surfaces.session.keyframeDiamondStateForParameter(firstMask.path) ==
               KeyframeDiamondState::AnimatedWithKey,
           "the path is animated with a key at the playhead");
    expect(surfaces.session.undo(), "undo the path key");

    // Track Matte column.
    auto* matte = [&]() -> kit::KDropdown* {
        auto* row = surfaces.layerRow(0);
        return row ? row->findChild<kit::KDropdown*>(QStringLiteral("layerTrackMatteDropdown"))
                   : nullptr;
    }();
    expect(matte != nullptr, "the layer row carries a Track Matte cell");
    if (matte == nullptr)
        return failures == 0 ? 0 : 1;
    // #744: a bound row shows only the current matte; the list fills when the cell opens.
    expect(matte->count() == 1 && matte->currentText() == QStringLiteral("None"),
           "the closed cell holds only the current matte");
    click(matte);
    expect(matte->isPopupVisible(), "clicking the cell opens its menu");
    expect(matte->count() == 5 && matte->currentIndex() == 0, "None, one layer, three actions");
    // A re-bind while the list is open keeps every choice in it. Any snapshot publication rebuilds
    // the rows (an edit landing, an agent's commit, the session's color converter finishing on its
    // worker); here an edit of the other layer forces one while the artist has the list open.
    const auto bindsBefore = stack->work().layerRowBinds;
    expect(surfaces.session.setLayerBlendMode(bottom, core::BlendMode::Multiply),
           "the other layer is edited while the list is open");
    expect(stack->work().layerRowBinds > bindsBefore, "the edit re-binds the open row");
    expect(matte->isPopupVisible(), "the open list stays open through the re-bind");
    expect(matte->count() == 5 && matte->currentIndex() == 0,
           "the re-bound open list still holds None, one layer and three actions");
    pickPopupRow(*matte, 1);
    expect(surfaces.session.composition()->graph().findLayer(top)->trackMatte &&
               surfaces.session.composition()->graph().findLayer(top)->trackMatte->source == bottom,
           "picking a layer dispatches SetTrackMatte");
    expect(surfaces.session.undoLabel() == QStringLiteral("Set Track Matte"),
           "through the typed command");
    matte =
        surfaces.layerRow(0)->findChild<kit::KDropdown*>(QStringLiteral("layerTrackMatteDropdown"));
    click(matte);
    pickPopupRow(*matte, 3); // Luma
    expect(surfaces.session.composition()->graph().findLayer(top)->trackMatte->channel ==
               document::TrackMatteChannel::Luma,
           "the Luma action edits the channel");
    matte =
        surfaces.layerRow(0)->findChild<kit::KDropdown*>(QStringLiteral("layerTrackMatteDropdown"));
    click(matte);
    pickPopupRow(*matte, 4); // Inverted
    expect(surfaces.session.composition()->graph().findLayer(top)->trackMatte->inverted,
           "the Inverted action toggles the bit");
    matte =
        surfaces.layerRow(0)->findChild<kit::KDropdown*>(QStringLiteral("layerTrackMatteDropdown"));
    click(matte);
    pickPopupRow(*matte, 0);
    expect(!surfaces.session.composition()->graph().findLayer(top)->trackMatte,
           "None clears the matte");
    // A re-bind that points the open row at another layer closes the list it opened for the old
    // one, so no choice ever acts on a layer the artist did not open it for.
    matte =
        surfaces.layerRow(0)->findChild<kit::KDropdown*>(QStringLiteral("layerTrackMatteDropdown"));
    click(matte);
    expect(matte->isPopupVisible() && matte->count() == 5, "the list is open on the top row");
    expect(addDefaultSolidLayer(surfaces.session), "a layer is added above while the list is open");
    expect(stack->entries()[0].layerId != top, "the new layer takes the top row");
    expect(!matte->isPopupVisible(), "the re-pointed row's list closes");
    expect(matte->count() == 1 && matte->currentText() == QStringLiteral("None"),
           "the re-pointed row shows its own layer's current matte");
    return failures == 0 ? 0 : 1;
}
