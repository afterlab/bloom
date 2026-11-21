#pragma once

// #752: subtitle cue lists linked to files (docs/architecture/subtitles.md, "Linked files"). A
// Subtitles node whose cue list was loaded with a link keeps the file's path, digest and stamp in
// its provenance; a render reads the cues saved in the project, never the file. This module
// answers whether each linked file still matches (headless warnings) and, for the desktop
// application, follows the files and publishes a reload when one changed.
//
// Qt-free. Nothing here touches the document: the monitor reads files on a blocking-I/O task and
// hands the owner thread events; applyReload() turns a reload event into one host-origin command
// through the session.

#include <bloom/document/cue_list.hpp>
#include <bloom/document/ids.hpp>
#include <bloom/document/project.hpp>
#include <bloom/host/project_session.hpp>
#include <bloom/runtime/task_scheduler.hpp>

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string_view>
#include <tuple>
#include <stop_token>
#include <string>
#include <vector>

namespace bloom::host {

// The code every linked-file warning carries.
inline constexpr std::string_view kLinkedSubtitleFileChangedCode =
    "bloom.subtitles.linked-file-changed";

// One Subtitles node whose own cue list follows a file.
struct LinkedCueList final {
    document::CompositionId composition;
    document::NodeId node;
    document::CueFileProvenance provenance;
    // The provenance resolved against the project directory.
    std::filesystem::path path;
};

// Every Subtitles node, in every composition, whose own cue list follows a file. A list another
// node supplies is skipped. Pure; no file access.
[[nodiscard]] std::vector<LinkedCueList> linkedCueLists(const document::Project& project,
                                                        const std::filesystem::path& projectDirectory);

enum class LinkedFileState : std::uint8_t { InSync, Changed, Missing, Unreadable };

// Compares the file with the list's provenance: a missing file is Missing; the same size and
// modification time is InSync without reading; otherwise the file is read and its digest compared
// (equal: InSync, only the stamp moved; different: Changed). A file that cannot be read, or a
// cancelled read, is Unreadable. Reads the file: never call it on the UI thread.
[[nodiscard]] LinkedFileState linkedFileState(const LinkedCueList& linked, std::stop_token stop = {});

struct LinkedFileWarning final {
    document::CompositionId composition;
    document::NodeId node;
    std::string code; // kLinkedSubtitleFileChangedCode
    std::string message;
    friend bool operator==(const LinkedFileWarning&, const LinkedFileWarning&) = default;
};

// The warning for one linked list in `state`; nullopt for InSync.
[[nodiscard]] std::optional<LinkedFileWarning> linkedFileWarning(const LinkedCueList& linked,
                                                                 LinkedFileState state);

// One warning per linked list whose file changed, is missing or cannot be read. Headless routes
// (bloom-cli render, export preflight) publish these and NEVER reload: they render the cues saved
// in the project. Reads files: never call it on the UI thread.
[[nodiscard]] std::vector<LinkedFileWarning>
linkedFileWarnings(const document::Project& project, const std::filesystem::path& projectDirectory,
                   std::stop_token stop = {});

struct LinkedFileEvent final {
    enum class Kind : std::uint8_t {
        Reload,  // the file changed: `cues` is its new list, `warnings` the read's report rows
        Failed,  // the file changed but could not be loaded: the cues are kept (`message`)
        Missing, // the file is gone: the cues are kept (reported once until it exists again)
    };
    Kind kind = Kind::Reload;
    document::CompositionId composition;
    document::NodeId node;
    // The resolved file.
    std::filesystem::path path;
    // The node's provenance when the check started; applyReload() drops the event when the node no
    // longer matches it.
    document::CueFileProvenance base;
    document::CueListValue cues;       // Reload
    std::vector<std::string> warnings; // Reload
    std::string message;               // Failed
};

// Follows the linked files of a project. Owner-thread API; the file work runs on the scheduler's
// blocking-I/O lane. A task copies everything it needs, so destroying the monitor never leaves a
// worker borrowing freed state (the scheduler must outlive the monitor).
namespace detail {
struct LinkedFileMonitorShared;
}

class LinkedFileMonitor final {
  public:
    explicit LinkedFileMonitor(runtime::TaskScheduler& scheduler);
    LinkedFileMonitor(const LinkedFileMonitor&) = delete;
    LinkedFileMonitor& operator=(const LinkedFileMonitor&) = delete;
    ~LinkedFileMonitor();

    // About once a second. Snapshots the linked lists and submits one task for each with no check
    // already in flight; never touches a file.
    void poll(const document::Project& project, const std::filesystem::path& projectDirectory);
    // The events completed since the last call.
    [[nodiscard]] std::vector<LinkedFileEvent> takeEvents();
    // Whether any check has not finished (a test waits on this; the desktop need not).
    [[nodiscard]] bool checksInFlight() const;
    // Cancels the checks in flight and forgets what was seen (a closed project). The monitor can
    // be polled again afterwards.
    void cancel();

  private:
    runtime::TaskScheduler* scheduler_;
    std::shared_ptr<detail::LinkedFileMonitorShared> shared_;
    std::stop_source stop_;
    std::map<std::tuple<std::uint64_t, std::uint64_t>, runtime::TaskHandle<void>> handles_;
    runtime::TaskOwner owner_;
};

// Applies a Reload event as one undo step, "Reload Subtitles", with host origin. The event is
// dropped (status Completed, no command; completed() is false) when it is not a Reload or the
// node no longer follows the same file with the same digest as when the check started.
[[nodiscard]] ProjectSessionCommandResult applyReload(ProjectSession& session,
                                                      const LinkedFileEvent& event);

} // namespace bloom::host
