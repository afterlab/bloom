#if defined(BLOOM_BUILD_PYTHON)
#include "runtime.hpp"
#endif

#include <bloom/commands/export_queue_operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/core/standard_streams.hpp>
#include <bloom/core/version.hpp>
#include <bloom/crash/crash_reporter.hpp>
#include <bloom/document/export_queue.hpp>
#include <bloom/document/node_definition_registry.hpp>
#include <bloom/host/export_verification.hpp>
#include <bloom/host/gpu_export_provider.hpp>
#include <bloom/host/gpu_export_tool_package.hpp>
#include <bloom/host/linked_subtitle_files.hpp>
#include <bloom/media/cache/source_mirror_settings.hpp>
#include <bloom/output/output_analysis.hpp>
#include <bloom/platform/cache_directory.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/headless_font_transport.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/scripting/command_diagnostics.hpp>
#include <bloom/scripting/export.hpp>
#include <bloom/scripting/json_script.hpp>
#include <bloom/scripting/operation_reference.hpp>
#include <bloom/scripting/render.hpp>
#include <bloom/scripting/session.hpp>

#include <bloom/host/export_queue.hpp>
#include <thread>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

namespace document = bloom::document;
namespace output = bloom::output;
namespace runtime = bloom::runtime;
namespace scripting = bloom::scripting;

// Declared after the provider and before render work. Normal return and exception unwind both
// retain the borrowed media evaluator and scheduler until actual native/owned-host retirement.
class ExportProviderTeardown final {
  public:
    explicit ExportProviderTeardown(std::shared_ptr<bloom::host::GpuExportProvider> provider)
        : provider_(std::move(provider)) {}
    ~ExportProviderTeardown() { finish(); }
    void finish() noexcept {
        if (finished_)
            return;
        while (!provider_->shutdownAndWait(std::chrono::seconds(10)))
            std::cerr << "CLI export retirement pending; provider/media/scheduler retained\n";
        finished_ = true;
    }

  private:
    std::shared_ptr<bloom::host::GpuExportProvider> provider_;
    bool finished_ = false;
};

// This executable's packaged GPU shader tools. The definitions that describe them belong to this
// target, so the resolver is made here and handed to every headless export queue.
[[nodiscard]] std::shared_ptr<bloom::runtime::GpuOcioContextResolver> packagedOcioResolver() {
    return bloom::host::makePackagedGpuOcioResolver(bloom::host::currentExecutablePath());
}

void printUsage() {
    std::cerr << "usage: bloom-cli --version | new <file> | open <file> | "
                 "run <script.json|script.py> | "
                 "python | render [--project <file>] [--frame N | --range A-B] --preset <id> --out "
                 "<dir> [--replace] [--require-device] [--subtitles srt,vtt,ttml] "
                 "[--subtitle-nodes <id>,...] [--background transparent|composition] | export "
                 "--project <file> (--all | --job "
                 "<id>...) [--replace] [--save] [--subtitles srt,vtt,ttml] [--embed-subtitles] "
                 "[--subtitle-nodes <id>,...] | "
                 "help [<name>] [--json]\n"
                 "Run `bloom-cli help` for every command, operation and tool, or `bloom-cli help "
                 "<command>` for one in full.\n";
}

// The same reference the MCP help tool and bloom.help() print. A command name is looked up as a
// bloom-cli command first, so `bloom-cli help render` describes this program's render.
[[nodiscard]] int commandHelp(int argc, char** argv) {
    bool json = false;
    std::string name;
    for (int index = 2; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--json") {
            json = true;
        } else if (name.empty()) {
            name = argument;
        } else {
            std::cerr << "error[bloom-cli.option]: help takes one name\n";
            return 2;
        }
    }
    const auto& reference = scripting::OperationReference::builtIn();
    if (!name.empty() && !name.starts_with("bloom-cli ")) {
        if (const auto* command = reference.find("bloom-cli " + name);
            command != nullptr && command->kind == scripting::ReferenceKind::CliCommand)
            name = command->name;
    }
    const auto answer = scripting::referenceHelp(name);
    std::cout << (json ? answer.json + "\n" : answer.text);
    return answer.found ? 0 : 1;
}

[[nodiscard]] std::optional<std::string> readText(const std::filesystem::path& path) {
    std::ifstream stream(path);
    if (!stream) {
        return std::nullopt;
    }
    return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

[[nodiscard]] std::optional<std::uint64_t> unsignedInteger(std::string_view value) {
    try {
        std::size_t consumed = 0;
        const auto parsed = std::stoull(std::string(value), &consumed);
        return consumed == value.size() ? std::optional(parsed) : std::nullopt;
    } catch (...) {
        return std::nullopt;
    }
}

// The subtitle options of render and export (#752): --subtitles srt,vtt,ttml (any order, each
// once), --embed-subtitles and --subtitle-nodes 12,15. The same rules as the MCP `subtitles`
// argument.
struct SubtitleOptions final {
    std::optional<std::vector<document::ExportSubtitleFormat>> sidecars;
    bool embed = false;
    std::optional<std::vector<document::NodeId>> nodes;
};

[[nodiscard]] std::vector<std::string_view> splitList(std::string_view text) {
    std::vector<std::string_view> parts;
    while (true) {
        const auto comma = text.find(',');
        parts.push_back(text.substr(0, comma));
        if (comma == std::string_view::npos)
            return parts;
        text.remove_prefix(comma + 1);
    }
}

// Reads the --subtitles list; a message when it is not srt, vtt and ttml, each at most once.
[[nodiscard]] std::optional<std::string> parseSubtitleFormats(std::string_view text,
                                                              SubtitleOptions& options) {
    std::vector<document::ExportSubtitleFormat> formats;
    for (const auto part : splitList(text)) {
        const auto format = document::parseExportSubtitleFormat(part);
        if (!format)
            return "--subtitles formats are srt, vtt and ttml, separated by commas";
        if (std::ranges::find(formats, *format) != formats.end())
            return "--subtitles names each format at most once";
        formats.push_back(*format);
    }
    std::ranges::sort(formats);
    options.sidecars = std::move(formats);
    return std::nullopt;
}

[[nodiscard]] std::optional<std::string> parseSubtitleNodes(std::string_view text,
                                                            SubtitleOptions& options) {
    std::vector<document::NodeId> nodes;
    for (const auto part : splitList(text)) {
        const auto id = unsignedInteger(part);
        if (!id || *id == 0)
            return "--subtitle-nodes are positive node ids separated by commas";
        const auto node = document::NodeId::fromRaw(*id);
        if (std::ranges::find(nodes, node) != nodes.end())
            return "--subtitle-nodes names each node at most once";
        nodes.push_back(node);
    }
    if (nodes.size() > document::kExportSubtitleMaximumNodes)
        return "--subtitle-nodes lists at most " +
               std::to_string(document::kExportSubtitleMaximumNodes) + " nodes";
    std::ranges::sort(nodes);
    options.nodes = std::move(nodes);
    return std::nullopt;
}

// The setting the options ask for; absent when none was given. A message when --subtitle-nodes is
// given without --subtitles or --embed-subtitles.
[[nodiscard]] std::optional<std::string>
subtitleSetting(const SubtitleOptions& options, std::optional<document::ExportSubtitles>& setting) {
    if (!options.sidecars && !options.embed) {
        if (options.nodes)
            return "--subtitle-nodes needs --subtitles or --embed-subtitles";
        return std::nullopt;
    }
    document::ExportSubtitles subtitles;
    if (options.sidecars)
        subtitles.sidecars = *options.sidecars;
    subtitles.embed = options.embed;
    subtitles.nodes = options.nodes;
    setting = std::move(subtitles);
    return std::nullopt;
}

[[nodiscard]] std::optional<output::OutputPresetV1> preset(std::string_view id) {
    if (id == "PngRgba8SrgbV1") {
        return output::OutputPresetV1::PngRgba8SrgbV1;
    }
    if (id == "FlatExrRgba32fLinRec709SceneV1") {
        return output::OutputPresetV1::FlatExrRgba32fLinRec709SceneV1;
    }
    return std::nullopt;
}

// #766: "A-B", "A-" (to the composition's last frame), "-B" (from its first) or "-" (all of it).
struct OpenFrameRange final {
    std::optional<std::uint64_t> first;
    std::optional<std::uint64_t> last;
};

[[nodiscard]] std::optional<OpenFrameRange> frameRange(std::string_view value) {
    const auto separator = value.find('-');
    if (separator == std::string_view::npos) {
        return std::nullopt;
    }
    OpenFrameRange range;
    if (separator > 0) {
        range.first = unsignedInteger(value.substr(0, separator));
        if (!range.first.has_value())
            return std::nullopt;
    }
    if (separator + 1 < value.size()) {
        range.last = unsignedInteger(value.substr(separator + 1));
        if (!range.last.has_value())
            return std::nullopt;
    }
    return range;
}

[[nodiscard]] int commandNew(const std::filesystem::path& path) {
    auto created = scripting::Session::createNew();
    if (!created) {
        std::cerr << "error[" << created.diagnostic().code << "]: " << created.diagnostic().message
                  << '\n';
        return 2;
    }
    auto session = std::move(created).takeSession();
    const auto saved = session->saveAs(path);
    if (!saved.succeeded()) {
        std::cerr << "error[" << saved.code << "]: " << saved.message << '\n';
        return 3;
    }
    std::cout << "created " << path << '\n';
    return 0;
}

[[nodiscard]] int commandOpen(const std::filesystem::path& path) {
    auto opened = scripting::Session::open(path);
    if (!opened) {
        std::cerr << "error[" << opened.diagnostic().code << "]: " << opened.diagnostic().message
                  << '\n';
        return 2;
    }
    const auto session = std::move(opened).takeSession();
    const auto state = session->state();
    std::cout << "opened " << path << " revision="
              << (state.currentRevision.has_value() ? state.currentRevision->value() : 0) << '\n';
    return 0;
}

[[nodiscard]] int commandRun(const std::filesystem::path& path) {
    const auto text = readText(path);
    if (!text.has_value()) {
        std::cerr << "error[bloom-cli.script-read]: could not read " << path << '\n';
        return 2;
    }
    auto parsed = scripting::parseScriptJson(*text);
    if (!parsed) {
        const auto* diagnostic = parsed.diagnostic();
        std::cerr << "error[" << diagnostic->code << "] at " << diagnostic->byteOffset << ": "
                  << diagnostic->message << '\n';
        return 2;
    }
    auto created = scripting::Session::createNew();
    if (!created) {
        std::cerr << "error[" << created.diagnostic().code << "]: " << created.diagnostic().message
                  << '\n';
        return 3;
    }
    auto session = std::move(created).takeSession();
    std::unique_ptr<scripting::HeadlessExportQueue> exports;
    for (const auto& transaction : parsed.transactions()) {
        if (scripting::isExportCall(transaction.operation)) {
            if (!exports)
                exports = std::make_unique<scripting::HeadlessExportQueue>(
                    *session, packagedOcioResolver(), std::filesystem::path{},
                    std::shared_ptr<std::timed_mutex>{},
                    bloom::media::cache::openUserSourceMirror());
            const auto answer =
                exports->calls().call(transaction.operation, transaction.arguments, true);
            if (!answer.ok) {
                std::cerr << "error[" << answer.code << "] " << transaction.operation << ": "
                          << answer.message << '\n';
                return 6;
            }
            std::cout << answer.json << '\n';
            continue;
        }
        const auto result =
            session->executeJsonOperation(transaction.operation, transaction.arguments);
        if (result.diagnostic.has_value()) {
            std::cerr << "error[" << result.diagnostic->code << "] "
                      << result.diagnostic->operationId << " " << result.diagnostic->argument
                      << ": " << result.diagnostic->message;
            // #752: a limit refusal names the bound, its value and the actual amount.
            if (const auto& limit = result.diagnostic->limit)
                std::cerr << " (limit " << limit->name << " " << limit->value << ", actual "
                          << limit->actual << ")";
            std::cerr << '\n';
            return 4;
        }
        if (!result.succeeded()) {
            // Say why: the operation's own refusal (or the validation that failed), not only that
            // it was refused.
            bool reason = false;
            if (result.command.has_value())
                for (const auto& diagnostic : scripting::commandDiagnostics(*result.command))
                    if (diagnostic.severity == "error") {
                        std::cerr << "error[bloom-cli.command-rejected] " << transaction.operation
                                  << ": " << diagnostic.message << '\n';
                        reason = true;
                    }
            if (!reason)
                std::cerr << "error[bloom-cli.command-rejected] " << transaction.operation << '\n';
            return 5;
        }
        for (const auto& warning : result.command->warnings)
            std::cerr << "warning[bloom-cli.operation-warning] " << warning.operationType << ": "
                      << warning.issue.message << '\n';
        if (const auto job = result.command->outputId<document::ExportJobId>("exportJob")) {
            if (!exports)
                exports = std::make_unique<scripting::HeadlessExportQueue>(
                    *session, packagedOcioResolver(), std::filesystem::path{},
                    std::shared_ptr<std::timed_mutex>{},
                    bloom::media::cache::openUserSourceMirror());
            exports->calls().noteAddedJob(*job);
        }
    }
    std::cout << "executed " << parsed.transactions().size()
              << " transaction(s), revision=" << session->snapshot().revision().value() << '\n';
    return 0;
}

// ADR 0033: renders the saved queue headlessly. Exits non-zero unless every selected job
// succeeds; prints the verification and import results as JSON. --replace confirms every existing
// destination that asks before replacing; --save writes asset imports back to the project.
[[nodiscard]] int commandExport(int argc, char** argv) {
    std::optional<std::filesystem::path> project;
    bool all = false;
    bool replace = false;
    bool save = false;
    SubtitleOptions subtitleOptions;
    std::vector<document::ExportJobId> jobs;
    for (int index = 2; index < argc; ++index) {
        const std::string_view option = argv[index];
        if ((option == "--project" || option == "--job" || option == "--subtitles" ||
             option == "--subtitle-nodes") &&
            index + 1 >= argc) {
            std::cerr << "error[bloom-cli.option]: " << option << " needs a value\n";
            return 2;
        }
        if (option == "--project") {
            project = argv[++index];
        } else if (option == "--job") {
            const auto id = unsignedInteger(argv[++index]);
            if (!id || *id == 0) {
                std::cerr << "error[bloom-cli.job]: invalid job id\n";
                return 2;
            }
            jobs.push_back(document::ExportJobId::fromRaw(*id));
        } else if (option == "--all") {
            all = true;
        } else if (option == "--replace") {
            replace = true;
        } else if (option == "--save") {
            save = true;
        } else if (option == "--subtitles" || option == "--subtitle-nodes") {
            const auto refusal = option == "--subtitles"
                                     ? parseSubtitleFormats(argv[++index], subtitleOptions)
                                     : parseSubtitleNodes(argv[++index], subtitleOptions);
            if (refusal) {
                std::cerr << "error[bloom-cli.subtitles]: " << *refusal << '\n';
                return 2;
            }
        } else if (option == "--embed-subtitles") {
            subtitleOptions.embed = true;
        } else {
            std::cerr << "error[bloom-cli.option]: unknown option " << option << '\n';
            return 2;
        }
    }
    if (!project || all == !jobs.empty()) {
        printUsage();
        return 2;
    }
    std::optional<document::ExportSubtitles> subtitles;
    if (const auto refusal = subtitleSetting(subtitleOptions, subtitles)) {
        std::cerr << "error[bloom-cli.subtitles]: " << *refusal << '\n';
        return 2;
    }
    auto opened = scripting::Session::open(*project);
    if (!opened) {
        std::cerr << "error[" << opened.diagnostic().code << "]: " << opened.diagnostic().message
                  << '\n';
        return 3;
    }
    auto session = std::move(opened).takeSession();
    // Every named job must exist: a missing one would otherwise be skipped while the rest run and
    // the command would still report success.
    const auto opening = session->snapshot();
    for (const auto id : jobs) {
        const auto saved = opening.project().exportJobs();
        if (std::ranges::none_of(saved, [id](const auto& job) { return job.id == id; })) {
            std::cerr << "error[bloom-cli.export."
                      << static_cast<int>(bloom::host::ExportUnavailableCode::UnknownJob)
                      << "]: The job does not exist: " << id.value() << '\n';
            return 5;
        }
    }
    // #752: --subtitles and --embed-subtitles replace the subtitles setting of every output of the
    // selected jobs for this run, in one edit. The saved settings are put back exactly (the same
    // settings records, in one edit) before --save writes the project.
    std::vector<std::pair<document::ExportOutputId, document::ExportOutputSettings>> original;
    if (subtitles) {
        const auto current = session->snapshot();
        bloom::commands::Transaction change("Export subtitles for this run", current.revision());
        const auto queued = current.project().exportJobs();
        for (const auto& job : queued) {
            if (!all && std::ranges::find(jobs, job.id) == jobs.end())
                continue;
            for (const auto& output : job.outputs) {
                auto changed = output.settings;
                changed.subtitles = subtitles;
                change.emplace<bloom::commands::SetExportOutputSettings>(output.id,
                                                                         std::move(changed));
                original.emplace_back(output.id, output.settings);
            }
        }
        if (!original.empty() && !session->execute(std::move(change)).succeeded()) {
            std::cerr << "error[bloom-cli.subtitles]: the subtitle settings were refused\n";
            return 5;
        }
    }
    int status = 0;
    {
        scripting::HeadlessExportQueue exports(*session, packagedOcioResolver(),
                                               std::filesystem::path{},
                                               std::shared_ptr<std::timed_mutex>{},
                                               bloom::media::cache::openUserSourceMirror());
        auto& service = exports.service();
        if (all)
            for (const auto& job : session->snapshot().project().exportJobs())
                if (job.enabled)
                    jobs.push_back(job.id);
        // Wait for the destination samples, then confirm replacements when asked to.
        const auto ready = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        while (std::chrono::steady_clock::now() < ready) {
            service.pump();
            bool pending = false;
            for (const auto& view : service.snapshot()->jobs)
                pending =
                    pending || view.commands[bloom::host::ExportCommand::Start].code ==
                                   bloom::host::ExportUnavailableCode::DestinationCheckPending;
            if (!pending)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (replace)
            for (const auto& view : service.snapshot()->jobs)
                for (const auto& output : view.outputs)
                    if (output.commands[bloom::host::ExportCommand::ConfirmReplace])
                        (void)service.confirmReplace(view.id, output.id);
        const auto started = service.start(jobs);
        if (!started.accepted) {
            std::cerr << "error[bloom-cli.export." << static_cast<int>(started.availability.code)
                      << "]: " << started.availability.reason << '\n';
            return 5;
        }
        bool active = true;
        while (active) {
            service.pump();
            active = false;
            for (const auto& view : service.snapshot()->jobs)
                if (std::ranges::find(jobs, view.id) != jobs.end() &&
                    bloom::host::isActive(view.state))
                    active = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        const auto snapshot = service.snapshot();
        for (const auto& view : snapshot->jobs)
            if (std::ranges::find(jobs, view.id) != jobs.end() &&
                view.state != bloom::host::ExportJobState::Done &&
                view.state != bloom::host::ExportJobState::DoneWithWarnings)
                status = 6;
        std::cout << scripting::exportEventsJson(service.events(0)) << '\n';
    }
    if (!original.empty()) {
        bloom::commands::Transaction restore("Restore the saved export subtitles",
                                             session->snapshot().revision());
        for (const auto& [output, settings] : original)
            restore.emplace<bloom::commands::SetExportOutputSettings>(output, settings);
        if (!session->execute(std::move(restore)).succeeded())
            std::cerr << "warning[bloom-cli.subtitles]: the saved subtitle settings could not be "
                         "restored\n";
    }
    if (save) {
        const auto saved = session->save();
        if (!saved.succeeded()) {
            std::cerr << "error[" << saved.code << "]: " << saved.message << '\n';
            return 7;
        }
    } else {
        std::cerr << "note[bloom-cli.export]: asset imports were not saved; pass --save\n";
    }
    return status;
}

[[nodiscard]] int commandRender(int argc, char** argv) {
    std::optional<std::filesystem::path> project;
    std::optional<std::uint64_t> frame;
    std::optional<OpenFrameRange> range;
    std::optional<output::OutputPresetV1> selectedPreset;
    std::filesystem::path outputDirectory;
    bool replace = false;
    bool requireDevice = false;
    SubtitleOptions subtitleOptions;
    auto background = document::ExportBackground::Transparent;
    for (int index = 2; index < argc; ++index) {
        const std::string_view option = argv[index];
        if (option == "--replace") {
            replace = true;
            continue;
        }
        if (option == "--embed-subtitles") {
            subtitleOptions.embed = true;
            continue;
        }
        if (option == "--require-device") {
            requireDevice = true;
            continue;
        }
        if ((option == "--project" || option == "--out" || option == "--preset" ||
             option == "--frame" || option == "--range" || option == "--subtitles" ||
             option == "--subtitle-nodes" || option == "--background") &&
            index + 1 >= argc) {
            std::cerr << "error[bloom-cli.option]: " << option << " needs a value\n";
            return 2;
        }
        if (option == "--project") {
            project = argv[++index];
        } else if (option == "--frame") {
            frame = unsignedInteger(argv[++index]);
            if (!frame.has_value()) {
                std::cerr << "error[bloom-cli.frame]: invalid frame\n";
                return 2;
            }
        } else if (option == "--range") {
            range = frameRange(argv[++index]);
            if (!range.has_value()) {
                std::cerr << "error[bloom-cli.range]: invalid range\n";
                return 2;
            }
        } else if (option == "--preset") {
            selectedPreset = preset(argv[++index]);
            if (!selectedPreset.has_value()) {
                std::cerr << "error[bloom-cli.preset]: unknown preset\n";
                return 2;
            }
        } else if (option == "--out") {
            outputDirectory = argv[++index];
        } else if (option == "--background") {
            // #760: transparent (the default) or composition.
            const auto parsed = document::parseExportBackground(argv[++index]);
            if (!parsed) {
                std::cerr << "error[bloom-cli.background]: --background is transparent or "
                             "composition\n";
                return 2;
            }
            background = *parsed;
        } else if (option == "--subtitles" || option == "--subtitle-nodes") {
            const auto refusal = option == "--subtitles"
                                     ? parseSubtitleFormats(argv[++index], subtitleOptions)
                                     : parseSubtitleNodes(argv[++index], subtitleOptions);
            if (refusal) {
                std::cerr << "error[bloom-cli.subtitles]: " << *refusal << '\n';
                return 2;
            }
        } else {
            std::cerr << "error[bloom-cli.option]: unknown option " << option << '\n';
            return 2;
        }
    }
    // #766: without --frame or --range the render covers the composition's whole range.
    if (!frame.has_value() && !range.has_value())
        range = OpenFrameRange{};
    if ((frame.has_value() && range.has_value()) || !selectedPreset.has_value() ||
        outputDirectory.empty()) {
        printUsage();
        return 2;
    }
    std::optional<document::ExportSubtitles> subtitles;
    if (const auto refusal = subtitleSetting(subtitleOptions, subtitles)) {
        std::cerr << "error[bloom-cli.subtitles]: " << *refusal << '\n';
        return 2;
    }
    // An image carries no subtitle track: sidecar files are the delivery for a render.
    if (subtitles && subtitles->embed) {
        std::cerr << "error[bloom-cli.subtitles]: --embed-subtitles needs a movie: "
                  << bloom::host::kSubtitleEmbedUnsupportedReason << '\n';
        return 2;
    }
    std::unique_ptr<scripting::Session> session;
    auto opened =
        project.has_value() ? scripting::Session::open(*project) : scripting::Session::createNew();
    if (!opened) {
        std::cerr << "error[" << opened.diagnostic().code << "]: " << opened.diagnostic().message
                  << '\n';
        return 3;
    }
    session = std::move(opened).takeSession();
    const auto snapshot = session->snapshot();
    if (snapshot.project().compositions().empty()) {
        std::cerr << "error[bloom-cli.composition]: no composition exists\n";
        return 4;
    }
    const auto composition = snapshot.project().compositions().front().id();
    const auto* const extension =
        *selectedPreset == output::OutputPresetV1::PngRgba8SrgbV1 ? ".png" : ".exr";
    std::error_code outputError;
    std::filesystem::create_directories(outputDirectory, outputError);
    if (outputError) {
        std::cerr << "error[bloom-cli.out]: could not create output directory\n";
        return 4;
    }
    const auto destination = outputDirectory / ("frame" + std::string(extension));
    // #752: a linked subtitle file that changed, is missing or cannot be read. A render never
    // reloads: it uses the cues saved in the project, and says so.
    for (const auto& warning : bloom::host::linkedFileWarnings(
             snapshot.project(), project.has_value() ? project->parent_path()
                                                     : std::filesystem::path{}))
        std::cerr << "warning[" << warning.code << "]: " << warning.message << '\n';
    runtime::TaskSchedulerConfig config = runtime::TaskSchedulerConfig::defaults();
    config.rowBandWorkerCount = runtime::kSerialRowBandWorkers;
    runtime::TaskScheduler scheduler(config);
    runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
    // Headless GPU export provider: the CLI composes the packaged GPU shader tools from its OWN
    // executable-relative packaging (never PATH) and hands the inert resolver to the provider,
    // whose CPU-worker bootstrap qualifies them once. The media context follows the opened
    // project's asset base directory so real media/effect scenes run through the production GPU
    // paths; a device/tool refusal keeps the unchanged CPU reference path.
    runtime::CpuCompositionEvaluator gpuMediaEvaluator;
    // #718: headless renders read slow sources through the user's local mirror, with the "Keep
    // local copies" choice the application published (null: disabled or no cache folder).
    gpuMediaEvaluator.setSourceStore(bloom::media::cache::openUserSourceMirror());
    if (project.has_value()) {
        gpuMediaEvaluator.setAssetBaseDirectory(project->parent_path());
    }
    runtime::GpuProcessFrameEvaluatorOptions gpuOptions;
    gpuOptions.ocioResolver =
        bloom::host::makePackagedGpuOcioResolver(bloom::host::currentExecutablePath());
    gpuOptions.pipelineCacheDirectory = bloom::platform::gpuPipelineCacheDirectory();
    gpuOptions.mediaContextProvider = [&gpuMediaEvaluator] {
        return runtime::GpuSceneMediaContext::fromEvaluator(gpuMediaEvaluator);
    };
    auto gpuProvider = bloom::host::GpuExportProvider::create(std::move(gpuOptions));
    ExportProviderTeardown providerTeardown(gpuProvider);
    gpuProvider->prepare(scheduler);
    const auto result =
        scripting::Render::run(*session, scheduler, compiler,
                               {.composition = composition,
                                .frame = frame,
                                .range = range && range->first && range->last
                                             ? std::optional(std::pair(*range->first, *range->last))
                                             : std::nullopt,
                                .rangeFirst = range && !(range->first && range->last)
                                                  ? range->first
                                                  : std::nullopt,
                                .rangeLast = range && !(range->first && range->last)
                                                 ? range->last
                                                 : std::nullopt,
                                .preset = *selectedPreset,
                                .destination = destination,
                                .replaceExisting = replace,
                                .requireDevice = requireDevice,
                                .workerLimits = {},
                                .loudness = std::nullopt,
                                .encoder = document::ExportEncoderPreference::Automatic,
                                .subtitles = subtitles,
                                .background = background},
                               {}, gpuProvider);
    providerTeardown.finish();
    std::cout << result.preservationReport << '\n';
    // Which device evaluated the frames (#452): a CPU reference frame is reported, never hidden.
    std::cout << "device gpu-frames=" << result.gpuEvaluatedFrames
              << " cpu-reference-frames=" << result.cpuReferenceFrames << '\n';
    // #751: a capacity admission could not sample ran on a stated assumption; say which.
    for (const auto& assumption : result.capacityAssumptions)
        std::cerr << "warning[bloom-cli.render.capacity-assumed]: " << assumption << '\n';
    if (!result.succeeded) {
        std::cerr << (result.diagnosticCode == scripting::RenderDiagnosticCode::DeviceRequired
                          ? "error[bloom-cli.render.device-required]: "
                      : result.diagnosticCode ==
                              scripting::RenderDiagnosticCode::MediaSourceUnavailable
                          ? "error[bloom-cli.render.media-source-unavailable]: "
                          : "error[bloom-cli.render]: ")
                  << result.diagnostic << '\n';
        return 5;
    }
    if (result.cpuReferenceFrames > 0)
        std::cerr << "warning[bloom-cli.render.cpu-reference]: " << result.cpuReferenceFrames
                  << " frame(s) were evaluated on the CPU reference path; pass --require-device "
                     "to fail instead\n";
    // #752: every subtitle sidecar the render delivered, with its cue count and SHA-256.
    for (const auto& track : result.subtitles) {
        std::cout << "subtitles node=" << track.node.value() << " language=" << track.language
                  << " format="
                  << (track.sidecar ? document::exportSubtitleFormatToken(*track.sidecar)
                                    : std::string_view("embedded"))
                  << " cues=" << track.cues << " path=";
        const auto path = track.path.u8string();
        std::cout << std::string_view(reinterpret_cast<const char*>(path.data()), path.size());
        if (track.digest) {
            const auto hex = track.digest->toLowercaseHex();
            std::cout << " sha256=" << std::string_view(hex.data(), hex.size());
        }
        std::cout << '\n';
    }
    // #761: a movie's or WAV's streams as the verifying reopen read them from the file.
    for (const auto& stream : result.streams)
        std::cout << "stream " << bloom::host::exportWrittenStreamText(stream) << '\n';
    std::cout << "rendered " << result.publishedFrames << " frame(s) to " << outputDirectory
              << '\n';
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    // The same bytes on every platform: no "\r\n" from the C runtime's text mode on Windows.
    bloom::core::useBinaryStandardStreams();
    if (argc == 2 && std::string_view(argv[1]) == "--version") {
        std::cout << "bloom-cli " << bloom::core::version() << '\n';
        return 0;
    }
    // Crash reports (#736): a crash from here on leaves a minidump and a text report in the
    // per-user crash folder; earlier crashes are collected and pruned in the background.
    bloom::crash::captureStandardError();
    static_cast<void>(
        bloom::crash::startCrashReporter({.program = "bloom-cli",
                                          .executable = bloom::host::currentExecutablePath(),
                                          .directory = {},
                                          .collect = true}));
    struct FinishCrashReporter final {
        FinishCrashReporter() = default;
        FinishCrashReporter(const FinishCrashReporter&) = delete;
        FinishCrashReporter& operator=(const FinishCrashReporter&) = delete;
        ~FinishCrashReporter() { bloom::crash::finishCrashReporter(); }
    } const finishCrashReporter;
    // A render that needs a Google font waits for the download (docs/architecture/fonts.md). The
    // downloads end when main() does, before the exit handlers tear down the HTTP client (#750).
    const runtime::fonts::HeadlessFontDownloads fontDownloads;
#if defined(BLOOM_BUILD_PYTHON)
    if ((argc == 2 && std::string_view(argv[1]) == "python") ||
        (argc == 3 && std::string_view(argv[1]) == "run" &&
         std::filesystem::path(argv[2]).extension() == ".py")) {
        try {
            scripting::python::EmbeddedPython python(std::filesystem::absolute(argv[0]),
                                                     packagedOcioResolver());
            return argc == 2 ? python.interactive() : python.runFile(argv[2]);
        } catch (const std::exception& error) {
            std::cerr << "error[bloom-cli.python]: " << error.what() << '\n';
            return 4;
        }
    }
#else
    if ((argc == 2 && std::string_view(argv[1]) == "python") ||
        (argc == 3 && std::string_view(argv[1]) == "run" &&
         std::filesystem::path(argv[2]).extension() == ".py")) {
        std::cerr << "error[bloom-cli.python-disabled]: rebuild with BLOOM_BUILD_PYTHON=ON\n";
        return 2;
    }
#endif
    if (argc >= 2) {
        const std::string_view first = argv[1];
        if (first == "help")
            return commandHelp(argc, argv);
        if (first == "--help" || first == "-h") {
            printUsage();
            return 0;
        }
        if (argc == 3 &&
            (std::string_view(argv[2]) == "--help" || std::string_view(argv[2]) == "-h")) {
            char program[] = "bloom-cli";
            char help[] = "help";
            std::array<char*, 3> forwarded{program, help, argv[1]};
            return commandHelp(3, forwarded.data());
        }
    }
    if (argc < 3) {
        printUsage();
        return 2;
    }
    const std::string_view command = argv[1];
    if (command == "new" && argc == 3) {
        return commandNew(argv[2]);
    }
    if (command == "open" && argc == 3) {
        return commandOpen(argv[2]);
    }
    // A headless export queue that cannot make its private scratch directory refuses to start.
    try {
        if (command == "run" && argc == 3) {
            return commandRun(argv[2]);
        }
        if (command == "render") {
            return commandRender(argc, argv);
        }
        if (command == "export") {
            return commandExport(argc, argv);
        }
    } catch (const std::exception& error) {
        std::cerr << "error[bloom-cli]: " << error.what() << '\n';
        return 1;
    }
    printUsage();
    return 2;
}
