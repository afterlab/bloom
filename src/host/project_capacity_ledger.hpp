#pragma once

// #748: the size of a project session's document as a save would write it, kept current as edits
// publish, so every edit is measured from the records it changed and never by a walk of the whole
// document on the owner (UI) thread. Private to bloom_host; ProjectSession owns one per content.
//
// What it knows is the latest published snapshot and its project::CanonicalDocumentSizeIndex. A
// measure advances that index to the candidate (project::advanceCanonicalDocumentIndex): its cost
// is the records that differ between the two snapshots, so on the owner thread an artist's edit
// counts the records it changed (plus any an undo or redo restored since the last measured edit).
// A live (agent or script) commit is measured during its preparation, on its client's thread, and
// publication adopts the index it built. Thread-safe: the lock guards only copies of shared
// pointers, never a measure.
//
// The whole measure the session starts from is taken when the project is opened (on the opening
// thread, project::OpenedArchive::sizeIndex) or created. When it is not known (opened content
// without one), it is measured on a background thread; until then an edit built on the owner
// thread is admitted unmeasured rather than refused on a guess, and a preparation measures its own
// base whole, off the owner thread.

#include <bloom/commands/command_stack.hpp>
#include <bloom/core/worker_thread.hpp>
#include <bloom/document/color_settings.hpp>
#include <bloom/document/document.hpp>
#include <bloom/host/project_session.hpp>
#include <bloom/project/canonical_document.hpp>
#include <bloom/project/project_capacity.hpp>
#include <bloom/project/round_trip_state.hpp>

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>

namespace bloom::host {

class ProjectCapacityLedger final : public std::enable_shared_from_this<ProjectCapacityLedger> {
  public:
    ProjectCapacityLedger(std::shared_ptr<const project::RoundTripState> roundTrip,
                          std::uint32_t schemaMinor);
    ProjectCapacityLedger(const ProjectCapacityLedger&) = delete;
    ProjectCapacityLedger& operator=(const ProjectCapacityLedger&) = delete;
    ProjectCapacityLedger(ProjectCapacityLedger&&) = delete;
    ProjectCapacityLedger& operator=(ProjectCapacityLedger&&) = delete;
    // Stops a background measure and waits for it (it checks its stop token between records).
    ~ProjectCapacityLedger();

    // Owner thread. The color settings state `id` of the document holds `settings`.
    void colorSettingsState(std::uint64_t id, const document::ColorSettings& settings);
    // Owner thread. Starts from `snapshot`: with its index when known, measured whole here when
    // `measureNow` (a new project, which is small), otherwise on a background thread.
    void start(const document::Snapshot& snapshot,
               std::shared_ptr<const project::CanonicalDocumentSizeIndex> index, bool measureNow);
    // The command stack's check: measure() and published() below.
    [[nodiscard]] std::shared_ptr<const commands::CommandStack::CapacityCheck> check();

    [[nodiscard]] ProjectCapacityWork work() const;

  private:
    struct Known final {
        std::optional<document::Snapshot> snapshot;
        std::shared_ptr<const project::CanonicalDocumentSizeIndex> index;
    };

    [[nodiscard]] commands::ProjectCapacityMeasure
    measure(const document::Snapshot& before, const document::Snapshot& candidate,
            commands::CommandStack::CapacityCheck::Thread thread);
    void published(const document::Snapshot& snapshot,
                   const std::shared_ptr<const void>& size) noexcept;
    [[nodiscard]] Known known() const;
    // Records `index` for `snapshot` only while nothing newer is known.
    void offer(const document::Snapshot& snapshot,
               std::shared_ptr<const project::CanonicalDocumentSizeIndex> index);
    void measureInBackground(const document::Snapshot& snapshot);
    [[nodiscard]] std::shared_ptr<const document::ColorSettings>
    colorSettingsOf(const document::Snapshot& snapshot) const;
    [[nodiscard]] project::CanonicalDocumentIndexResult whole(const document::Snapshot& snapshot,
                                                              std::stop_token stop = {});
    [[nodiscard]] project::CanonicalDocumentIndexResult advance(const Known& from,
                                                                const document::Snapshot& to);

    const std::shared_ptr<const project::RoundTripState> roundTrip_;
    const std::uint32_t schemaMinor_;
    mutable std::mutex mutex_;
    Known known_;
    std::map<std::uint64_t, std::shared_ptr<const document::ColorSettings>> colorSettings_;
    std::atomic<std::uint64_t> wholeMeasures_{0};
    std::atomic<std::uint64_t> ownerMeasures_{0};
    std::atomic<std::uint64_t> ownerRecordsCounted_{0};
    std::atomic<std::uint64_t> ownerCompositionsCounted_{0};
    std::atomic<std::uint64_t> preparationMeasures_{0};
    std::atomic<std::uint64_t> preparationWholeMeasures_{0};
    std::atomic<std::uint64_t> unmeasuredEdits_{0};
    std::atomic<std::uint64_t> adopted_{0};
    // Last: stopped and joined first on destruction, while everything it uses is alive.
    core::StoppableWorkerThread background_;
};

} // namespace bloom::host
