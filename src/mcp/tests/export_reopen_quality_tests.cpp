// The export result JSON carries a published movie's reopen measurements (issue #424) beside the
// rest of its verification, named as in host::ExportReopenQuality, and omits the block for an
// output without them. A wrapper around the fake export run adds the measurements the worker
// reports for a lossy movie.

#include <bloom/host/export_queue.hpp>
#include <bloom/host/testing/fake_export_runner.hpp>
#include <bloom/mcp/server_core.hpp>
#include <bloom/output/output_export_resource_ledger.hpp>
#include <bloom/runtime/node_definition_registry.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/scripting/events.hpp>
#include <bloom/scripting/live_session_host.hpp>
#include <bloom/scripting/session.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <yyjson.h>

namespace {

using namespace bloom;

int failures = 0;

void expect(const bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAILED: " << message << '\n';
    }
}

constexpr std::string_view kDigest =
    "ee48a55241f0028b78fd554f14bee0cc5fa9dcdde8c62a7879db0e6043a38292";

// Adds `quality` to every verification the wrapped run reports.
class QualityRun final : public host::ExportRun {
  public:
    QualityRun(std::unique_ptr<host::ExportRun> inner,
               std::optional<host::ExportReopenQuality> quality)
        : inner_(std::move(inner)), quality_(std::move(quality)) {
        refresh();
    }

    void poll() override {
        inner_->poll();
        refresh();
    }
    void stop() override { inner_->stop(); }
    void setPaused(const bool paused) override { inner_->setPaused(paused); }
    [[nodiscard]] bool paused() const noexcept override { return inner_->paused(); }
    [[nodiscard]] bool pauseRequested() const noexcept override { return inner_->pauseRequested(); }
    [[nodiscard]] host::ExportJobRunPhase phase() const noexcept override {
        return inner_->phase();
    }
    [[nodiscard]] bool stopped() const noexcept override { return inner_->stopped(); }
    [[nodiscard]] const std::optional<host::ExportFailure>& failure() const noexcept override {
        return inner_->failure();
    }
    [[nodiscard]] std::span<const host::ExportOutputRunState> outputs() const noexcept override {
        return outputs_;
    }
    [[nodiscard]] std::uint64_t framesCompleted() const noexcept override {
        return inner_->framesCompleted();
    }
    [[nodiscard]] std::uint64_t framesTotal() const noexcept override {
        return inner_->framesTotal();
    }
    [[nodiscard]] std::optional<std::uint64_t> currentFrame() const noexcept override {
        return inner_->currentFrame();
    }
    [[nodiscard]] std::uint32_t outputIndex() const noexcept override {
        return inner_->outputIndex();
    }
    [[nodiscard]] host::ExportDeviceRoute route() const noexcept override {
        return inner_->route();
    }
    [[nodiscard]] std::optional<std::uint64_t> memoryNeededBytes() const noexcept override {
        return inner_->memoryNeededBytes();
    }
    [[nodiscard]] std::vector<host::ExportLogEntry> takeLog() override { return inner_->takeLog(); }

  private:
    void refresh() {
        const auto states = inner_->outputs();
        outputs_.assign(states.begin(), states.end());
        for (auto& output : outputs_)
            if (output.verification)
                output.verification->reopenQuality = quality_;
    }

    std::unique_ptr<host::ExportRun> inner_;
    std::optional<host::ExportReopenQuality> quality_;
    std::vector<host::ExportOutputRunState> outputs_;
};

class QualityBackend final : public host::ExportRunBackend {
  public:
    std::shared_ptr<host::testing::FakeExportRunBackend> fake =
        std::make_shared<host::testing::FakeExportRunBackend>();
    // The measurements the next started run reports (none: an output without them).
    std::optional<host::ExportReopenQuality> next;

    std::unique_ptr<host::ExportRun> start(host::ExportJobRunnerDependencies dependencies,
                                           host::ExportJobRunRequest request) override {
        return std::make_unique<QualityRun>(
            fake->start(std::move(dependencies), std::move(request)), next);
    }
    host::ExportDestinationSample
    sampleDestination(const host::ExportDestinationSampleRequest& request) override {
        return fake->sampleDestination(request);
    }
};

struct Live final {
    std::shared_ptr<scripting::Session> session = [] {
        auto created = scripting::Session::createNew();
        if (!created)
            throw std::runtime_error("session");
        return std::shared_ptr<scripting::Session>(std::move(created).takeSession());
    }();
    std::shared_ptr<scripting::EventStream> events = std::make_shared<scripting::EventStream>();
    scripting::LiveSessionHost host;
    runtime::TaskScheduler scheduler;
    std::unique_ptr<mcp::McpServerCore> core;
    std::unique_ptr<mcp::McpProtocolSession> protocol;
    int nextId = 1;

    Live(mcp::McpExportAccess exports, std::filesystem::path captures) {
        host.replace({events, session->publicationCoordinator(), session->artifactCoordinator()},
                     {.renderSource = session->renderSourceSnapshot(),
                      .context = session->operationContext(),
                      .viewerDisplay = {},
                      .viewerView = {},
                      .projectPath = std::nullopt,
                      .readOnly = false});
        core = std::make_unique<mcp::McpServerCore>(
            mcp::McpCoreDependencies{.session = std::ref(host),
                                     .exports = std::move(exports),
                                     .scheduler = scheduler,
                                     .gpuProvider = nullptr,
                                     .captureDirectory = std::move(captures),
                                     .ui = {},
                                     .serverVersion = "test",
                                     .limits = mcp::kMcpMessageLimits});
        protocol = core->openSession();
        static_cast<void>(send(R"({"jsonrpc":"2.0","id":0,"method":"initialize","params":{)"
                               R"("protocolVersion":"2025-11-25","capabilities":{},)"
                               R"("clientInfo":{"name":"core","version":"1"}}})"));
        static_cast<void>(send(R"({"jsonrpc":"2.0","method":"notifications/initialized"})"));
    }
    ~Live() {
        core.reset();
        scheduler.beginShutdown();
    }
    Live(const Live&) = delete;
    Live& operator=(const Live&) = delete;

    std::string send(const std::string& message) {
        const mcp::McpRequestControl control{};
        return core->handle(*protocol, message, control).response.value_or(std::string{});
    }
    std::string call(const std::string& tool, const std::string& arguments) {
        return send(R"({"jsonrpc":"2.0","id":)" + std::to_string(++nextId) +
                    R"(,"method":"tools/call","params":{"name":")" + tool + R"(","arguments":)" +
                    arguments + "}}");
    }
};

// The id after the first `"job":` of a response.
std::uint64_t jobOf(const std::string& response) {
    const auto at = response.find(R"("job":)");
    return at == std::string::npos ? 0 : std::stoull(response.substr(at + 6));
}

// The first output's verification of `job` in an export.status response's recentTransientJobs.
yyjson_val* verificationOf(yyjson_doc* document, const std::uint64_t job) {
    auto* jobs =
        yyjson_obj_getn(yyjson_obj_get(yyjson_obj_get(yyjson_doc_get_root(document), "result"),
                                       "structuredContent"),
                        "recentTransientJobs", 19);
    std::size_t index = 0;
    std::size_t count = 0;
    yyjson_val* entry = nullptr;
    yyjson_arr_foreach(jobs, index, count, entry) {
        if (yyjson_get_uint(yyjson_obj_get(entry, "id")) != job)
            continue;
        return yyjson_obj_get(yyjson_arr_get_first(yyjson_obj_get(entry, "outputs")),
                              "verification");
    }
    return nullptr;
}

void reopenQualityInStatus() {
    const auto captures =
        std::filesystem::temp_directory_path() /
        ("bloom-mcp-reopen-quality-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(captures);
    auto backend = std::make_shared<QualityBackend>();
    std::unique_ptr<host::ExportQueueService> queue;
    mcp::McpExportAccess exports{
        .run =
            [&](const mcp::McpExportAccess::Work& work, const std::stop_token&) {
                work(queue.get(), 1);
                return true;
            },
        .callerIsOwner = true};
    runtime::NodeDefinitionRegistry registry;
    const runtime::SnapshotCompiler compiler{registry};
    Live server(std::move(exports), captures);
    // The queue borrows the server's session and scheduler: it goes first.
    struct Release final {
        std::unique_ptr<host::ExportQueueService>& queue;
        std::filesystem::path folder;
        ~Release() {
            queue.reset();
            std::error_code ignored;
            std::filesystem::remove_all(folder, ignored);
        }
    } release{queue, captures};
    host::ExportEncoderStatus encoders;
    encoders.probed = true;
    queue = std::make_unique<host::ExportQueueService>(host::ExportQueueServiceDependencies{
        .session = *server.session->hostSession(),
        .scheduler = server.scheduler,
        .compiler = compiler,
        .publications = *server.session->publicationCoordinator(),
        .artifacts = *server.session->artifactCoordinator(),
        .ledger = std::make_shared<output::ExportResourceLedgerV1>(),
        .gpu = nullptr,
        .retirement = nullptr,
        .presets = host::ExportPresetCatalogue::create(
            {.userDataDirectory = std::nullopt, .initialEncoders = encoders}),
        .limits = {},
        .displayProcessor = nullptr,
        .scratchDirectory = {},
        .backend = backend});

    const auto render = [&](const std::string& name) {
        const auto destination = (captures / name).string();
        const auto response = server.call("render", R"({"composition":1,"frame":0,)"
                                                    R"("preset":"PngRgba8SrgbV1","destination":")" +
                                                        destination + R"("})");
        expect(response.find(R"("isError":false)") != std::string::npos,
               "the render publishes: " + response);
        return jobOf(response);
    };
    backend->next =
        host::ExportReopenQuality{.toleranceProfile = "bloom.video-rgba16-srgb.v3",
                                  .toleranceDigest = *core::Sha256Digest::fromLowercaseHex(kDigest),
                                  .maximumError = 30734,
                                  .percentileMaximumError = 6309,
                                  .meanError = 599};
    const auto measured = render("measured.png");
    backend->next.reset();
    const auto plain = render("plain.png");
    expect(measured != 0 && plain != 0 && measured != plain, "both renders report their jobs");

    const auto status = server.call("export.status", "{}");
    auto* document = yyjson_read(status.data(), status.size(), 0);
    expect(document != nullptr, "export.status is JSON");
    if (document == nullptr)
        return;
    auto* withQuality = verificationOf(document, measured);
    auto* quality = yyjson_obj_get(withQuality, "reopenQuality");
    expect(yyjson_is_obj(quality),
           "the measured output's verification carries reopenQuality: " + status);
    expect(yyjson_obj_size(quality) == 5, "reopenQuality has exactly its five fields");
    const auto text = [&](const char* key) {
        const char* value = yyjson_get_str(yyjson_obj_get(quality, key));
        return value == nullptr ? std::string{} : std::string(value);
    };
    const auto number = [&](const char* key) {
        auto* value = yyjson_obj_get(quality, key);
        return yyjson_is_uint(value) ? yyjson_get_uint(value) : ~std::uint64_t{0};
    };
    expect(text("toleranceProfile") == "bloom.video-rgba16-srgb.v3", "the profile name");
    expect(text("toleranceDigest") == "sha256:" + std::string(kDigest),
           "the profile digest, written as the other digests are");
    expect(number("maximumError") == 30734, "the raw maximum");
    expect(number("percentileMaximumError") == 6309, "the 99.99th-percentile maximum");
    expect(number("meanError") == 599, "the mean");
    expect(yyjson_is_str(yyjson_obj_get(withQuality, "overall")),
           "the block sits beside the rest of the verification");

    auto* without = verificationOf(document, plain);
    expect(yyjson_is_obj(without) && yyjson_obj_get(without, "reopenQuality") == nullptr,
           "an output without measurements omits reopenQuality");
    yyjson_doc_free(document);
}

} // namespace

int main() {
    try {
        reopenQualityInStatus();
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
    if (failures == 0)
        std::cout << "export reopen quality passed\n";
    return failures == 0 ? 0 : 1;
}
