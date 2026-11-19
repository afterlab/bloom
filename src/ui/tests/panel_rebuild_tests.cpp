// #744: agent and script commits rebuild the panels at most once per display frame, the artist's
// own edit and a rebind rebuild them at once, and a retirement of undo steps is a notice (no
// refusal) reported once per burst, naming the bound reached. Over the real Timeline, Node editor,
// Properties panel, keyframe panel and Assets panel, built as the application builds them.
#include "surface_harness.hpp"

#include <bloom/commands/node_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/ui/assets_editor.hpp>

#include <QSignalSpy>

#include <algorithm>
#include <string>

namespace bloom::ui::surface_test {
namespace {

struct Counts final {
    std::uint64_t timeline = 0;
    std::uint64_t nodes = 0;
    std::uint64_t properties = 0;
    std::uint64_t keyframes = 0;
    std::uint64_t assets = 0;
};

class PanelRig final {
  public:
    Surfaces surfaces;
    AssetsEditor assets{surfaces.session};
    TimelineKeyframePanel* keyframes = nullptr;

    PanelRig() {
        keyframes = surfaces.timeline->findChild<TimelineKeyframePanel*>();
        // The panels need rows to rebuild.
        expect(surfaces.session.addSolidLayer("First", {1.0, 0.0, 0.0, 1.0}) &&
                   surfaces.session.addSolidLayer("Second", {0.0, 1.0, 0.0, 1.0}),
               "the fixture's layers are added");
        QCoreApplication::processEvents();
    }

    [[nodiscard]] Counts counts() const {
        return {surfaces.timeline->rebuildCount(), surfaces.nodes->rebuildCount(),
                surfaces.properties->work().fullRebuilds,
                keyframes != nullptr ? keyframes->rebuildCount() : 0, assets.rowRebuildCount()};
    }

    // One commit made the way the live session marshal applies an agent's (or a script's).
    bool commit(const commands::CommandOrigin origin, const int index) {
        auto& session = surfaces.session;
        const auto layer = session.composition()->graph().layerOutputs().front().layerId;
        commands::Transaction transaction("Rename", surfaces.document.snapshot().revision());
        transaction.setOrigin(origin);
        transaction.emplace<commands::RenameLayer>(session.compositionId(), layer,
                                                   "Renamed " + std::to_string(index));
        // A composition name is an Assets row, so the Assets panel has something to rebuild.
        transaction.emplace<commands::SetCompositionName>(session.compositionId(),
                                                          "Main " + std::to_string(index));
        return session.executeTransaction(std::move(transaction)).changed();
    }

    [[nodiscard]] bool awaitRebuild() {
        return test_support::awaitQtCondition(
            "panel rebuild", [&] { return !surfaces.session.panelRebuildPending(); });
    }
};

[[nodiscard]] bool each(const Counts& before, const Counts& after, const std::uint64_t added,
                        const bool keyframePanel) {
    return after.timeline == before.timeline + added && after.nodes == before.nodes + added &&
           after.properties == before.properties + added &&
           (!keyframePanel || after.keyframes == before.keyframes + added) &&
           after.assets == before.assets + added;
}

[[nodiscard]] bool timelineShows(const Surfaces& surfaces, const QString& name) {
    auto* stack = surfaces.layerStack();
    return stack != nullptr &&
           std::ranges::any_of(stack->entries(),
                               [&](const TimelineLayerEntry& entry) { return entry.name == name; });
}

// 100 agent commits in one burst: every commit is published, and each panel rebuilds once, when
// the frame ends, not once per commit.
void burstRebuildsOncePerFrame() {
    PanelRig rig;
    auto& session = rig.surfaces.session;
    const bool keyframePanel = rig.keyframes != nullptr;
    expect(keyframePanel, "the Timeline carries its keyframe panel");
    QSignalSpy published(&session, &CompositionSession::snapshotChanged);
    QSignalSpy due(&session, &CompositionSession::panelRebuildDue);
    const auto before = rig.counts();
    bool committed = true;
    for (int index = 0; index < 100; ++index)
        committed = rig.commit(commands::CommandOrigin::Agent, index) && committed;
    expect(committed && published.count() == 100,
           "every agent commit is adopted and published at once");
    expect(due.count() == 0 && each(before, rig.counts(), 0, keyframePanel),
           "no panel rebuilds while the burst runs");
    expect(session.panelRebuildPending(), "one rebuild waits for the frame");
    expect(rig.awaitRebuild(), "the frame ends");
    expect(due.count() == 1 && each(before, rig.counts(), 1, keyframePanel),
           "each panel rebuilds once for the 100 commits");
    const auto settled = rig.counts();
    QTest::qWait(3 * static_cast<int>(CompositionSession::kPanelRebuildInterval.count()));
    expect(each(settled, rig.counts(), 0, keyframePanel), "and nothing more once it settled");

    // Three bursts in three frames: one rebuild per frame.
    for (int frame = 0; frame < 3; ++frame) {
        for (int index = 0; index < 10; ++index)
            (void)rig.commit(commands::CommandOrigin::Script, 1000 + frame * 10 + index);
        expect(rig.awaitRebuild(), "the burst's frame ends");
    }
    expect(each(settled, rig.counts(), 3, keyframePanel),
           "script commits rebuild the panels once per frame too");
}

// The artist's own edit rebuilds at once and takes a waiting rebuild with it.
void artistEditRebuildsAtOnce() {
    PanelRig rig;
    auto& session = rig.surfaces.session;
    const auto before = rig.counts();
    expect(rig.commit(commands::CommandOrigin::User, 1), "the artist's edit applies");
    expect(each(before, rig.counts(), 1, rig.keyframes != nullptr) && !session.panelRebuildPending(),
           "the artist's edit rebuilds every panel at once");
    (void)rig.commit(commands::CommandOrigin::Agent, 2);
    (void)rig.commit(commands::CommandOrigin::Agent, 3);
    expect(session.panelRebuildPending(), "an agent burst waits for the frame");
    expect(rig.commit(commands::CommandOrigin::User, 4) && !session.panelRebuildPending() &&
               each(before, rig.counts(), 2, rig.keyframes != nullptr),
           "the artist's edit rebuilds at once, the waiting rebuild with it");
    expect(timelineShows(rig.surfaces, QStringLiteral("Renamed 4")),
           "the Timeline shows the newest name");
    QTest::qWait(3 * static_cast<int>(CompositionSession::kPanelRebuildInterval.count()));
    expect(each(before, rig.counts(), 2, rig.keyframes != nullptr),
           "and no stale rebuild follows");
}

// A rebind (New, Open) drops a waiting rebuild into its own: no row of the old project survives it.
void rebindDropsTheWait() {
    PanelRig rig;
    auto& session = rig.surfaces.session;
    (void)rig.commit(commands::CommandOrigin::Agent, 1);
    expect(session.panelRebuildPending(), "an agent commit waits for the frame");
    auto initial = document::makeNewProject("Other", "Main", core::RationalTime::fromInteger(5));
    document::Document other(std::move(initial.project));
    commands::CommandStack otherStack(other);
    const auto before = rig.counts();
    session.rebind(other, otherStack, initial.initialCompositionId);
    expect(!session.panelRebuildPending(), "the rebind rebuilds at once");
    expect(rig.surfaces.layerStack() != nullptr && rig.surfaces.layerStack()->rowCount() == 0,
           "no row of the old project outlives the rebind");
    const auto rebound = rig.counts();
    expect(rebound.timeline > before.timeline, "the Timeline rebuilt for the new project");
    QTest::qWait(3 * static_cast<int>(CompositionSession::kPanelRebuildInterval.count()));
    expect(rig.counts().timeline == rebound.timeline, "the old wait never fires afterwards");
    // Back to the fixture's own document before it is destroyed.
    session.rebind(rig.surfaces.document, rig.surfaces.stack, rig.surfaces.initial.initialCompositionId);
}

// #744 A5: retiring undo steps is information: no refusal, once per burst, and it names the bound.
void undoRetirementIsANotice() {
    auto initial = document::makeNewProject("Undo", "Main", core::RationalTime::fromInteger(5));
    document::Document doc(std::move(initial.project));
    commands::CommandStack stack(doc, {.maxEntries = 2, .maxChargedBytes = 512ULL * 1024 * 1024});
    CompositionSession session(doc, stack, initial.initialCompositionId);
    session.setUndoRetirementBurstGapForTest(std::chrono::milliseconds{150});
    QSignalSpy notices(&session, &CompositionSession::noticePosted);
    QSignalSpy refusals(&session, &CompositionSession::commandRejected);
    const auto edit = [&](const commands::CommandOrigin origin, const int index) {
        commands::Transaction transaction("Add", doc.snapshot().revision());
        transaction.setOrigin(origin);
        transaction.emplace<commands::AddSolidLayer>(initial.initialCompositionId,
                                                     "Layer " + std::to_string(index),
                                                     core::Color4d{1.0, 1.0, 1.0, 1.0});
        return session.executeTransaction(std::move(transaction));
    };
    (void)edit(commands::CommandOrigin::User, 0);
    (void)edit(commands::CommandOrigin::User, 1);
    expect(notices.count() == 0, "nothing is retired while the history has room");
    for (int index = 2; index < 7; ++index)
        expect(edit(commands::CommandOrigin::User, index).historyEntriesDropped == 1,
               "each further edit retires the oldest step");
    expect(notices.count() == 1 && refusals.count() == 0,
           "five retirements in a burst are one notice and no refusal");
    expect(!notices.isEmpty() &&
               notices.front().front().toString().contains(QStringLiteral("2-step limit")) &&
               !notices.front().front().toString().contains(QStringLiteral("memory")),
           "the notice names the step limit: " +
               (notices.isEmpty() ? std::string() : notices.front().front().toString().toStdString()));
    QTest::qWait(300);
    (void)edit(commands::CommandOrigin::User, 7);
    expect(notices.count() == 2, "a retirement after a quiet gap is a new burst");
    QTest::qWait(300);
    (void)edit(commands::CommandOrigin::Agent, 8);
    expect(notices.count() == 2 && refusals.count() == 0,
           "an agent's retirement says nothing (#746)");

    // The memory bound, named as such.
    commands::CommandResult memory;
    memory.historyEntriesDropped = 1;
    memory.historyMemoryLimitReached = true;
    const auto memoryText = CompositionSession::undoRetirementNotice(
        memory, {.maxEntries = 256, .maxChargedBytes = 512ULL * 1024 * 1024});
    expect(memoryText.contains(QStringLiteral("512 MiB memory limit")) &&
               !memoryText.contains(QStringLiteral("step limit")),
           "the memory bound is named with its size: " + memoryText.toStdString());
    memory.historyEntryLimitReached = true;
    const auto bothText = CompositionSession::undoRetirementNotice(
        memory, {.maxEntries = 256, .maxChargedBytes = 512ULL * 1024 * 1024});
    expect(bothText.contains(QStringLiteral("256-step limit")) &&
               bothText.contains(QStringLiteral("512 MiB memory limit")),
           "both bounds are named when both were reached");
}

} // namespace
} // namespace bloom::ui::surface_test

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    using namespace bloom::ui::surface_test;
    burstRebuildsOncePerFrame();
    artistEditRebuildsAtOnce();
    rebindDropsTheWait();
    undoRetirementIsANotice();
    failures += bloom::test_support::awaitFailureCount();
    if (failures != 0)
        std::cerr << failures << " failure(s)\n";
    return failures == 0 ? 0 : 1;
}
