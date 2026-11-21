// #752 SB3: the Subtitles artist interface (docs/ux/interaction-model.md, "Subtitles"). The
// Properties cue list and its actions, Edit as Text, the Source row, the Timeline cue blocks, the
// Viewer outlines and gestures, the node card, Import and Add > Subtitles. Every edit is checked to be
// exactly one undo step; the 10,000-cue list is checked to build no widget per cue.
#include "await_qt.hpp"
#include "node_editor_items.hpp"
#include "properties_subtitles.hpp"
#include "subtitles_editing.hpp"
#include "subtitles_inspector.hpp"

#include <QAbstractItemModel>
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QGraphicsView>
#include <QHeaderView>
#include <QPainter>
#include <QDialog>
#include <QFile>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QSettings>
#include <QTableView>
#include <QTemporaryDir>
#include <QTest>
#include <QVBoxLayout>
#include <bloom/commands/subtitles_operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/document/subtitles_nodes.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/node_definition_registry.hpp>
#include <bloom/runtime/reference_display_preparation.hpp>
#include <bloom/ui/asset_controller.hpp>
#include <bloom/ui/composition_authoring.hpp>
#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/composition_preview_pipeline.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/grading_controls.hpp>
#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/dialog.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/node_editor.hpp>
#include <bloom/ui/project_host.hpp>
#include <bloom/ui/properties_editor.hpp>
#include <bloom/ui/task_ui_bridge.hpp>
#include <bloom/ui/timeline_editor.hpp>
#include <bloom/ui/viewer_editor.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <source_location>
#include <string>

namespace {
using namespace bloom;
using namespace std::chrono_literals;
using bloom::test_support::awaitQtCondition;

int failures = 0;
void expect(const bool condition, const std::string& message,
            const std::source_location location = std::source_location::current()) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL line " << location.line() << ": " << message << '\n';
    }
}

core::RationalTime seconds(const std::int64_t numerator, const std::int64_t denominator = 1) {
    return *core::RationalTime::create(numerator, denominator);
}

document::Cue cue(const core::RationalTime start, const core::RationalTime end, std::string text) {
    document::Cue made;
    made.start = start;
    made.end = end;
    made.text = std::move(text);
    return made;
}

runtime::TaskSchedulerConfig schedulerConfig() {
    auto config = runtime::TaskSchedulerConfig::defaults();
    config.cpuWorkerCount = 2;
    config.rowBandWorkerCount = runtime::kSerialRowBandWorkers;
    return config;
}

struct Fixture {
    QTemporaryDir settings;
    QTemporaryDir files;
    runtime::TaskScheduler scheduler{schedulerConfig()};
    ui::ProjectHost projectHost{scheduler};
    ui::CompositionSession session{*projectHost.liveDocumentAndStack().first,
                                   *projectHost.liveDocumentAndStack().second,
                                   projectHost.lowestCompositionId()};
    ui::TaskUiBridge bridge{scheduler, nullptr, 1ms};
    struct Definitions {
        runtime::NodeDefinitionRegistry registry;
        Definitions() {
            expect(runtime::registerBuiltInNodeDefinitions(registry), "register node definitions");
            registry.freeze();
        }
    } definitions;
    runtime::SnapshotCompiler compiler{definitions.registry};
    runtime::CpuCompositionEvaluator evaluator;
    runtime::CpuReferenceDisplayPreparer display;
    runtime::QualifiedDisplayProcessorProvider qualified;
    std::unique_ptr<ui::CompositionPreviewController> preview;
    std::unique_ptr<ui::AssetController> assets;
    QWidget host;
    QVBoxLayout* layout = new QVBoxLayout(&host);
    ui::ViewerEditor* viewer = nullptr;
    ui::PropertiesEditor* properties = nullptr;
    ui::TimelineEditor* timeline = nullptr;
    ui::NodeGraphEditor* nodes = nullptr;
    document::LayerId layer;
    document::NodeId node;

    Fixture() {
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
        if (session.composition() == nullptr) {
            const auto format = document::CompositionFormat::create(640, 360);
            commands::Transaction seed("Seed", session.snapshot().revision());
            seed.emplace<commands::AddComposition>("Subtitles", *format,
                                                   core::RationalTime::fromInteger(12));
            const auto seeded = session.executeTransaction(std::move(seed));
            const auto id = seeded.outputId<document::CompositionId>(commands::kAddCompositionOutput);
            expect(id && session.setComposition(*id), "seed a composition");
        }
        preview = std::make_unique<ui::CompositionPreviewController>(
            session, scheduler, bridge,
            ui::makeCompositionPreviewPipeline(compiler, evaluator, display, qualified));
        preview->setResolutionPolicy(runtime::PreviewResolutionPolicy::Full);
        assets = std::make_unique<ui::AssetController>(
            session, projectHost, scheduler, bridge, nullptr, nullptr,
            ui::AudioProcessingAdmission{.policy = nullptr,
                                         .standaloneByteAllowance = 16 * 1024 * 1024,
                                         .cacheByteCapacity = 64 * 1024 * 1024});
        viewer = new ui::ViewerEditor(session, *preview);
        properties = new ui::PropertiesEditor(session);
        timeline = new ui::TimelineEditor(session, *preview);
        nodes = new ui::NodeGraphEditor(session);
        layout->addWidget(viewer, 3);
        layout->addWidget(timeline, 2);
        auto* side = new QWidget(&host);
        auto* sideLayout = new QHBoxLayout(side);
        sideLayout->addWidget(properties);
        sideLayout->addWidget(nodes);
        layout->addWidget(side, 3);
        host.resize(1400, 1500);
        host.show();
        // Add > Subtitles, through the Timeline's Add menu.
        auto* add = timeline->findChild<QAction*>(QStringLiteral("addSubtitlesLayerAction"));
        expect(add != nullptr, "the Timeline's Add menu offers Subtitles");
        const auto before = session.snapshot().revision();
        if (add != nullptr)
            add->trigger();
        expect(session.undoLabel().contains(QStringLiteral("Subtitles")),
               "Add Subtitles is one undo step: " + session.undoLabel().toStdString());
        expect(session.snapshot().revision() != before, "the layer was added");
        if (const auto* selected = std::get_if<document::LayerId>(&session.selection().primary))
            layer = *selected;
        expect(layer.isValid(), "the new layer is selected");
        if (const auto found = ui::subtitles_editing::subtitlesNodeOfLayer(session, layer))
            node = *found;
        expect(node.isValid(), "the new layer shows a Subtitles node");
        const auto* composition = session.composition();
        const auto* record = composition ? composition->graph().findNode(node) : nullptr;
        // The size follows the composition's height (5.5 %, 20 px at 360).
        const auto size = ui::nodeRoleParameter(session, node, "size");
        expect(size && session.constantValue(*size) == 20.0, "the size is 5.5 % of the height");
        expect(record != nullptr, "node exists");
    }
    ~Fixture() {
        preview->beginShutdown();
        bridge.beginShutdown();
        expect(awaitQtCondition("scheduler quiescent", [&] { return scheduler.isQuiescent(); }),
               "the preview shuts down");
        host.hide();
    }

    void setCues(std::vector<document::Cue> cues) {
        commands::Transaction transaction("Set Cues", session.snapshot().revision());
        transaction.emplace<commands::SetCues>(session.compositionId(), node, std::move(cues),
                                               std::nullopt, std::nullopt, std::nullopt);
        expect(session.executeTransaction(std::move(transaction)).changed(), "set the cues");
    }
    [[nodiscard]] const document::CueListValue* list() const {
        return ui::subtitles_editing::cueSource(session, node).list;
    }
    [[nodiscard]] std::size_t count() const { return list() ? list()->cues.size() : 0; }
    [[nodiscard]] const document::Cue& at(const std::size_t index) const {
        return *list()->cues.at(index);
    }
    [[nodiscard]] QWidget* panel() const {
        return properties->findChild<QWidget*>(QStringLiteral("subtitlesCuePanel"));
    }
    template <typename T> [[nodiscard]] T* find(const char* name) const {
        auto* found = panel() ? panel()->findChild<T*>(QString::fromLatin1(name)) : nullptr;
        if (found == nullptr)
            std::cerr << "missing " << name << '\n';
        return found;
    }
    [[nodiscard]] QTableView* table() const { return find<QTableView>("subtitlesCueTable"); }
    void time(const core::RationalTime at) {
        expect(session.currentTime() == at || session.setCurrentTime(at), "move the playhead");
        QCoreApplication::processEvents();
    }
    void selectRow(const int row) {
        auto* view = table();
        view->setFocus();
        view->selectionModel()->setCurrentIndex(
            view->model()->index(row, 4),
            QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        QCoreApplication::processEvents();
    }
    void click(const char* name) {
        auto* button = find<ui::kit::KButton>(name);
        expect(button != nullptr && button->isEnabled(), std::string(name) + " is enabled");
        if (button != nullptr)
            button->click();
        QCoreApplication::processEvents();
    }
    // One undo step labelled `label` that undo takes back to `before`.
    void oneStep(const QString& label, const document::Revision before) {
        expect(session.undoLabel() == label,
               "the step is " + label.toStdString() + ", got " + session.undoLabel().toStdString());
        const auto after = session.snapshot().revision();
        expect(session.undo(), "undo " + label.toStdString());
        expect(session.undoLabel() != label || session.snapshot().revision() != after,
               "undo took one step");
        expect(session.redo(), "redo " + label.toStdString());
        (void)before;
    }
    void inspected() {
        expect(awaitQtCondition("subtitles inspected",
                                [&] { return session.subtitlesInspector().idle(); }),
               "the inspection finishes");
        QCoreApplication::processEvents();
    }
    void ready() {
        expect(awaitQtCondition("preview ready",
                                [&] {
                                    return preview->state().activity == ui::PreviewActivity::Ready &&
                                           preview->state().frame &&
                                           preview->state().frame->desiredIdentity().sourceRevision ==
                                               session.snapshot().revision() &&
                                           preview->state().frame->desiredIdentity().time ==
                                               session.currentTime();
                                }),
               "the preview is ready");
        inspected();
        viewer->repaint();
        QCoreApplication::processEvents();
    }
};

std::vector<document::Cue> threeCues() {
    return {cue(seconds(1), seconds(3), "Hello"), cue(seconds(4), seconds(6), "<i>World</i>"),
            cue(seconds(7), seconds(9), "Third")};
}

void testPropertiesListAndActions(Fixture& f) {
    f.session.selectLayer(f.layer);
    f.setCues(threeCues());
    QCoreApplication::processEvents();
    expect(f.panel() != nullptr, "Properties shows the cue panel for a Subtitles layer");
    if (f.panel() == nullptr)
        return;
    auto* view = f.table();
    expect(view->model()->rowCount() == 3, "the table lists the three cues");
    expect(view->model()->index(1, 4).data().toString() == QStringLiteral("World"),
           "the Text column shows the first line without markup");
    expect(view->model()->index(0, 1).data().toString() == QStringLiteral("24"),
           "the Start column reads in frames (24 at 1 s)");
    // The generic Cues row is the panel: no second Cues row.
    for (auto* row : f.properties->findChildren<QWidget*>())
        expect(row->property("rowLabel").toString() != QStringLiteral("Cues"),
               "no generic Cues row");
    expect(f.find<ui::kit::KLabel>("subtitlesSourceState")->text().contains(QStringLiteral("Typed")),
           "the Source row says the cues are typed");

    // Add at the playhead: a two-second cue (or up to the next cue).
    f.time(seconds(19, 2));
    auto before = f.session.snapshot().revision();
    f.click("subtitlesAddCueButton");
    expect(f.count() == 4 && f.at(3).start == seconds(19, 2) && f.at(3).end == seconds(23, 2),
           "Add puts a two-second cue at the playhead");
    f.oneStep(QStringLiteral("Add Cue"), before);
    f.time(seconds(3, 2));
    f.setCues(threeCues());
    f.time(seconds(7, 2));
    f.click("subtitlesAddCueButton");
    expect(f.count() == 4 && f.at(1).start == seconds(7, 2) && f.at(1).end == seconds(4),
           "Add stops at the next cue");
    // An open cell editor commits on close; leave it before the next step.
    QTest::keyClick(QApplication::focusWidget(), Qt::Key_Escape);

    // Edit cells in place: text, speaker, start.
    f.setCues(threeCues());
    auto* model = f.table()->model();
    before = f.session.snapshot().revision();
    expect(model->setData(model->index(0, 4), QStringLiteral("Hello there"), Qt::EditRole),
           "edit the text cell");
    expect(f.at(0).text == "Hello there", "the text changed");
    f.oneStep(QStringLiteral("Edit Cue Text"), before);
    expect(model->setData(model->index(0, 5), QStringLiteral("ANNA"), Qt::EditRole) &&
               f.at(0).speaker == "ANNA" && f.session.undoLabel() == QStringLiteral("Set Cue Speaker"),
           "edit the speaker cell");
    expect(model->setData(model->index(0, 1), QStringLiteral("36"), Qt::EditRole) &&
               f.at(0).start == seconds(3, 2) && f.session.undoLabel() == QStringLiteral("Set Cue Start"),
           "a frame number in the Start cell starts the cue on that frame");
    QSettings().setValue(QStringLiteral("timeline/time-format"), QStringLiteral("timecode"));
    expect(model->setData(model->index(0, 2), QStringLiteral("00:00:03:12"), Qt::EditRole) &&
               f.at(0).end == seconds(7, 2),
           "a timecode in the End cell ends the cue there");
    f.table()->viewport()->update();
    expect(model->index(0, 2).data().toString() == QStringLiteral("00:00:03:12"),
           "the cells follow the timecode reading");
    QSettings().setValue(QStringLiteral("timeline/time-format"), QStringLiteral("frames"));
    expect(!model->setData(model->index(0, 1), QStringLiteral("soon"), Qt::EditRole),
           "text that is not a time is refused");

    // Delete the selected cue.
    f.setCues(threeCues());
    f.selectRow(1);
    before = f.session.snapshot().revision();
    f.click("subtitlesDeleteCueButton");
    expect(f.count() == 2 && f.at(1).text == "Third", "Delete removes the selected cue");
    f.oneStep(QStringLiteral("Delete Cue"), before);

    // Split at the playhead.
    f.setCues(threeCues());
    f.selectRow(0);
    f.time(seconds(2));
    before = f.session.snapshot().revision();
    f.click("subtitlesSplitCueButton");
    expect(f.count() == 4 && f.at(0).end == seconds(2) && f.at(1).start == seconds(2) &&
               f.at(1).end == seconds(3) && f.at(1).text == "Hello",
           "Split cuts the cue at the playhead");
    f.oneStep(QStringLiteral("Split Cue"), before);
    expect(f.session.undo() && f.count() == 3, "undoing the split restores one cue");

    // Merge with the next.
    f.setCues(threeCues());
    f.selectRow(0);
    before = f.session.snapshot().revision();
    f.click("subtitlesMergeCueButton");
    expect(f.count() == 2 && f.at(0).end == seconds(6) && f.at(0).text == "Hello\n<i>World</i>",
           "Merge joins the cue and the next");
    f.oneStep(QStringLiteral("Merge Cues"), before);

    // Set start and end to the playhead.
    f.setCues(threeCues());
    f.selectRow(0);
    f.time(seconds(1, 2));
    f.click("subtitlesSetStartButton");
    expect(f.at(0).start == seconds(1, 2) && f.session.undoLabel() == QStringLiteral("Set Cue Start"),
           "Set Start");
    f.time(seconds(7, 2));
    f.selectRow(0);
    f.click("subtitlesSetEndButton");
    expect(f.at(0).end == seconds(7, 2) && f.session.undoLabel() == QStringLiteral("Set Cue End"),
           "Set End");

    // The keys while the list holds focus.
    f.setCues(threeCues());
    f.selectRow(2);
    f.table()->setFocus();
    QTest::keyClick(f.table(), Qt::Key_Delete);
    expect(f.count() == 2, "Delete in the list deletes the cue");
    f.selectRow(0);
    f.time(seconds(2));
    QTest::keyClick(f.table(), Qt::Key_B, Qt::ControlModifier);
    expect(f.count() == 3 && f.session.undoLabel() == QStringLiteral("Split Cue"), "Ctrl+B splits");
    f.selectRow(1);
    QTest::keyClick(f.table(), Qt::Key_J, Qt::ControlModifier);
    expect(f.count() == 2 && f.session.undoLabel() == QStringLiteral("Merge Cues"), "Ctrl+J merges");
    f.selectRow(0);
    f.time(seconds(1, 4));
    QTest::keyClick(f.table(), Qt::Key_BracketLeft);
    expect(f.at(0).start == seconds(1, 4), "[ sets the start");

    // Shift timing through its dialog: one second later, every cue.
    f.setCues(threeCues());
    before = f.session.snapshot().revision();
    f.click("subtitlesShiftButton");
    auto* shift = f.panel()->window()->findChild<ui::kit::KDialog*>(QStringLiteral("subtitlesShiftDialog"));
    expect(shift != nullptr, "Shift opens its dialog");
    if (shift != nullptr) {
        shift->findChild<QLineEdit*>(QStringLiteral("subtitlesShiftAmount"))->setText("24");
        expect(shift->activate(ui::kit::KDialog::Role::Accept), "accept the shift");
        QCoreApplication::processEvents();
    }
    expect(f.at(0).start == seconds(2) && f.at(2).end == seconds(10),
           "Shift moves every cue a second later");
    f.oneStep(QStringLiteral("Shift Cue Timing"), before);

    // Scale timing: timed for 25, played at 23.976.
    f.setCues(threeCues());
    before = f.session.snapshot().revision();
    f.click("subtitlesScaleButton");
    auto* scale = f.panel()->window()->findChild<ui::kit::KDialog*>(QStringLiteral("subtitlesScaleDialog"));
    expect(scale != nullptr, "Scale opens its dialog");
    if (scale != nullptr) {
        expect(scale->activate(ui::kit::KDialog::Role::Accept), "accept the scale");
        QCoreApplication::processEvents();
    }
    expect(f.at(0).start == seconds(25025, 24000), "Scale converts 25 to 23.976 exactly");
    f.oneStep(QStringLiteral("Scale Cue Timing"), before);

    // The filter.
    f.setCues(threeCues());
    auto* filter = f.find<QLineEdit>("subtitlesCueFilter");
    filter->setText(QStringLiteral("world"));
    expect(f.table()->model()->rowCount() == 1, "the filter keeps the matching cue");
    filter->clear();
    expect(f.table()->model()->rowCount() == 3, "an empty filter shows every cue");

    // Copy as SRT.
    f.panel()->findChild<QAction*>(QStringLiteral("subtitlesCopySrtAction"))->trigger();
    expect(QApplication::clipboard()->text().contains(
               QStringLiteral("00:00:04,000 --> 00:00:06,000\n<i>World</i>")),
           "Copy as SRT puts the list on the clipboard");
    f.panel()->findChild<QAction*>(QStringLiteral("subtitlesCopyVttAction"))->trigger();
    expect(QApplication::clipboard()->text().startsWith(QStringLiteral("WEBVTT")),
           "Copy as WebVTT");
}

void testChecksShowPerCue(Fixture& f) {
    f.session.selectLayer(f.layer);
    f.setCues({cue(seconds(1), seconds(3), "A line that is far longer than forty-two characters, "
                                           "so the check flags it"),
               cue(seconds(4), seconds(6), "Fine")});
    f.inspected();
    expect(awaitQtCondition("checks shown",
                            [&] { return !f.table()->model()->index(0, 6).data().toString().isEmpty(); }),
           "the checks arrive");
    const auto checks = f.table()->model()->index(0, 6).data().toString();
    expect(checks.contains(QStringLiteral("characters")),
           "the long cue's characters-per-line check shows in its row: " + checks.toStdString());
    expect(f.table()->model()->index(1, 6).data().toString().isEmpty(), "the short cue has none");
}

void testEditAsText(Fixture& f) {
    f.session.selectLayer(f.layer);
    f.setCues(threeCues());
    const auto keep = f.at(2).id;
    auto* toggle = f.find<ui::kit::KButton>("subtitlesEditAsTextButton");
    toggle->click();
    auto* text = f.find<QPlainTextEdit>("subtitlesTextEditor");
    expect(text->isVisible() &&
               text->toPlainText().contains(QStringLiteral("00:00:01,000 --> 00:00:03,000\nHello")),
           "Edit as Text shows the list as SRT");
    // Text that does not parse writes nothing and lists the error with its line.
    const auto before = f.session.snapshot().revision();
    text->setPlainText(QStringLiteral("1\n00:00:05,000 --> 00:00:04,000\nBackwards\n"));
    f.click("subtitlesTextApplyButton");
    f.inspected();
    auto* errors = f.find<ui::kit::KLabel>("subtitlesTextErrors");
    expect(errors->isVisible() && errors->text().contains(QStringLiteral("line 2")),
           "the error is listed with its line: " + errors->text().toStdString());
    expect(f.session.snapshot().revision() == before, "nothing is written until it parses");
    // Valid text: one step; the cue that kept its times and text keeps its id.
    text->setPlainText(QStringLiteral("1\n00:00:02,000 --> 00:00:03,000\nNew\n\n2\n"
                                      "00:00:07,000 --> 00:00:09,000\nThird\n"));
    f.click("subtitlesTextApplyButton");
    f.inspected();
    expect(f.count() == 2 && f.at(0).text == "New" && f.at(1).id == keep,
           "Apply reads the text into the cues, keeping matching ids");
    f.oneStep(QStringLiteral("Edit Cues as Text"), before);
    expect(!text->isVisible(), "Apply returns to the list");
}

void testSourceRowLinkedFile(Fixture& f) {
    f.session.selectLayer(f.layer);
    const auto path = std::filesystem::path(f.files.filePath(QStringLiteral("talk.vtt")).toStdU16String());
    {
        QFile file(QString::fromStdU16String(path.u16string()));
        expect(file.open(QIODevice::WriteOnly), "write a WebVTT file");
        file.write("WEBVTT\n\nNOTE a note the reader drops\n\n00:00:01.000 --> 00:00:02.000\n"
                   "One\n\n00:00:03.000 --> 00:00:04.000\nTwo\n");
    }
    f.setCues(threeCues());
    const auto before = f.session.snapshot().revision();
    ui::loadSubtitlesFile(*f.panel(), path, std::nullopt, {}, true);
    expect(awaitQtCondition("loaded", [&] { return f.count() == 2; }), "Load File reads the cues");
    f.oneStep(QStringLiteral("Load Subtitles"), before);
    f.inspected();
    QCoreApplication::processEvents();
    auto* state = f.find<ui::kit::KLabel>("subtitlesSourceState");
    expect(state->text().contains(QStringLiteral("talk.vtt")) &&
               state->text().contains(QStringLiteral("In sync")),
           "the Source row names the file and its state: " + state->text().toStdString());
    auto* report = f.find<ui::kit::KLabel>("subtitlesLoadReport");
    expect(report->isVisible() && report->text().contains(QStringLiteral("dropped")),
           "the last load's report lists what the reader dropped: " + report->text().toStdString());
    auto* refusal = f.find<ui::kit::KLabel>("subtitlesEditRefusal");
    expect(refusal->isVisible() && refusal->text().contains(QStringLiteral("Unlink")),
           "linked cues say why they cannot be edited here");
    expect(!f.find<ui::kit::KButton>("subtitlesAddCueButton")->isEnabled() &&
               !(f.table()->model()->flags(f.table()->model()->index(0, 4)) & Qt::ItemIsEditable),
           "linked cues are read-only in the list");
    // The node card says the same.
    auto* showing = f.nodes->findChild<QWidget*>(); // keep the editor alive
    (void)showing;
    // Missing file.
    std::filesystem::remove(path);
    f.session.subtitlesInspector().refresh();
    f.inspected();
    expect(awaitQtCondition("missing shown",
                            [&] { return state->text().contains(QStringLiteral("Missing")); }),
           "a missing file is named: " + state->text().toStdString());
    expect(f.count() == 2, "a missing file changes nothing");
    // Unlink: the cues stay and can be edited here.
    f.click("subtitlesUnlinkButton");
    expect(f.session.undoLabel() == QStringLiteral("Unlink Subtitles") &&
               ui::subtitles_editing::cueSource(f.session, f.node).kind ==
                   ui::subtitles_editing::CueSourceKind::Typed &&
               f.count() == 2,
           "Unlink keeps the cues and makes them the project's own");
}

void testTenThousandCuesBuildNoWidgets(Fixture& f) {
    f.session.selectLayer(f.layer);
    f.setCues(threeCues());
    QCoreApplication::processEvents();
    const auto widgetsBefore = f.table()->findChildren<QWidget*>().size();
    std::vector<document::Cue> many;
    for (std::int64_t index = 0; index < 10000; ++index)
        many.push_back(cue(seconds(index * 2, 10), seconds(index * 2 + 1, 10),
                           "Cue " + std::to_string(index)));
    f.setCues(std::move(many));
    QCoreApplication::processEvents();
    f.table()->scrollToBottom();
    QCoreApplication::processEvents();
    expect(f.table()->model()->rowCount() == 10000, "the table lists 10,000 cues");
    expect(f.table()->findChildren<QWidget*>().size() == widgetsBefore,
           "10,000 cues build no widget per cue");
    expect(f.table()->indexWidget(f.table()->model()->index(9999, 4)) == nullptr,
           "no row carries a widget");
}

int subtitlesRow(Fixture& f) {
    auto* lanes = f.timeline->laneRegionForTest();
    for (int row = 0; row < 32; ++row)
        if (!lanes->cueBlocks(row).empty())
            return row;
    return -1;
}

void testTimelineCueBlocks(Fixture& f) {
    f.setCues(threeCues());
    f.session.selectLayer(f.layer);
    f.time(seconds(0));
    QCoreApplication::processEvents();
    auto* lanes = f.timeline->laneRegionForTest();
    const int row = subtitlesRow(f);
    expect(row >= 0, "the Subtitles layer's row shows cue blocks");
    if (row < 0)
        return;
    auto blocks = lanes->cueBlocks(row);
    expect(blocks.size() == 3, "one block per cue");
    // Drag the first cue's body by about half a second: it lands on a frame, one step.
    const auto first = blocks.front().rect;
    const auto pixelsPerSecond = (blocks[1].rect.left() - first.left()) / 3.0;
    const QPoint press = first.center();
    const QPoint to = press + QPoint(static_cast<int>(pixelsPerSecond * 0.5), 0);
    const auto before = f.session.snapshot().revision();
    QTest::mousePress(lanes, Qt::LeftButton, {}, press);
    QTest::mouseMove(lanes, press + QPoint(5, 0));
    QTest::mouseMove(lanes, to);
    expect(f.session.valueEditActive() && f.session.snapshot().revision() == before,
           "the drag previews through the session and writes nothing");
    QTest::mouseRelease(lanes, Qt::LeftButton, {}, to);
    const auto& moved = f.at(0);
    const double start = moved.start.toSeconds();
    expect(start > 1.3 && start < 1.7, "the cue moved about half a second");
    expect(std::abs(start * 24.0 - std::round(start * 24.0)) < 1e-9, "the start is on a frame");
    expect(moved.end.toSeconds() - start == 2.0, "a move keeps the length");
    expect(f.session.selectedCue() && f.session.selectedCue()->cue == moved.id,
           "the dragged cue is selected");
    f.oneStep(QStringLiteral("Move Cue"), before);

    // Retime the second cue's end, snapped to the playhead.
    f.setCues(threeCues());
    f.time(seconds(25, 4));
    QCoreApplication::processEvents();
    blocks = lanes->cueBlocks(row);
    const auto second = blocks[1].rect;
    const QPoint edge(second.right() - 1, second.center().y());
    const QPoint near(edge.x() + static_cast<int>(pixelsPerSecond * 0.25) + 1, edge.y());
    QTest::mousePress(lanes, Qt::LeftButton, {}, edge);
    QTest::mouseMove(lanes, edge + QPoint(3, 0));
    QTest::mouseMove(lanes, near);
    QTest::mouseRelease(lanes, Qt::LeftButton, {}, near);
    expect(f.at(1).start == seconds(4) && f.at(1).end == seconds(25, 4),
           "dragging the end retimes it onto the playhead");
    expect(f.session.undoLabel() == QStringLiteral("Retime Cue"), "one Retime Cue step");

    // Snapping to a neighboring cue's edge: drag the third cue's start onto the second's end.
    f.setCues(threeCues());
    f.time(seconds(0));
    blocks = lanes->cueBlocks(row);
    const auto third = blocks[2].rect;
    const QPoint left(third.left() + 1, third.center().y());
    const QPoint toward(blocks[1].rect.right() + 3, left.y());
    QTest::mousePress(lanes, Qt::LeftButton, {}, left);
    QTest::mouseMove(lanes, left - QPoint(3, 0));
    QTest::mouseMove(lanes, toward);
    QTest::mouseRelease(lanes, Qt::LeftButton, {}, toward);
    expect(f.at(2).start == seconds(6), "the start snaps to the neighboring cue's end");

    // Click a block and Delete: the cue goes, the layer stays.
    blocks = lanes->cueBlocks(row);
    QTest::mouseClick(lanes, Qt::LeftButton, {}, blocks[0].rect.center());
    f.host.activateWindow();
    lanes->setFocus();
    QCoreApplication::processEvents();
    const auto layers = f.session.composition()->graph().layerOutputs().size();
    QTest::keyClick(lanes, Qt::Key_Delete);
    expect(f.count() == 2 && f.session.composition()->graph().layerOutputs().size() == layers,
           "Delete with a cue picked deletes the cue, not the layer");
    expect(f.session.undoLabel() == QStringLiteral("Delete Cue"), "one Delete Cue step");
}

std::optional<QPolygonF> viewerBlock(Fixture& f, const std::uint64_t id) {
    for (const auto& [cueId, polygon] : f.viewer->subtitlesBlocksForTest())
        if (cueId == id)
            return polygon;
    return std::nullopt;
}

void testViewerOverlaysAndGestures(Fixture& f) {
    f.setCues(threeCues());
    f.session.selectLayer(f.layer);
    f.time(seconds(2));
    f.ready();
    const auto id = f.at(0).id;
    expect(awaitQtCondition("viewer block", [&] { return viewerBlock(f, id).has_value(); }),
           "the Viewer knows the shown cue's block");
    const auto block = viewerBlock(f, id);
    if (!block)
        return;
    const auto centre = block->boundingRect().center().toPoint();
    // A click selects the cue.
    f.session.clearCueSelection();
    QTest::mouseClick(f.viewer, Qt::LeftButton, {}, centre);
    expect(f.session.selectedCue() && f.session.selectedCue()->cue == id,
           "a click on the block selects its cue");
    // A drag moves the node's Offset, one step on release.
    const auto offset = ui::nodeRoleParameter(f.session, f.node, "offset");
    const auto before = f.session.snapshot().revision();
    QTest::mousePress(f.viewer, Qt::LeftButton, {}, centre);
    QTest::mouseMove(f.viewer, centre + QPoint(4, 0));
    QTest::mouseMove(f.viewer, centre + QPoint(30, -20));
    QTest::mouseRelease(f.viewer, Qt::LeftButton, {}, centre + QPoint(30, -20));
    const auto value = offset ? f.session.constantVec2Value(*offset) : std::nullopt;
    expect(value && value->x > 0.0 && value->y < 0.0, "dragging the block moves the Offset");
    expect(f.session.snapshot().revision() != before && f.session.canUndo(),
           "the Offset drag is one step");
    expect(f.session.undo(), "undo the drag");
    f.ready();
    // A double-click edits the cue's text in place.
    const auto again = viewerBlock(f, id);
    if (!again)
        return;
    QTest::mouseDClick(f.viewer, Qt::LeftButton, {}, again->boundingRect().center().toPoint());
    auto* editor = qobject_cast<QPlainTextEdit*>(f.viewer->cueEditorForTest());
    expect(editor != nullptr && editor->toPlainText() == QStringLiteral("Hello"),
           "a double-click opens the cue's text over its block");
    if (editor != nullptr) {
        editor->setPlainText(QStringLiteral("Hello, edited"));
        QTest::keyClick(editor, Qt::Key_Return, Qt::ControlModifier);
        QCoreApplication::processEvents();
        expect(f.at(0).text == "Hello, edited" &&
                   f.session.undoLabel() == QStringLiteral("Edit Cue Text"),
               "Ctrl+Return commits the text as one Edit Cue Text step");
    }
    // Track-only cues show as a guide while the Guides option is on (the default).
    auto* option = f.viewer->findChild<QAction*>(QStringLiteral("viewerTrackOnlySubtitlesAction"));
    expect(option != nullptr && option->isChecked(), "Show Track-Only Subtitles is on by default");
    const auto burn = ui::nodeRoleParameter(f.session, f.node, "burnIn");
    expect(burn && f.session.setParameterValue(*burn, document::ParameterValue{false},
                                               QStringLiteral("Burn In")),
           "turn Burn in off");
    f.session.clearCueSelection();
    f.session.clearSelection();
    f.ready();
    const auto guide = viewerBlock(f, id);
    expect(guide.has_value(), "a track-only cue still has its block");
    if (guide && option != nullptr) {
        const auto bounds = guide->boundingRect().toAlignedRect();
        const auto withGuide = f.viewer->grab().toImage();
        option->setChecked(false);
        f.viewer->repaint();
        const auto without = f.viewer->grab().toImage();
        const auto scale = withGuide.devicePixelRatio();
        int differing = 0;
        for (int y = bounds.top(); y <= bounds.bottom(); ++y)
            for (int x = bounds.left(); x <= bounds.right(); ++x) {
                const QPoint device(static_cast<int>(x * scale), static_cast<int>(y * scale));
                if (withGuide.valid(device) && withGuide.pixel(device) != without.pixel(device))
                    ++differing;
            }
        expect(differing > 0, "the guide draws the track-only cue over the frame");
        option->setChecked(true);
    }
    expect(f.session.setParameterValue(*burn, document::ParameterValue{true},
                                       QStringLiteral("Burn In")),
           "burn in again");
}

void testNodeCard(Fixture& f) {
    f.setCues(threeCues());
    f.time(seconds(2));
    QCoreApplication::processEvents();
    auto* showing = f.nodes->graphScene()->nodeFieldForTest(f.node, QStringLiteral("nodeSubtitlesShowing"));
    expect(showing != nullptr, "the card reads out the shown cue");
    auto* label = qobject_cast<QLabel*>(showing);
    expect(label != nullptr && label->text() == QStringLiteral("Hello"),
           "the card shows the cue at the playhead");
    f.time(seconds(7, 2));
    QCoreApplication::processEvents();
    expect(label != nullptr && label->text() == QStringLiteral("No cue at the playhead"),
           "and says when none shows");
}

void testImportAddsALinkedLayer(Fixture& f) {
    const auto path = f.files.filePath(QStringLiteral("dialogue.srt"));
    {
        QFile file(path);
        expect(file.open(QIODevice::WriteOnly), "write an SRT file");
        file.write("1\n00:00:00,500 --> 00:00:01,500\nImported\n");
    }
    const auto layers = f.session.composition()->graph().layerOutputs().size();
    f.assets->importFiles({path});
    expect(awaitQtCondition("imported", [&] {
               return f.session.composition()->graph().layerOutputs().size() == layers + 1;
           }),
           "importing a subtitle file adds a Subtitles layer");
    expect(f.session.undoLabel() == QStringLiteral("Add Subtitles Layer"), "one step");
    const auto* selected = std::get_if<document::LayerId>(&f.session.selection().primary);
    const auto node = selected ? ui::subtitles_editing::subtitlesNodeOfLayer(f.session, *selected)
                               : std::nullopt;
    const auto source = node ? ui::subtitles_editing::cueSource(f.session, *node)
                             : ui::subtitles_editing::CueSource{};
    expect(source.kind == ui::subtitles_editing::CueSourceKind::Linked &&
               source.fileName == QStringLiteral("dialogue.srt") && source.list->cues.size() == 1,
           "the layer follows the file");
    expect(f.session.snapshot().project().assets().empty(), "a subtitle file is not an asset");
    expect(f.session.undo(), "undo the import");
}

// A label, button or column heading whose text is wider than the room it was given is cut
// short (or elided).
int truncatedText(const QWidget& root) {
    int cut = 0;
    const auto report = [&](const QString& text) {
        ++cut;
        std::cout << "cut: " << text.toStdString() << '\n';
    };
    for (const auto* label : root.findChildren<QLabel*>()) {
        if (!label->isVisible() || label->text().isEmpty() || label->wordWrap())
            continue;
        if (label->fontMetrics().horizontalAdvance(label->text()) > label->contentsRect().width() + 1)
            report(label->text());
    }
    // The cue panel's buttons show their text (other surfaces' tool buttons show icons).
    const auto* panel = root.findChild<QWidget*>(QStringLiteral("subtitlesCuePanel"));
    for (const auto* button :
         panel ? panel->findChildren<QAbstractButton*>() : QList<QAbstractButton*>{}) {
        if (!button->isVisible() || button->text().isEmpty())
            continue;
        if (button->fontMetrics().horizontalAdvance(QString(button->text()).remove('&')) >
            button->contentsRect().width())
            report(button->text());
    }
    for (const auto* header : root.findChildren<QHeaderView*>()) {
        if (!header->isVisible() || header->orientation() != Qt::Horizontal)
            continue;
        const auto* model = header->model();
        for (int section = 0; model != nullptr && section < header->count(); ++section) {
            const auto text = model->headerData(section, Qt::Horizontal).toString();
            if (!header->isSectionHidden(section) &&
                header->fontMetrics().horizontalAdvance(text) > header->sectionSize(section))
                report(text);
        }
    }
    return cut;
}

// The evidence captures at the process's scale (QT_SCALE_FACTOR 1, 1.5 or 2): Properties with the
// cue list, the node card, the Timeline with cue blocks and the Viewer's outlines, each checked
// for cut text.
int capture(const QString& directory) {
    ui::kit::installAfterlabTheme(*qApp);
    QDir().mkpath(directory);
    const auto dpr = QStringLiteral("dpr") + qEnvironmentVariable("QT_SCALE_FACTOR", QStringLiteral("1"));
    Fixture f;
    f.setCues({cue(seconds(1), seconds(3), "Hello there,\nthis is the first cue"),
               cue(seconds(4), seconds(6), "<i>A second cue in italics</i>"),
               cue(seconds(7), seconds(9), "A third cue")});
    const auto speaker = f.at(0).id;
    {
        commands::CueEdit edit;
        edit.speaker = std::string("ANNA");
        (void)ui::subtitles_editing::editCue(f.session, f.node, speaker, edit, QStringLiteral("Speaker"));
    }
    f.session.selectLayer(f.layer);
    f.session.selectCue(f.node, f.at(0).id);
    f.time(seconds(2));
    f.ready();
    f.host.resize(1600, 1500);
    QCoreApplication::processEvents();
    f.ready();
    int cut = 0;
    const auto save = [&](const QImage& image, const QString& surface) {
        const auto name = QStringLiteral("752-%1-%2").arg(surface, dpr);
        expect(!image.isNull() && image.save(directory + '/' + name + ".png"), "save " + name.toStdString());
        std::cout << name.toStdString() << ' ' << image.width() << 'x' << image.height() << '\n';
    };
    cut += truncatedText(*f.properties);
    save(f.properties->grab().toImage(), QStringLiteral("properties"));
    if (auto* panel = f.panel())
        save(panel->grab().toImage(), QStringLiteral("cue-panel"));
    cut += truncatedText(*f.timeline);
    save(f.timeline->grab().toImage(), QStringLiteral("timeline"));
    cut += truncatedText(*f.viewer);
    save(f.viewer->grab().toImage(), QStringLiteral("viewer"));
    if (auto* showing = f.nodes->graphScene()->nodeFieldForTest(f.node, QStringLiteral("nodeSubtitlesShowing")))
        if (const auto* label = qobject_cast<QLabel*>(showing);
            label != nullptr && label->fontMetrics().horizontalAdvance(label->text()) >
                                    label->contentsRect().width() + 1) {
            ++cut;
            std::cout << "cut: " << label->text().toStdString() << '\n';
        }
    if (auto* view = f.nodes->findChild<QGraphicsView*>()) {
        QRectF bounds;
        for (auto* item : view->scene()->items())
            if (auto* card = dynamic_cast<ui::node_editor::NodeItem*>(item); card && card->id() == f.node)
                bounds = card->sceneBoundingRect();
        expect(!bounds.isEmpty(), "the Subtitles card is in the graph");
        const auto source = bounds.adjusted(-16, -16, 16, 16);
        const qreal ratio = f.nodes->devicePixelRatioF();
        QImage image((source.size() * ratio).toSize(), QImage::Format_ARGB32_Premultiplied);
        image.setDevicePixelRatio(ratio);
        image.fill(f.nodes->palette().window().color());
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing);
        view->scene()->render(&painter, QRectF(QPointF(), source.size()), source);
        painter.end();
        save(image, QStringLiteral("node-card"));
    }
    // The track-only guide (Burn in off, nothing selected).
    if (const auto burn = ui::nodeRoleParameter(f.session, f.node, "burnIn"))
        (void)f.session.setParameterValue(*burn, document::ParameterValue{false}, QStringLiteral("Burn In"));
    f.session.clearCueSelection();
    f.session.clearSelection();
    f.ready();
    save(f.viewer->grab().toImage(), QStringLiteral("viewer-track-only"));
    // Edit as Text.
    f.session.selectLayer(f.layer);
    QCoreApplication::processEvents();
    if (auto* toggle = f.find<ui::kit::KButton>("subtitlesEditAsTextButton"))
        toggle->click();
    QCoreApplication::processEvents();
    cut += truncatedText(*f.properties);
    if (auto* panel = f.panel())
        save(panel->grab().toImage(), QStringLiteral("cue-panel-text"));
    expect(cut == 0, "no text is cut short");
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("bloom-subtitles-ui-test"));
    if (argc > 2 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("--capture"))
        return capture(QString::fromLocal8Bit(argv[2]));
    {
        Fixture f;
        testPropertiesListAndActions(f);
        testChecksShowPerCue(f);
        testEditAsText(f);
        testSourceRowLinkedFile(f);
        testTenThousandCuesBuildNoWidgets(f);
        testTimelineCueBlocks(f);
        testViewerOverlaysAndGestures(f);
        testNodeCard(f);
        testImportAddsALinkedLayer(f);
    }
    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return EXIT_FAILURE;
    }
    std::cout << "bloom.ui.subtitles passed\n";
    return EXIT_SUCCESS;
}
