#include "core_internal.hpp"

#include <bloom/document/node_definition_registry.hpp>
#include <bloom/host/export_presets.hpp>
#include <bloom/host/sequence_export_runner.hpp>
#include <bloom/host/session_save.hpp>
#include <bloom/output/output_export_resource_ledger.hpp>
#include <bloom/platform/private_temporary_directory.hpp>
#include <bloom/platform/user_data_directory.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <system_error>
#include <thread>

namespace bloom::mcp {
namespace {

std::filesystem::path normalized(const std::filesystem::path& path) {
    std::error_code error;
    auto result = std::filesystem::weakly_canonical(path, error);
    return error ? path.lexically_normal() : result;
}

} // namespace

// ---- #783: a projectFile session's export queue

struct McpFileExports::Impl final {
    std::shared_ptr<scripting::Session> session;
    runtime::TaskScheduler& scheduler;
    runtime::SnapshotCompiler compiler{document::builtInNodeDefinitions()};
    std::shared_ptr<host::SequenceExportRetirementOwnerV1> retirement =
        host::SequenceExportRetirementOwnerV1::create();
    std::filesystem::path scratch;
    std::mutex mutex;
    std::atomic<std::thread::id> holder{};
    // A provider of this queue's own when the core has none for projectFile sessions: disabled,
    // so every frame reports the CPU reference path. Retired after the queue.
    std::shared_ptr<host::GpuExportProvider> ownedGpu;
    std::unique_ptr<host::ExportQueueService> service;
    McpExportAccess access;
    McpRetiredTransients retired;
    bool shutDown = false;
    // Advances running jobs between calls, as the application's owner thread does for its queue.
    std::optional<core::StoppableWorkerThread> pump;

    Impl(std::shared_ptr<scripting::Session> owned, runtime::TaskScheduler& tasks)
        : session(std::move(owned)), scheduler(tasks) {}

    void lock() {
        mutex.lock();
        holder.store(std::this_thread::get_id());
    }
    void unlock() {
        holder.store(std::thread::id{});
        mutex.unlock();
    }
    [[nodiscard]] bool heldHere() const noexcept {
        return holder.load() == std::this_thread::get_id();
    }
    // The caller holds the lock.
    void stopAll(const host::ExportStopCause cause) {
        if (shutDown || !service)
            return;
        shutDown = true;
        service->beginShutdown(cause);
        while (!service->quiescent()) {
            service->pump();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
};

McpFileExports::McpFileExports(std::shared_ptr<scripting::Session> session,
                               runtime::TaskScheduler& scheduler,
                               std::shared_ptr<host::GpuExportProvider> gpu)
    : impl_(std::make_shared<Impl>(std::move(session), scheduler)) {
    auto& s = *impl_;
    // Scratch files are opened by path and truncated, so they live in a fresh owner-only folder
    // of this queue's own (as scripting::HeadlessExportQueue does for bloom-cli).
    auto scratch = platform::createPrivateTemporaryDirectory("bloom-export-");
    if (!scratch)
        throw ToolFailure(McpToolErrorCode::ExportUnavailable,
                          "Bloom could not create a private folder for this file's export scratch "
                          "files.");
    s.scratch = std::move(*scratch);
    if (!gpu) {
        runtime::GpuProcessFrameEvaluatorOptions options;
        options.enabled = false;
        s.ownedGpu = host::GpuExportProvider::create(std::move(options));
        gpu = s.ownedGpu;
    }
    // Idempotent: the provider the application or the stdio server passes is already prepared.
    // A frame waits for the provider's bootstrap to end, so one that was never prepared would
    // never render.
    gpu->prepare(scheduler);
    auto presets = host::ExportPresetCatalogue::create(
        {.userDataDirectory = platform::userDataDirectory(), .initialEncoders = {}});
    presets->refreshEncoders(scheduler);
    presets->loadUserPresets(scheduler);
    s.service = std::make_unique<host::ExportQueueService>(host::ExportQueueServiceDependencies{
        .session = *s.session->hostSession(),
        .scheduler = scheduler,
        .compiler = s.compiler,
        .publications = *s.session->publicationCoordinator(),
        .artifacts = *s.session->artifactCoordinator(),
        .ledger = std::make_shared<output::ExportResourceLedgerV1>(),
        .gpu = std::move(gpu),
        .retirement = s.retirement,
        .presets = std::move(presets),
        .limits = {},
        .displayProcessor = nullptr,
        .scratchDirectory = s.scratch,
        .backend = nullptr});
    // The queue's owner is whichever thread holds the lock; the work runs inline (callerIsOwner),
    // and a waiting call pumps the queue itself.
    s.access.callerIsOwner = true;
    s.access.run = [impl = impl_.get()](const McpExportAccess::Work& work,
                                        const std::stop_token& stop) {
        if (stop.stop_requested())
            return false;
        if (impl->heldHere()) {
            work(impl->service.get(), 1);
            return true;
        }
        impl->lock();
        try {
            work(impl->service.get(), 1);
        } catch (...) {
            impl->unlock();
            throw;
        }
        impl->unlock();
        return true;
    };
    {
        const std::lock_guard lock(s.mutex);
        s.service->pump();
    }
    s.pump.emplace([impl = impl_.get()](const std::stop_token& stop) {
        std::mutex sleepMutex;
        std::condition_variable_any sleeper;
        while (!stop.stop_requested()) {
            bool active = false;
            {
                impl->lock();
                if (!impl->shutDown) {
                    impl->service->pump();
                    active = impl->service->activeJobCount() != 0;
                }
                impl->unlock();
            }
            std::unique_lock sleepLock(sleepMutex);
            sleeper.wait_for(sleepLock, stop,
                             active ? std::chrono::milliseconds(5) : std::chrono::milliseconds(100),
                             [] { return false; });
        }
    });
}

McpFileExports::~McpFileExports() {
    auto& s = *impl_;
    s.pump.reset();
    const bool held = s.heldHere();
    if (!held)
        s.lock();
    s.stopAll(host::ExportStopCause::ProjectClosing);
    s.service.reset();
    if (!held)
        s.unlock();
    if (s.ownedGpu)
        while (!s.ownedGpu->shutdownAndWait(std::chrono::seconds(10))) {
        }
    s.retirement->beginShutdown();
    while (!s.retirement->retirementComplete())
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    while (!s.retirement->collect())
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    std::error_code ignored;
    std::filesystem::remove_all(s.scratch, ignored);
}

const McpExportAccess& McpFileExports::access() const noexcept { return impl_->access; }
McpRetiredTransients& McpFileExports::retired() noexcept { return impl_->retired; }
scripting::Session& McpFileExports::session() const noexcept { return *impl_->session; }
void McpFileExports::lock() { impl_->lock(); }
void McpFileExports::unlock() { impl_->unlock(); }
bool McpFileExports::heldHere() const noexcept { return impl_->heldHere(); }

void McpFileExports::idle(const std::chrono::milliseconds duration) {
    if (!impl_->heldHere()) {
        std::this_thread::sleep_for(duration);
        return;
    }
    impl_->unlock();
    std::this_thread::sleep_for(duration);
    impl_->lock();
}

std::size_t McpFileExports::activeJobCount() const {
    return impl_->service ? impl_->service->activeJobCount() : 0;
}

void McpFileExports::shutdown(const host::ExportStopCause cause) { impl_->stopAll(cause); }

bool isExportQueueTool(const McpTool tool) noexcept {
    switch (tool) {
    case McpTool::ExportStart:
    case McpTool::ExportStartAll:
    case McpTool::ExportPause:
    case McpTool::ExportResume:
    case McpTool::ExportStop:
    case McpTool::ExportStopAll:
    case McpTool::ExportRetry:
    case McpTool::ExportConfirmReplace:
    case McpTool::ExportStatus:
    case McpTool::ExportEvents:
    case McpTool::ExportLog:
    case McpTool::ExportWait:
    case McpTool::ExportPresets:
    case McpTool::ExportScript:
        return true;
    default:
        return false;
    }
}

McpQueueRoute queueRoute(McpServerCore::State& core, const McpCall& call) {
    if (call.fileExports != nullptr)
        return {call.fileExports->access(), call.fileExports->retired(), call.fileExports};
    return {core.dependencies.exports, core.retiredTransients, nullptr};
}

std::optional<std::filesystem::path> projectFileArgument(yyjson_val* arguments) {
    if (arguments == nullptr || !yyjson_is_obj(arguments))
        return std::nullopt;
    auto* value = member(arguments, "projectFile");
    if (value == nullptr)
        return std::nullopt;
    const std::filesystem::path path(string(value, 4096));
    if (path.empty() || !path.is_absolute())
        throw InvalidInput("projectFile is the absolute path of a Bloom project file");
    return normalized(path);
}

FrozenJson withoutProjectFile(yyjson_val* arguments) {
    // #745: copied without the member, never written out and parsed again.
    const std::unique_ptr<yyjson_mut_doc, decltype(&yyjson_mut_doc_free)> document(
        yyjson_mut_doc_new(nullptr), yyjson_mut_doc_free);
    if (!document)
        throw std::bad_alloc();
    yyjson_mut_doc_set_root(document.get(), yyjson_val_mut_copy(document.get(), arguments));
    yyjson_mut_obj_remove_str(yyjson_mut_doc_get_root(document.get()), "projectFile");
    FrozenJson frozen(yyjson_mut_doc_imut_copy(document.get(), nullptr), yyjson_doc_free);
    if (!frozen)
        throw std::bad_alloc();
    return frozen;
}

namespace {

ToolFailure busy(const std::filesystem::path& path) {
    return ToolFailure(McpToolErrorCode::ProjectUnavailable,
                       "Bloom is busy, so it could not be confirmed that " +
                           path.filename().string() + " is not open there; try again in a moment.");
}

// A projectFile session that holds `path` while the application (or this server) has the same
// file open: #775 makes a call attach to the open project, so this is reached only by project.save
// on a session that already held the file when the artist opened it. Two copies of one file would
// overwrite each other's saves, and the window would show a project that differs from the
// session's, so the save is refused with what to do. When the application cannot be asked in time
// the answer is no, never an assumption that the file is free.
void refuseIfOpenElsewhere(McpServerCore::State& core, const std::filesystem::path& path,
                           const McpRequestControl& control) {
    const auto refuse = [&](const std::string& where) -> ToolFailure {
        return ToolFailure(McpToolErrorCode::PreflightBlocked,
                           path.filename().string() + " is already open " + where +
                               ", and this server's projectFile session of it is a second copy; "
                               "saving it would overwrite that project's file. Close this session "
                               "with project.close, projectFile and discardChanges:true, then work "
                               "on the open project (a call with projectFile attaches to it).");
    };
    if (auto* live = core.liveHost()) {
        auto lease = live->acquire(
            {.stop = control.stop,
             .deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2)});
        if (!lease)
            throw busy(path);
        if (lease.state().projectPath && normalized(*lease.state().projectPath) == path)
            throw refuse("in Bloom");
    } else if (const auto* headless = std::get_if<std::reference_wrapper<scripting::Session>>(
                   &core.dependencies.session)) {
        const auto& own = headless->get().displayPath();
        if (!own.empty() && normalized(own) == path)
            throw refuse("as this server's project");
    }
}

// Forgets a session the caller holds fileMutex for; its export queue (already stopped) goes with
// the last call that still uses it.
void dropFileSession(McpServerCore::State& core, const std::string& key) {
    const std::lock_guard index(core.fileIndexMutex);
    core.fileIndex.erase(key);
    core.fileSessions.erase(key);
}

} // namespace

bool fileSessionHolds(McpServerCore::State& core, const std::filesystem::path& path) {
    const std::lock_guard lock(core.fileIndexMutex);
    return core.fileIndex.contains(normalized(path).string());
}

scripting::Session& fileSessionFor(McpServerCore::State& core, const std::filesystem::path& path,
                                   const McpRequestControl& control, const bool recheck) {
    const auto key = path.string();
    const auto found = core.fileSessions.find(key);
    // Every call asks again: the artist may have opened the file in Bloom after this session did.
    // Closing a session writes nothing, so it skips the question.
    if (recheck || found == core.fileSessions.end())
        refuseIfOpenElsewhere(core, path, control);
    if (found != core.fileSessions.end())
        return *found->second.session;
    if (core.fileSessions.size() >= kMcpFileSessions)
        throw ToolFailure(McpToolErrorCode::PreflightBlocked,
                          "At most " + std::to_string(kMcpFileSessions) +
                              " other project files can be open at once; close one with "
                              "project.close and its projectFile.");
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error))
        throw ToolFailure(McpToolErrorCode::ProjectUnavailable,
                          "There is no project file at " + path.string());
    auto opened = scripting::Session::open(path);
    if (!opened)
        throw ToolFailure(McpToolErrorCode::ProjectUnavailable,
                          "The project file could not be opened: " + opened.diagnostic().message);
    std::shared_ptr<scripting::Session> session = std::move(opened).takeSession();
    const auto revision = session->snapshot().revision();
    const std::lock_guard index(core.fileIndexMutex);
    auto& slot = core.fileSessions[key];
    slot.session = std::move(session);
    slot.savedRevision = revision;
    core.fileIndex[key] = nullptr;
    return *slot.session;
}

std::shared_ptr<McpFileExports> existingFileExports(McpServerCore::State& core,
                                                    const std::filesystem::path& path) {
    const auto found = core.fileSessions.find(path.string());
    return found == core.fileSessions.end() ? nullptr : found->second.exports;
}

std::shared_ptr<McpFileExports> fileExportsLocked(McpServerCore::State& core,
                                                  const std::filesystem::path& path) {
    const auto key = path.string();
    auto& slot = core.fileSessions.at(key);
    if (!slot.exports) {
        slot.exports = std::make_shared<McpFileExports>(slot.session, core.dependencies.scheduler,
                                                        core.fileGpuProvider);
        const std::lock_guard index(core.fileIndexMutex);
        core.fileIndex[key] = slot.exports;
    }
    return slot.exports;
}

std::shared_ptr<McpFileExports> fileExportsFor(McpServerCore::State& core,
                                               const std::filesystem::path& path,
                                               const McpRequestControl& control) {
    {
        const std::lock_guard index(core.fileIndexMutex);
        const auto found = core.fileIndex.find(path.string());
        if (found != core.fileIndex.end() && found->second)
            return found->second;
    }
    const std::lock_guard lock(core.fileMutex);
    static_cast<void>(fileSessionFor(core, path, control));
    return fileExportsLocked(core, path);
}

yyjson_mut_val* fileProjectTool(McpServerCore::State& core, Json& out, const McpTool tool,
                                const std::filesystem::path& path, yyjson_val* arguments,
                                const McpRequestControl& control) {
    const std::lock_guard lock(core.fileMutex);
    auto& session = fileSessionFor(core, path, control, tool != McpTool::ProjectClose);
    auto& slot = core.fileSessions.at(path.string());
    const auto exports = slot.exports;
    std::unique_lock<McpFileExports> owner;
    if (exports)
        owner = std::unique_lock(*exports);
    auto* value = out.object();
    out.set(value, "projectFile", out.text(path.string()));
    if (tool == McpTool::ProjectSave) {
        const auto saved = session.save();
        // Issue #748: a save that ran and did not write the file names where it stopped and why,
        // as project.save does for the open project.
        if (!saved.succeeded() && saved.failure.has_value())
            return saveFailedAnswer(out, saved.message, saved.failure->stage,
                                    saved.failure->cause);
        if (!saved.succeeded())
            throw ToolFailure(McpToolErrorCode::ProjectUnavailable,
                              "The project file could not be saved: " + saved.message);
        slot.savedRevision = session.snapshot().revision();
        out.set(value, "saved", out.boolean(true));
        out.set(value, "revision", out.number(session.snapshot().revision().value()));
        return value;
    }
    const auto flag = [&](const char* name) {
        return arguments != nullptr && member(arguments, name) && boolean(member(arguments, name));
    };
    const bool discard = flag("discardChanges");
    const bool stopJobs = flag("stopJobs");
    const bool unsaved = session.snapshot().revision() != slot.savedRevision;
    if (unsaved && !discard)
        throw ToolFailure(McpToolErrorCode::PreflightBlocked,
                          path.filename().string() +
                              " has unsaved changes; project.save it, or pass discardChanges=true "
                              "to close it without saving.");
    // #783: like project.close of the open project, running export jobs are not stopped silently.
    if (exports && exports->activeJobCount() != 0 && !stopJobs)
        throw ToolFailure(McpToolErrorCode::PreflightBlocked,
                          "Export jobs of " + path.filename().string() +
                              " are running; wait for them (export.wait with its projectFile), or "
                              "pass stopJobs=true to stop them and close.",
                          "jobs-running");
    if (exports)
        exports->shutdown(host::ExportStopCause::ProjectClosing);
    owner = {};
    dropFileSession(core, path.string());
    out.set(value, "closed", out.boolean(true));
    out.set(value, "discardedChanges", out.boolean(unsaved));
    return value;
}

// ---- #775: a projectFile call for the open project attaches to it

namespace {

// The digest of a project's canonical document (host::projectContentDigest) as hex; empty when it
// cannot be written. Two copies of a project with the same digest hold the same project.
std::string canonicalDigest(const scripting::SessionRenderSourceSnapshot& source,
                            const std::filesystem::path& path) {
    const auto digest = host::projectContentDigest(
        source.snapshot, source.colorSettings.value_or(document::ColorSettings{}), path);
    if (!digest)
        return {};
    const auto hex = digest->toLowercaseHex();
    return {hex.data(), hex.size()};
}

struct Comparison final {
    bool differs = false;
    std::string difference;
};

// Compares the open project (`open`, at `generation`) with its file, reusing the newest answer
// while neither the open project's revision nor the file's size and time changed.
Comparison compareWithFile(McpServerCore::State& core, const std::filesystem::path& path,
                           const scripting::SessionRenderSourceSnapshot& open,
                           const std::uint64_t generation) {
    const auto revision = open.snapshot.revision().value();
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    const auto time =
        error ? std::filesystem::file_time_type{} : std::filesystem::last_write_time(path, error);
    std::optional<McpServerCore::State::OpenProjectComparison> cached;
    {
        const std::lock_guard lock(core.comparisonMutex);
        cached = core.comparison;
    }
    McpServerCore::State::OpenProjectComparison next;
    next.path = path.string();
    next.generation = generation;
    next.revision = revision;
    next.fileSize = error ? 0 : size;
    next.fileTime = time;
    const bool samePath = cached && cached->path == next.path;
    if (samePath && cached->generation == generation && cached->revision == revision)
        next.openDigest = cached->openDigest;
    else
        try {
            next.openDigest = canonicalDigest(open, path);
        } catch (const std::exception&) {
            next.openDigest.clear();
        }
    if (error) {
        next.fileProblem = "the file can't be read (" + error.message() + ")";
    } else if (samePath && cached->fileSize == next.fileSize && cached->fileTime == time) {
        next.fileDigest = cached->fileDigest;
        next.fileProblem = cached->fileProblem;
    } else {
        try {
            auto opened = scripting::Session::open(path);
            if (!opened) {
                next.fileProblem = "the file can't be opened (" + opened.diagnostic().message + ")";
            } else {
                const auto session = std::move(opened).takeSession();
                const auto source = session->renderSourceSnapshot();
                next.fileDigest = canonicalDigest(source, path);
            }
        } catch (const std::exception& exception) {
            next.fileProblem = std::string("the file can't be read (") + exception.what() + ")";
        }
    }
    Comparison answer;
    if (!next.fileProblem.empty()) {
        answer.differs = true;
        answer.difference = "The project open in Bloom could not be compared with " +
                            path.filename().string() + ": " + next.fileProblem + ".";
    } else if (next.openDigest.empty() || next.fileDigest.empty()) {
        answer.differs = true;
        answer.difference = "The project open in Bloom could not be compared with " +
                            path.filename().string() + ".";
    } else if (next.openDigest != next.fileDigest) {
        answer.differs = true;
        answer.difference = "The project open in Bloom differs from " + path.filename().string() +
                            " on disk: Bloom has changes that are not saved, or the file changed "
                            "after Bloom opened it.";
    }
    {
        const std::lock_guard lock(core.comparisonMutex);
        core.comparison = std::move(next);
    }
    return answer;
}

} // namespace

std::optional<McpOpenProjectAttachment> attachToOpenProject(McpServerCore::State& core,
                                                            const McpTool tool,
                                                            const std::filesystem::path& path,
                                                            const McpRequestControl& control) {
    std::optional<scripting::SessionRenderSourceSnapshot> open;
    std::uint64_t generation = 0;
    if (auto* live = core.liveHost()) {
        auto lease =
            live->acquire({.stop = control.stop,
                           .deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2)});
        if (!lease)
            throw busy(path);
        if (!lease.state().projectPath || normalized(*lease.state().projectPath) != path)
            return std::nullopt;
        open = lease.session().renderSourceSnapshot();
        generation = lease.generation();
    } else if (const auto* headless = std::get_if<std::reference_wrapper<scripting::Session>>(
                   &core.dependencies.session)) {
        const auto& own = headless->get().displayPath();
        if (own.empty() || normalized(own) != path)
            return std::nullopt;
        open = headless->get().renderSourceSnapshot();
    } else {
        return std::nullopt;
    }
    // A projectFile session of this server opened the file before it was open here.
    const auto key = path.string();
    bool held = false;
    {
        const std::lock_guard index(core.fileIndexMutex);
        held = core.fileIndex.contains(key);
    }
    if (held) {
        // Saving or closing that session is its own business (fileProjectTool).
        if (tool == McpTool::ProjectSave || tool == McpTool::ProjectClose)
            return std::nullopt;
        const std::lock_guard lock(core.fileMutex);
        const auto found = core.fileSessions.find(key);
        if (found != core.fileSessions.end()) {
            const auto name = path.filename().string();
            if (found->second.session->snapshot().revision() != found->second.savedRevision)
                throw ToolFailure(
                    McpToolErrorCode::PreflightBlocked,
                    name +
                        " is open in Bloom, and this server's projectFile session of it has "
                        "unsaved changes, so the two copies differ. Close that session with "
                        "project.close, projectFile and discardChanges:true to work on the "
                        "project open in Bloom (the session's changes are lost), or ask the "
                        "artist to close " +
                        name + " in Bloom.");
            if (const auto exports = found->second.exports) {
                const std::lock_guard<McpFileExports> owner(*exports);
                if (exports->activeJobCount() != 0)
                    throw ToolFailure(McpToolErrorCode::PreflightBlocked,
                                      name + " is open in Bloom, and export jobs of this server's "
                                             "projectFile session of it are running; wait for "
                                             "them (export.wait with its projectFile).",
                                      "jobs-running");
                exports->shutdown(host::ExportStopCause::ProjectClosing);
            }
            // The session holds nothing the file lacks: the open project takes over.
            dropFileSession(core, key);
        }
    }
    const auto compared = compareWithFile(core, path, *open, generation);
    return McpOpenProjectAttachment{
        .path = path, .fileDiffers = compared.differs, .difference = compared.difference};
}

void addAttachment(Json& out, yyjson_mut_val* value, const McpOpenProjectAttachment& attachment) {
    if (value == nullptr || !yyjson_mut_is_obj(value))
        return;
    if (yyjson_mut_obj_get(value, "projectFile") == nullptr)
        out.set(value, "projectFile", out.text(attachment.path.string()));
    out.set(value, "attachedToOpenProject", out.boolean(true));
    out.set(value, "fileDiffers", out.boolean(attachment.fileDiffers));
    if (!attachment.fileDiffers)
        return;
    out.set(value, "fileDifference", out.text(attachment.difference));
    auto* reload = out.object();
    out.set(reload, "tool", out.text("project.open"));
    auto* arguments = out.object();
    out.set(arguments, "path", out.text(attachment.path.string()));
    out.set(arguments, "discardChanges", out.boolean(true));
    out.set(reload, "arguments", arguments);
    out.set(reload, "effect",
            out.text("Opens " + attachment.path.filename().string() +
                     " from disk in Bloom, so the open project is the file; changes in Bloom "
                     "that are not saved are discarded. To keep them instead, project.save."));
    out.set(value, "reload", reload);
}

yyjson_mut_val* attachedCloseAnswer(Json& out, const McpOpenProjectAttachment& attachment) {
    auto* value = out.object();
    out.set(value, "closed", out.boolean(false));
    out.set(value, "note",
            out.text(attachment.path.filename().string() +
                     " is the project open in Bloom, and calls with this projectFile work on it, "
                     "so there is no separate session to close. project.close without projectFile "
                     "closes it in Bloom."));
    return value;
}

} // namespace bloom::mcp
