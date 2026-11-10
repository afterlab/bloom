// Import options and preferences (#381): the options pane under the Import Media dialog's file type
// row, the Preferences Import page, an import that reads both into the new assets'
// interpretation, Create a composition as the import's follow-up, and Assets thumbnails that show
// Interpret Footage's alpha and pixel aspect. With BLOOM_INTERPRET_CAPTURE_DIR set it also saves
// the Import dialog and the Import preferences page as PNGs.

#include "import_options_pane.hpp"
#include "preferences_window.hpp"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QGridLayout>
#include <QImage>
#include <QLabel>
#include <QListWidget>
#include <QSettings>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <bloom/commands/footage_interpretation_operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/ui/application_preferences.hpp>
#include <bloom/ui/asset_controller.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/file_chooser.hpp>
#include <bloom/ui/kit/switch_control.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/kit/value_field.hpp>
#include <bloom/ui/project_host.hpp>
#include <bloom/ui/task_ui_bridge.hpp>

#include <cstdio>

namespace {
using namespace bloom;
bool ok = true;
void expect(const bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ok = false;
    }
}
template <typename Predicate> bool waitUntil(Predicate predicate, const int timeoutMs = 15000) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeoutMs)
        QTest::qWait(5);
    return predicate();
}
void capture(QWidget& widget, const QString& name) {
    const auto directory = qEnvironmentVariable("BLOOM_INTERPRET_CAPTURE_DIR");
    if (directory.isEmpty())
        return;
    QDir().mkpath(directory);
    widget.show();
    QApplication::processEvents();
    const QString scale = qEnvironmentVariable("QT_SCALE_FACTOR", QStringLiteral("1")).remove('.');
    expect(
        widget.grab().toImage().save(directory + QStringLiteral("/%1-dpr%2.png").arg(name, scale)),
        "the capture is written");
    widget.hide();
}
void writePng(const QString& path, const bool transparentHalf = false) {
    QImage image(8, 8, QImage::Format_RGBA8888);
    image.fill(QColor(255, 0, 0, 255));
    // The left half is transparent red: Ignore shows its stored red, Straight shows nothing.
    if (transparentHalf)
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 4; ++x)
                image.setPixelColor(x, y, QColor(255, 0, 0, 0));
    expect(image.save(path), "PNG fixture");
}

struct Harness final {
    runtime::TaskScheduler scheduler;
    ui::ProjectHost host{scheduler};
    ui::CompositionSession session{*host.liveDocumentAndStack().first,
                                   *host.liveDocumentAndStack().second, host.lowestCompositionId()};
    ui::TaskUiBridge bridge{scheduler};
    ui::AssetController controller{session, host, scheduler, bridge};
    ~Harness() {
        waitUntil([this] { return !controller.busy(); });
    }
    [[nodiscard]] const document::AssetRecord* asset(const std::size_t index) const {
        const auto& assets = session.snapshot().project().assets();
        return index < assets.size() ? &assets[index] : nullptr;
    }
};

void pane(const QTemporaryDir& directory) {
    QSettings().remove(QStringLiteral("dialogs/import"));
    ui::ImportOptionsPane options(true);
    auto chosen = options.options();
    expect(chosen.sequences && !chosen.skipMissingFrames && !chosen.frameRange &&
               !options.createComposition(),
           "the pane starts as Bloom has always imported");
    options.range()->setChecked(true);
    options.firstFrame()->setIntegerValue(10);
    options.lastFrame()->setIntegerValue(4);
    expect(options.firstFrame()->integerValue() == 4,
           "moving the last frame before the first moves the first with it");
    options.lastFrame()->setIntegerValue(20);
    options.skipMissing()->setChecked(true);
    options.composition()->setChecked(true);
    chosen = options.options();
    expect(chosen.frameRange == std::optional{document::AssetSequenceRange{4, 20}} &&
               chosen.skipMissingFrames && options.createComposition(),
           "the pane reports the range, Skip and Create a composition");
    options.sequence()->setChecked(false);
    chosen = options.options();
    expect(!chosen.sequences && !chosen.frameRange && !chosen.skipMissingFrames &&
               !options.skipMissing()->isEnabled() && !options.firstFrame()->isEnabled(),
           "single images have no frame range and nothing to skip");
    options.remember();
    const ui::ImportOptionsPane again(true);
    expect(!again.sequence()->isChecked() && again.skipMissing()->isChecked() &&
               again.composition()->isChecked() && !again.range()->isChecked(),
           "the check boxes are remembered for the next import, the range is not");
    const ui::ImportOptionsPane layers(false);
    expect(layers.composition()->isHidden() && !layers.createComposition(),
           "an import that adds layers offers no composition");
    QSettings().remove(QStringLiteral("dialogs/import"));

    // The pane sits under the file type row of Bloom's own file dialog.
    QWidget owner;
    owner.resize(800, 600);
    const auto dialog = ui::kit::buildFileDialog(
        owner, {.mode = ui::kit::KFileRequest::Mode::OpenMany,
                .title = QStringLiteral("Import Media"),
                .kind = QStringLiteral("import-options-test"),
                .nameFilters = QStringLiteral("Media (*.png)"),
                .defaultSuffix = {},
                .startPath = directory.filePath(QStringLiteral("import"))});
    auto* shown = new ui::ImportOptionsPane(true, dialog.get());
    ui::kit::addFileDialogOptions(*dialog, QStringLiteral("Options:"), *shown);
    const auto* label = dialog->findChild<QLabel*>(QStringLiteral("fileDialogOptionsLabel"));
    auto* grid = qobject_cast<QGridLayout*>(dialog->layout());
    expect(shown->parent() == dialog.get() && label != nullptr && grid != nullptr &&
               grid->indexOf(shown) >= 0,
           "the options pane is a row of the file dialog");
    capture(*dialog, QStringLiteral("import-dialog"));
}

void preferencesPage() {
    QWidget owner;
    ui::PreferencesWindow window(owner, ui::ApplicationPreferences{});
    auto* rate = window.findChild<ui::kit::KValueField*>(
        QStringLiteral("preferencesImportSequenceFrameRateField"));
    auto* rateMode = window.findChild<ui::kit::KDropdown*>(
        QStringLiteral("preferencesImportSequenceFrameRateFieldMode"));
    auto* still = window.findChild<ui::kit::KValueField*>(
        QStringLiteral("preferencesImportStillDurationField"));
    expect(rate != nullptr && rateMode != nullptr && still != nullptr,
           "the Import page has the sequence rate and still duration");
    if (rate == nullptr || rateMode == nullptr || still == nullptr)
        return;
    expect(std::abs(rate->value() - 24.0) < 1e-9 && rateMode->currentIndex() == 1 &&
               still->value() == 0.0,
           "the page shows 24 fps and the composition's length");
    rate->setValue(23.976);
    expect(window.preferences().importSequenceFrameRate == document::FrameRate::create(24000, 1001),
           "23.976 is the exact NTSC rate");
    rate->setValue(0.0);
    expect(!window.preferences().importSequenceFrameRate,
           "Match composition leaves new sequences on the composition's frames");
    rate->setValue(30.0);
    still->setValue(2.5);
    expect(window.preferences().importStillDuration == core::RationalTime::create(5, 2),
           "a fixed still duration is exact to the hundredth");
    // #642: what an SVG file imports as.
    auto* svg = window.findChild<ui::kit::KDropdown*>(QStringLiteral("preferencesImportSvgModeDropdown"));
    expect(svg != nullptr && svg->count() == 3 && svg->currentIndex() == 0 &&
               window.preferences().importSvgMode == ui::SvgImportChoice::Ask,
           "the Import page asks about SVG files by default");
    if (svg != nullptr) {
        svg->setCurrentIndex(1);
        expect(window.preferences().importSvgMode == ui::SvgImportChoice::Layer,
               "SVG files import as one layer when so chosen");
        svg->setCurrentIndex(2);
        expect(window.preferences().importSvgMode == ui::SvgImportChoice::Shapes,
               "SVG files import as editable shapes when so chosen");
    }
    auto* rail = window.findChild<QListWidget*>(QStringLiteral("preferencesCategoryRail"));
    for (int row = 0; rail != nullptr && row < rail->count(); ++row)
        if (rail->item(row)->text() == QStringLiteral("Import"))
            rail->setCurrentRow(row);
    auto* pages = window.findChild<QStackedWidget*>(QStringLiteral("preferencesPages"));
    expect(pages != nullptr &&
               pages->currentWidget()->objectName() == QStringLiteral("preferencesImportPage"),
           "the rail opens the Import page");
    capture(window, QStringLiteral("preferences-import"));
}

void importReadsOptionsAndPreferences(const QTemporaryDir& directory) {
    // The Import preferences as the Preferences window saves them.
    auto preferences = ui::ApplicationPreferences{};
    preferences.importSequenceFrameRate = document::FrameRate::create(25, 1);
    preferences.importStillDuration = core::RationalTime::fromInteger(3);
    {
        QSettings settings;
        ui::saveApplicationPreferences(settings, preferences);
    }
    for (const auto* name : {"shot.0001.png", "shot.0002.png", "shot.0004.png", "logo.png"})
        writePng(directory.filePath(QStringLiteral("import/%1").arg(name)));
    Harness harness;
    commands::ImportOptions options;
    options.skipMissingFrames = true;
    const auto compositions = harness.session.snapshot().project().compositions().size();
    harness.controller.importFiles(
        {directory.filePath("import/shot.0001.png"), directory.filePath("import/logo.png")},
        std::nullopt, harness.controller.compositionFollowUp(), options);
    expect(waitUntil([&] { return !harness.controller.busy(); }), "the import finishes");
    const auto* shot = harness.asset(0);
    const auto* logo = harness.asset(1);
    expect(shot != nullptr && shot->kind == document::AssetKind::Sequence &&
               shot->interpretation.frameRate == document::FrameRate::create(25, 1) &&
               shot->interpretation.missingFrames == document::AssetMissingFrames::Skip,
           "a new sequence takes the preference rate and Skip");
    expect(logo != nullptr && logo->interpretation.stillDuration ==
                                  std::optional{core::RationalTime::fromInteger(3)},
           "a new still takes the preference duration");
    const auto& project = harness.session.snapshot().project();

    const auto* composition = project.findComposition(harness.session.compositionId());
    expect(project.compositions().size() == compositions + 1 && composition != nullptr &&
               composition->graph().layerOutputs().size() == 2 &&
               composition->format().frameRate() == document::FrameRate::create(25, 1),
           "Create a composition makes one from the footage and opens it");
    QSettings().remove(QStringLiteral("import"));
}

void thumbnailsFollowInterpretation(const QTemporaryDir& directory) {
    writePng(directory.filePath("import/half.png"), true);
    Harness harness;
    commands::ImportOptions options;
    harness.controller.importFiles({directory.filePath("import/half.png")}, std::nullopt, {},
                                   options);
    expect(waitUntil([&] { return !harness.controller.busy(); }), "the still imports");
    const auto* asset = harness.asset(0);
    if (asset == nullptr) {
        expect(false, "the still is in the project");
        return;
    }
    const auto id = asset->id;
    const auto thumbnail = [&] {
        waitUntil([&] {
            return !harness.controller.thumbnailsPending() &&
                   !harness.controller.thumbnail(id).isNull();
        });
        return harness.controller.thumbnail(id);
    };
    const auto before = thumbnail();
    expect(!before.isNull() && qAlpha(before.pixel(0, 4)) == 0,
           "the transparent half is transparent in the thumbnail");
    const auto interpret = [&](commands::AssetInterpretationChange change) {
        commands::Transaction transaction("Interpret Footage",
                                          harness.session.snapshot().revision());
        transaction.emplace<commands::SetAssetInterpretation>(std::vector{id}, std::move(change));
        expect(harness.session.executeTransaction(std::move(transaction)).changed(),
               "the interpretation changes");
    };
    commands::AssetInterpretationChange ignore;
    ignore.alphaMode = document::AssetAlphaMode::Ignore;
    interpret(ignore);
    QImage ignored;
    waitUntil([&] {
        ignored = thumbnail();
        return !ignored.isNull() && qAlpha(ignored.pixel(0, 4)) == 255;
    });
    expect(!ignored.isNull() && qAlpha(ignored.pixel(0, 4)) == 255 &&
               qRed(ignored.pixel(0, 4)) > 200,
           "Ignore shows the stored red as opaque, as the renderer does");
    commands::AssetInterpretationChange invert;
    invert.alphaMode = document::AssetAlphaMode::Straight;
    invert.invertAlpha = true;
    interpret(invert);
    QImage inverted;
    waitUntil([&] {
        inverted = thumbnail();
        return !inverted.isNull() && qAlpha(inverted.pixel(7, 4)) == 0;
    });
    expect(!inverted.isNull() && qAlpha(inverted.pixel(0, 4)) == 255 &&
               qAlpha(inverted.pixel(7, 4)) == 0,
           "Invert swaps the transparent and the opaque halves");
    commands::AssetInterpretationChange wide;
    wide.pixelAspect = core::PixelAspectRatio::create(2, 1);
    interpret(wide);
    QImage stretched;
    waitUntil([&] {
        stretched = thumbnail();
        return stretched.width() == 2 * stretched.height();
    });
    expect(stretched.width() == 16 && stretched.height() == 8,
           "a 2:1 pixel aspect draws the thumbnail twice as wide");
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    ui::kit::installKinetikTheme(application);
    QApplication::setOrganizationName(QStringLiteral("BloomImportOptionsTests"));
    QApplication::setApplicationName(QStringLiteral("ImportOptions"));
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir directory;
    expect(directory.isValid() && QDir(directory.path()).mkpath(QStringLiteral("import")),
           "fixture folder");
    pane(directory);
    preferencesPage();
    importReadsOptionsAndPreferences(directory);
    thumbnailsFollowInterpretation(directory);
    return ok ? 0 : 1;
}
