// The update download (ADR 0041, #734) as a background job of the update service: progress,
// cancel, verification before anything is prepared, never on the interface thread, and the
// hand-over to the helper. The server is on this machine, inside the test.
#include "update_test_support.hpp"

#include <bloom/update/apply.hpp>

#include <QCoreApplication>

using namespace bloom;
using namespace bloom::ui::update_tests;
using bloom::test_support::awaitQtCondition;

namespace {

struct Signals final {
    int checks = 0;
    int downloads = 0;
    int restarts = 0;
    std::uint64_t received = 0;
    std::uint64_t total = 0;
    int progressReports = 0;
    explicit Signals(ui::UpdateService& service) {
        QObject::connect(&service, &ui::UpdateService::checkFinished, &service,
                         [this](bool) { ++checks; });
        QObject::connect(&service, &ui::UpdateService::downloadFinished, &service,
                         [this] { ++downloads; });
        QObject::connect(&service, &ui::UpdateService::restartRequested, &service,
                         [this] { ++restarts; });
        QObject::connect(&service, &ui::UpdateService::downloadProgress, &service,
                         [this](const std::uint64_t nowReceived, const std::uint64_t nowTotal) {
                             ++progressReports;
                             received = nowReceived;
                             total = nowTotal;
                         });
    }
};

// Serves the feed, and `file` as every release file.
void serve(Loopback& server, const QByteArray& file) {
    server.respond = [file](const QByteArray& request) {
        return request.startsWith("GET /alpha.json ") ? answer(fixture("feed.json")) : answer(file);
    };
}

[[nodiscard]] bool empty(const std::filesystem::path& folder) {
    std::error_code error;
    return !std::filesystem::exists(folder, error) || std::filesystem::is_empty(folder, error);
}

[[nodiscard]] bool check(ui::UpdateService& service, const Signals& seen, const std::string& name) {
    const bool started = service.checkNow();
    const bool finished = started && awaitQtCondition("the check finishes",
                                                      [&] { return seen.checks == 1; });
    expect(finished && service.lastCheck()->decision.file.has_value(),
           name + ": the check offers a file");
    return finished && service.lastCheck()->decision.file.has_value();
}

struct Staged final {
    std::shared_ptr<std::atomic<bool>> onInterfaceThread = std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<std::atomic<int>> calls = std::make_shared<std::atomic<int>>(0);
    std::shared_ptr<std::atomic<bool>> fileWasWhole = std::make_shared<std::atomic<bool>>(false);

    // Stands in for unpacking: the fixture file is not a package. Notes that the file it was
    // given is the whole, verified one.
    void into(ui::UpdateEnvironment& environment, const std::filesystem::path& staged,
              const std::uintmax_t size) const {
        environment.stage = [=, onThread = onInterfaceThread, count = calls,
                             whole = fileWasWhole](const std::filesystem::path& file,
                                                   const update::InstallFacts&,
                                                   const std::function<bool()>&) {
            count->fetch_add(1);
            if (QThread::currentThread() == QCoreApplication::instance()->thread()) {
                onThread->store(true);
            }
            std::error_code error;
            whole->store(std::filesystem::file_size(file, error) == size);
            std::filesystem::create_directories(staged, error);
            return update::StagingResult{staged, {}};
        };
    }
};

void testDownload() {
    const QByteArray file = fixture("update.bin");

    // The file arrives, is verified and prepared; the helper is then given the plan.
    {
        Loopback server;
        serve(server, file);
        Bench bench("ready");
        auto environment = bench.environment(server.origin());
        const auto staged = bench.root / "opt" / ".bloom-update" / "Bloom";
        Staged staging;
        staging.into(environment, staged, static_cast<std::uintmax_t>(file.size()));
        environment.installedHelper = bench.root / "helper-in-package";
        std::ofstream(environment.installedHelper) << "helper\n";
        auto started = std::make_shared<std::vector<std::filesystem::path>>();
        environment.startHelper = [started](const std::filesystem::path& helper,
                                            const std::filesystem::path& plan) {
            started->push_back(helper);
            started->push_back(plan);
            return true;
        };
        const auto folders = environment.folders;
        ui::UpdateService service(bench.scheduler, bench.bridge, std::move(environment));
        Signals seen(service);
        expect(!service.startDownload(), "ready: nothing to download before a check");
        if (!check(service, seen, "ready")) {
            return;
        }
        expect(service.startDownload(), "ready: the download starts");
        expect(service.state() == ui::UpdateService::State::Downloading && seen.downloads == 0,
               "ready: the call returns while the download runs");
        expect(!service.startDownload() && !service.checkNow(),
               "ready: nothing else starts beside it");
        expect(awaitQtCondition("the download finishes", [&] { return seen.downloads == 1; }),
               "ready: it finishes");
        const auto& outcome = service.lastDownload();
        expect(outcome.has_value() && outcome->ready() && outcome->version == "0.2.0-alpha.3" &&
                   outcome->staging.staged == staged,
               "ready: the version is downloaded, verified and prepared");
        expect(service.state() == ui::UpdateService::State::ReadyToRestart,
               "ready: the service waits for the restart");
        expect(seen.progressReports > 0 && seen.received == static_cast<std::uint64_t>(file.size()) &&
                   seen.total == static_cast<std::uint64_t>(file.size()),
               "ready: progress was reported up to the file's size");
        expect(staging.calls->load() == 1 && staging.fileWasWhole->load(),
               "ready: the prepared version is made from the whole verified file");
        expect(!bench.usedOnInterfaceThread->load() && !staging.onInterfaceThread->load(),
               "ready: download and preparation ran on a job's thread, never the interface thread");
        expect(empty(folders.downloads()), "ready: the downloaded file is gone once prepared");
        expect(server.requests.size() == 2 &&
                   server.requests.back().startsWith(
                       "GET /Bloom-0.2.0-alpha.3-linux-x86_64.tar.xz HTTP/1.1\r\n"),
               "ready: one request for the feed and one for the file");

        expect(service.restartIntoUpdate() && seen.restarts == 1,
               "ready: the restart is asked for once the helper has the plan");
        const auto plan = update::readApplyPlan(folders.plan());
        expect(plan.has_value() && plan->mode == update::ApplyMode::Swap &&
                   plan->staged == staged && plan->installed == bench.root / "opt" / "bloom" &&
                   plan->version == "0.2.0-alpha.3" && plan->processId == 4242 &&
                   plan->program == bench.root / "opt" / "bloom" / "bin" / "Bloom",
               "ready: the plan names the prepared version, the installation and this process");
        expect(started->size() == 2 && started->front() == folders.helper() &&
                   started->back() == folders.plan() &&
                   std::filesystem::exists(folders.helper()),
               "ready: the helper is started from its copy outside the installation");
    }
    // A file that is not the one the feed describes is refused and nothing is prepared.
    {
        Loopback server;
        QByteArray tampered = file;
        tampered[tampered.size() / 2] = static_cast<char>(tampered[tampered.size() / 2] ^ 0x01);
        serve(server, tampered);
        Bench bench("tampered");
        auto environment = bench.environment(server.origin());
        Staged staging;
        staging.into(environment, bench.root / "staged", 0);
        const auto folders = environment.folders;
        ui::UpdateService service(bench.scheduler, bench.bridge, std::move(environment));
        Signals seen(service);
        if (!check(service, seen, "tampered")) {
            return;
        }
        expect(service.startDownload(), "tampered: the download starts");
        expect(awaitQtCondition("the download finishes", [&] { return seen.downloads == 1; }),
               "tampered: it finishes");
        const auto& outcome = service.lastDownload();
        expect(outcome.has_value() && !outcome->ready() && !outcome->cancelled &&
                   outcome->download.diagnostic.refusal ==
                       update::UpdateRefusal::FileChecksumMismatch,
               "tampered: the file is refused for its checksum");
        expect(staging.calls->load() == 0 && empty(folders.downloads()),
               "tampered: nothing is prepared and nothing of the file is kept");
        expect(service.state() == ui::UpdateService::State::Idle && !service.restartIntoUpdate(),
               "tampered: there is nothing to restart into");
    }
    // A longer file than the feed gives is cut off.
    {
        Loopback server;
        serve(server, file + "more");
        Bench bench("longer");
        auto environment = bench.environment(server.origin());
        const auto folders = environment.folders;
        ui::UpdateService service(bench.scheduler, bench.bridge, std::move(environment));
        Signals seen(service);
        if (!check(service, seen, "longer")) {
            return;
        }
        expect(service.startDownload(), "longer: the download starts");
        expect(awaitQtCondition("the download finishes", [&] { return seen.downloads == 1; }),
               "longer: it finishes");
        expect(service.lastDownload().has_value() && !service.lastDownload()->ready() &&
                   service.lastDownload()->download.diagnostic.refusal ==
                       update::UpdateRefusal::FileSizeMismatch &&
                   empty(folders.downloads()),
               "longer: a file over its size is refused and nothing is kept");
    }
    // Half the file arrives and then nothing: the download is cancelled and leaves nothing.
    {
        Loopback server;
        serve(server, file);
        Bench bench("cancelled");
        auto environment = bench.environment(server.origin());
        Staged staging;
        staging.into(environment, bench.root / "staged", 0);
        const auto folders = environment.folders;
        ui::UpdateService service(bench.scheduler, bench.bridge, std::move(environment));
        Signals seen(service);
        if (!check(service, seen, "cancelled")) {
            return;
        }
        server.stallAfter = answer(file).size() - file.size() / 2;
        expect(service.startDownload(), "cancelled: the download starts");
        expect(awaitQtCondition("part of the file arrives", [&] { return seen.received > 0; }),
               "cancelled: progress is reported while the file arrives");
        expect(seen.received < static_cast<std::uint64_t>(file.size()) && seen.downloads == 0,
               "cancelled: the download is still running");
        service.cancel();
        expect(awaitQtCondition("the download ends", [&] { return seen.downloads == 1; }),
               "cancelled: the download ends");
        expect(service.lastDownload().has_value() && service.lastDownload()->cancelled &&
                   !service.lastDownload()->ready() &&
                   service.state() == ui::UpdateService::State::Idle,
               "cancelled: it is reported as cancelled");
        expect(staging.calls->load() == 0 && empty(folders.downloads()),
               "cancelled: nothing is prepared and no part of the file is left");
    }
    // The server has no such file.
    {
        Loopback server;
        server.respond = [](const QByteArray& request) {
            return request.startsWith("GET /alpha.json ")
                       ? answer(fixture("feed.json"))
                       : QByteArray("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n"
                                    "Connection: close\r\n\r\n");
        };
        Bench bench("missing");
        auto environment = bench.environment(server.origin());
        const auto folders = environment.folders;
        ui::UpdateService service(bench.scheduler, bench.bridge, std::move(environment));
        Signals seen(service);
        if (!check(service, seen, "missing")) {
            return;
        }
        expect(service.startDownload(), "missing: the download starts");
        expect(awaitQtCondition("the download ends", [&] { return seen.downloads == 1; }),
               "missing: the download ends");
        expect(service.lastDownload().has_value() && !service.lastDownload()->ready() &&
                   service.lastDownload()->download.transport.status ==
                       update::TransportStatus::HttpError &&
                   empty(folders.downloads()),
               "missing: the server's answer is reported and nothing is kept");
    }
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    testDownload();
    return failures + bloom::test_support::awaitFailureCount() == 0 ? 0 : 1;
}
