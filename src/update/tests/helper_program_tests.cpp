// The update helper as the program it is: `bloom-update-helper --plan <file>` run as a process
// of its own against folders in a temporary directory, with a small stand-in program as the
// installed and the new Bloom (fake_app.cpp). The steps themselves, with a failure at every one
// of them, are helper_tests.cpp's; this proves the program around them: the plan file, waiting
// for a running process, really moving folders, really starting the new version, the log.
//
//   helper_program_tests <bloom-update-helper> <fake_app>
#include "unique_temp_dir.hpp"
#include "update_test_support.hpp"

#include <bloom/update/apply.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <random>

namespace {

using namespace bloom::update;
using namespace bloom::update::testing;
using std::filesystem::path;

[[nodiscard]] std::string firstLine(const path& file) {
    std::ifstream stream(file);
    std::string line;
    std::getline(stream, line);
    return line;
}

// Lets the other processes run until `done`, for at most a minute.
[[nodiscard]] bool until(ApplyHost& host, const std::function<bool()>& done) {
    for (int waited = 0; waited < 3000; ++waited) {
        if (done()) {
            return true;
        }
        host.wait(std::chrono::milliseconds(20));
    }
    return done();
}

struct Scene final {
    path root;
    ApplyPlan plan;

    Scene(const path& base, const std::string_view name, const path& fakeApp) : root(base / name) {
        std::filesystem::create_directories(root / "data");
        makeVersion(root / "Bloom", "1.0.0", fakeApp, false);
        plan.mode = ApplyMode::Swap;
        plan.installed = root / "Bloom";
        plan.staged = root / "Bloom.new";
        plan.previous = root / "Bloom.previous";
        plan.program = plan.installed / fakeApp.filename();
        plan.confirmation = root / "data" / "started";
        plan.arguments = {"--confirm", plan.confirmation.string()};
        plan.notice = root / "data" / "restored";
        plan.version = "2.0.0";
        plan.exitTimeout = std::chrono::seconds(60);
        plan.confirmTimeout = std::chrono::seconds(60);
    }

    static void makeVersion(const path& folder, const std::string_view version,
                            const path& fakeApp, const bool fails) {
        std::filesystem::create_directories(folder);
        std::filesystem::copy_file(fakeApp, folder / fakeApp.filename());
        std::ofstream(folder / "version") << version << '\n';
        if (fails) {
            std::ofstream(folder / "fails") << '\n';
        }
    }

    [[nodiscard]] path planFile() const { return root / "data" / "plan"; }
    [[nodiscard]] std::string log() const { return firstLine(root / "data" / "plan.log"); }
};

// Runs the helper to its end. False when it could not be started or did not end.
[[nodiscard]] bool runHelper(ApplyHost& host, const path& helper, const Scene& scene,
                             const std::function<void()>& meanwhile = {}) {
    if (!writeApplyPlan(scene.planFile(), scene.plan)) {
        return false;
    }
    const auto started = host.launch(helper, {"--plan", scene.planFile().string()});
    if (!started.has_value()) {
        return false;
    }
    if (meanwhile) {
        meanwhile();
    }
    if (until(host, [&] { return !host.processRunning(*started); })) {
        return true;
    }
    // A helper that did not end is not left running after the test.
    host.terminate(*started);
    return false;
}

} // namespace

int main(const int argc, char* argv[]) {
    Expectations checks;
    if (argc != 3) {
        std::cerr << "usage: helper_program_tests <bloom-update-helper> <fake_app>\n";
        return 2;
    }
    const path helper = argv[1];
    const path fakeApp = argv[2];
    std::error_code error;
    const auto base = bloom::test_support::processTempDirectory() /
                      ("bloom-update-program-" + std::to_string(std::random_device{}()));
    const auto host = makeSystemApplyHost();

    // A folder is replaced while the installed version is still running: the helper waits for
    // it, moves the folders, starts the new version and keeps the previous one.
    {
        Scene scene(base, "updated", fakeApp);
        Scene::makeVersion(scene.plan.staged, "2.0.0", fakeApp, false);
        const auto release = scene.root / "data" / "release";
        const auto running =
            host->launch(scene.plan.program, {"--hold", release.string()});
        checks.expect(running.has_value(), "updated: the installed version runs");
        scene.plan.processId = running.value_or(0);
        bool waitedForExit = false;
        const bool ended = runHelper(*host, helper, scene, [&] {
            // The helper has nothing to do but wait: it writes its first journal line only
            // after the installed version has ended.
            waitedForExit = !std::filesystem::exists(scene.root / "data" / "plan.journal");
            std::ofstream(release) << '\n';
            // (This test started the installed version, so it also collects it.)
            (void)until(*host, [&] { return !host->processRunning(scene.plan.processId); });
        });
        checks.expect(ended, "updated: the helper ends");
        checks.expect(waitedForExit, "updated: nothing is touched while the version runs");
        checks.expect(scene.log() == "updated: the new version started",
                      "updated: the log says so (" + scene.log() + ")");
        checks.expect(firstLine(scene.plan.installed / "version") == "2.0.0",
                      "updated: the new version is installed");
        checks.expect(firstLine(scene.plan.previous / "version") == "1.0.0",
                      "updated: the previous version is kept");
        checks.expect(!std::filesystem::exists(scene.plan.staged),
                      "updated: nothing is left under the prepared version's name");
        checks.expect(readStartConfirmation(scene.plan.confirmation) == std::string("2.0.0"),
                      "updated: the new version confirmed its start");
        checks.expect(!std::filesystem::exists(scene.plan.notice), "updated: no restore notice");

        // The same plan again changes nothing.
        checks.expect(runHelper(*host, helper, scene), "updated: a second run ends");
        checks.expect(firstLine(scene.plan.installed / "version") == "2.0.0" &&
                          firstLine(scene.plan.previous / "version") == "1.0.0",
                      "updated: a second run of the plan changes nothing");
    }

    // The new version cannot start: the previous one is put back and started, and is told.
    {
        Scene scene(base, "restored", fakeApp);
        Scene::makeVersion(scene.plan.staged, "2.0.0", fakeApp, true);
        checks.expect(runHelper(*host, helper, scene), "restored: the helper ends");
        checks.expect(scene.log() == "restored: the new version did not start",
                      "restored: the log says so (" + scene.log() + ")");
        checks.expect(firstLine(scene.plan.installed / "version") == "1.0.0",
                      "restored: the previous version is installed again");
        checks.expect(!std::filesystem::exists(scene.plan.installed / "fails"),
                      "restored: nothing of the failed version is in the installation");
        checks.expect(!std::filesystem::exists(scene.plan.staged) &&
                          !std::filesystem::exists(scene.plan.previous),
                      "restored: the failed version is removed");
        checks.expect(until(*host,
                            [&] {
                                return readStartConfirmation(scene.plan.confirmation) ==
                                       std::string("1.0.0");
                            }),
                      "restored: the previous version was started");
        checks.expect(takeRestoreNotice(scene.plan.notice) == std::string("2.0.0"),
                      "restored: the notice names the version that did not start");
    }

    // Watch: the platform's updater has already put the new version in place; the helper only
    // waits for its start, and puts the kept version back when it does not come.
    {
        Scene scene(base, "watched", fakeApp);
        std::filesystem::rename(scene.plan.installed, scene.plan.previous);
        Scene::makeVersion(scene.plan.installed, "2.0.0", fakeApp, true);
        scene.plan.mode = ApplyMode::Watch;
        scene.plan.staged.clear();
        // Nothing starts the new version here, as when it ends at once; the wait is the plan's.
        scene.plan.confirmTimeout = std::chrono::seconds(1);
        checks.expect(runHelper(*host, helper, scene), "watched: the helper ends");
        checks.expect(scene.log() == "restored: the new version did not start",
                      "watched: the log says so (" + scene.log() + ")");
        checks.expect(firstLine(scene.plan.installed / "version") == "1.0.0",
                      "watched: the kept version is installed again");
        checks.expect(until(*host,
                            [&] {
                                return readStartConfirmation(scene.plan.confirmation) ==
                                       std::string("1.0.0");
                            }),
                      "watched: the kept version was started");
    }

    // Watch, with a Bloom whose parent does not collect it at once (a launcher or script that
    // started it and has not yet asked how it ended). The ended Bloom stays in the process table
    // as a zombie until then; the helper is not its parent and has to take it for ended all the
    // same, on every system. Here this test is that parent and collects it only at the end.
    {
        Scene scene(base, "uncollected", fakeApp);
        std::filesystem::rename(scene.plan.installed, scene.plan.previous);
        Scene::makeVersion(scene.plan.installed, "2.0.0", fakeApp, false);
        scene.plan.mode = ApplyMode::Watch;
        scene.plan.staged.clear();
        const auto release = scene.root / "data" / "release";
        const auto journal = scene.root / "data" / "plan.journal";
        const auto running = host->launch(scene.plan.previous / fakeApp.filename(),
                                          {"--hold", release.string()});
        checks.expect(running.has_value(), "uncollected: the installed version runs");
        scene.plan.processId = running.value_or(0);
        bool exitSeen = false;
        const bool ended = runHelper(*host, helper, scene, [&] {
            std::ofstream(release) << '\n';
            // The helper writes its first journal line when it takes Bloom for ended. Only then
            // does the new version start, as the platform's updater starts it after the quit.
            exitSeen = until(*host, [&] { return std::filesystem::exists(journal); });
            const auto started = host->launch(scene.plan.program,
                                              {"--confirm", scene.plan.confirmation.string()});
            (void)until(*host, [&] { return !started || !host->processRunning(*started); });
        });
        checks.expect(exitSeen, "uncollected: the helper takes the ended Bloom for ended");
        checks.expect(ended, "uncollected: the helper ends");
        checks.expect(scene.log() == "updated: the new version started",
                      "uncollected: the log says so (" + scene.log() + ")");
        checks.expect(firstLine(scene.plan.installed / "version") == "2.0.0",
                      "uncollected: the new version stays installed");
        // The ended Bloom is collected only now.
        (void)until(*host, [&] { return !host->processRunning(scene.plan.processId); });
    }

    // A plan that cannot be read is refused and nothing is started.
    {
        const auto root = base / "unreadable";
        std::filesystem::create_directories(root);
        std::ofstream(root / "plan") << "mode=swap\n";
        const auto started = host->launch(helper, {"--plan", (root / "plan").string()});
        checks.expect(started.has_value() &&
                          until(*host, [&] { return !host->processRunning(*started); }),
                      "unreadable: the helper ends");
        checks.expect(firstLine(root / "plan.log") == "failed: the plan cannot be read",
                      "unreadable: the log says so");
    }

    std::filesystem::remove_all(base, error);
    return checks.result();
}
