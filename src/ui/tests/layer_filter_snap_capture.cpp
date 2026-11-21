// Review captures of the layer Filter and Snap rows (#742, #743): the Properties Object group, the
// Timeline's Filter and Snap rows, the Layer node's card and the Composition Settings Advanced tab
// with Pixel Art. A tool, not a test: it writes 742-743-<surface>-dpr<d>.png to the directory given
// as the first argument. The device pixel ratio follows QT_SCALE_FACTOR (run it at 1, 1.5 and 2).
#include "isolated_qt_settings.hpp"

#include "composition_editor_support.hpp"
#include "composition_settings_dialog.hpp"
#include "node_editor_items.hpp"
#include "timeline_property_rows.hpp"

#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/composition_settings_operations.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/resample_filter.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/tab_bar.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/node_editor.hpp>
#include <bloom/ui/properties_editor.hpp>

#include <QApplication>
#include <QDir>
#include <QGraphicsView>
#include <QImage>
#include <QPainter>
#include <QSettings>
#include <QStandardPaths>
#include <QVBoxLayout>

#include <iostream>

namespace {
using namespace bloom;

void settle() {
    for (int pass = 0; pass < 3; ++pass) {
        QCoreApplication::sendPostedEvents();
        QCoreApplication::processEvents();
    }
}
} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    bloom::test_support::isolateQtSettings();
    app.setOrganizationName("BloomLayerFilterSnapCapture");
    app.setApplicationName("LayerFilterSnapCapture");
    QStandardPaths::setTestModeEnabled(true);
    QSettings().clear();
    ui::kit::installAfterlabTheme(app);
    const QString directory = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QStringLiteral(".");
    QDir().mkpath(directory);
    const QString dpr =
        QStringLiteral("dpr") + qEnvironmentVariable("QT_SCALE_FACTOR", QStringLiteral("1"));

    auto seed = document::makeNewProject("Filter and Snap", "Main",
                                         core::RationalTime::fromInteger(24));
    document::Document doc(std::move(seed.project));
    commands::CommandStack stack(doc);
    ui::CompositionSession session(doc, stack, seed.initialCompositionId);
    if (!session.addSolidLayer(QStringLiteral("Sprite"), {0.9, 0.5, 0.2, 1.0}))
        return 1;
    const auto layer = session.composition()->graph().layerOutputs().front().layerId;
    // One explicit choice so the captures show a value other than the defaults.
    if (!session.setLayerFilter(layer, 1))
        return 1;
    session.selectLayer(layer);
    settle();

    int failures = 0;
    const auto save = [&](QImage image, const QString& surface) {
        const QString name = QStringLiteral("742-743-%1-%2").arg(surface, dpr);
        if (image.isNull() || !image.save(directory + '/' + name + ".png"))
            ++failures;
        std::cout << name.toStdString() << ' ' << image.width() << 'x' << image.height()
                  << " ratio=" << image.devicePixelRatio() << '\n';
    };

    {
        ui::PropertiesEditor properties(session);
        properties.resize(380, 720);
        properties.show();
        settle();
        save(properties.grab().toImage(), QStringLiteral("properties"));
        const auto* filter = properties.findChild<ui::kit::KDropdown*>("layerFilterEditor");
        const auto* snap = properties.findChild<ui::kit::KDropdown*>("layerSnapEditor");
        if (filter == nullptr || snap == nullptr || filter->currentText() != "Nearest" ||
            snap->currentText() != "Automatic")
            ++failures;
    }
    {
        QWidget host;
        auto* column = new QVBoxLayout(&host);
        column->setContentsMargins(0, 0, 0, 0);
        column->setSpacing(0);
        ui::TimelineLayerEntry entry;
        entry.layerId = layer;
        std::vector<std::unique_ptr<ui::TimelinePropertyRow>> rows;
        for (const auto& item : ui::timelinePropertyEntries(session, {entry}, {layer})) {
            if (item.role != document::kLayerFilterParameterRole &&
                item.role != document::kLayerSnapParameterRole)
                continue;
            auto row = std::make_unique<ui::TimelinePropertyRow>(session, &host);
            row->bind(item);
            column->addWidget(row.get());
            rows.push_back(std::move(row));
        }
        if (rows.size() != 2)
            ++failures;
        host.resize(520, host.sizeHint().height());
        host.show();
        settle();
        save(host.grab().toImage(), QStringLiteral("timeline"));
        host.hide();
    }
    {
        ui::NodeGraphEditor nodes(session);
        nodes.resize(900, 700);
        nodes.show();
        settle();
        auto* view = nodes.findChild<QGraphicsView*>();
        const auto layerNode = session.composition()->graph().layerOutputs().front().nodeId;
        ui::node_editor::NodeItem* card = nullptr;
        if (view != nullptr)
            for (auto* item : view->scene()->items())
                if (auto* candidate = dynamic_cast<ui::node_editor::NodeItem*>(item);
                    candidate && candidate->id() == layerNode)
                    card = candidate;
        if (card == nullptr) {
            ++failures;
        } else {
            const auto source = card->sceneBoundingRect().adjusted(-8, -8, 8, 8);
            const qreal ratio = nodes.devicePixelRatioF();
            QImage image((source.size() * ratio).toSize(), QImage::Format_ARGB32_Premultiplied);
            image.setDevicePixelRatio(ratio);
            image.fill(nodes.palette().window().color());
            QPainter painter(&image);
            painter.setRenderHint(QPainter::Antialiasing);
            view->scene()->render(&painter, QRectF(QPointF(), source.size()), source);
            painter.end();
            save(image, QStringLiteral("node-card"));
        }
    }
    {
        QWidget owner;
        auto initial = commands::compositionSettingsOf(*session.composition());
        initial.pixelArt = true;
        ui::CompositionSettingsDialog dialog(ui::CompositionSettingsDialog::Mode::Edit, initial,
                                             ui::workingColorSpaceChoices(session.colorSettings()),
                                             owner);
        dialog.show();
        settle();
        if (auto* tabs = dialog.findChild<ui::kit::KTabs*>("compositionSettingsTabs"))
            tabs->setCurrentIndex(1);
        else
            ++failures;
        settle();
        save(dialog.grab().toImage(), QStringLiteral("composition-settings"));
    }
    return failures == 0 ? 0 : 1;
}
