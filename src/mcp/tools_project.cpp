// project.set-color-settings: Project Settings > Colour for agents. The settings are session state
// of the project (host::ProjectSession), not part of the document, so this is a tool beside
// transact rather than an operation; the change itself is one undoable history entry ("Change
// Project Color Settings") and marks the project dirty, exactly as the dialog's change does.
// Headless it changes the stdio session's project; in the desktop application it goes through
// McpLiveUiPort::setColorSettings on the application's thread. An external or project-relative
// OCIO configuration is read here, on the call's own thread, in the supervised color helper.

#include "core_internal.hpp"

#include <bloom/host/project_color_choice.hpp>

namespace bloom::mcp {

namespace {

yyjson_mut_val* colorResult(Json& out, const std::string& code, const std::string& message,
                            const std::optional<document::ColorSettings>& settings,
                            const bool succeeded) {
    auto* value = out.object();
    out.set(value, "succeeded", out.boolean(succeeded));
    out.set(value, "code", out.text(code));
    out.set(value, "message", out.text(message));
    if (settings) {
        out.set(value, "config",
                out.text(host::projectColorConfigName(host::projectColorConfigOf(*settings))));
        out.set(value, "workingSpace", out.text(settings->processColorSpaceId));
        out.set(value, "workingSpaceLabel",
                out.text(std::string(host::workingColorSpaceLabel(settings->processColorSpaceId))));
        // What the configuration is: its kind, where it is, its pinned revision and the project's
        // OCIO context variables.
        const auto description = host::describeProjectColorConfig(*settings);
        out.set(value, "configKind", out.text(description.kind));
        out.set(value, "configLocation", out.text(description.location));
        out.set(value, "configRevision", out.text(description.revision));
        auto* context = out.object();
        for (const auto& variable : description.contextVariables)
            out.set(context, variable.name.c_str(), out.text(variable.value));
        out.set(value, "context", context);
    }
    return value;
}

// The desktop application: the settings are resolved here, against the live project's current
// ones, and applied by the application on its own thread through McpLiveUiPort::setColorSettings.
yyjson_mut_val* liveColorSettings(McpServerCore::State& core, Json& out, McpCall& call,
                                  const std::string& config, std::optional<std::string> working,
                                  const std::optional<host::ProjectColorConfigFile>& file) {
    const auto& apply = core.dependencies.ui.setColorSettings;
    if (!apply) {
        auto* value = out.object();
        out.set(value, "succeeded", out.boolean(false));
        auto* diagnostic = out.object();
        out.set(diagnostic, "code", out.text(uiRefusalCode(McpUiRefusal::Unbound)));
        out.set(diagnostic, "message",
                out.text("This Bloom window does not offer project.set-color-settings to agents "
                         "yet."));
        out.set(value, "diagnostic", diagnostic);
        return value;
    }
    const bool fromFile = config == "file";
    const auto choice = fromFile ? std::optional(host::ProjectColorConfig::Current)
                                 : host::parseProjectColorConfig(config);
    if (!choice)
        return colorResult(out, "invalid-config",
                           R"(Expected config "neutral", "aces", "current", "clear" or "file")",
                           std::nullopt, false);
    if (fromFile && !file)
        return colorResult(out, "invalid-config", R"(config "file" needs file: the path to use)",
                           std::nullopt, false);
    const auto source = call.session.renderSourceSnapshot();
    const auto& current = source.colorSettings;
    if (!current)
        return colorResult(out, "unavailable", "This project has no color settings", std::nullopt,
                           false);
    // An external configuration is read on this call's thread; the stop token ends the wait.
    auto resolved =
        fromFile ? host::resolveProjectColorConfigFile(
                       *current, *file, working, source.assetBaseDirectory,
                       [&call] { return call.control.stop.stop_requested(); })
                 : host::resolveProjectColorSettings(*current, *choice, std::move(working));
    if (!resolved.settings)
        return colorResult(out, "invalid-settings", resolved.reason, current, false);
    if (*resolved.settings == *current)
        return colorResult(out, "no-change", "The project already uses these color settings",
                           current, true);
    McpUiResult applied;
    if (!core.dependencies.exports.run(
            [&](host::ExportQueueService*, std::uint64_t) { applied = apply(*resolved.settings); },
            call.control.stop))
        throw ToolFailure(McpToolErrorCode::Cancelled, "The request was cancelled.");
    if (applied.refusal == McpUiRefusal::ReadOnly)
        throw ToolFailure(McpToolErrorCode::ReadOnly,
                          applied.message.empty() ? "The project is read-only." : applied.message,
                          "read-only");
    if (!applied.ok()) {
        auto* value = out.object();
        out.set(value, "succeeded", out.boolean(false));
        auto* diagnostic = out.object();
        out.set(diagnostic, "code", out.text(uiRefusalCode(applied.refusal)));
        out.set(diagnostic, "message",
                out.text(applied.message.empty() ? "Bloom refused the color settings."
                                                 : applied.message));
        out.set(value, "diagnostic", diagnostic);
        return value;
    }
    return colorResult(out, "updated", "The project color settings changed", resolved.settings,
                       true);
}

} // namespace

yyjson_mut_val* colorSettingsTool(McpServerCore::State& core, Json& out, McpCall& call,
                                  yyjson_val* arguments) {
    members(arguments, {"config", "workingSpace", "file", "projectRelative", "context"}, {"config"});
    const auto config = string(member(arguments, "config"), 16);
    std::optional<std::string> working;
    if (auto* value = member(arguments, "workingSpace"))
        working = string(value, document::kMaxWorkingColorSpaceIdBytes);
    std::optional<host::ProjectColorConfigFile> file;
    if (auto* value = member(arguments, "file")) {
        file.emplace();
        file->path = string(value, document::kMaxOcioExternalUriBytes);
        if (auto* relative = member(arguments, "projectRelative"))
            file->projectRelative = boolean(relative);
        if (auto* context = member(arguments, "context")) {
            if (!yyjson_is_obj(context))
                throw InvalidInput("context must be an object of text values");
            yyjson_val* key = nullptr;
            yyjson_obj_iter iterator = yyjson_obj_iter_with(context);
            while ((key = yyjson_obj_iter_next(&iterator)) != nullptr) {
                auto* entry = yyjson_obj_iter_get_val(key);
                file->contextVariables.push_back(
                    {std::string(yyjson_get_str(key), yyjson_get_len(key)),
                     string(entry, document::kMaxOcioContextValueBytes)});
            }
        }
    } else if (member(arguments, "projectRelative") != nullptr ||
               member(arguments, "context") != nullptr) {
        throw InvalidInput("projectRelative and context apply to a file; give file as well");
    }
    call.summary = config;
    if (call.live != nullptr)
        return liveColorSettings(core, out, call, config, std::move(working), file);
    const auto result = call.session.setColorSettings(config, std::move(working), file);
    if (result.code == "unavailable")
        throw ToolFailure(McpToolErrorCode::OperationRejected, result.message, result.code);
    if (result.code == "read-only")
        throw ToolFailure(McpToolErrorCode::ReadOnly, result.message, result.code);
    return colorResult(out, result.code, result.message, result.settings, result.succeeded());
}

} // namespace bloom::mcp
