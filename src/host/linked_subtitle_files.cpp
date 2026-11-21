#include <bloom/host/linked_subtitle_files.hpp>

#include <bloom/commands/subtitles_operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/document/subtitles_nodes.hpp>
#include <bloom/subtitles/subtitles.hpp>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <set>
#include <utility>
#include <variant>

namespace bloom::host {

namespace {

using Key = std::tuple<std::uint64_t, std::uint64_t>;

[[nodiscard]] Key keyOf(const document::CompositionId composition, const document::NodeId node) {
    return {composition.value(), node.value()};
}

[[nodiscard]] bool stampMatches(const document::CueFileProvenance& provenance,
                                const subtitles::FileStamp& stamp) {
    return stamp.size == provenance.size && stamp.modifiedNanoseconds == provenance.modifiedNanoseconds;
}

// A path as UTF-8 text on every platform (generic_string() converts through the Windows code
// page, which can fail on a name outside it).
[[nodiscard]] std::string pathKey(const std::filesystem::path& path) {
    const auto text = path.generic_u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}
[[nodiscard]] runtime::TaskOwnerId nextOwnerId() {
    // Distinct from the fixed owner the session I/O tasks use.
    static std::atomic<std::uint64_t> next{1ULL << 40U};
    return runtime::TaskOwnerId::fromRaw(next.fetch_add(1, std::memory_order_relaxed));
}

} // namespace

std::vector<LinkedCueList> linkedCueLists(const document::Project& project,
                                          const std::filesystem::path& projectDirectory) {
    std::vector<LinkedCueList> lists;
    for (const auto& composition : project.compositions()) {
        for (const auto& record : composition.graph().nodes()) {
            if (record.typeId != document::kSubtitlesNodeType)
                continue;
            const auto located = commands::locateCueList(composition, record.id, true);
            const auto* location = std::get_if<commands::CueListLocation>(&located);
            if (location == nullptr || location->list == nullptr)
                continue;
            const auto* file = std::get_if<document::CueFileProvenance>(&location->list->provenance);
            if (file == nullptr)
                continue;
            lists.push_back({composition.id(), record.id, *file,
                             subtitles::linkedFilePath(*file, projectDirectory)});
        }
    }
    return lists;
}

LinkedFileState linkedFileState(const LinkedCueList& linked, const std::stop_token stop) {
    const auto stamp = subtitles::statFile(linked.path);
    if (!stamp)
        return LinkedFileState::Missing;
    if (stampMatches(linked.provenance, *stamp))
        return LinkedFileState::InSync;
    const auto read = subtitles::readFile(linked.path, stop);
    const auto* contents = std::get_if<subtitles::FileContents>(&read);
    if (contents == nullptr)
        return LinkedFileState::Unreadable;
    return contents->digest == linked.provenance.digest ? LinkedFileState::InSync
                                                         : LinkedFileState::Changed;
}

std::optional<LinkedFileWarning> linkedFileWarning(const LinkedCueList& linked,
                                                   const LinkedFileState state) {
    const auto prefix = "The cues of node " + std::to_string(linked.node.value()) + " follow " +
                        linked.provenance.path + ", which ";
    std::string message;
    switch (state) {
    case LinkedFileState::InSync:
        return std::nullopt;
    case LinkedFileState::Changed:
        message = prefix + "changed after they were loaded; this render uses the cues saved in "
                           "the project. Reload them to use the file's cues.";
        break;
    case LinkedFileState::Missing:
        message = prefix + "is missing; this render uses the cues saved in the project.";
        break;
    case LinkedFileState::Unreadable:
        message = prefix + "could not be read; this render uses the cues saved in the project.";
        break;
    }
    return LinkedFileWarning{linked.composition, linked.node, std::string(kLinkedSubtitleFileChangedCode),
                             std::move(message)};
}

std::vector<LinkedFileWarning> linkedFileWarnings(const document::Project& project,
                                                  const std::filesystem::path& projectDirectory,
                                                  const std::stop_token stop) {
    std::vector<LinkedFileWarning> warnings;
    for (const auto& linked : linkedCueLists(project, projectDirectory)) {
        if (stop.stop_requested())
            break;
        if (auto warning = linkedFileWarning(linked, linkedFileState(linked, stop)))
            warnings.push_back(std::move(*warning));
    }
    return warnings;
}

// ---------------------------------------------------------------------------------------------
// The monitor.

namespace detail {
struct LinkedFileMonitorShared final {
    std::mutex mutex;
    std::set<Key> inFlight;
    // The stamp last seen for a (node, file); absent: the provenance's own stamp.
    std::map<std::tuple<std::uint64_t, std::uint64_t, std::string>, subtitles::FileStamp> seen;
    // (node, file) pairs whose missing file was already reported.
    std::set<std::tuple<std::uint64_t, std::uint64_t, std::string>> missingReported;
    std::vector<LinkedFileEvent> events;
};
} // namespace detail

namespace {

using SeenKey = std::tuple<std::uint64_t, std::uint64_t, std::string>;

void checkFile(const std::shared_ptr<detail::LinkedFileMonitorShared>& shared, const LinkedCueList& linked,
               const std::filesystem::path& projectDirectory, const std::stop_token stop) {
    const auto key = keyOf(linked.composition, linked.node);
    const SeenKey seenKey{std::get<0>(key), std::get<1>(key), pathKey(linked.path)};
    const auto push = [&](LinkedFileEvent event) {
        event.composition = linked.composition;
        event.node = linked.node;
        event.path = linked.path;
        event.base = linked.provenance;
        std::lock_guard lock(shared->mutex);
        // cancel() requests stop before it takes this lock, so nothing is published after it.
        if (!stop.stop_requested())
            shared->events.push_back(std::move(event));
    };
    const auto remember = [&](const subtitles::FileStamp& stamp) {
        std::lock_guard lock(shared->mutex);
        if (!stop.stop_requested())
            shared->seen[seenKey] = stamp;
    };

    const auto stamp = subtitles::statFile(linked.path);
    if (!stamp) {
        bool first = false;
        {
            std::lock_guard lock(shared->mutex);
            first = !stop.stop_requested() && shared->missingReported.insert(seenKey).second;
        }
        if (first) {
            LinkedFileEvent event;
            event.kind = LinkedFileEvent::Kind::Missing;
            push(std::move(event));
        }
        return;
    }
    subtitles::FileStamp last{linked.provenance.size, linked.provenance.modifiedNanoseconds};
    {
        std::lock_guard lock(shared->mutex);
        shared->missingReported.erase(seenKey);
        if (const auto found = shared->seen.find(seenKey); found != shared->seen.end())
            last = found->second;
    }
    if (*stamp == last || stop.stop_requested())
        return;

    const auto read = subtitles::readFile(linked.path, stop);
    if (const auto* failure = std::get_if<subtitles::LoadFailure>(&read)) {
        if (failure->code == subtitles::codes::kCancelled || stop.stop_requested())
            return;
        remember(*stamp);
        LinkedFileEvent event;
        event.kind = LinkedFileEvent::Kind::Failed;
        event.message = failure->message;
        push(std::move(event));
        return;
    }
    const auto& contents = std::get<subtitles::FileContents>(read);
    if (contents.digest == linked.provenance.digest) {
        // Only the stamp moved.
        remember({contents.size, contents.modifiedNanoseconds});
        return;
    }

    subtitles::LoadOptions options;
    options.format = subtitles::formatFromName(linked.provenance.format);
    options.encoding = linked.provenance.encoding;
    options.stop = stop;
    auto loaded = subtitles::loadFile(
        {.path = linked.path, .projectDirectory = projectDirectory, .options = std::move(options), .link = true});
    if (auto* failure = std::get_if<subtitles::LoadFailure>(&loaded)) {
        if (failure->code == subtitles::codes::kCancelled || stop.stop_requested())
            return;
        remember(*stamp);
        LinkedFileEvent event;
        event.kind = LinkedFileEvent::Kind::Failed;
        event.message = failure->message;
        push(std::move(event));
        return;
    }
    auto& file = std::get<subtitles::FileLoad>(loaded);
    remember({file.size, file.modifiedNanoseconds});
    if (file.digest == linked.provenance.digest)
        return;
    LinkedFileEvent event;
    event.kind = LinkedFileEvent::Kind::Reload;
    event.cues = std::move(file.loaded.cues);
    // The list keeps the form of the path it already followed.
    if (auto* provenance = std::get_if<document::CueFileProvenance>(&event.cues.provenance)) {
        provenance->portability = linked.provenance.portability;
        provenance->path = linked.provenance.path;
    }
    event.warnings = std::move(file.warnings);
    push(std::move(event));
}

} // namespace

LinkedFileMonitor::LinkedFileMonitor(runtime::TaskScheduler& scheduler)
    : scheduler_(&scheduler), shared_(std::make_shared<detail::LinkedFileMonitorShared>()),
      owner_{runtime::TaskOwnerKind::Application, nextOwnerId()} {}

LinkedFileMonitor::~LinkedFileMonitor() {
    cancel();
}

void LinkedFileMonitor::poll(const document::Project& project,
                             const std::filesystem::path& projectDirectory) {
    auto lists = linkedCueLists(project, projectDirectory);
    std::set<SeenKey> current;
    for (const auto& linked : lists)
        current.insert({linked.composition.value(), linked.node.value(), pathKey(linked.path)});
    {
        // Forget what a node that no longer follows a file (or follows another) had.
        std::lock_guard lock(shared_->mutex);
        std::erase_if(shared_->seen, [&](const auto& entry) { return !current.contains(entry.first); });
        std::erase_if(shared_->missingReported, [&](const auto& entry) { return !current.contains(entry); });
    }
    for (auto& linked : lists) {
        const auto key = keyOf(linked.composition, linked.node);
        {
            std::lock_guard lock(shared_->mutex);
            if (!shared_->inFlight.insert(key).second)
                continue;
        }
        auto request = runtime::TaskRequest("Check linked subtitle file", owner_,
                                            runtime::TaskPriority::Background,
                                            runtime::TaskExecutor::BlockingIo);
        auto submission = scheduler_->submit<void>(
            std::move(request),
            [shared = shared_, linked, projectDirectory, stop = stop_.get_token(),
             key](runtime::TaskContext& context) -> runtime::TaskResult<void> {
                struct Release final {
                    detail::LinkedFileMonitorShared& shared;
                    Key key;
                    ~Release() {
                        std::lock_guard lock(shared.mutex);
                        shared.inFlight.erase(key);
                    }
                } release{*shared, key};
                if (context.isCancellationRequested() || stop.stop_requested())
                    return runtime::TaskResult<void>::cancelled();
                checkFile(shared, linked, projectDirectory, stop);
                return runtime::TaskResult<void>::succeeded();
            });
        if (!submission.accepted()) {
            std::lock_guard lock(shared_->mutex);
            shared_->inFlight.erase(key);
            continue;
        }
        handles_.insert_or_assign(key, std::move(submission.handle));
    }
    // Handles of nodes that no longer follow a file are of no further use.
    std::erase_if(handles_, [&](const auto& entry) {
        return std::ranges::none_of(lists, [&](const auto& linked) {
            return keyOf(linked.composition, linked.node) == entry.first;
        });
    });
}

std::vector<LinkedFileEvent> LinkedFileMonitor::takeEvents() {
    std::lock_guard lock(shared_->mutex);
    return std::exchange(shared_->events, {});
}

bool LinkedFileMonitor::checksInFlight() const {
    std::lock_guard lock(shared_->mutex);
    return !shared_->inFlight.empty();
}

void LinkedFileMonitor::cancel() {
    stop_.request_stop();
    for (const auto& [key, handle] : handles_)
        handle.cancel();
    handles_.clear();
    stop_ = std::stop_source();
    // Tasks already running see the old source's stop and drop their results; forget the rest.
    std::lock_guard lock(shared_->mutex);
    shared_->inFlight.clear();
    shared_->events.clear();
    shared_->seen.clear();
    shared_->missingReported.clear();
}

ProjectSessionCommandResult applyReload(ProjectSession& session, const LinkedFileEvent& event) {
    const ProjectSessionCommandResult dropped{ProjectSessionCommandStatus::Completed, std::nullopt};
    if (event.kind != LinkedFileEvent::Kind::Reload)
        return dropped;
    const auto decoded = session.decodedSnapshot();
    if (!decoded)
        return {ProjectSessionCommandStatus::InvalidSession, std::nullopt};
    const auto& snapshot = decoded.snapshot();
    const auto* composition = snapshot.project().findComposition(event.composition);
    if (composition == nullptr)
        return dropped;
    const auto located = commands::locateCueList(*composition, event.node, true);
    const auto* location = std::get_if<commands::CueListLocation>(&located);
    if (location == nullptr || location->list == nullptr)
        return dropped;
    const auto* file = std::get_if<document::CueFileProvenance>(&location->list->provenance);
    if (file == nullptr || file->portability != event.base.portability ||
        file->path != event.base.path || file->digest != event.base.digest)
        return dropped;
    commands::Transaction transaction("Reload Subtitles");
    transaction.emplace<commands::ReplaceCues>(event.composition, event.node, event.cues,
                                               commands::ReplaceCues::Kind::Reload, event.warnings);
    transaction.setOrigin(commands::CommandOrigin::Host);
    return session.execute(std::move(transaction));
}

} // namespace bloom::host
