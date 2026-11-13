// Review captures for drilling into grouped SVG artwork (#645): the whole window and the Viewer at
// the top level, two groups in (the outside dimmed, the breadcrumb in the Viewer header), point
// editing on a shape whose path is keyed, and the Break Apart offer a one-layer SVG makes. Writes
// PNGs to the directory given as the first argument; the scale factor is the environment's
// QT_SCALE_FACTOR. Not a ctest.
#include "await_qt.hpp"
#include "window_fixture.hpp"

#include <QDir>
#include <QMouseEvent>
#include <bloom/commands/svg_import_operations.hpp>
#include <bloom/document/drill_levels.hpp>
#include <bloom/ui/kit/dialog.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/viewer_editor.hpp>
#include <bloom/ui/viewer_overlays.hpp>

#include <algorithm>
#include <cstring>
#include <iostream>

namespace {

using namespace bloom;

constexpr auto kArtwork =
    R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="400" height="300" viewBox="0 0 400 300">
<defs><linearGradient id="fade"><stop offset="0" stop-color="#ffcc33"/><stop offset="1" stop-color="#ee3377"/></linearGradient></defs>
<rect id="backdrop" x="0" y="0" width="400" height="300" fill="#1d2433"/>
<g id="outer">
<rect id="badge" x="24" y="196" width="120" height="76" rx="12" fill="url(#fade)"/>
<g id="middle">
<circle id="dot" cx="330" cy="70" r="40" fill="#55aa77"/>
<g id="inner">
<path id="ring" fill="#3377dd" fill-rule="evenodd" d="M110 50H250V190H110Z M145 85H215V155H145Z"/>
<path id="tri" fill="#ffffff" d="M200 150L300 150L300 250Z"/>
</g>
</g>
</g>
</svg>
)svg";

constexpr auto kOneLayer =
    R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="400" height="300" viewBox="0 0 400 300">
<rect id="box" x="40" y="40" width="140" height="120" rx="10" fill="#ee5533"/>
<circle id="disc" cx="280" cy="170" r="90" fill="#3377dd"/>
</svg>
)svg";

std::shared_ptr<const commands::PreparedSvgSource> prepare(const char* text) {
    const auto size = std::strlen(text);
    auto bytes = std::make_shared<std::vector<std::byte>>(size);
    std::memcpy(bytes->data(), text, size);
    auto read = commands::prepareSvgSource(std::move(bytes), "artwork.svg");
    if (!read.source)
        throw std::runtime_error("the SVG could not be prepared: " + read.diagnostic);
    return read.source;
}

document::LayerId import(ui::CompositionSession& session, const char* text, const std::string& name,
                         const commands::SvgImportMode mode) {
    commands::Transaction transaction("Import " + name, session.snapshot().revision());
    transaction.emplace<commands::AddSvgArtwork>(session.compositionId(), prepare(text), name, mode,
                                                 true);
    const auto result = session.executeTransaction(std::move(transaction));
    const auto layer = result.outputId<document::LayerId>("layer");
    if (!result.succeeded() || !layer)
        throw std::runtime_error("the SVG import failed");
    return *layer;
}

document::LayerId child(const document::Composition& composition, const document::LayerId parent,
                        const std::string_view name) {
    const auto merge = parent.isValid() ? document::layerContentMerge(composition, parent)
                                        : composition.graph().outputMergeId();
    if (merge)
        for (const auto layer : document::mergeEntryLayers(composition, *merge))
            if (const auto* boundary = composition.graph().findLayer(layer);
                boundary != nullptr && boundary->name == name)
                return layer;
    throw std::runtime_error("no layer named " + std::string(name));
}

// Keys `shape`'s Path node: at 0 s as drawn, at 2 s moved and widened, so the playhead between the
// keys shows a keyed path being edited.
void keyShape(ui::CompositionSession& session, const document::LayerId shape) {
    const auto& composition = *session.composition();
    const auto info = document::drillShape(composition, shape);
    const auto* node =
        info && info->pathNode ? composition.graph().findNode(*info->pathNode) : nullptr;
    if (node == nullptr)
        throw std::runtime_error("the shape has no Path node");
    const auto found = std::ranges::find(node->parameters, document::kValueParameterRole,
                                         &document::ParameterBinding::role);
    if (found == node->parameters.end())
        throw std::runtime_error("the Path node has no value");
    const auto id = found->parameterId;
    const auto* parameter = composition.parameters().find(id);
    const auto* constant =
        parameter ? std::get_if<document::ConstantValueSource>(&parameter->source) : nullptr;
    const auto* base = constant ? std::get_if<document::PathValue>(&constant->value) : nullptr;
    if (base == nullptr)
        throw std::runtime_error("the path is not a constant");
    auto moved = *base;
    for (auto& contour : moved.contours)
        if (!contour.anchors.empty())
            contour.anchors.front().point.x -= 60.0;
    commands::Transaction keys("Key the path", session.snapshot().revision());
    keys.emplace<commands::SetKeyframeAtTimeForParameter>(
        session.compositionId(), id, core::RationalTime::fromInteger(0), *base);
    keys.emplace<commands::SetKeyframeAtTimeForParameter>(
        session.compositionId(), id, core::RationalTime::fromInteger(2), moved);
    if (!session.executeTransaction(std::move(keys)).succeeded())
        throw std::runtime_error("key the path");
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    ui::kit::installAfterlabTheme(application);
    if (argc < 2) {
        std::cerr << "usage: bloom_svg_drill_capture <output directory>\n";
        return 2;
    }
    const QString directory = QString::fromLocal8Bit(argv[1]);
    QDir().mkpath(directory);
    const QString ratio = application.devicePixelRatio() > 1.25 ? "dpr15" : "dpr1";
    const auto save = [&](QWidget& widget, const QString& name) {
        if (!widget.grab().save(directory + "/" + name + "-" + ratio + ".png"))
            throw std::runtime_error("could not write a capture");
    };
    const auto settle = [] {
        for (int pass = 0; pass < 3; ++pass) {
            QCoreApplication::sendPostedEvents();
            QCoreApplication::processEvents();
        }
    };
    try {
        document::LayerId root;
        ui::test::WindowFixture fixture([&](ui::test::WindowFixture& f) {
            root = import(f.session, kArtwork, "Badge", commands::SvgImportMode::Shapes);
            f.session.selectLayer(root);
        });
        auto& session = fixture.session;
        auto& window = *fixture.window;
        if (session.undoLabel() == QStringLiteral("Arrange sample graph"))
            static_cast<void>(session.undo());
        window.resize(1700, 1000);
        settle();
        auto* viewer = window.findChild<ui::ViewerEditor*>();
        if (viewer == nullptr)
            throw std::runtime_error("the window has no Viewer");
        const auto viewerReady = [&] {
            if (!bloom::test_support::awaitQtCondition("Viewer shows the current frame", [&] {
                    const auto state = viewer->displayedPreviewState();
                    return state.frame != nullptr &&
                           state.frame->desiredIdentity().time == session.currentTime() &&
                           state.frame->desiredIdentity().isolation ==
                               session.viewerLevel().groups &&
                           viewer->mappingForTest().has_value();
                }))
                throw std::runtime_error("the Viewer did not show the current frame");
            settle();
        };
        const auto& composition = *session.composition();
        const auto outer = child(composition, root, "outer");
        const auto middle = child(composition, outer, "middle");
        const auto inner = child(composition, middle, "inner");
        const auto tri = child(composition, inner, "tri");
        keyShape(session, tri);

        viewerReady();
        save(window, "drill-window-top-level");
        save(*viewer, "drill-viewer-top-level");

        // Two groups in: the artwork's root and outer are entered, middle is selected.
        if (!session.setViewerLevel({{root, outer}, std::nullopt}).ok())
            throw std::runtime_error("enter two groups");
        session.selectLayerFromViewer(middle);
        viewerReady();
        save(window, "drill-window-two-levels-in");
        save(*viewer, "drill-viewer-two-levels-in");

        // Point editing on the keyed triangle, with the playhead between its keys.
        if (!session.setCurrentTime(core::RationalTime::fromInteger(1)))
            throw std::runtime_error("move the playhead");
        if (!session.setViewerLevel({{root, outer, middle, inner}, std::nullopt}).ok())
            throw std::runtime_error("enter inner");
        session.selectLayerFromViewer(tri);
        if (!session.enterViewerPoints(tri))
            throw std::runtime_error("edit the triangle's points");
        viewerReady();
        save(window, "drill-window-point-editing");
        save(*viewer, "drill-viewer-point-editing");

        // The Break Apart offer: a double-click on a one-layer SVG at the top level.
        if (!session.setViewerLevel({}).ok() ||
            !session.setCurrentTime(core::RationalTime::fromInteger(0)))
            throw std::runtime_error("back to the top level");
        const auto logo = import(session, kOneLayer, "Logo", commands::SvgImportMode::Layer);
        session.selectLayer(logo);
        viewerReady();
        const auto mapping = viewer->mappingForTest();
        std::optional<QPointF> at;
        for (const auto& bounds : fixture.preview->currentLayerBounds())
            if (bounds.layerId == logo) {
                const auto& p = bounds.polygon;
                // The box, a fifth of the way in from the artwork's top-left corner.
                at = mapping->toScreen({p[0].x + 0.2 * (p[2].x - p[0].x),
                                        p[0].y + 0.25 * (p[2].y - p[0].y)});
            }
        if (!at)
            throw std::runtime_error("the one-layer SVG has no bounds on the Viewer");
        const auto mouse = [&](const QEvent::Type type, const Qt::MouseButtons buttons) {
            QMouseEvent event(type, *at, viewer->mapToGlobal(*at), Qt::LeftButton, buttons,
                              Qt::NoModifier);
            QApplication::sendEvent(viewer, &event);
        };
        mouse(QEvent::MouseButtonPress, Qt::LeftButton);
        mouse(QEvent::MouseButtonRelease, Qt::NoButton);
        QCoreApplication::sendPostedEvents();
        QCoreApplication::processEvents();
        mouse(QEvent::MouseButtonDblClick, Qt::LeftButton);
        mouse(QEvent::MouseButtonRelease, Qt::NoButton);
        ui::kit::KDialog* offer = nullptr;
        const bool offerOpened =
            bloom::test_support::awaitQtCondition("Break Apart offer opens", [&] {
                for (auto* widget : QApplication::topLevelWidgets())
                    if (auto* dialog = qobject_cast<ui::kit::KDialog*>(widget);
                        dialog != nullptr &&
                        dialog->objectName() == QStringLiteral("viewerBreakApartOffer") &&
                        dialog->isVisible())
                        offer = dialog;
                return offer != nullptr;
            });
        if (!offerOpened || offer == nullptr)
            throw std::runtime_error("the Break Apart offer did not open");
        settle();
        save(*offer, "drill-break-apart-offer");
        save(window, "drill-window-break-apart-offer");
        static_cast<void>(offer->activate(ui::kit::KDialog::Role::Reject));
        settle();
    } catch (const std::exception& error) {
        std::cerr << "capture failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
