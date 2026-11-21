#include <bloom/commands/operations.hpp>
#include <bloom/commands/subtitles_operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/cue_list.hpp>
#include <bloom/host/linked_subtitle_files.hpp>
#include <bloom/host/project_session.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/subtitles/subtitles.hpp>

#include "unique_temp_dir.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <source_location>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

// bloom.host.linked-subtitle-files (#752): the state of a linked subtitle file, the headless
// warnings, and the monitor that follows the files and publishes one host-origin reload.
namespace {

namespace commands = bloom::commands;
namespace document = bloom::document;
namespace host = bloom::host;
namespace runtime = bloom::runtime;
namespace subtitles = bloom::subtitles;
namespace fs = std::filesystem;

class Expectations final {
  public:
    void expect(const bool condition, const std::string_view message,
                const std::source_location location = std::source_location::current()) {
        if (condition)
            return;
        ++failures_;
        std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
    }
    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

// A fixture folder of the test's own (#749): never a fixed name in the system temporary folder.
class TempDirectory final {
  public:
    [[nodiscard]] const fs::path& path() const noexcept { return folder_.path(); }

  private:
    bloom::test_support::UniqueTempDir folder_{"bloom-linked-subtitles"};
};

constexpr std::string_view kOriginal =
    "1\n00:00:01,000 --> 00:00:02,000\none\n\n2\n00:00:03,000 --> 00:00:04,000\ntwo\n";
constexpr std::string_view kEdited =
    "1\n00:00:01,000 --> 00:00:02,000\none\n\n2\n00:00:03,000 --> 00:00:04,500\ntwo changed\n\n"
    "3\n00:00:05,000 --> 00:00:06,000\nthree\n";

void writeFile(const fs::path& path, const std::string_view text) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream << text;
}

// A session with one composition holding a Subtitles layer whose list follows `file`.
struct Fixture final {
    host::ProjectSessionIdentitySource identities;
    std::optional<host::ProjectSession> session;
    document::CompositionId composition;
    document::NodeId node;
    fs::path directory;
    fs::path file;
};

[[nodiscard]] bool build(Fixture& fixture, const fs::path& directory, const std::string_view text) {
    fixture.directory = directory;
    fixture.file = directory / "titles.srt";
    writeFile(fixture.file, text);
    auto created = host::ProjectSession::createNew(
        fixture.identities,
        {.projectName = "Linked",
         .compositionName = "Main",
         .duration = bloom::core::RationalTime::fromInteger(10),
         .format = {}});
    if (!created)
        return false;
    fixture.session.emplace(std::move(created).takeSession());
    const auto decoded = fixture.session->decodedSnapshot();
    if (!decoded)
        return false;
    fixture.composition = decoded.snapshot().project().compositions().front().id();

    commands::Transaction add("Add Subtitles");
    add.emplace<commands::AddSubtitlesLayer>(fixture.composition, std::string("Subtitles"),
                                             document::CueListValue{});
    const auto added = fixture.session->execute(std::move(add));
    if (!added.completed())
        return false;
    const auto node = added.command->outputId<document::NodeId>("subtitlesNode");
    if (!node)
        return false;
    fixture.node = *node;

    auto loaded = subtitles::loadFile({.path = fixture.file,
                                       .projectDirectory = directory,
                                       .options = {},
                                       .link = true});
    auto* file = std::get_if<subtitles::FileLoad>(&loaded);
    if (file == nullptr)
        return false;
    commands::Transaction load("Load Subtitles");
    load.emplace<commands::ReplaceCues>(fixture.composition, fixture.node,
                                        std::move(file->loaded.cues),
                                        commands::ReplaceCues::Kind::Load);
    return fixture.session->execute(std::move(load)).completed();
}

[[nodiscard]] document::Snapshot snapshotOf(Fixture& fixture) {
    const auto decoded = fixture.session->decodedSnapshot();
    return decoded.snapshot();
}

[[nodiscard]] const document::CueListValue* listOf(const document::Snapshot& snapshot,
                                                   const Fixture& fixture) {
    const auto* composition = snapshot.project().findComposition(fixture.composition);
    if (composition == nullptr)
        return nullptr;
    const auto located = commands::locateCueList(*composition, fixture.node, true);
    const auto* location = std::get_if<commands::CueListLocation>(&located);
    return location == nullptr ? nullptr : location->list;
}

// Polls and drains until `done(events)` or the deadline; returns what was collected.
template <typename Done>
[[nodiscard]] std::optional<std::vector<host::LinkedFileEvent>>
awaitEvents(host::LinkedFileMonitor& monitor, Fixture& fixture, Done done) {
    std::vector<host::LinkedFileEvent> collected;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto snapshot = snapshotOf(fixture);
        monitor.poll(snapshot.project(), fixture.directory);
        for (auto& event : monitor.takeEvents())
            collected.push_back(std::move(event));
        if (done(collected))
            return collected;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return std::nullopt;
}

// Waits for every check to finish, then returns what completed.
[[nodiscard]] std::optional<std::vector<host::LinkedFileEvent>>
settle(host::LinkedFileMonitor& monitor, Fixture& fixture) {
    const auto snapshot = snapshotOf(fixture);
    monitor.poll(snapshot.project(), fixture.directory);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (monitor.checksInFlight()) {
        if (std::chrono::steady_clock::now() >= deadline)
            return std::nullopt;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return monitor.takeEvents();
}

void testInSync(Expectations& expect) {
    TempDirectory directory;
    Fixture fixture;
    expect.expect(build(fixture, directory.path(), kOriginal), "the fixture builds");
    if (!fixture.session)
        return;
    const auto snapshot = snapshotOf(fixture);
    const auto lists = host::linkedCueLists(snapshot.project(), directory.path());
    expect.expect(lists.size() == 1 && lists[0].node == fixture.node &&
                      lists[0].composition == fixture.composition &&
                      lists[0].provenance.path == "titles.srt" &&
                      lists[0].path == directory.path() / "titles.srt",
                  "the linked list is found with its resolved path");
    if (lists.empty())
        return;
    expect.expect(host::linkedFileState(lists[0]) == host::LinkedFileState::InSync,
                  "an untouched file is in sync");
    expect.expect(host::linkedFileWarnings(snapshot.project(), directory.path()).empty(),
                  "an untouched file warns of nothing");

    runtime::TaskScheduler scheduler;
    host::LinkedFileMonitor monitor(scheduler);
    const auto events = settle(monitor, fixture);
    expect.expect(events && events->empty(), "an untouched file publishes nothing");
}

void testChanged(Expectations& expect) {
    TempDirectory directory;
    Fixture fixture;
    expect.expect(build(fixture, directory.path(), kOriginal), "the fixture builds");
    if (!fixture.session)
        return;
    const auto before = snapshotOf(fixture);
    const auto* original = listOf(before, fixture);
    expect.expect(original != nullptr && original->cues.size() == 2, "two cues are loaded");
    if (original == nullptr)
        return;
    const auto firstId = original->cues[0]->id;

    writeFile(fixture.file, kEdited);
    const auto lists = host::linkedCueLists(before.project(), directory.path());
    expect.expect(!lists.empty() && host::linkedFileState(lists[0]) == host::LinkedFileState::Changed,
                  "edited bytes are Changed");

    runtime::TaskScheduler scheduler;
    host::LinkedFileMonitor monitor(scheduler);
    const auto events = awaitEvents(monitor, fixture, [](const auto& found) { return !found.empty(); });
    expect.expect(events.has_value(), "the monitor publishes an event before the deadline");
    if (!events)
        return;
    expect.expect(events->size() == 1 && events->front().kind == host::LinkedFileEvent::Kind::Reload,
                  "one Reload event");
    const auto& event = events->front();
    expect.expect(event.cues.cues.size() == 3 && event.cues.cues[2]->text == "three" &&
                      event.cues.cues[1]->text == "two changed",
                  "the reload carries the new file's cues");
    const auto* provenance = std::get_if<document::CueFileProvenance>(&event.cues.provenance);
    expect.expect(provenance != nullptr && provenance->path == "titles.srt" &&
                      provenance->portability == "project-relative",
                  "the reloaded list keeps the form of the path");

    const auto undoDepth = fixture.session->stateSnapshot().historySize;
    const auto applied = host::applyReload(*fixture.session, event);
    expect.expect(applied.changed(), "the reload applies");
    expect.expect(applied.command && applied.command->origin == commands::CommandOrigin::Host,
                  "the reload has host origin");
    expect.expect(fixture.session->stateSnapshot().historySize == undoDepth + 1 &&
                      fixture.session->stateSnapshot().undoLabel == "Reload Subtitles",
                  "the reload is one undo step");
    const auto after = snapshotOf(fixture);
    const auto* reloaded = listOf(after, fixture);
    expect.expect(reloaded != nullptr && reloaded->cues.size() == 3 &&
                      reloaded->cues[0]->id == firstId,
                  "a cue that did not change keeps its id");
    const auto* stored =
        reloaded == nullptr ? nullptr : std::get_if<document::CueFileProvenance>(&reloaded->provenance);
    expect.expect(stored != nullptr && stored->digest == provenance->digest,
                  "the stored provenance is the new file's");

    // Applying the same event again is stale: the digest the check started from has moved on.
    const auto again = host::applyReload(*fixture.session, event);
    expect.expect(!again.completed() && fixture.session->stateSnapshot().historySize == undoDepth + 1,
                  "a stale reload is dropped");
    const auto settled = settle(monitor, fixture);
    expect.expect(settled && settled->empty(), "a reloaded file raises nothing more");

    const auto undone = fixture.session->undo();
    expect.expect(undone.changed(), "undo succeeds");
    const auto restored = snapshotOf(fixture);
    const auto* restoredList = listOf(restored, fixture);
    expect.expect(restoredList != nullptr && restoredList->cues.size() == 2,
                  "one undo restores the previous list");
}

void testTouchedSameBytes(Expectations& expect) {
    TempDirectory directory;
    Fixture fixture;
    expect.expect(build(fixture, directory.path(), kOriginal), "the fixture builds");
    if (!fixture.session)
        return;
    std::error_code error;
    fs::last_write_time(fixture.file, fs::last_write_time(fixture.file) + std::chrono::hours(1), error);
    expect.expect(!error, "the file's time can be changed");
    const auto snapshot = snapshotOf(fixture);
    const auto lists = host::linkedCueLists(snapshot.project(), directory.path());
    expect.expect(!lists.empty() && host::linkedFileState(lists[0]) == host::LinkedFileState::InSync,
                  "a touched file with the same bytes is in sync");
    runtime::TaskScheduler scheduler;
    host::LinkedFileMonitor monitor(scheduler);
    const auto events = settle(monitor, fixture);
    expect.expect(events && events->empty(), "a touched file does not reload");
    const auto again = settle(monitor, fixture);
    expect.expect(again && again->empty(), "and is not checked again");
}

void testMissing(Expectations& expect) {
    TempDirectory directory;
    Fixture fixture;
    expect.expect(build(fixture, directory.path(), kOriginal), "the fixture builds");
    if (!fixture.session)
        return;
    std::error_code error;
    fs::remove(fixture.file, error);
    const auto snapshot = snapshotOf(fixture);
    const auto lists = host::linkedCueLists(snapshot.project(), directory.path());
    expect.expect(!lists.empty() && host::linkedFileState(lists[0]) == host::LinkedFileState::Missing,
                  "a removed file is Missing");

    runtime::TaskScheduler scheduler;
    host::LinkedFileMonitor monitor(scheduler);
    const auto first = settle(monitor, fixture);
    expect.expect(first && first->size() == 1 &&
                      first->front().kind == host::LinkedFileEvent::Kind::Missing &&
                      first->front().node == fixture.node && first->front().path == fixture.file,
                  "one Missing event");
    const auto second = settle(monitor, fixture);
    expect.expect(second && second->empty(), "a missing file is reported once");

    writeFile(fixture.file, kOriginal);
    const auto present = settle(monitor, fixture);
    expect.expect(present && present->empty(), "a file that reappears unchanged raises nothing");
    fs::remove(fixture.file, error);
    const auto gone = settle(monitor, fixture);
    expect.expect(gone && gone->size() == 1 &&
                      gone->front().kind == host::LinkedFileEvent::Kind::Missing,
                  "once it existed again, a new disappearance is reported");
}

void testInvalidFile(Expectations& expect) {
    TempDirectory directory;
    Fixture fixture;
    expect.expect(build(fixture, directory.path(), kOriginal), "the fixture builds");
    if (!fixture.session)
        return;
    const auto before = snapshotOf(fixture);
    const auto* original = listOf(before, fixture);
    const auto originalCues = original == nullptr ? document::CueListValue{} : *original;
    writeFile(fixture.file, "this is not a subtitle file\n");

    // What the reader says about the same bytes.
    subtitles::LoadOptions srt;
    srt.format = subtitles::Format::Srt;
    const auto expected = subtitles::loadFile(
        {.path = fixture.file, .projectDirectory = directory.path(), .options = srt, .link = true});
    const auto* failure = std::get_if<subtitles::LoadFailure>(&expected);
    expect.expect(failure != nullptr, "the reader refuses the file");

    runtime::TaskScheduler scheduler;
    host::LinkedFileMonitor monitor(scheduler);
    const auto events = awaitEvents(monitor, fixture, [](const auto& found) { return !found.empty(); });
    expect.expect(events && events->size() == 1 &&
                      events->front().kind == host::LinkedFileEvent::Kind::Failed,
                  "one Failed event");
    if (events && !events->empty() && failure != nullptr)
        expect.expect(events->front().message == failure->message,
                      "the event carries the reader's message");
    if (events && !events->empty())
        expect.expect(!host::applyReload(*fixture.session, events->front()).changed(),
                      "a failed check changes nothing");
    const auto after = snapshotOf(fixture);
    const auto* list = listOf(after, fixture);
    expect.expect(list != nullptr && *list == originalCues, "the cues are unchanged");
    const auto again = settle(monitor, fixture);
    expect.expect(again && again->empty(), "the same stamp is not retried");

    writeFile(fixture.file, kEdited);
    const auto fixed = awaitEvents(monitor, fixture, [](const auto& found) { return !found.empty(); });
    expect.expect(fixed && fixed->front().kind == host::LinkedFileEvent::Kind::Reload,
                  "a corrected file reloads");
}

void testHeadlessWarnings(Expectations& expect) {
    TempDirectory directory;
    Fixture fixture;
    expect.expect(build(fixture, directory.path(), kOriginal), "the fixture builds");
    if (!fixture.session)
        return;
    const auto snapshot = snapshotOf(fixture);
    const auto node = std::to_string(fixture.node.value());
    const auto prefix = "The cues of node " + node + " follow titles.srt, which ";

    writeFile(fixture.file, kEdited);
    auto warnings = host::linkedFileWarnings(snapshot.project(), directory.path());
    expect.expect(warnings.size() == 1 && warnings[0].code == "bloom.subtitles.linked-file-changed" &&
                      warnings[0].node == fixture.node &&
                      warnings[0].composition == fixture.composition &&
                      warnings[0].message ==
                          prefix + "changed after they were loaded; this render uses the cues "
                                   "saved in the project. Reload them to use the file's cues.",
                  "the changed warning reads exactly");

    std::error_code error;
    fs::remove(fixture.file, error);
    warnings = host::linkedFileWarnings(snapshot.project(), directory.path());
    expect.expect(warnings.size() == 1 &&
                      warnings[0].message ==
                          prefix + "is missing; this render uses the cues saved in the project.",
                  "the missing warning reads exactly");

    // A file over the reader's size limit cannot be read.
    writeFile(fixture.file, std::string(document::kMaximumSubtitleFileBytes + 1U, 'x'));
    warnings = host::linkedFileWarnings(snapshot.project(), directory.path());
    expect.expect(warnings.size() == 1 &&
                      warnings[0].message ==
                          prefix + "could not be read; this render uses the cues saved in the "
                                   "project.",
                  "the unreadable warning reads exactly");
    writeFile(fixture.file, kOriginal);
    const auto stamp = subtitles::statFile(fixture.file);
    expect.expect(stamp.has_value() &&
                      host::linkedFileWarnings(snapshot.project(), directory.path()).empty(),
                  "a restored file warns of nothing");
    // The headless route never reloads: the saved cues are untouched.
    const auto after = snapshotOf(fixture);
    const auto* list = listOf(after, fixture);
    expect.expect(list != nullptr && list->cues.size() == 2, "the cues saved in the project are kept");
}

void testCancelAndDestroy(Expectations& expect) {
    TempDirectory directory;
    Fixture fixture;
    expect.expect(build(fixture, directory.path(), kOriginal), "the fixture builds");
    if (!fixture.session)
        return;
    writeFile(fixture.file, kEdited);
    const auto snapshot = snapshotOf(fixture);
    runtime::TaskScheduler scheduler;
    {
        host::LinkedFileMonitor monitor(scheduler);
        monitor.poll(snapshot.project(), directory.path());
        monitor.cancel();
        expect.expect(monitor.takeEvents().empty(), "nothing is published once cancelled");
        // Usable again after a cancel.
        const auto events =
            awaitEvents(monitor, fixture, [](const auto& found) { return !found.empty(); });
        expect.expect(events && events->size() == 1, "a cancelled monitor can be polled again");
    }
    {
        // Destroyed with a check possibly in flight: the worker owns what it needs.
        host::LinkedFileMonitor monitor(scheduler);
        monitor.poll(snapshot.project(), directory.path());
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!scheduler.isQuiescent() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    expect.expect(scheduler.isQuiescent(), "the scheduler drains after the monitor is destroyed");
}

} // namespace

int main() {
    Expectations expect;
    testInSync(expect);
    testChanged(expect);
    testTouchedSameBytes(expect);
    testMissing(expect);
    testInvalidFile(expect);
    testHeadlessWarnings(expect);
    testCancelAndDestroy(expect);
    if (expect.failures() != 0) {
        std::cerr << expect.failures() << " expectation(s) failed\n";
        return 1;
    }
    std::cout << "bloom.host.linked-subtitle-files passed\n";
    return 0;
}
