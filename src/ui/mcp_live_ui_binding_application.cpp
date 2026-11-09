// MCP parity (#389): the application-state members of the live-UI port -- Preferences, Edit >
// Purge, the Jobs panel, File > Cancel Open, Reveal in File Manager and the Composition Settings
// dialog's presets. Each does what the artist's own control does, through the same model or
// command body; none of them is a project edit.

#include <bloom/ui/mcp_live_ui_binding.hpp>

#include "reveal_in_file_manager.hpp"

#include <bloom/commands/composition_settings_operations.hpp>
#include <bloom/document/project.hpp>
#include <bloom/host/export_worker_limits.hpp>
#include <bloom/runtime/memory_headroom.hpp>
#include <bloom/ui/application_preferences.hpp>
#include <bloom/ui/asset_controller.hpp>
#include <bloom/ui/composition_presets.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/main_window.hpp>
#include <bloom/ui/project_host.hpp>
#include <bloom/ui/task_monitor_model.hpp>

#ifdef BLOOM_BUILD_PYTHON
#include "script_panel_runtime.hpp"
#endif

#include <QApplication>
#include <QSettings>
#include <QString>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <functional>
#include <optional>
#include <string_view>

namespace bloom::ui {

using mcp::McpUiRefusal;

namespace {

mcp::McpUiResult uiResult(const McpUiRefusal refusal, const QString& message) {
    return {.refusal = refusal, .message = message.toStdString()};
}

mcp::McpUiResult uiOk(const QString& message) { return uiResult(McpUiRefusal::None, message); }

// ---- Preferences -------------------------------------------------------------------------------

using Value = mcp::McpPreferenceValue;
// A refusal sentence, or nullopt once applied.
using Apply = std::function<std::optional<QString>(ApplicationPreferences&, const Value&)>;

struct PreferenceKey final {
    std::string_view key;
    std::function<Value(const ApplicationPreferences&)> read;
    Apply write;
};

Apply flag(bool ApplicationPreferences::* field) {
    return [field](ApplicationPreferences& preferences, const Value& value) {
        const auto* held = std::get_if<bool>(&value);
        if (held == nullptr)
            return std::optional(QObject::tr("takes true or false"));
        preferences.*field = *held;
        return std::optional<QString>{};
    };
}

PreferenceKey flagKey(const std::string_view key, bool ApplicationPreferences::* field) {
    return {key, [field](const ApplicationPreferences& p) { return Value(p.*field); },
            flag(field)};
}

std::optional<double> numberOf(const Value& value) {
    const auto* held = std::get_if<double>(&value);
    return held != nullptr && std::isfinite(*held) ? std::optional(*held) : std::nullopt;
}

// A choice among lowercase names, in the enum's own order.
template <typename Enum, std::size_t Count>
PreferenceKey choiceKey(const std::string_view key, Enum ApplicationPreferences::* field,
                        const std::array<std::string_view, Count> names) {
    return {key,
            [field, names](const ApplicationPreferences& p) {
                return Value(std::string(names.at(static_cast<std::size_t>(p.*field))));
            },
            [field, names](ApplicationPreferences& p, const Value& value) {
                const auto* text = std::get_if<std::string>(&value);
                const auto found =
                    text != nullptr ? std::ranges::find(names, *text) : names.end();
                if (found == names.end()) {
                    QStringList choices;
                    for (const auto name : names)
                        choices << QString::fromUtf8(name.data(), static_cast<int>(name.size()));
                    return std::optional(
                        QObject::tr("is one of %1").arg(choices.join(QStringLiteral(", "))));
                }
                p.*field = static_cast<Enum>(std::distance(names.begin(), found));
                return std::optional<QString>{};
            }};
}

PreferenceKey bytesKey(const std::string_view key, std::uint64_t ApplicationPreferences::* field,
                       const std::uint64_t maximum) {
    return {key,
            [field](const ApplicationPreferences& p) {
                return Value(static_cast<double>(p.*field));
            },
            [field, maximum](ApplicationPreferences& p, const Value& value) {
                const auto number = numberOf(value);
                if (!number || *number < 0.0 || *number != std::floor(*number) ||
                    *number > static_cast<double>(maximum))
                    return std::optional(QObject::tr("is a whole number of bytes from 0 to %1")
                                             .arg(static_cast<qulonglong>(maximum)));
                p.*field = static_cast<std::uint64_t>(*number);
                return std::optional<QString>{};
            }};
}

// "a/b" or "a": a positive rational with 32-bit parts.
std::optional<std::pair<std::int64_t, std::int64_t>> ratioOf(const std::string& text) {
    const auto slash = text.find('/');
    const auto part = [](const std::string_view piece) -> std::optional<std::int64_t> {
        std::int64_t value = 0;
        const auto* end = piece.data() + piece.size();
        const auto parsed = std::from_chars(piece.data(), end, value);
        if (parsed.ec != std::errc{} || parsed.ptr != end || value <= 0 || value > 0xffffffffLL)
            return std::nullopt;
        return value;
    };
    const auto numerator = part(std::string_view(text).substr(0, slash));
    const auto denominator =
        slash == std::string::npos ? std::optional<std::int64_t>(1)
                                   : part(std::string_view(text).substr(slash + 1));
    if (!numerator || !denominator)
        return std::nullopt;
    return std::pair{*numerator, *denominator};
}

// A switch an agent can read but only the Preferences window can turn on (agent routes that act on
// the machine): preferences.set refuses it, so an agent cannot open its own route.
PreferenceKey artistOnlyFlagKey(const std::string_view key, bool ApplicationPreferences::* field) {
    return {key, [field](const ApplicationPreferences& p) { return Value(p.*field); },
            [](ApplicationPreferences&, const Value&) {
                return std::optional(QObject::tr(
                    "can only be changed by the artist in Edit > Preferences > General > Agents"));
            }};
}

const std::vector<PreferenceKey>& preferenceKeys() {
    static const std::vector<PreferenceKey> keys = [] {
        std::vector<PreferenceKey> list;
        list.push_back(flagKey("audioPlayback", &ApplicationPreferences::audioEnabled));
        list.push_back(flagKey("loopPlayback", &ApplicationPreferences::loopPlayback));
        list.push_back(flagKey("crashRecovery", &ApplicationPreferences::crashRecoveryEnabled));
        list.push_back(bytesKey("ramHeadroomBytes", &ApplicationPreferences::ramHeadroomBytes,
                                runtime::kMaximumMemoryHeadroomBytes));
        list.push_back(bytesKey("vramHeadroomBytes", &ApplicationPreferences::vramHeadroomBytes,
                                runtime::kMaximumMemoryHeadroomBytes));
        list.push_back(bytesKey("exportWorkerMemoryCapBytes",
                                &ApplicationPreferences::exportWorkerMemoryCapBytes,
                                host::kMaximumExportWorkerMemoryCapBytes));
        list.push_back(
            {"exportWorkerThreads",
             [](const ApplicationPreferences& p) { return Value(static_cast<double>(p.exportWorkerThreads)); },
             [](ApplicationPreferences& p, const Value& value) {
                 const auto number = numberOf(value);
                 if (!number || *number < 0.0 || *number != std::floor(*number) ||
                     *number > static_cast<double>(host::kMaximumExportWorkerThreads))
                     return std::optional(
                         QObject::tr("is a whole number of encoder threads from 0 (automatic) to %1")
                             .arg(host::kMaximumExportWorkerThreads));
                 p.exportWorkerThreads = static_cast<std::uint32_t>(*number);
                 return std::optional<QString>{};
             }});
        list.push_back(flagKey("mediaDiskCache", &ApplicationPreferences::mediaDiskCacheEnabled));
        list.push_back(
            {"mediaDiskCacheDirectory",
             [](const ApplicationPreferences& p) { return Value(p.mediaDiskCacheDirectory); },
             [](ApplicationPreferences& p, const Value& value) {
                 const auto* text = std::get_if<std::string>(&value);
                 if (text == nullptr ||
                     (!text->empty() && !std::filesystem::path(*text).is_absolute()))
                     return std::optional(QObject::tr(
                         "is an absolute folder path, or empty for the default folder"));
                 p.mediaDiskCacheDirectory = *text;
                 return std::optional<QString>{};
             }});
        list.push_back(bytesKey("mediaDiskCacheBudgetBytes",
                                &ApplicationPreferences::mediaDiskCacheBudgetBytes,
                                std::uint64_t{1} << 50U));
        list.push_back(choiceKey("timelineTimeFormat", &ApplicationPreferences::timelineTimeFormat,
                                 std::array<std::string_view, 2>{"frames", "timecode"}));
        list.push_back(flagKey("timelineSnapping", &ApplicationPreferences::timelineSnapping));
        list.push_back(
            flagKey("timelineKeyframesVisible", &ApplicationPreferences::timelineKeyframesVisible));
        list.push_back(flagKey("timelineGraphEditor", &ApplicationPreferences::timelineGraphEditor));
        list.push_back(choiceKey("nodeLinkStyle", &ApplicationPreferences::nodeLinkStyle,
                                 std::array<std::string_view, 3>{"spline", "straight", "angled"}));
        list.push_back(flagKey("nodeSnap", &ApplicationPreferences::nodeSnap));
        list.push_back({"nodeGridSize",
                        [](const ApplicationPreferences& p) { return Value(p.nodeGridSize); },
                        [](ApplicationPreferences& p, const Value& value) {
                            const auto number = numberOf(value);
                            if (!number || *number < 1.0)
                                return std::optional(QObject::tr("is a size of 1 pixel or more"));
                            p.nodeGridSize = *number;
                            return std::optional<QString>{};
                        }});
        list.push_back(choiceKey("viewerResolution", &ApplicationPreferences::viewerResolution,
                                 std::array<std::string_view, 4>{"auto", "full", "half",
                                                                 "quarter"}));
        list.push_back(choiceKey("viewerBackground", &ApplicationPreferences::viewerBackground,
                                 std::array<std::string_view, 4>{"solid", "gray", "black",
                                                                 "white"}));
        list.push_back(
            flagKey("viewerTransparencyGrid", &ApplicationPreferences::viewerTransparencyGrid));
        list.push_back(flagKey("viewerSafeAreas", &ApplicationPreferences::viewerSafeAreas));
        list.push_back(flagKey("viewerCenterCross", &ApplicationPreferences::viewerCentreCross));
        list.push_back(flagKey("viewerThirds", &ApplicationPreferences::viewerThirds));
        list.push_back(flagKey("viewerRulers", &ApplicationPreferences::viewerRulers));
        list.push_back(flagKey("viewerPixelGrid", &ApplicationPreferences::viewerPixelGrid));
        list.push_back(
            {"importSequenceFrameRate",
             [](const ApplicationPreferences& p) {
                 const auto& rate = p.importSequenceFrameRate;
                 return Value(rate ? std::to_string(rate->numerator()) + "/" +
                                         std::to_string(rate->denominator())
                                   : std::string{});
             },
             [](ApplicationPreferences& p, const Value& value) {
                 const auto* text = std::get_if<std::string>(&value);
                 if (text != nullptr && text->empty()) {
                     p.importSequenceFrameRate.reset();
                     return std::optional<QString>{};
                 }
                 const auto ratio = text != nullptr ? ratioOf(*text) : std::nullopt;
                 const auto rate =
                     ratio ? document::FrameRate::create(static_cast<std::uint32_t>(ratio->first),
                                                         static_cast<std::uint32_t>(ratio->second))
                           : std::nullopt;
                 if (!rate)
                     return std::optional(QObject::tr(
                         "is a frame rate such as \"24\" or \"24000/1001\", or empty for one "
                         "image per composition frame"));
                 p.importSequenceFrameRate = *rate;
                 return std::optional<QString>{};
             }});
        list.push_back(
            {"importStillDuration",
             [](const ApplicationPreferences& p) {
                 const auto& duration = p.importStillDuration;
                 return Value(duration ? std::to_string(duration->numerator()) + "/" +
                                             std::to_string(duration->denominator())
                                       : std::string{});
             },
             [](ApplicationPreferences& p, const Value& value) {
                 const auto* text = std::get_if<std::string>(&value);
                 if (text != nullptr && text->empty()) {
                     p.importStillDuration.reset();
                     return std::optional<QString>{};
                 }
                 const auto ratio = text != nullptr ? ratioOf(*text) : std::nullopt;
                 const auto duration =
                     ratio ? core::RationalTime::create(ratio->first, ratio->second)
                           : std::nullopt;
                 if (!duration)
                     return std::optional(QObject::tr(
                         "is a duration in seconds such as \"5\" or \"1001/240\", or empty for "
                         "the composition's length"));
                 p.importStillDuration = *duration;
                 return std::optional<QString>{};
             }});
        list.push_back(flagKey("sounds", &ApplicationPreferences::soundsEnabled));
        list.push_back({"soundVolume",
                        [](const ApplicationPreferences& p) {
                            return Value(static_cast<double>(p.soundVolumePercent));
                        },
                        [](ApplicationPreferences& p, const Value& value) {
                            const auto number = numberOf(value);
                            if (!number || *number < 0.0 || *number > 100.0 ||
                                *number != std::floor(*number))
                                return std::optional(
                                    QObject::tr("is a whole percentage from 0 to 100"));
                            p.soundVolumePercent = static_cast<int>(*number);
                            return std::optional<QString>{};
                        }});
        list.push_back(flagKey("eventSounds", &ApplicationPreferences::eventSoundsEnabled));
        list.push_back(
            flagKey("interactionSounds", &ApplicationPreferences::interactionSoundsEnabled));
        list.push_back(
            flagKey("renderNotifications", &ApplicationPreferences::renderNotificationsEnabled));
        list.push_back(artistOnlyFlagKey("agentsAnswerDialogs",
                                         &ApplicationPreferences::agentsAnswerDialogs));
        list.push_back(
            artistOnlyFlagKey("agentsRunScripts", &ApplicationPreferences::agentsRunScripts));
        return list;
    }();
    return keys;
}

} // namespace

std::vector<mcp::McpPreference> McpLiveUiBinding::preferences() const {
    const auto current = loadApplicationPreferences(QSettings{});
    std::vector<mcp::McpPreference> values;
    for (const auto& key : preferenceKeys())
        values.push_back({.key = std::string(key.key), .value = key.read(current)});
    return values;
}

mcp::McpUiResult McpLiveUiBinding::setPreferences(const std::vector<mcp::McpPreference>& entries) {
    if (!window_)
        return uiResult(McpUiRefusal::Unbound, tr("There is no Bloom window to apply them to."));
    if (QApplication::activeModalWidget() != nullptr)
        return uiResult(McpUiRefusal::Busy, openDialogMessage());
    auto next = loadApplicationPreferences(QSettings{});
    for (const auto& entry : entries) {
        const auto found = std::ranges::find(preferenceKeys(), std::string_view(entry.key),
                                             &PreferenceKey::key);
        if (found == preferenceKeys().end())
            return uiResult(McpUiRefusal::InvalidArgument,
                            tr("There is no preference “%1”; preferences.get lists them.")
                                .arg(QString::fromStdString(entry.key)));
        if (const auto refused = found->write(next, entry.value))
            return uiResult(McpUiRefusal::InvalidArgument,
                            tr("%1 %2.").arg(QString::fromStdString(entry.key), *refused));
    }
    window_->commitPreferences(next);
    return uiOk(tr("The preferences were saved and applied."));
}

// ---- script.run (#603) -------------------------------------------------------------------------

void McpLiveUiBinding::setScriptRuntime(const std::shared_ptr<ScriptPanelRuntime>& runtime) {
    script_ = runtime;
}

void McpLiveUiBinding::runScript(mcp::McpScriptRequest request,
                                 std::function<void(mcp::McpScriptResult)> done) {
    const auto refuse = [&done](const McpUiRefusal refusal, const QString& message) {
        mcp::McpScriptResult result;
        result.refusal = refusal;
        result.message = message.toStdString();
        done(std::move(result));
    };
    // The artist's switch, read from the saved settings each time; only Preferences turns it on.
    if (!loadApplicationPreferences(QSettings{}).agentsRunScripts)
        return refuse(McpUiRefusal::NotAllowed,
                      tr("Agents are not allowed to run Python scripts. The artist can allow it "
                         "in Edit > Preferences > General > Agents."));
#ifdef BLOOM_BUILD_PYTHON
    const auto runtime = script_.lock();
    if (!runtime)
        return refuse(McpUiRefusal::Unbound,
                      tr("This Bloom build has no Python scripting, so there is nothing to run "
                         "the script."));
    if (request.source.empty())
        return refuse(McpUiRefusal::InvalidArgument, tr("The script is empty."));
    const bool accepted = runtime->submit(
        QString::fromStdString(request.source), [done](const QString& output, const bool ok) {
            mcp::McpScriptResult result;
            result.output = output.toStdString();
            result.succeeded = ok;
            done(std::move(result));
        });
    if (!accepted)
        refuse(McpUiRefusal::Busy,
               tr("The Script editor's queue is full (or the script is longer than 65536 "
                  "characters). Try again in a moment."));
#else
    static_cast<void>(request);
    refuse(McpUiRefusal::Unbound,
           tr("This Bloom build has no Python scripting, so there is nothing to run the script."));
#endif
}

void McpLiveUiBinding::cancelScript() {
#ifdef BLOOM_BUILD_PYTHON
    if (const auto runtime = script_.lock())
        runtime->cancel();
#endif
}

// ---- cache.purge -------------------------------------------------------------------------------

mcp::McpUiResult McpLiveUiBinding::purgeCache(const bool media) {
    if (!window_)
        return uiResult(McpUiRefusal::Unbound, tr("There is no Bloom window to purge."));
    const bool started = media ? window_->purgeMediaCache() : window_->purgePreviewCache();
    if (!started)
        return uiResult(McpUiRefusal::Busy, tr("A cache purge is already running."));
    return uiOk(media ? tr("The media cache is being purged.")
                      : tr("The preview cache is being purged."));
}

// ---- jobs.list and jobs.cancel -----------------------------------------------------------------

std::vector<mcp::McpJob> McpLiveUiBinding::jobs() const {
    std::vector<mcp::McpJob> rows;
    if (!tasks_)
        return rows;
    const auto& model = *tasks_;
    for (int row = 0; row < model.rowCount(); ++row) {
        const auto at = [&](const int column) { return model.index(row, column); };
        mcp::McpJob job;
        job.id = model.data(at(TaskMonitorModel::JobColumn), TaskMonitorModel::TaskIdRole)
                     .toULongLong();
        job.name = model.data(at(TaskMonitorModel::JobColumn)).toString().toStdString();
        job.phase = model.data(at(TaskMonitorModel::PhaseColumn)).toString().toStdString();
        job.state = model.data(at(TaskMonitorModel::StateColumn)).toString().toStdString();
        const auto completed =
            model.data(at(TaskMonitorModel::JobColumn), TaskMonitorModel::ProgressCompletedRole);
        const auto total =
            model.data(at(TaskMonitorModel::JobColumn), TaskMonitorModel::ProgressTotalRole);
        if (completed.isValid())
            job.completed = completed.toULongLong();
        if (total.isValid())
            job.total = total.toULongLong();
        job.canCancel =
            model.data(at(TaskMonitorModel::JobColumn), TaskMonitorModel::CanCancelRole).toBool();
        job.cancellationRequested =
            model.data(at(TaskMonitorModel::JobColumn), TaskMonitorModel::CancellationRequestedRole)
                .toBool();
        if (const auto exportJob =
                model.data(at(TaskMonitorModel::JobColumn), TaskMonitorModel::ExportJobRole)
                    .toULongLong();
            exportJob != 0)
            job.exportJob = document::ExportJobId::fromRaw(exportJob);
        if (const auto* task = model.snapshotForIndex(at(TaskMonitorModel::JobColumn))) {
            constexpr std::array<std::string_view, 5> owners{
                "application", "project", "composition", "panel-request", "export"};
            constexpr std::array<std::string_view, 5> states{"queued", "running", "succeeded",
                                                             "cancelled", "failed"};
            job.kind = std::string(owners.at(static_cast<std::size_t>(task->owner.kind)));
            job.stateToken = std::string(states.at(static_cast<std::size_t>(task->state)));
            if (task->state == runtime::TaskState::Failed) {
                const auto error = std::ranges::find(task->diagnostics,
                                                     runtime::DiagnosticSeverity::Error,
                                                     &runtime::TaskDiagnostic::severity);
                if (error != task->diagnostics.end())
                    job.failure = error->detail.empty() ? error->summary
                                                        : error->summary + " " + error->detail;
            }
        }
        rows.push_back(std::move(job));
    }
    return rows;
}

mcp::McpUiResult McpLiveUiBinding::cancelJob(const std::uint64_t job) {
    if (!tasks_)
        return uiResult(McpUiRefusal::Unbound, tr("This Bloom window has no Jobs list."));
    const auto index = tasks_->indexForTask(runtime::TaskId::fromRaw(job));
    if (!index.isValid())
        return uiResult(McpUiRefusal::UnknownTarget,
                        tr("Job %1 is not in the Jobs list; jobs.list lists them.").arg(job));
    if (!tasks_->data(index, TaskMonitorModel::CanCancelRole).toBool())
        return uiResult(
            McpUiRefusal::InvalidArgument,
            tasks_->data(index, TaskMonitorModel::ExportJobRole).toULongLong() != 0
                ? tr("Job %1 is an export run; stop it with export.stop.").arg(job)
                : tr("Job %1 has finished or cannot be cancelled.").arg(job));
    if (!tasks_->requestCancellation(index))
        return uiResult(McpUiRefusal::Failed, tr("Job %1 could not be cancelled.").arg(job));
    return uiOk(tr("Cancellation of job %1 was requested.").arg(job));
}

// ---- project.cancel-open -----------------------------------------------------------------------

mcp::McpUiResult McpLiveUiBinding::cancelOpen() {
    if (project_.activity() != ProjectHostActivity::Opening)
        return uiResult(McpUiRefusal::InvalidArgument, tr("No project is opening."));
    project_.cancelOpen();
    return uiOk(tr("The open was cancelled; the project that was open stays."));
}

// ---- asset.reveal ------------------------------------------------------------------------------

mcp::McpUiResult McpLiveUiBinding::revealAsset(const document::AssetId asset) {
    if (session_.snapshot().project().findAsset(asset) == nullptr)
        return uiResult(McpUiRefusal::UnknownTarget,
                        tr("Asset %1 does not exist; query kind=assets lists them.")
                            .arg(asset.value()));
    auto* assets = session_.assetController();
    const auto path = assets != nullptr ? assets->sourcePath(asset) : std::filesystem::path{};
    if (path.empty())
        return uiResult(McpUiRefusal::InvalidArgument,
                        tr("Asset %1 has no file to reveal.").arg(asset.value()));
    // Selects the file where the platform's file manager can, else opens its folder (#380).
    revealInFileManager(path, window_ ? static_cast<QObject*>(window_.data())
                                      : static_cast<QObject*>(this));
    return uiOk(tr("The file manager shows %1.")
                    .arg(QString::fromStdU16String(path.filename().u16string())));
}

// ---- Composition presets -----------------------------------------------------------------------

std::vector<mcp::McpCompositionPreset> McpLiveUiBinding::compositionPresets() const {
    QSettings settings;
    auto presets = loadUserCompositionPresets(settings);
    const auto builtIn = builtInCompositionPresets();
    presets.insert(presets.begin(), builtIn.begin(), builtIn.end());
    std::vector<mcp::McpCompositionPreset> list;
    for (const auto& preset : presets) {
        mcp::McpCompositionPreset item{.name = preset.name.toStdString(),
                                       .builtIn = preset.builtIn,
                                       .width = preset.width,
                                       .height = preset.height,
                                       .pixelAspectNumerator = preset.pixelAspect.numerator(),
                                       .pixelAspectDenominator = preset.pixelAspect.denominator(),
                                       .frameRate = std::nullopt,
                                       .duration = preset.duration,
                                       .startTimecode = std::nullopt,
                                       .dropFrame = std::nullopt,
                                       .background = std::nullopt};
        if (preset.frameRate)
            item.frameRate = std::pair{preset.frameRate->numerator(),
                                       preset.frameRate->denominator()};
        if (preset.timecode) {
            item.startTimecode = preset.timecode->start;
            item.dropFrame = preset.timecode->dropFrame;
        }
        if (preset.background)
            item.background = std::array{preset.background->red, preset.background->green,
                                         preset.background->blue, preset.background->alpha};
        list.push_back(std::move(item));
    }
    return list;
}

namespace {

// The index of the saved preset named `name`, or nullopt.
std::optional<std::size_t> userPresetNamed(const std::vector<CompositionPreset>& presets,
                                           const QString& name) {
    for (std::size_t index = 0; index < presets.size(); ++index)
        if (presets[index].name == name.trimmed())
            return index;
    return std::nullopt;
}

void persist(QSettings& settings, const std::vector<CompositionPreset>& presets) {
    saveUserCompositionPresets(settings, presets);
    settings.sync();
}

} // namespace

mcp::McpUiResult McpLiveUiBinding::saveCompositionPreset(const document::CompositionId composition,
                                                         const std::string& name) {
    const auto snapshot = session_.snapshot();
    const auto* found = snapshot.project().findComposition(composition);
    if (found == nullptr)
        return uiResult(McpUiRefusal::UnknownTarget,
                        tr("Composition %1 does not exist; query kind=compositions lists them.")
                            .arg(composition.value()));
    QSettings settings;
    auto presets = loadUserCompositionPresets(settings);
    const auto text = QString::fromStdString(name);
    if (const auto refused = userPresetNameIssue(text, presets))
        return uiResult(McpUiRefusal::InvalidArgument, *refused);
    presets.push_back(userPresetFrom(text.trimmed(), commands::compositionSettingsOf(*found)));
    persist(settings, presets);
    return uiOk(tr("Saved the preset “%1”.").arg(text.trimmed()));
}

mcp::McpUiResult McpLiveUiBinding::renameCompositionPreset(const std::string& name,
                                                           const std::string& newName) {
    QSettings settings;
    auto presets = loadUserCompositionPresets(settings);
    const auto index = userPresetNamed(presets, QString::fromStdString(name));
    if (!index)
        return uiResult(McpUiRefusal::UnknownTarget,
                        tr("No saved preset is called “%1”; composition.presets lists "
                           "them (built-in presets cannot be renamed).")
                            .arg(QString::fromStdString(name)));
    const auto text = QString::fromStdString(newName);
    if (const auto refused = userPresetNameIssue(text, presets, *index))
        return uiResult(McpUiRefusal::InvalidArgument, *refused);
    presets[*index].name = text.trimmed();
    persist(settings, presets);
    return uiOk(tr("Renamed the preset to “%1”.").arg(text.trimmed()));
}

mcp::McpUiResult McpLiveUiBinding::deleteCompositionPreset(const std::string& name) {
    QSettings settings;
    auto presets = loadUserCompositionPresets(settings);
    const auto index = userPresetNamed(presets, QString::fromStdString(name));
    if (!index)
        return uiResult(McpUiRefusal::UnknownTarget,
                        tr("No saved preset is called “%1”; composition.presets lists "
                           "them (built-in presets cannot be deleted).")
                            .arg(QString::fromStdString(name)));
    presets.erase(presets.begin() + static_cast<std::ptrdiff_t>(*index));
    persist(settings, presets);
    return uiOk(tr("Deleted the preset “%1”.").arg(QString::fromStdString(name)));
}

} // namespace bloom::ui
