#include "core_internal.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <system_error>

namespace bloom::mcp {
namespace {

std::filesystem::path normalized(const std::filesystem::path& path) {
    std::error_code error;
    auto result = std::filesystem::weakly_canonical(path, error);
    return error ? path.lexically_normal() : result;
}

} // namespace

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

std::string withoutProjectFile(yyjson_val* arguments) {
    auto* document = yyjson_mut_doc_new(nullptr);
    yyjson_mut_doc_set_root(document, yyjson_val_mut_copy(document, arguments));
    yyjson_mut_obj_remove_str(yyjson_mut_doc_get_root(document), "projectFile");
    std::size_t length = 0;
    char* text = yyjson_mut_write(document, 0, &length);
    std::string result = text != nullptr ? std::string(text, length) : std::string("{}");
    std::free(text);
    yyjson_mut_doc_free(document);
    return result;
}

namespace {

// Refuses `path` when the application (or this server's own project) has it open: two sessions
// on one file would overwrite each other's saves. When the application cannot be asked in time
// the answer is no, never an assumption that the file is free.
void refuseIfOpenElsewhere(McpServerCore::State& core, const std::filesystem::path& path,
                           const McpRequestControl& control) {
    const auto refuse = [&](const std::string& where) -> ToolFailure {
        return ToolFailure(McpToolErrorCode::PreflightBlocked,
                           path.filename().string() + " is already open " + where +
                               "; two sessions on one file would overwrite each other's saves. "
                               "Use that project's own session, or close it first.");
    };
    if (auto* live = core.liveHost()) {
        auto lease = live->acquire(
            {.stop = control.stop,
             .deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2)});
        if (!lease)
            throw ToolFailure(McpToolErrorCode::ProjectUnavailable,
                              "Bloom is busy, so it could not be confirmed that " +
                                  path.filename().string() +
                                  " is not open there; try again in a moment.");
        if (lease.state().projectPath && normalized(*lease.state().projectPath) == path)
            throw refuse("in Bloom");
    } else if (const auto* headless = std::get_if<std::reference_wrapper<scripting::Session>>(
                   &core.dependencies.session)) {
        const auto& own = headless->get().displayPath();
        if (!own.empty() && normalized(own) == path)
            throw refuse("as this server's project");
    }
}

} // namespace

bool fileSessionHolds(McpServerCore::State& core, const std::filesystem::path& path) {
    std::unique_lock lock(core.fileMutex, std::defer_lock);
    if (!lock.try_lock_for(std::chrono::seconds(2)))
        return true;
    return core.fileSessions.contains(normalized(path).string());
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
    auto session = std::move(opened).takeSession();
    const auto revision = session->snapshot().revision();
    auto& slot = core.fileSessions[key];
    slot.session = std::move(session);
    slot.savedRevision = revision;
    return *slot.session;
}

yyjson_mut_val* fileProjectTool(McpServerCore::State& core, Json& out, const McpTool tool,
                                const std::filesystem::path& path, yyjson_val* arguments,
                                const McpRequestControl& control) {
    const std::lock_guard lock(core.fileMutex);
    auto& session = fileSessionFor(core, path, control, tool != McpTool::ProjectClose);
    auto& slot = core.fileSessions.at(path.string());
    auto* value = out.object();
    out.set(value, "projectFile", out.text(path.string()));
    if (tool == McpTool::ProjectSave) {
        const auto saved = session.save();
        if (!saved.succeeded())
            throw ToolFailure(McpToolErrorCode::ProjectUnavailable,
                              "The project file could not be saved: " + saved.message);
        slot.savedRevision = session.snapshot().revision();
        out.set(value, "saved", out.boolean(true));
        out.set(value, "revision", out.number(session.snapshot().revision().value()));
        return value;
    }
    const bool discard =
        arguments != nullptr && member(arguments, "discardChanges") &&
        boolean(member(arguments, "discardChanges"));
    const bool unsaved = session.snapshot().revision() != slot.savedRevision;
    if (unsaved && !discard)
        throw ToolFailure(McpToolErrorCode::PreflightBlocked,
                          path.filename().string() +
                              " has unsaved changes; project.save it, or pass discardChanges=true "
                              "to close it without saving.");
    core.fileSessions.erase(path.string());
    out.set(value, "closed", out.boolean(true));
    out.set(value, "discardedChanges", out.boolean(unsaved));
    return value;
}

} // namespace bloom::mcp
