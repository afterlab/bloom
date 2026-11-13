// #698: marquee selection of layers in the Viewer. Included by viewer_editor_tests.cpp after
// context_bar_tests.ipp (it uses that file's tool fixture helpers).

// A drag on empty Viewer space selects the layers it touches; Shift adds; the toggle modifier
// toggles; a press released in place clears; Escape cancels. The selection is the session's, the
// one the Timeline's layer rows and the node cards show. The border is one hairline at any zoom.
void testViewerMarqueeSelection(Expectations& expectations) {
    using namespace bloom;
    ViewerFixture fixture(toolProject());
    fixture.viewer.resize(900, 700);
    fixture.viewer.show();
    toolReady(fixture, expectations);
    // Three small rectangles, far apart, on an otherwise empty frame.
    std::vector<document::LayerId> layers;
    for (const auto& [x, y] : std::array{std::pair{60.0, 60.0}, std::pair{200.0, 60.0},
                                         std::pair{60.0, 220.0}}) {
        commands::ShapeLayerGeometry geometry;
        geometry.position = document::Vec2d{x, y};
        geometry.size = document::Vec2d{40.0, 30.0};
        const auto before = fixture.session.composition()->graph().layerOutputs().size();
        (void)fixture.session.addShapeLayer(document::ShapeKind::Rectangle, geometry);
        const auto& outputs = fixture.session.composition()->graph().layerOutputs();
        if (outputs.size() == before + 1)
            layers.push_back(outputs.back().layerId);
    }
    expectations.expect(layers.size() == 3, "viewer marquee: three shape layers exist");
    if (layers.size() != 3)
        return;
    fixture.session.clearSelection();
    toolReady(fixture, expectations);

    const auto selected = [&] {
        std::set<document::LayerId> result;
        for (const auto node : fixture.session.selectedNodes())
            if (const auto layer = fixture.session.layerForNode(node))
                result.insert(*layer);
        return result;
    };
    const auto set = [&](std::initializer_list<int> indices) {
        std::set<document::LayerId> result;
        for (const int index : indices)
            result.insert(layers[static_cast<std::size_t>(index)]);
        return result;
    };
    // The Viewer answers a press once it has painted the current selection's overlay.
    const auto ready = [&] {
        toolReady(fixture, expectations);
        expectations.expect(
            awaitQtCondition("the viewer has painted the selection overlay mapping",
                             [&] { return fixture.viewer.mappingForTest().has_value(); }),
            "the selection overlay mapping is painted before the press");
    };
    // A marquee in composition coordinates. Every drag starts well clear of the selection's box
    // and handles, on empty space.
    // A marquee in composition coordinates, dragged through the Viewer's own mouse handling.
    const auto drag = [&](const document::Vec2d from, const document::Vec2d to,
                          const Qt::KeyboardModifiers modifiers) {
        ready();
        const auto mapping = fixture.viewer.mappingForTest();
        if (!mapping) {
            expectations.expect(false, "viewer marquee: the Viewer has a mapping");
            return;
        }
        const QPoint a = mapping->toScreen(from).toPoint();
        const QPoint b = mapping->toScreen(to).toPoint();
        QTest::mousePress(&fixture.viewer, Qt::LeftButton, modifiers, a);
        QTest::mouseMove(&fixture.viewer, (a + b) / 2);
        QTest::mouseMove(&fixture.viewer, b);
        QTest::mouseRelease(&fixture.viewer, Qt::LeftButton, modifiers, b);
        QCoreApplication::processEvents();
    };
    int changes = 0;
    const auto counter = QObject::connect(&fixture.session, &ui::CompositionSession::selectionChanged,
                                          [&changes] { ++changes; });

    // Select: the two rectangles on the top row.
    drag({380.0, 10.0}, {20.0, 110.0}, Qt::NoModifier);
    expectations.expect(selected() == set({0, 1}) && changes == 1,
                        "viewer marquee: a drag on empty space selects the layers it touches, as "
                        "one selection change");
    // Touching is enough: a marquee over one corner of the third rectangle.
    drag({10.0, 290.0}, {45.0, 230.0}, Qt::NoModifier);
    expectations.expect(selected() == set({2}),
                        "viewer marquee: a marquee that only touches a layer selects it, and "
                        "replaces the selection");
    // Shift adds.
    drag({380.0, 10.0}, {170.0, 110.0}, Qt::ShiftModifier);
    expectations.expect(selected() == set({1, 2}), "viewer marquee: Shift adds to the selection");
    // The toggle modifier toggles: over the top row, 1 leaves and 0 joins.
    drag({380.0, 10.0}, {20.0, 110.0}, Qt::ControlModifier);
    {
        std::string got;
        for (std::size_t index = 0; index < layers.size(); ++index)
            if (selected().contains(layers[index]))
                got += std::to_string(index) + " ";
        expectations.expect(selected() == set({0, 2}),
                            "viewer marquee: the toggle modifier toggles the touched layers "
                            "(selected: " + got + ")");
    }
    // The same selection everywhere: the session's nodes are these layers' nodes.
    std::set<document::NodeId> nodes;
    for (const auto& boundary : fixture.session.composition()->graph().layerOutputs())
        if (set({0, 2}).contains(boundary.layerId))
            nodes.insert(boundary.nodeId);
    expectations.expect(fixture.session.selectedNodes() == nodes &&
                            fixture.session.selection().contextualLayer.has_value(),
                        "viewer marquee: it is the session's selection, with a primary layer");
    // A marquee over nothing with Shift keeps the selection; plain, it clears it.
    drag({300.0, 180.0}, {340.0, 220.0}, Qt::ShiftModifier);
    expectations.expect(selected() == set({0, 2}),
                        "viewer marquee: an empty Shift marquee leaves the selection alone");
    drag({300.0, 180.0}, {340.0, 220.0}, Qt::NoModifier);
    expectations.expect(selected().empty(), "viewer marquee: an empty marquee clears the selection");
    // A press released in place is still a click on empty space.
    fixture.session.selectLayer(layers[0]);
    ready();
    {
        const auto mapping = fixture.viewer.mappingForTest();
        const QPoint at = mapping ? mapping->toScreen({320.0, 200.0}).toPoint() : QPoint();
        QTest::mouseClick(&fixture.viewer, Qt::LeftButton, Qt::NoModifier, at);
        QCoreApplication::processEvents();
    }
    expectations.expect(selected().empty(),
                        "viewer marquee: a click on empty space clears the selection");
    // Escape cancels a marquee in progress.
    ready();
    {
        const auto mapping = fixture.viewer.mappingForTest();
        const QPoint a = mapping ? mapping->toScreen({380.0, 10.0}).toPoint() : QPoint();
        const QPoint b = mapping ? mapping->toScreen({20.0, 110.0}).toPoint() : QPoint();
        QTest::mousePress(&fixture.viewer, Qt::LeftButton, Qt::NoModifier, a);
        QTest::mouseMove(&fixture.viewer, b);
        QTest::keyClick(&fixture.viewer, Qt::Key_Escape);
        QTest::mouseRelease(&fixture.viewer, Qt::LeftButton, Qt::NoModifier, b);
        QCoreApplication::processEvents();
    }
    expectations.expect(selected().empty(), "viewer marquee: Escape cancels the marquee");
    QObject::disconnect(counter);

    // The border: the same width in widget pixels at two zooms. The run of border-colored pixels
    // on a row through the marquee's left edge is counted on a grab taken mid-drag.
    const auto borderRun = [&](const double zoom) {
        auto* dropdown = fixture.viewer.zoomDropdownForTest();
        if (dropdown != nullptr)
            for (int index = 0; index < dropdown->count(); ++index)
                if (dropdown->itemText(index) == QStringLiteral("%1%").arg(qRound(zoom * 100)))
                    dropdown->chooseIndex(index);
        ready();
        const QRectF content = fixture.viewer.contentRectForTest();
        const QPoint a = (content.topLeft() + QPointF(24, 24)).toPoint();
        const QPoint b = (content.topLeft() + QPointF(124, 104)).toPoint();
        // Only where the press lands on empty space does a marquee start.
        QTest::mousePress(&fixture.viewer, Qt::LeftButton, Qt::NoModifier, a);
        QTest::mouseMove(&fixture.viewer, (a + b) / 2);
        QTest::mouseMove(&fixture.viewer, b);
        QCoreApplication::processEvents();
        const auto image = fixture.viewer.grab().toImage();
        const auto capture = qEnvironmentVariable("BLOOM_CONTEXT_BAR_CAPTURE_DIR");
        if (!capture.isEmpty())
            (void)image.save(capture + QStringLiteral("/698-viewer-marquee-%1.png").arg(qRound(zoom * 100)));
        QTest::keyClick(&fixture.viewer, Qt::Key_Escape);
        QTest::mouseRelease(&fixture.viewer, Qt::LeftButton, Qt::NoModifier, b);
        QCoreApplication::processEvents();
        const qreal ratio = image.devicePixelRatio();
        const QColor accent = ui::kit::color(ui::kit::Color::Accent);
        const int row = qRound((a.y() + b.y()) / 2.0 * ratio);
        int run = 0;
        for (int x = qRound((a.x() - 6) * ratio); x < qRound((a.x() + 6) * ratio); ++x) {
            if (x < 0 || x >= image.width() || row < 0 || row >= image.height())
                continue;
            const QColor pixel = image.pixelColor(x, row);
            if (std::abs(pixel.red() - accent.red()) + std::abs(pixel.green() - accent.green()) +
                    std::abs(pixel.blue() - accent.blue()) <
                24)
                ++run;
        }
        return run;
    };
    const int atHalf = borderRun(0.5);
    const int atDouble = borderRun(2.0);
    expectations.expect(atHalf >= 1 && atHalf <= 2 && atDouble == atHalf,
                        "viewer marquee: the border is the same hairline at 50% and 200% (" +
                            std::to_string(atHalf) + " and " + std::to_string(atDouble) +
                            " pixels)");
    toolShutdown(fixture, expectations);
}
