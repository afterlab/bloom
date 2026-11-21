// Review captures of the Text Source styling (#757): the Properties of a styled text (Markup, the
// accents, the outline and the background rows), its node card, and an older Text node with no
// styling bound, whose Properties offer the button that binds them; the button is clicked and the
// rows must then appear. A tool, not a test: it writes 757-<surface>-dpr<d>.png to the directory
// given as the first argument and exits non-zero when a check fails. The device pixel ratio follows
// QT_SCALE_FACTOR (run it at 1, 1.5 and 2).
#include "isolated_qt_settings.hpp"

#include "node_editor_items.hpp"

#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/node_definition_registry.hpp>
#include <bloom/document/text_styling.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/button.hpp>
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
#include <optional>
#include <string>
#include <vector>

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

// The styling button, when one is shown (a rebuilt panel deletes the old one later).
ui::kit::KButton* stylingButton(const QWidget& root) {
    for (auto* button : root.findChildren<ui::kit::KButton*>("propertiesTextShowStyling"))
        if (button->isVisible())
            return button;
    return nullptr;
}

bool hasLabel(const QWidget& root, const QString& text) {
    for (const auto* label : root.findChildren<QLabel*>())
        if (label->isVisible() && label->text() == text)
            return true;
    return false;
}

// Strips the styling bindings (and their parameters) from a Text Source, leaving the twelve layout
// parameters a node saved before #757 has.
bool makeOlder(document::Document& doc, const document::CompositionId compositionId,
               const document::NodeId nodeId) {
    const auto snapshot = doc.snapshot();
    auto draft = doc.draft(snapshot);
    auto* composition = draft.project().findComposition(compositionId);
    auto* node = composition == nullptr ? nullptr : composition->graph().findNode(nodeId);
    if (node == nullptr)
        return false;
    std::vector<document::ParameterBinding> kept;
    for (const auto& binding : node->parameters) {
        if (document::textStylingParameterSpecForRole(binding.role) == nullptr) {
            kept.push_back(binding);
            continue;
        }
        if (!composition->parameters().erase(binding.parameterId))
            return false;
    }
    node->parameters = std::move(kept);
    if (!draft.validate().ok()) {
        std::cout << "validation refused the older node\n";
        return false;
    }
    return doc.commit(snapshot.revision(), std::move(draft)).committed();
}
} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    bloom::test_support::isolateQtSettings();
    app.setOrganizationName("BloomTextStylingCapture");
    app.setApplicationName("TextStylingCapture");
    QStandardPaths::setTestModeEnabled(true);
    QSettings().clear();
    ui::kit::installAfterlabTheme(app);
    const QString directory = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QStringLiteral(".");
    QDir().mkpath(directory);
    const QString dpr =
        QStringLiteral("dpr") + qEnvironmentVariable("QT_SCALE_FACTOR", QStringLiteral("1"));

    auto seed =
        document::makeNewProject("Text styling", "Main", core::RationalTime::fromInteger(24));
    document::Document doc(std::move(seed.project));
    commands::CommandStack stack(doc);
    ui::CompositionSession session(doc, stack, seed.initialCompositionId);
    commands::TextLayerStyle style;
    style.styling = {{"markup", true},
                     {"accent1", core::Color4d{1.0, 0.55, 0.0, 1.0}},
                     {"outlineWidth", 4.0},
                     {"background", std::int64_t{document::kTextBackgroundPerLine}}};
    if (!session.addTextLayer(QStringLiteral("Caption"),
                              QStringLiteral("Bloom <c.accent1>now</c> <b>live</b>"), 96.0,
                              core::Color4d{1.0, 1.0, 1.0, 1.0}, std::nullopt, style))
        return 1;
    std::optional<document::NodeId> textNode;
    for (const auto& node : session.composition()->graph().nodes())
        if (node.typeId == document::kTextSourceNodeType)
            textNode = node.id;
    if (!textNode)
        return 1;

    int failures = 0;
    const auto save = [&](QImage image, const QString& surface) {
        const QString name = QStringLiteral("757-%1-%2").arg(surface, dpr);
        if (image.isNull() || !image.save(directory + '/' + name + ".png"))
            ++failures;
        std::cout << name.toStdString() << ' ' << image.width() << 'x' << image.height()
                  << " ratio=" << image.devicePixelRatio() << '\n';
    };

    {
        session.selectNode(*textNode);
        settle();
        ui::PropertiesEditor properties(session);
        properties.resize(380, 2000);
        properties.show();
        settle();
        failures += truncatedLabels(properties);
        for (const auto* label :
             {"Markup", "Accent 1", "Accent 4", "Outline color", "Outline width", "Outline join",
              "Background", "Background color", "Background padding", "Background corner radius"})
            if (!hasLabel(properties, QString::fromLatin1(label))) {
                ++failures;
                std::cout << "missing row: " << label << '\n';
            }
        if (stylingButton(properties) != nullptr) {
            ++failures;
            std::cout << "a new text layer offers the styling button\n";
        }
        save(properties.grab().toImage(), QStringLiteral("properties"));
    }
    {
        ui::NodeGraphEditor nodes(session);
        nodes.resize(900, 700);
        nodes.show();
        settle();
        auto* view = nodes.findChild<QGraphicsView*>();
        ui::node_editor::NodeItem* card = nullptr;
        if (view != nullptr)
            for (auto* item : view->scene()->items())
                if (auto* candidate = dynamic_cast<ui::node_editor::NodeItem*>(item);
                    candidate && candidate->id() == *textNode)
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
        if (!session.addTextLayer(QStringLiteral("Older title"), QStringLiteral("Older title")))
            return 1;
        std::optional<document::NodeId> older;
        for (const auto& node : session.composition()->graph().nodes())
            if (node.typeId == document::kTextSourceNodeType && node.id != *textNode)
                older = node.id;
        if (!older || !makeOlder(doc, seed.initialCompositionId, *older)) {
            std::cout << "the older text node could not be made\n";
            return 1;
        }
        // A session opened on the edited document, as when the older project is opened.
        commands::CommandStack reopenedStack(doc);
        ui::CompositionSession reopened(doc, reopenedStack, seed.initialCompositionId);
        reopened.selectNode(*older);
        settle();
        ui::PropertiesEditor properties(reopened);
        properties.resize(380, 1500);
        properties.show();
        settle();
        failures += truncatedLabels(properties);
        auto* button = stylingButton(properties);
        if (button == nullptr || hasLabel(properties, QStringLiteral("Accent 1"))) {
            ++failures;
            std::cout << "an older text node does not offer the styling button alone\n";
        }
        save(properties.grab().toImage(), QStringLiteral("properties-older"));
        if (button != nullptr) {
            button->click();
            settle();
            const auto* node = reopened.composition()->graph().findNode(*older);
            if (node == nullptr || node->parameters.size() != 24 ||
                !hasLabel(properties, QStringLiteral("Outline width")) ||
                stylingButton(properties) != nullptr) {
                ++failures;
                std::cout << "the styling button does not bind the rows (bindings "
                          << (node ? node->parameters.size() : 0U) << ", outline row "
                          << hasLabel(properties, QStringLiteral("Outline width")) << ", button "
                          << (stylingButton(properties) != nullptr) << ")\n";
            }
            failures += truncatedLabels(properties);
            save(properties.grab().toImage(), QStringLiteral("properties-older-shown"));
        }
    }
    if (failures != 0)
        std::cout << failures << " check(s) failed\n";
    return failures == 0 ? 0 : 1;
}
