// Interpret Footage (#381): the dialog's rows follow the footage kind and report only what the
// artist changed; the Assets context menu and selection header open it on the selection, and one
// OK is one undo entry; Copy and Paste Interpretation copy every member. With
// BLOOM_INTERPRET_CAPTURE_DIR set it also saves the video, still and sequence dialogs as PNGs.
#include "assets_editor_internal.hpp"
#include "interpret_footage_dialog.hpp"

#include <bloom/commands/transaction.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/project.hpp>
#include <bloom/ui/assets_editor.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/switch_control.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/kit/value_field.hpp>

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QLineEdit>
#include <QMenu>
#include <QStandardPaths>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>

#include <cstdio>

namespace {
using namespace bloom;
namespace assets = ui::assets;
bool ok = true;
void expect(const bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ok = false;
    }
}

void capture(QWidget& widget, const QString& name) {
    const auto directory = qEnvironmentVariable("BLOOM_INTERPRET_CAPTURE_DIR");
    if (directory.isEmpty())
        return;
    QDir().mkpath(directory);
    widget.show();
    QApplication::processEvents();
    const QString scale = qEnvironmentVariable("QT_SCALE_FACTOR", QStringLiteral("1")).remove('.');
    expect(widget.grab().toImage().save(directory + QStringLiteral("/%1-dpr%2.png").arg(name, scale)),
           "the capture is written");
    widget.hide();
}

document::AssetRecord video(const std::uint64_t id, const std::string& name) {
    document::AssetRecord asset;
    asset.id = document::AssetId::fromRaw(id);
    asset.name = name;
    asset.kind = document::AssetKind::Video;
    asset.locator = {"file", "project-relative", name + ".mov", "file:///media/" + name + ".mov"};
    asset.width = 64;
    asset.height = 48;
    asset.frames = 48;
    asset.duration = core::RationalTime::fromInteger(2);
    document::AssetVideoStream stream;
    stream.codec = "h264";
    stream.timebase = *core::RationalTime::create(1, 24);
    stream.framePeriod = *core::RationalTime::create(1, 24);
    stream.width = 64;
    stream.height = 48;
    stream.duration = asset.duration;
    stream.fieldOrder = 2;
    stream.timecode = "01:00:00:00";
    stream.timecodeType = 1;
    asset.videoStreams.push_back(stream);
    return asset;
}

document::AssetRecord still(const std::uint64_t id) {
    document::AssetRecord asset;
    asset.id = document::AssetId::fromRaw(id);
    asset.name = "logo";
    asset.kind = document::AssetKind::Image;
    asset.locator = {"file", "project-relative", "logo.png", "file:///media/logo.png"};
    asset.width = 8;
    asset.height = 8;
    return asset;
}

document::AssetRecord sequence(const std::uint64_t id) {
    document::AssetRecord asset;
    asset.id = document::AssetId::fromRaw(id);
    asset.name = "shot";
    asset.kind = document::AssetKind::Sequence;
    asset.locator = {"file", "project-relative", "shot.0001.png", "file:///media/shot.0001.png"};
    asset.width = 8;
    asset.height = 8;
    asset.manifest.pattern = "shot.####.png";
    asset.manifest.padding = 4;
    asset.manifest.first = 1;
    asset.manifest.last = 4;
    for (const std::int64_t frame : {1, 2, 4})
        asset.manifest.members.push_back(
            {frame,
             {"file", "project-relative", "shot.000" + std::to_string(frame) + ".png",
              "file:///media/shot.png"},
             bloom::core::Sha256Digest{},
             std::nullopt});
    asset.manifest.gaps = {3};
    return asset;
}

void dialogRows() {
    QWidget owner;
    const auto clip = video(1, "clip");
    ui::InterpretFootageDialog dialog(clip, 1, {document::AssetKind::Video}, {}, {}, owner);
    expect(dialog.rateMode() != nullptr && dialog.loop() != nullptr &&
               dialog.fieldOrder() != nullptr && dialog.missingFrames() == nullptr,
           "a video shows rate, loop and fields but no missing frames");
    expect(dialog.rateMode()->itemText(0).contains(QStringLiteral("24 fps")),
           "the file's rate is named");
    expect(dialog.fieldOrder()->itemText(0).contains(QStringLiteral("Upper")),
           "the file's field order is named");
    expect(dialog.change() && dialog.change()->empty(), "an untouched dialog changes nothing");
    dialog.rateMode()->setCurrentIndex(1);
    dialog.rate()->setText(QStringLiteral("23.976"));
    auto change = dialog.change();
    expect(change && change->frameRate == std::optional{document::FrameRate::create(24000, 1001)} &&
               !change->alphaMode && !change->loopCount,
           "conforming to 23.976 is the exact NTSC rational and nothing else");
    dialog.rate()->setText(QStringLiteral("fast"));
    expect(!dialog.change() && !dialog.issue().isEmpty(), "an unreadable rate is refused");
    dialog.rate()->setText(QStringLiteral("30000/1001"));
    dialog.loop()->setIntegerValue(3);
    dialog.alpha()->setCurrentIndex(
        dialog.alpha()->findData(static_cast<int>(document::AssetAlphaMode::Premultiplied)));
    dialog.deinterlace()->setChecked(true);
    dialog.pixelAspect()->setCurrentIndex(dialog.pixelAspect()->findData(QStringLiteral("2:1")));
    change = dialog.change();
    expect(change && change->loopCount == 3u &&
               change->alphaMode == document::AssetAlphaMode::Premultiplied &&
               change->deinterlace == true &&
               change->pixelAspect == std::optional{core::PixelAspectRatio::create(2, 1)},
           "loop, alpha, deinterlace and pixel aspect are reported");
    dialog.timecodeMode()->setCurrentIndex(1);
    dialog.timecode()->setText(QStringLiteral("00:00:10:00"));
    change = dialog.change();
    expect(change && change->startTimecode && *change->startTimecode &&
               (*change->startTimecode)->start == *core::RationalTime::create(300 * 1001, 30000),
           "a start timecode override at the conformed rate");
    capture(dialog, QStringLiteral("interpret-video"));

    const auto logo = still(2);
    ui::InterpretFootageDialog stillDialog(logo, 1, {document::AssetKind::Image}, {}, {}, owner);
    expect(stillDialog.rateMode() == nullptr && stillDialog.loop() == nullptr &&
               stillDialog.fieldOrder() == nullptr && stillDialog.alpha() != nullptr &&
               stillDialog.pixelAspect() != nullptr,
           "a still has no timing rows but has alpha and pixel aspect");
    stillDialog.color()->setCurrentIndex(stillDialog.color()->findData(QStringLiteral("raw")));
    change = stillDialog.change();
    expect(change && change->inputColor &&
               change->inputColor->first == document::AssetColorSpace::Raw,
           "Preserve RGB is the no-conversion input");
    capture(stillDialog, QStringLiteral("interpret-still"));
    expect(stillDialog.pixelAspect()->itemText(0) ==
                   QStringLiteral("Use the file's: Square Pixels (1.0)") &&
               stillDialog.alpha()->itemText(0) == QStringLiteral("Guess from the file: Straight"),
           "the file's values read without nested parentheses");

    auto shot = sequence(4);
    ui::InterpretFootageDialog sequenceDialog(shot, 1, {document::AssetKind::Sequence}, {}, {},
                                              owner);
    expect(sequenceDialog.missingFrames() != nullptr &&
               sequenceDialog.missingFrames()->findData(
                   static_cast<int>(document::AssetMissingFrames::Skip)) >= 0,
           "a sequence can skip its missing frames");
    sequenceDialog.missingFrames()->setCurrentIndex(sequenceDialog.missingFrames()->findData(
        static_cast<int>(document::AssetMissingFrames::Skip)));
    change = sequenceDialog.change();
    expect(change && change->missingFrames == document::AssetMissingFrames::Skip,
           "Skip them is reported");
    sequenceDialog.rateMode()->setCurrentIndex(1);
    expect(sequenceDialog.rateMode()->width() == sequenceDialog.timecodeMode()->width() &&
               sequenceDialog.timecode()->alignment().testFlag(Qt::AlignRight),
           "the rate and timecode fields line up and read right-aligned");
    capture(sequenceDialog, QStringLiteral("interpret-sequence"));
}

QTreeWidgetItem* findText(QTreeWidget& tree, const QString& name) {
    for (QTreeWidgetItemIterator it(&tree); *it; ++it)
        if ((*it)->text(assets::NameColumn) == name)
            return *it;
    return nullptr;
}

void triggerContext(QTreeWidget& tree, QTreeWidgetItem* item, const QString& action) {
    QTimer::singleShot(0, &tree, [&] {
        auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        if (menu == nullptr)
            return;
        QAction* chosen = nullptr;
        for (auto* candidate : menu->actions())
            if (candidate->objectName() == action)
                chosen = candidate;
        menu->close();
        if (chosen != nullptr && chosen->isEnabled())
            chosen->trigger();
    });
    emit tree.customContextMenuRequested(tree.visualItemRect(item).center());
    QApplication::processEvents();
}

ui::InterpretFootageDialog* openDialog() {
    for (auto* widget : QApplication::topLevelWidgets())
        if (auto* dialog = qobject_cast<ui::InterpretFootageDialog*>(widget);
            dialog != nullptr && dialog->isVisible())
            return dialog;
    return nullptr;
}

void editorFlow() {
    auto seed = document::makeNewProject("Interpret", "Main", core::RationalTime::fromInteger(10));
    expect(static_cast<bool>(seed.project.addAsset(video(1, "clip"))) &&
               static_cast<bool>(seed.project.addAsset(video(2, "other"))) &&
               static_cast<bool>(seed.project.addAsset(still(3))),
           "fixtures");
    document::Document document(std::move(seed.project));
    commands::CommandStack stack(document);
    ui::CompositionSession session(document, stack, seed.initialCompositionId);
    QWidget window;
    ui::AssetsEditor editor(session, &window);
    window.resize(700, 500);
    editor.resize(700, 500);
    window.show();
    QApplication::processEvents();
    auto* tree = editor.findChild<QTreeWidget*>(QStringLiteral("assetsTree"));
    expect(tree != nullptr, "assets tree");
    if (tree == nullptr)
        return;
    auto* clip = findText(*tree, QStringLiteral("clip"));
    auto* other = findText(*tree, QStringLiteral("other"));
    tree->clearSelection();
    clip->setSelected(true);
    other->setSelected(true);
    tree->setCurrentItem(clip, 0, QItemSelectionModel::NoUpdate);
    QApplication::processEvents();
    auto* header =
        editor.findChild<ui::kit::KIconButton*>(QStringLiteral("assetsSelectionInterpretFootage"));
    expect(header != nullptr && header->isVisible(),
           "the selection header offers Interpret Footage for footage");

    const auto before = stack.size();
    triggerContext(*tree, clip, QStringLiteral("assetsInterpretFootageAction"));
    auto* dialog = openDialog();
    expect(dialog != nullptr, "the context menu opens Interpret Footage");
    if (dialog == nullptr)
        return;
    expect(dialog->windowTitle().contains(QStringLiteral("2 items")),
           "the dialog names the selection it applies to");
    dialog->loop()->setIntegerValue(2);
    expect(dialog->activate(ui::kit::KDialog::Role::Accept), "OK is accepted");
    QApplication::processEvents();
    const auto& project = session.snapshot().project();
    expect(project.findAsset(document::AssetId::fromRaw(1))->interpretation.loopCount == 2 &&
               project.findAsset(document::AssetId::fromRaw(2))->interpretation.loopCount == 2,
           "OK applies the change to every selected clip");
    expect(stack.size() == before + 1, "one OK is one undo entry");
    clip = findText(*tree, QStringLiteral("clip"));
    expect(clip != nullptr && clip->text(assets::DurationColumn) == QStringLiteral("0:00:04:00"),
           "the Duration column shows the looped length");

    // Copy the first clip's interpretation, paste it onto the still: only what a still has.
    clip = findText(*tree, QStringLiteral("clip"));
    tree->clearSelection();
    tree->setCurrentItem(clip);
    triggerContext(*tree, clip, QStringLiteral("assetsCopyInterpretationAction"));
    expect(assets::copiedInterpretation().has_value(), "Copy Interpretation copies");
    auto* logo = findText(*tree, QStringLiteral("logo"));
    tree->clearSelection();
    tree->setCurrentItem(logo);
    triggerContext(*tree, logo, QStringLiteral("assetsPasteInterpretationAction"));
    const auto& pasted = session.snapshot().project().findAsset(document::AssetId::fromRaw(3));
    expect(pasted && pasted->interpretation.loopCount == 1 && !pasted->interpretation.frameRate,
           "a still takes no timing from a pasted video interpretation");
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    ui::kit::installAfterlabTheme(application);
    QApplication::setOrganizationName(QStringLiteral("BloomInterpretFootageTests"));
    QApplication::setApplicationName(QStringLiteral("InterpretFootage"));
    QStandardPaths::setTestModeEnabled(true);
    dialogRows();
    editorFlow();
    return ok ? 0 : 1;
}
