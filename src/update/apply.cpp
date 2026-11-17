#include <bloom/update/apply.hpp>

#include <charconv>
#include <fstream>

namespace bloom::update {
namespace {

constexpr std::chrono::milliseconds kPoll{200};

// The steps a run has taken, one per line, added to as they happen. A run that was stopped is
// continued from what the journal and the folders say together.
class Journal final {
  public:
    explicit Journal(std::filesystem::path path) : path_(std::move(path)) {
        std::ifstream file(path_);
        for (std::string line; std::getline(file, line);) {
            lines_.push_back(std::move(line));
        }
    }

    [[nodiscard]] bool has(const std::string_view step) const { return value(step).has_value(); }

    [[nodiscard]] std::optional<std::string> value(const std::string_view step) const {
        for (const auto& line : lines_) {
            const auto name = std::string_view(line).substr(0, line.find(' '));
            if (name == step) {
                return name.size() == line.size() ? std::string{} : line.substr(name.size() + 1);
            }
        }
        return std::nullopt;
    }

    void add(const std::string_view step, const std::string_view detail = {}) {
        std::string line(step);
        if (!detail.empty()) {
            line += ' ';
            line += detail;
        }
        std::ofstream file(path_, std::ios::app);
        file << line << '\n';
        file.flush();
        lines_.push_back(std::move(line));
    }

  private:
    std::filesystem::path path_;
    std::vector<std::string> lines_;
};

[[nodiscard]] bool present(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::symlink_status(path, error).type() != std::filesystem::file_type::not_found &&
           !error;
}

[[nodiscard]] std::filesystem::path beside(const std::filesystem::path& path,
                                           const std::string_view suffix) {
    auto result = path;
    result += suffix;
    return result;
}

void discard(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::remove_all(path, error);
}

enum class Start : std::uint8_t { New, Other, None };

// Waits for a started Bloom to say which version it is. A started program that is known to have
// ended will not say anything more.
[[nodiscard]] Start waitForStart(const ApplyPlan& plan, ApplyHost& host,
                                 const std::optional<std::uint64_t> started) {
    std::chrono::milliseconds waited{0};
    while (true) {
        const bool ended = started.has_value() && !host.processRunning(*started);
        if (const auto version = readStartConfirmation(plan.confirmation); version.has_value()) {
            return *version == plan.version ? Start::New : Start::Other;
        }
        if (ended || waited >= plan.confirmTimeout) {
            return Start::None;
        }
        host.wait(kPoll);
        waited += kPoll;
    }
}

[[nodiscard]] ApplyResult finish(Journal& journal, const ApplyOutcome outcome,
                                 std::string detail) {
    journal.add("done", detail);
    return {outcome, std::move(detail)};
}

// Puts `source` where `installed` is and what was installed at `aside`. Where the system can
// exchange two names in one step there is no moment without an installed version; elsewhere it
// is two moves. False when `installed` is what it was before (or, at worst, missing with both
// versions kept beside it under their names).
[[nodiscard]] bool replaceInstalled(ApplyHost& host, const std::filesystem::path& installed,
                                    const std::filesystem::path& source,
                                    const std::filesystem::path& aside) {
    if (present(installed)) {
        if (!host.exchange(installed, source)) {
            // `source` now names what was installed. Left there when it cannot be moved aside.
            (void)host.rename(source, aside);
            return true;
        }
        if (host.rename(installed, aside)) {
            return false;
        }
    }
    if (host.rename(source, installed)) {
        if (present(aside)) {
            (void)host.rename(aside, installed);
        }
        return false;
    }
    return true;
}

// Starts the installed version after an update that did not happen, and says so to it.
[[nodiscard]] ApplyResult startInstalled(const ApplyPlan& plan, Journal& journal, ApplyHost& host,
                                         const std::string_view reason) {
    if (!present(plan.installed)) {
        return {ApplyOutcome::Failed,
                std::string(reason) + "; no installed version is in place, the kept versions "
                                      "are beside it"};
    }
    (void)writeStartConfirmation(plan.notice, plan.version);
    (void)host.launch(plan.program, plan.arguments);
    return finish(journal, ApplyOutcome::Restored, std::string(reason));
}

// Puts the version that was set aside back and starts it. `kept` is where it is. The intent is
// written down with the kept version's identity first, so a run that was stopped in the middle
// knows on its next start whether that version is already back.
[[nodiscard]] ApplyResult restore(const ApplyPlan& plan, Journal& journal, ApplyHost& host,
                                  const std::filesystem::path& kept,
                                  const std::string_view reason) {
    const auto failed = beside(plan.installed, ".failed");
    bool back = false;
    if (const auto recorded = journal.value("restoring"); recorded.has_value()) {
        const auto now = host.identity(plan.installed);
        back = !recorded->empty() && !now.empty()
                   ? now == *recorded
                   : present(plan.installed) && !present(kept);
    } else {
        if (!present(kept)) {
            // Starting the version that just failed to start again would help nobody.
            return {ApplyOutcome::Failed, std::string(reason) + "; no previous version was kept"};
        }
        journal.add("restoring", host.identity(kept));
    }
    if (!back) {
        if (!present(kept)) {
            return {ApplyOutcome::Failed, std::string(reason) + "; no previous version was kept"};
        }
        discard(failed);
        if (!replaceInstalled(host, plan.installed, kept, failed)) {
            return {ApplyOutcome::Failed,
                    std::string(reason) + "; the previous version could not be put back and is "
                                          "kept beside the installation"};
        }
    }
    discard(failed);
    // An exchange that could not move the failed version aside leaves it under this name.
    discard(kept);
    return startInstalled(plan, journal, host, reason);
}

// Where the version this run replaced is: set aside as `previous`, or still under the prepared
// version's name when an exchange could not move it on.
[[nodiscard]] std::filesystem::path keptVersion(const ApplyPlan& plan) {
    return present(plan.previous) ? plan.previous : plan.staged;
}

[[nodiscard]] ApplyResult swap(const ApplyPlan& plan, Journal& journal, ApplyHost& host) {
    if (journal.has("restoring")) {
        return restore(plan, journal, host, keptVersion(plan), "the new version did not start");
    }
    if (!journal.has("swapped")) {
        bool replaced = false;
        if (const auto prepared = journal.value("replacing"); prepared.has_value()) {
            // A run was stopped around the replacement. The prepared version is in place when
            // the installed name holds it; without identities, when its own name is gone.
            const auto now = host.identity(plan.installed);
            replaced = !prepared->empty() && !now.empty()
                           ? now == *prepared
                           : present(plan.installed) && !present(plan.staged);
            if (!replaced && !present(plan.installed)) {
                // Between two moves: the installed version is set aside, the prepared one waits.
                if (host.rename(plan.staged, plan.installed)) {
                    return restore(plan, journal, host, plan.previous,
                                   "the prepared version could not be put in place");
                }
                replaced = true;
            }
            if (replaced && !present(plan.previous) && present(plan.staged)) {
                (void)host.rename(plan.staged, plan.previous);
            }
        }
        if (!replaced) {
            if (!present(plan.staged)) {
                return startInstalled(plan, journal, host, "no prepared version was found");
            }
            // The version kept from the update before this one has had its turn.
            if (present(plan.previous)) {
                const auto old = beside(plan.previous, ".old");
                discard(old);
                if (host.rename(plan.previous, old)) {
                    return startInstalled(
                        plan, journal, host,
                        "the version kept from an earlier update could not be removed");
                }
                discard(old);
            }
            if (!journal.has("replacing")) {
                journal.add("replacing", host.identity(plan.staged));
            }
            if (!replaceInstalled(host, plan.installed, plan.staged, plan.previous)) {
                return startInstalled(plan, journal, host,
                                      "the prepared version could not be put in place");
            }
        }
        journal.add("swapped");
    }

    std::optional<std::uint64_t> started;
    if (const auto recorded = journal.value("launched"); recorded.has_value()) {
        std::uint64_t processId = 0;
        const auto result =
            std::from_chars(recorded->data(), recorded->data() + recorded->size(), processId);
        if (result.ec == std::errc{} && !recorded->empty()) {
            started = processId;
        }
    } else if (readStartConfirmation(plan.confirmation) != plan.version) {
        // (A run continued from inside the new version finds its confirmation already there.)
        started = host.launch(plan.program, plan.arguments);
        if (!started.has_value()) {
            return restore(plan, journal, host, keptVersion(plan),
                           "the new version could not be started");
        }
        journal.add("launched", std::to_string(*started));
    }
    switch (waitForStart(plan, host, started)) {
    case Start::New:
        return finish(journal, ApplyOutcome::Updated, "the new version started");
    case Start::Other:
        // Whatever is installed did start; it is left alone.
        return finish(journal, ApplyOutcome::Unchanged, "another version than the new one started");
    case Start::None:
        break;
    }
    if (started.has_value() && host.processRunning(*started)) {
        host.terminate(*started);
    }
    return restore(plan, journal, host, keptVersion(plan), "the new version did not start");
}

[[nodiscard]] ApplyResult watch(const ApplyPlan& plan, Journal& journal, ApplyHost& host) {
    if (journal.has("restoring")) {
        return restore(plan, journal, host, plan.previous, "the new version did not start");
    }
    switch (waitForStart(plan, host, std::nullopt)) {
    case Start::New:
        return finish(journal, ApplyOutcome::Updated, "the new version started");
    case Start::Other:
        return finish(journal, ApplyOutcome::Unchanged, "the installed version started again");
    case Start::None:
        break;
    }
    return restore(plan, journal, host, plan.previous, "the new version did not start");
}

} // namespace

ApplyResult applyUpdate(const ApplyPlan& plan, const std::filesystem::path& journalPath,
                        ApplyHost& host) {
    Journal journal(journalPath);
    if (const auto done = journal.value("done"); done.has_value()) {
        return {ApplyOutcome::AlreadyDone, *done};
    }
    if (!journal.has("exited")) {
        // What the running version wrote when it started says nothing about the next start.
        std::error_code error;
        std::filesystem::remove(plan.confirmation, error);
        std::chrono::milliseconds waited{0};
        while (host.processRunning(plan.processId)) {
            // A watch lasts as long as Bloom runs: the platform's updater installs whenever
            // Bloom is gone, be it an hour after a quit that stopped at unsaved work.
            if (plan.mode == ApplyMode::Swap && waited >= plan.exitTimeout) {
                // Not recorded: nothing was touched, and a later run may find Bloom gone.
                return {ApplyOutcome::StillRunning, "Bloom did not exit"};
            }
            host.wait(kPoll);
            waited += kPoll;
        }
        journal.add("exited");
    }
    return plan.mode == ApplyMode::Swap ? swap(plan, journal, host) : watch(plan, journal, host);
}

} // namespace bloom::update
