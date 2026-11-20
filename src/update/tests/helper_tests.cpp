// The update helper's steps over folders and files made in a temporary directory, with a host
// that fails or stops on command: an installation is replaced and confirmed, or put back, and at
// no step is it lost.
#include "unique_temp_dir.hpp"
#include "update_test_support.hpp"

#include <bloom/update/apply.hpp>

#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <random>
#include <set>

namespace {

using namespace bloom::update;
using namespace bloom::update::testing;
using std::filesystem::path;

// Thrown by the host to stand for the helper being stopped (a crash, a power cut) at a step.
struct Stopped final {};

enum class Candidate : std::uint8_t { Confirms, ExitsAtStart, Hangs, CannotStart };

[[nodiscard]] std::string versionOf(const path& tree) {
    std::error_code error;
    std::ifstream file(std::filesystem::is_directory(tree, error) ? tree / "version" : tree);
    std::string version;
    std::getline(file, version);
    return version;
}

void makeVersion(const path& tree, const std::string_view version, const bool folder) {
    std::error_code error;
    std::filesystem::remove_all(tree, error);
    if (folder) {
        std::filesystem::create_directories(tree / "bin");
        std::ofstream(tree / "version") << version << '\n';
        std::ofstream(tree / "bin" / "Bloom") << "program " << version << '\n';
    } else {
        std::filesystem::create_directories(tree.parent_path());
        std::ofstream(tree) << version << '\n';
    }
}

class FakeHost final : public ApplyHost {
  public:
    FakeHost(ApplyPlan plan, const Candidate candidate)
        : plan_(std::move(plan)), candidate_(candidate) {}

    // What the test sets.
    bool exchangeSupported = true;
    int bloomExitsAfterWaits = 0;         // -1: never
    int failMoveAt = -1;                  // the move (rename or exchange) that returns an error
    int stopBeforeMove = -1;              // the move the helper is stopped before
    int stopAfterMove = -1;               // the move the helper is stopped after
    std::function<void()> duringWait;     // what happens elsewhere while the helper waits
    // What the test reads.
    int moves = 0;
    std::chrono::milliseconds waited{0};
    std::vector<std::string> launchedVersions;
    std::set<std::uint64_t> terminated;

    [[nodiscard]] bool processRunning(const std::uint64_t processId) override {
        if (processId == plan_.processId) {
            return bloomExitsAfterWaits < 0 || waits_ < bloomExitsAfterWaits;
        }
        return running_.contains(processId) && !terminated.contains(processId);
    }

    [[nodiscard]] std::optional<std::uint64_t>
    launch(const path& program, const std::vector<std::string>& /*arguments*/) override {
        // The program that is started is whatever is installed now; it says its own version.
        const auto version = versionOf(plan_.installed);
        launchedVersions.push_back(version);
        const bool isCandidate = version == plan_.version;
        std::error_code error;
        if (!std::filesystem::exists(program, error) ||
            (isCandidate && candidate_ == Candidate::CannotStart)) {
            return std::nullopt;
        }
        const std::uint64_t processId = nextProcess_++;
        if (!isCandidate || candidate_ == Candidate::Confirms) {
            (void)writeStartConfirmation(plan_.confirmation, version);
            running_.insert(processId);
        } else if (candidate_ == Candidate::Hangs) {
            running_.insert(processId);
        }
        return processId;
    }

    void terminate(const std::uint64_t processId) override { terminated.insert(processId); }

    void wait(const std::chrono::milliseconds duration) override {
        waited += duration;
        ++waits_;
        if (duringWait) {
            duringWait();
        }
    }

    [[nodiscard]] std::error_code rename(const path& from, const path& to) override {
        return move([&](std::error_code& error) { std::filesystem::rename(from, to, error); });
    }

    [[nodiscard]] std::error_code exchange(const path& first, const path& second) override {
        if (!exchangeSupported) {
            return std::make_error_code(std::errc::function_not_supported);
        }
        return move([&](std::error_code& error) {
            auto between = first;
            between += ".exchange";
            std::filesystem::rename(first, between, error);
            if (!error) {
                std::filesystem::rename(second, first, error);
            }
            if (!error) {
                std::filesystem::rename(between, second, error);
            }
        });
    }

    [[nodiscard]] std::string identity(const path& tree) override { return versionOf(tree); }

  private:
    [[nodiscard]] std::error_code move(const std::function<void(std::error_code&)>& action) {
        const int index = moves++;
        if (index == stopBeforeMove) {
            throw Stopped{};
        }
        if (index == failMoveAt) {
            return std::make_error_code(std::errc::permission_denied);
        }
        std::error_code error;
        action(error);
        if (index == stopAfterMove) {
            throw Stopped{};
        }
        return error;
    }

    ApplyPlan plan_;
    Candidate candidate_;
    int waits_ = 0;
    std::uint64_t nextProcess_ = 1000;
    std::set<std::uint64_t> running_;
};

struct Scene final {
    path root;
    ApplyPlan plan;
    path journal;

    Scene(const path& base, const std::string_view name, const bool folder, const ApplyMode mode) {
        root = base / name;
        std::error_code error;
        std::filesystem::remove_all(root, error);
        plan.mode = mode;
        plan.processId = 77;
        plan.installed = root / (folder ? "opt/bloom" : "apps/Bloom.AppImage");
        plan.staged = root / (folder ? "opt/.bloom-next" : "apps/.Bloom.AppImage.next");
        plan.previous = root / (folder ? "opt/bloom.previous" : "apps/Bloom.AppImage.previous");
        plan.program = folder ? plan.installed / "bin" / "Bloom" : plan.installed;
        plan.confirmation = root / "data" / "started";
        plan.notice = root / "data" / "restored";
        plan.version = "2";
        plan.exitTimeout = std::chrono::seconds(10);
        plan.confirmTimeout = std::chrono::seconds(10);
        journal = root / "data" / "plan.journal";
        std::filesystem::create_directories(root / "data");
        makeVersion(plan.installed, "1", folder);
        if (mode == ApplyMode::Swap) {
            makeVersion(plan.staged, "2", folder);
        }
        // What the running version wrote when it started.
        (void)writeStartConfirmation(plan.confirmation, "1");
    }

    [[nodiscard]] bool exists(const path& tree) const {
        std::error_code error;
        return std::filesystem::exists(tree, error);
    }
    [[nodiscard]] std::string installed() const { return versionOf(plan.installed); }
    [[nodiscard]] std::string previous() const { return versionOf(plan.previous); }
    // The installation is whole: the program of the version the folder says it is.
    [[nodiscard]] bool whole() const {
        std::error_code error;
        if (!std::filesystem::is_directory(plan.installed, error)) {
            return !installed().empty();
        }
        std::ifstream program(plan.program);
        std::string text;
        std::getline(program, text);
        return text == "program " + installed();
    }
    [[nodiscard]] bool leftovers() const {
        auto failed = plan.installed;
        failed += ".failed";
        auto old = plan.previous;
        old += ".old";
        return exists(failed) || exists(old);
    }
};

} // namespace

int main() {
    Expectations checks;
    std::error_code error;
    const auto base = bloom::test_support::processTempDirectory() /
                      ("bloom-update-helper-" + std::to_string(std::random_device{}()));

    // The plan file.
    {
        Scene scene(base, "plan", true, ApplyMode::Swap);
        scene.plan.arguments = {"--from-update", "two words"};
        const auto file = scene.root / "data" / "plan";
        checks.expect(writeApplyPlan(file, scene.plan), "plan: it is written");
        const auto read = readApplyPlan(file);
        checks.expect(read.has_value() && read->mode == ApplyMode::Swap && read->processId == 77 &&
                          read->installed == scene.plan.installed &&
                          read->staged == scene.plan.staged &&
                          read->previous == scene.plan.previous &&
                          read->program == scene.plan.program &&
                          read->arguments == scene.plan.arguments &&
                          read->confirmation == scene.plan.confirmation &&
                          read->notice == scene.plan.notice && read->version == "2" &&
                          read->exitTimeout == scene.plan.exitTimeout &&
                          read->confirmTimeout == scene.plan.confirmTimeout,
                      "plan: what is read is what was written");
        auto relative = scene.plan;
        relative.installed = "opt/bloom";
        checks.expect(writeApplyPlan(file, relative) && !readApplyPlan(file).has_value(),
                      "plan: a path that is not absolute is refused");
        auto same = scene.plan;
        same.staged = same.installed;
        checks.expect(writeApplyPlan(file, same) && !readApplyPlan(file).has_value(),
                      "plan: a prepared version at the installed path is refused");
        auto twoLines = scene.plan;
        twoLines.version = "2\nmode=watch";
        checks.expect(!writeApplyPlan(file, twoLines), "plan: a value of two lines is not written");
        checks.expect(!readApplyPlan(scene.root / "data" / "missing").has_value(),
                      "plan: a missing file is no plan");
    }

    // A folder and a single file are replaced and confirmed, with and without the one-step
    // exchange.
    for (const bool folder : {true, false}) {
        for (const bool exchange : {true, false}) {
            const std::string label = std::string(folder ? "folder" : "file") +
                                      (exchange ? ", exchange" : ", two moves");
            Scene scene(base, "happy", folder, ApplyMode::Swap);
            FakeHost host(scene.plan, Candidate::Confirms);
            host.exchangeSupported = exchange;
            host.bloomExitsAfterWaits = 3;
            const auto result = applyUpdate(scene.plan, scene.journal, host);
            checks.expect(result.outcome == ApplyOutcome::Updated && scene.installed() == "2" &&
                              scene.previous() == "1" && !scene.exists(scene.plan.staged) &&
                              scene.whole() && !scene.leftovers(),
                          "replace (" + label + "): the new version is installed, the previous "
                                                "one is kept: " + result.detail);
            checks.expect(host.launchedVersions == std::vector<std::string>{"2"} &&
                              !scene.exists(scene.plan.notice),
                          "replace (" + label + "): the new version is started once");
            checks.expect(host.waited == std::chrono::milliseconds(600),
                          "replace (" + label + "): it waited for Bloom to exit and no longer");
            const auto again = applyUpdate(scene.plan, scene.journal, host);
            checks.expect(again.outcome == ApplyOutcome::AlreadyDone && scene.installed() == "2",
                          "replace (" + label + "): a second run of a finished plan does nothing");
        }
    }

    // The version kept from the update before this one is removed, not restored.
    {
        Scene scene(base, "older", true, ApplyMode::Swap);
        makeVersion(scene.plan.previous, "0", true);
        FakeHost host(scene.plan, Candidate::Confirms);
        const auto result = applyUpdate(scene.plan, scene.journal, host);
        checks.expect(result.outcome == ApplyOutcome::Updated && scene.installed() == "2" &&
                          scene.previous() == "1" && !scene.leftovers(),
                      "older previous: one previous version is kept, the one just replaced");
    }

    // A candidate that does not start is replaced by the previous version again.
    for (const auto candidate :
         {Candidate::ExitsAtStart, Candidate::Hangs, Candidate::CannotStart}) {
        for (const bool exchange : {true, false}) {
            const std::string label =
                std::string(candidate == Candidate::ExitsAtStart
                                ? "exits at start"
                                : candidate == Candidate::Hangs ? "hangs" : "cannot be started") +
                (exchange ? ", exchange" : ", two moves");
            Scene scene(base, "rollback", true, ApplyMode::Swap);
            FakeHost host(scene.plan, candidate);
            host.exchangeSupported = exchange;
            const auto result = applyUpdate(scene.plan, scene.journal, host);
            checks.expect(result.outcome == ApplyOutcome::Restored && scene.installed() == "1" &&
                              scene.whole() && !scene.exists(scene.plan.previous) &&
                              !scene.exists(scene.plan.staged) && !scene.leftovers(),
                          "rollback (" + label + "): the previous version is back: " +
                              result.detail);
            const auto notice = takeRestoreNotice(scene.plan.notice);
            checks.expect(notice == std::optional<std::string>("2") &&
                              !scene.exists(scene.plan.notice),
                          "rollback (" + label + "): the restored version is told which update "
                                                 "failed, once");
            checks.expect(host.launchedVersions == std::vector<std::string>{"2", "1"},
                          "rollback (" + label + "): the previous version is started after the "
                                                 "new one");
            checks.expect((candidate == Candidate::Hangs) == !host.terminated.empty(),
                          "rollback (" + label + "): only a candidate that still runs is stopped");
            checks.expect(candidate != Candidate::Hangs ||
                              host.waited >= scene.plan.confirmTimeout,
                          "rollback (" + label + "): a hanging candidate is given its time");
            checks.expect(candidate == Candidate::Hangs ||
                              host.waited < std::chrono::seconds(1),
                          "rollback (" + label + "): a candidate that is gone is not waited for");
        }
    }

    // Bloom does not exit: nothing is touched, and a later run carries the plan out.
    {
        Scene scene(base, "running", true, ApplyMode::Swap);
        FakeHost stuck(scene.plan, Candidate::Confirms);
        stuck.bloomExitsAfterWaits = -1;
        const auto result = applyUpdate(scene.plan, scene.journal, stuck);
        checks.expect(result.outcome == ApplyOutcome::StillRunning && scene.installed() == "1" &&
                          versionOf(scene.plan.staged) == "2" && stuck.moves == 0 &&
                          stuck.launchedVersions.empty() &&
                          stuck.waited >= scene.plan.exitTimeout,
                      "still running: nothing is moved or started while Bloom runs");
        FakeHost later(scene.plan, Candidate::Confirms);
        checks.expect(applyUpdate(scene.plan, scene.journal, later).outcome ==
                              ApplyOutcome::Updated &&
                          scene.installed() == "2",
                      "still running: a later run finds Bloom gone and updates");
    }

    // A failure, or a stop, at every move: afterwards (after one more run, for a stop) the
    // installation is whole and is the new or the previous version, and it was started.
    for (const bool folder : {true, false}) {
        for (const bool exchange : {true, false}) {
            for (const auto candidate : {Candidate::Confirms, Candidate::ExitsAtStart}) {
                // How many moves a clean run makes.
                int count = 0;
                {
                    Scene scene(base, "count", folder, ApplyMode::Swap);
                    FakeHost host(scene.plan, candidate);
                    host.exchangeSupported = exchange;
                    (void)applyUpdate(scene.plan, scene.journal, host);
                    count = host.moves;
                }
                checks.expect(count >= 2, "every step: a run makes moves");
                for (int kind = 0; kind < 3; ++kind) {
                    for (int at = 0; at < count; ++at) {
                        const std::string label =
                            std::string(kind == 0 ? "failure at" : kind == 1 ? "stop before"
                                                                               : "stop after") +
                            " move " + std::to_string(at) + (folder ? ", folder" : ", file") +
                            (exchange ? ", exchange" : ", two moves") +
                            (candidate == Candidate::Confirms ? ", starts" : ", does not start");
                        Scene scene(base, "step", folder, ApplyMode::Swap);
                        FakeHost host(scene.plan, candidate);
                        host.exchangeSupported = exchange;
                        (kind == 0 ? host.failMoveAt : kind == 1 ? host.stopBeforeMove
                                                                 : host.stopAfterMove) = at;
                        ApplyResult result;
                        bool stopped = false;
                        try {
                            result = applyUpdate(scene.plan, scene.journal, host);
                        } catch (const Stopped&) {
                            stopped = true;
                        }
                        std::vector<std::string> launched = host.launchedVersions;
                        if (stopped) {
                            FakeHost second(scene.plan, candidate);
                            second.exchangeSupported = exchange;
                            result = applyUpdate(scene.plan, scene.journal, second);
                            launched.insert(launched.end(), second.launchedVersions.begin(),
                                            second.launchedVersions.end());
                        }
                        const auto installed = scene.installed();
                        const bool settled = result.outcome == ApplyOutcome::Updated ||
                                             result.outcome == ApplyOutcome::Restored;
                        // A move that fails while a version that does not start is being taken
                        // out again is two faults at once. The helper then stops and says so,
                        // with the installation whole and the previous version kept beside it.
                        const bool keptBeside = scene.previous() == "1" ||
                                                versionOf(scene.plan.staged) == "1";
                        const bool stoppedSafely = kind == 0 &&
                                                   candidate != Candidate::Confirms &&
                                                   result.outcome == ApplyOutcome::Failed &&
                                                   keptBeside;
                        checks.expect((settled || stoppedSafely) && scene.whole() &&
                                          (installed == "1" || installed == "2"),
                                      "every step (" + label + "): the installation is whole (" +
                                          installed + "): " + result.detail);
                        checks.expect(result.outcome != ApplyOutcome::Updated ||
                                          (installed == "2" && candidate == Candidate::Confirms),
                                      "every step (" + label + "): updated means the new version "
                                                               "runs");
                        checks.expect(result.outcome != ApplyOutcome::Restored ||
                                          (installed == "1" && !launched.empty() &&
                                           launched.back() == "1"),
                                      "every step (" + label + "): restored means the previous "
                                                               "version is back and started");
                        checks.expect(candidate == Candidate::Confirms || installed == "1" ||
                                          stoppedSafely,
                                      "every step (" + label + "): a version that does not start "
                                                               "stays installed only when it "
                                                               "could not be moved away");
                    }
                }
            }
        }
    }

    // Watching a platform updater: confirmed, unchanged, or put back.
    {
        Scene scene(base, "watch-new", true, ApplyMode::Watch);
        makeVersion(scene.plan.previous, "1", true);
        FakeHost host(scene.plan, Candidate::Confirms);
        // The platform's updater replaces the installation and starts it.
        host.duringWait = [&scene] {
            makeVersion(scene.plan.installed, "2", true);
            (void)writeStartConfirmation(scene.plan.confirmation, "2");
        };
        const auto result = applyUpdate(scene.plan, scene.journal, host);
        checks.expect(result.outcome == ApplyOutcome::Updated && scene.installed() == "2" &&
                          scene.previous() == "1" && host.launchedVersions.empty() &&
                          host.moves == 0,
                      "watch: a new version that confirms is left in place, and the helper "
                      "starts and moves nothing");
    }
    {
        // Bloom's quit stops at unsaved work and is finished much later; the platform's updater
        // installs then, and the helper is still there to see the new version start.
        Scene scene(base, "watch-late", true, ApplyMode::Watch);
        makeVersion(scene.plan.previous, "1", true);
        FakeHost host(scene.plan, Candidate::Confirms);
        const int waitsOfAnHour = 5 * 60 * 60;
        host.bloomExitsAfterWaits = waitsOfAnHour;
        host.duringWait = [&scene, &host] {
            if (host.waited > std::chrono::hours(1)) {
                makeVersion(scene.plan.installed, "2", true);
                (void)writeStartConfirmation(scene.plan.confirmation, "2");
            }
        };
        const auto result = applyUpdate(scene.plan, scene.journal, host);
        checks.expect(result.outcome == ApplyOutcome::Updated && scene.installed() == "2" &&
                          scene.previous() == "1" && host.waited > std::chrono::hours(1) &&
                          host.waited > scene.plan.exitTimeout && host.moves == 0,
                      "watch: a Bloom that quits an hour later is still watched: " +
                          result.detail);
    }
    {
        Scene scene(base, "watch-none", true, ApplyMode::Watch);
        makeVersion(scene.plan.previous, "1", true);
        makeVersion(scene.plan.installed, "2", true);
        FakeHost host(scene.plan, Candidate::ExitsAtStart);
        const auto result = applyUpdate(scene.plan, scene.journal, host);
        checks.expect(result.outcome == ApplyOutcome::Restored && scene.installed() == "1" &&
                          scene.whole() && !scene.leftovers() &&
                          host.launchedVersions == std::vector<std::string>{"1"} &&
                          host.waited >= scene.plan.confirmTimeout &&
                          takeRestoreNotice(scene.plan.notice) == std::optional<std::string>("2"),
                      "watch: a new version that never confirms is replaced by the kept one: " +
                          result.detail);
    }
    {
        Scene scene(base, "watch-same", true, ApplyMode::Watch);
        makeVersion(scene.plan.previous, "1", true);
        FakeHost host(scene.plan, Candidate::Confirms);
        // The platform's updater did nothing, and the installed version starts again.
        host.duringWait = [&scene] {
            (void)writeStartConfirmation(scene.plan.confirmation, "1");
        };
        const auto result = applyUpdate(scene.plan, scene.journal, host);
        checks.expect(result.outcome == ApplyOutcome::Unchanged && scene.installed() == "1" &&
                          host.moves == 0 && host.launchedVersions.empty(),
                      "watch: an installation the platform's updater left alone stays as it is");
    }
    {
        Scene scene(base, "watch-nothing-kept", true, ApplyMode::Watch);
        makeVersion(scene.plan.installed, "2", true);
        FakeHost host(scene.plan, Candidate::ExitsAtStart);
        const auto result = applyUpdate(scene.plan, scene.journal, host);
        checks.expect(result.outcome == ApplyOutcome::Failed && scene.installed() == "2" &&
                          host.launchedVersions.empty(),
                      "watch: without a kept version nothing is moved and nothing is started");
    }

    std::filesystem::remove_all(base, error);
    return checks.result();
}
