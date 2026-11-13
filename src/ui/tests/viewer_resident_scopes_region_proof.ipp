// #724 Scopes: the scope region overlay on the genuine GPU-resident Viewer route. On the device
// presentation the overlay is not painted by the widget: the Viewer records it as a picture that
// the presenter composites over the device-presented image. With a visible Scopes panel and a
// region, that recorded overlay must draw the region (its rectangle and handles) and nothing
// outside it; with no region, or with no visible panel, it must equal the plain overlay. Included
// into viewer_resident_interaction_native_tests.cpp.

#include <bloom/ui/scopes_editor.hpp>
#include <bloom/ui/scopes_region_store.hpp>

namespace {

void pumpEvents() {
    for (int turn = 0; turn < 20; ++turn)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
}

// The overlay the Viewer would hand the presenter, drawn into an image of the container's size.
[[nodiscard]] QImage residentOverlayImage(ui::ViewerEditor& viewer, QRectF& container) {
    const auto request = viewer.buildResidentPresentRequestForTest();
    container = request.containerRect;
    if (!request.overlay.valid)
        return {};
    QImage image(request.overlay.logicalSize.toSize(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.drawPicture(QPointF(0.0, 0.0), request.overlay.picture);
    painter.end();
    return image;
}

void exerciseResidentScopesRegion(Expectations& checks, ui::CompositionSession& session,
                                  ui::ViewerEditor& viewer) {
    using Region = runtime::ScopeRegion;
    checks.expect(viewer.residentPresentationActiveForTest(),
                  "scopes region: the Viewer shows the device-presented picture");
    ui::ScopesRegionStore store;
    viewer.setScopesRegionStore(&store);
    ui::ScopesServices services;
    services.regions = &store;
    ui::ScopesEditor panel(services);
    panel.show();
    panel.followViewer(&viewer);
    pumpEvents();
    checks.expect(store.active(), "scopes region: the visible Scopes panel makes the store active");
    const ui::ScopesRegionStore::Key key{session.snapshot().project().id(),
                                         session.compositionId()};

    QRectF container;
    const auto plain = residentOverlayImage(viewer, container);
    checks.expect(!plain.isNull(), "scopes region: the resident request records an overlay");
    if (plain.isNull())
        return;

    constexpr Region kRegion{200, 150, 640, 360};
    store.setRegion(key, kRegion);
    pumpEvents();
    QRectF withContainer;
    const auto drawn = residentOverlayImage(viewer, withContainer);
    checks.expect(!drawn.isNull() && drawn.size() == plain.size(),
                  "scopes region: the overlay with a region has the same extent");
    if (drawn.isNull() || drawn.size() != plain.size())
        return;

    // Where the region sits in the container: its corners through the Viewer's own mapping, less
    // the container's origin, with room for the handles and the label above.
    const auto mapping = viewer.mappingForTest();
    checks.expect(mapping.has_value(), "scopes region: the Viewer has a mapping");
    if (!mapping)
        return;
    const QPointF topLeft = mapping->toScreen({static_cast<double>(kRegion.x),
                                               static_cast<double>(kRegion.y)}) -
                            container.topLeft();
    const QPointF bottomRight =
        mapping->toScreen({static_cast<double>(kRegion.x + static_cast<std::int32_t>(kRegion.width)),
                           static_cast<double>(kRegion.y + static_cast<std::int32_t>(kRegion.height))}) -
        container.topLeft();
    const QRectF bounds = QRectF(topLeft, bottomRight).normalized().adjusted(-40, -48, 40, 40);
    std::size_t changed = 0;
    std::size_t outside = 0;
    for (int y = 0; y < drawn.height(); ++y)
        for (int x = 0; x < drawn.width(); ++x)
            if (drawn.pixel(x, y) != plain.pixel(x, y)) {
                ++changed;
                if (!bounds.contains(QPointF(x, y)))
                    ++outside;
            }
    checks.expect(changed > 200,
                  "scopes region: the region is drawn over the device-presented picture (" +
                      std::to_string(changed) + " pixels differ)");
    checks.expect(outside == 0, "scopes region: nothing is drawn outside the region and its "
                                "label (" +
                                    std::to_string(outside) + " pixels do)");

    // No region: the plain overlay again.
    store.clearRegion(key);
    pumpEvents();
    QRectF cleared;
    checks.expect(residentOverlayImage(viewer, cleared) == plain,
                  "scopes region: clearing the region restores the plain overlay");

    // A region with no visible Scopes panel draws nothing.
    store.setRegion(key, kRegion);
    panel.hide();
    pumpEvents();
    checks.expect(!store.active(), "scopes region: hiding the panel makes the store inactive");
    QRectF hidden;
    checks.expect(residentOverlayImage(viewer, hidden) == plain,
                  "scopes region: with no visible panel the overlay is the plain one");
    viewer.setScopesRegionStore(nullptr);
}

} // namespace
