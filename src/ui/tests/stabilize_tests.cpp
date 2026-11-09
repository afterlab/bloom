// #513 Stabilize in the UI: its card (a pad of the two points, the Track button, progress and status
// line, sized and laid out with the node's own rows), and the tracking job end to end through the
// controller: refused with a reason when it cannot run, run off the UI thread over a picture with
// detail, its progress and its result stored as one undoable edit (a data block and the keys), a
// cancel that stores nothing, and a track that loses its feature stored as far as it got.

#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/project.hpp>
#include <bloom/document/warp_nodes.hpp>
#include <bloom/runtime/node_definition_registry.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/progress_bar.hpp>
#include <bloom/ui/kit/point_pad.hpp>
#include <bloom/ui/node_card_widget.hpp>
#include <bloom/ui/node_controls_widget.hpp>
#include <bloom/ui/stabilize_widget.hpp>
#include <bloom/ui/task_ui_bridge.hpp>
#include <bloom/ui/track_controller.hpp>

#include <QApplication>
#include <QLabel>
#include <QElapsedTimer>

#include <chrono>
#include <iostream>
#include <string>

namespace {
using namespace bloom;
using namespace std::chrono_literals;

int failures = 0;
void expect(const bool value, const std::string& message) {
    if (!value) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

document::NodeId addNode(ui::CompositionSession& session, const std::string_view type) {
    commands::Transaction transaction("Add", session.snapshot().revision());
    transaction.emplace<commands::AddNode>(session.compositionId(), std::string(type),
                                           document::Vec2d{});
    const auto result = session.executeNodeTransaction(std::move(transaction));
    return result.outputId<document::NodeId>(commands::kAddNodeOutput).value_or(document::NodeId{});
}

void connect(ui::CompositionSession& session, const document::NodeId from, const document::NodeId to) {
    commands::Transaction transaction("Connect", session.snapshot().revision());
    transaction.emplace<commands::ConnectPorts>(
        session.compositionId(), document::OutputPortRef{from, "image"},
        document::InputPortRef{document::NodeInputRef{to, "input"}});
    (void)session.executeNodeTransaction(std::move(transaction));
}

struct Fixture final {
    document::NewProject project{
        document::makeNewProject("Stabilize", "Main", core::RationalTime::fromInteger(2))};
    document::Document document{std::move(project.project)};
    commands::CommandStack stack{document};
    ui::CompositionSession session{document, stack, project.initialCompositionId};
    runtime::TaskScheduler scheduler{runtime::TaskSchedulerConfig::defaults()};
    ui::TaskUiBridge bridge{scheduler, nullptr, 1ms};
    document::NodeDefinitionRegistry registry;
    std::unique_ptr<ui::TrackController> controller;
    document::NodeId stabilize;

    explicit Fixture(const char* source) {
        if (!runtime::registerBuiltInNodeDefinitions(registry))
            std::abort();
        registry.freeze();
        controller = std::make_unique<ui::TrackController>(session, scheduler, bridge, registry);
        stabilize = addNode(session, document::kStabilizeNodeType);
        if (source != nullptr)
            connect(session, addNode(session, source), stabilize);
    }
    ~Fixture() {
        controller.reset();
        scheduler.beginShutdown();
        QElapsedTimer timer;
        timer.start();
        while (!scheduler.isQuiescent() && timer.elapsed() < 10'000)
            QApplication::processEvents(QEventLoop::AllEvents, 10);
    }
    // Runs the event loop until the job is no longer running.
    bool wait() {
        QElapsedTimer timer;
        timer.start();
        while (controller->running() && timer.elapsed() < 120'000)
            QApplication::processEvents(QEventLoop::AllEvents, 10);
        return !controller->running();
    }
    [[nodiscard]] std::size_t blocks() const {
        return session.snapshot().project().dataBlocks().size();
    }
};

void testCard() {
    Fixture fixture(nullptr);
    auto* card = ui::makeNodeCardWidget(fixture.session, fixture.stabilize,
                                        document::kStabilizeNodeType, true, nullptr);
    auto* stabilizeCard = qobject_cast<ui::StabilizeCardWidget*>(card);
    expect(stabilizeCard != nullptr, "Stabilize draws its card widget");
    if (stabilizeCard == nullptr)
        return;
    stabilizeCard->resize(360, stabilizeCard->heightAt(360));
    stabilizeCard->show();
    QApplication::processEvents();
    expect(stabilizeCard->padRow() != nullptr && stabilizeCard->padRow()->pad()->points().size() == 1 &&
               stabilizeCard->padRow()->pad()->points()[0].label == QStringLiteral("Track Point"),
           "the pad has the track point, named");
    expect(stabilizeCard->trackButton()->text() == QStringLiteral("Track") &&
               stabilizeCard->trackButton()->isEnabled(),
           "the button starts a track");
    expect(stabilizeCard->trackButton()->geometry().bottom() <= stabilizeCard->height() &&
               stabilizeCard->statusLabel()->geometry().bottom() <= stabilizeCard->height() + 1,
           "the controls fit inside the card");
    // Not connected: starting says why and changes nothing.
    stabilizeCard->trackButton()->click();
    QApplication::processEvents();
    expect(stabilizeCard->statusLabel()->text().contains(QStringLiteral("Connect a picture")) &&
               fixture.blocks() == 0,
           "a node with no picture says so and stores nothing");
    delete stabilizeCard;
}

void testTrack() {
    Fixture fixture("bloom.checker-source");
    const auto before = fixture.session.snapshot();
    auto* card = qobject_cast<ui::StabilizeCardWidget*>(ui::makeNodeCardWidget(
        fixture.session, fixture.stabilize, document::kStabilizeNodeType, true, nullptr));
    card->resize(360, card->heightAt(360));
    card->show();
    expect(fixture.controller->start(fixture.stabilize) && fixture.controller->running(),
           "a connected node starts a track");
    QApplication::processEvents();
    expect(card->trackButton()->text() == QStringLiteral("Cancel"), "the button offers Cancel while tracking");
    const auto grab = [&](const char* name) {
        const auto directory = qEnvironmentVariable("BLOOM_GRAMMAR_ARTIFACT_DIR");
        if (!directory.isEmpty())
            card->grab().save(directory + QStringLiteral("/") + QString::fromLatin1(name));
    };
    grab("stabilize-card-tracking.png");
    expect(!fixture.controller->start(fixture.stabilize), "a second track does not start meanwhile");
    expect(fixture.wait(), "the track finishes");
    const auto status = fixture.controller->status();
    expect(status.state == ui::TrackController::State::Stored && status.done == status.total &&
               status.total > 1,
           "a still picture with detail is tracked through every frame");
    QApplication::processEvents();
    expect(card->statusLabel()->text().contains(QStringLiteral("Tracked")) &&
               card->trackButton()->text() == QStringLiteral("Track"),
           "the card says how many frames were tracked");
    grab("stabilize-card-tracked.png");
    delete card;
    expect(fixture.blocks() == 1, "the track is stored as a data block");
    const auto* composition = fixture.session.composition();
    const auto* node = composition->graph().findNode(fixture.stabilize);
    document::ParameterId position;
    for (const auto& binding : node->parameters)
        if (binding.role == "tracked")
            position = binding.parameterId;
    const auto* record = composition->parameters().find(position);
    expect(record != nullptr && std::holds_alternative<document::AnimationCurveSource>(record->source),
           "the position is keyed");
    expect(fixture.session.undo() && fixture.blocks() == 0, "one undo removes the block and the keys");
    static_cast<void>(before);
}

void testCancel() {
    Fixture fixture("bloom.checker-source");
    expect(fixture.controller->start(fixture.stabilize), "the track starts");
    fixture.controller->cancel();
    expect(fixture.wait(), "a cancelled track ends");
    expect(fixture.controller->status().state == ui::TrackController::State::Cancelled &&
               fixture.blocks() == 0,
           "a cancelled track stores nothing");
}

void testNoDetail() {
    Fixture fixture("bloom.solid-source");
    expect(fixture.controller->start(fixture.stabilize), "the track starts over a flat picture");
    expect(fixture.wait(), "the track ends");
    const auto status = fixture.controller->status();
    expect(status.state == ui::TrackController::State::Failed && status.message.contains(QStringLiteral("could not")) &&
               fixture.blocks() == 0,
           "a picture with nothing to follow is reported and stores nothing");
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    testCard();
    testTrack();
    testCancel();
    testNoDetail();
    if (failures != 0) {
        std::cerr << failures << " stabilize expectation(s) failed\n";
        return 1;
    }
    return 0;
}
