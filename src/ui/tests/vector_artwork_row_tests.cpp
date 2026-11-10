// #642: the Vector Artwork layer's source row. The node card and Properties show what the layer
// holds as a read-only line: the file's name and how much of it the project keeps ("logo.svg · 1 KB
// held"), "No SVG" for an empty node, and "The SVG is missing from the project" when the block is
// gone. With BLOOM_CAPTURE_DIR set, the card and Properties are also written as PNGs (the scale
// factor is the environment's QT_SCALE_FACTOR), the review captures at DPR 1 and 1.5.
#include "node_production_harness.hpp"

#include <bloom/commands/svg_import_operations.hpp>
#include <bloom/document/vector_artwork_nodes.hpp>
#include <bloom/ui/properties_editor.hpp>

#include <QDir>
#include <QGraphicsProxyWidget>
#include <QImage>
#include <QScrollArea>

#include <filesystem>
#include <fstream>

using namespace bloom;
using namespace bloom::ui;
using namespace bloom::ui::production_test;

namespace {
[[nodiscard]] std::vector<QWidget*> cardWidgets(App& app, const document::NodeId id) {
    std::vector<QWidget*> widgets;
    if (auto* card = app.card(id))
        for (auto* child : card->childItems())
            if (auto* proxy = qgraphicsitem_cast<QGraphicsProxyWidget*>(child))
                if (proxy->widget() != nullptr)
                    widgets.push_back(proxy->widget());
    return widgets;
}

[[nodiscard]] QString cardReadout(App& app, const document::NodeId id) {
    for (auto* widget : cardWidgets(app, id))
        if (auto* label = widget->findChild<kit::KLabel*>(QStringLiteral("nodeReadOnlyValue")))
            return label->text();
    return {};
}

[[nodiscard]] QString propertiesReadout(PropertiesEditor& properties) {
    if (auto* label = properties.findChild<kit::KLabel*>(QStringLiteral("propertiesVectorArtworkSource")))
        return label->text();
    return {};
}

void capture(const QString& name, QWidget& widget) {
    const auto directory = qEnvironmentVariable("BLOOM_CAPTURE_DIR");
    if (directory.isEmpty())
        return;
    QDir().mkpath(directory);
    const auto ratio = widget.devicePixelRatioF() > 1.25 ? QStringLiteral("dpr15") : QStringLiteral("dpr1");
    const auto image = widget.grab().toImage();
    expect(image.save(QStringLiteral("%1/%2-%3.png").arg(directory, name, ratio)), "the capture is written");
}

void run(App& app) {
    const auto path = std::filesystem::temp_directory_path() / "bloom-vector-artwork-row-logo.svg";
    std::ofstream(path, std::ios::binary)
        << R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="100"><rect x="10" y="10" width="80" height="60" fill="#c33"/><circle cx="150" cy="50" r="30" fill="#36c"/></svg>)svg";
    const auto bytes = std::filesystem::file_size(path);

    commands::Transaction transaction("Import SVG", app.session.snapshot().revision());
    transaction
        .emplace<commands::AddSvgArtwork>(app.session.compositionId(), nullptr, path, "Logo", true,
                                          commands::SvgImportMode::Layer)
        .withProjectDirectory(path.parent_path());
    const auto imported = app.session.executeNodeTransaction(std::move(transaction));
    expect(imported.succeeded(), "the SVG imports as one layer");
    const auto node = imported.outputId<document::NodeId>("node");
    expect(node.has_value(), "the import names the Vector Artwork node");
    if (!node)
        return;
    QCoreApplication::processEvents();

    const QString held = QStringLiteral("bloom-vector-artwork-row-logo.svg · %1 KB held").arg((bytes + 1023) / 1024);
    expect(app.card(*node) != nullptr, "the node has a card");
    const auto card = cardReadout(app, *node);
    expect(card == held, ("the card reads the file and what the project holds: " + card.toStdString()).c_str());

    PropertiesEditor properties(app.session);
    properties.resize(700, 600);
    properties.show();
    app.session.selectNode(*node);
    QCoreApplication::processEvents();
    const auto row = propertiesReadout(properties);
    expect(row == held, ("Properties reads the same line: " + row.toStdString()).c_str());
    app.editor.graphView()->centerOn(app.card(*node));
    QCoreApplication::processEvents();
    capture(QStringLiteral("artwork-row-card"), *app.editor.graphView());
    if (auto* label = properties.findChild<kit::KLabel*>(QStringLiteral("propertiesVectorArtworkSource"))) {
        for (auto* scroll : properties.findChildren<QScrollArea*>())
            scroll->ensureWidgetVisible(label);
        QCoreApplication::processEvents();
        std::cerr << "properties row label width " << label->width() << " text width "
                  << label->fontMetrics().horizontalAdvance(label->text()) << '\n';
        expect(label->width() >= label->fontMetrics().horizontalAdvance(label->text()),
               "Properties' row is wide enough to show the whole line");
        capture(QStringLiteral("artwork-row-properties"), properties);
    }
    // The line is not editable: neither surface offers a control that changes the block.
    for (auto* widget : cardWidgets(app, *node))
        expect(widget->findChild<QLineEdit*>() == nullptr, "the card's row has no text field");
    expect(properties.findChild<QLineEdit*>(QStringLiteral("propertiesVectorArtworkSource")) == nullptr,
           "Properties' row is a label, not a field");

    // An empty node says so; a block that is gone is named.
    const auto* record = app.session.composition()->graph().findNode(*node);
    document::ParameterId artwork;
    for (const auto& binding : record->parameters)
        if (binding.role == document::kVectorArtworkBlockParameterRole)
            artwork = binding.parameterId;
    expect(artwork.isValid(), "the node has its artwork parameter");
    const auto ownText = [&](const std::int64_t stored) {
        return vectorArtworkSourceText(app.session.snapshot().project(), stored);
    };
    expect(ownText(0) == QStringLiteral("No SVG"), "an empty node reads No SVG");
    expect(ownText(987654) == QStringLiteral("The SVG is missing from the project"),
           "a block the project does not hold is named");
    std::filesystem::remove(path);
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    try {
        App fixture;
        run(fixture);
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    if (failures != 0)
        return 1;
    std::cout << "vector artwork row: PASS\n";
    return 0;
}
