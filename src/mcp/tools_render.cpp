// render, export and capture (ADR 0034, section 5). The stdio host keeps the scripting render path
// (scripting::Render::run and the sequence runner) with its byte parity; the live host submits
// transient export queue jobs (tools_export.cpp).
#include "core_internal.hpp"
#include "frame_statistics.hpp"

#include <bloom/core/file_uri.hpp>
#include <bloom/core/frame_time_mapping.hpp>
#include <bloom/host/export_presets.hpp>
#include <bloom/scripting/render.hpp>

#include <algorithm>
#include <bloom/core/sha256.hpp>
#include <bloom/host/gpu_export_provider.hpp>
#include <bloom/host/sequence_export_runner.hpp>
#include <bloom/output/frame_readback.hpp>
#include <fstream>

#include <array>
#include <chrono>
#include <fstream>
#include <thread>

namespace bloom::mcp {

std::string digestFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    core::Sha256Hasher hasher;
    std::array<char, 65536> bytes{};
    while (input) {
        input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!hasher.update(
                std::as_bytes(std::span(bytes.data(), static_cast<std::size_t>(input.gcount())))))
            throw std::runtime_error("File exceeds SHA-256 limits");
    }
    if (!input.eof())
        throw std::runtime_error("Cannot read published file");
    const auto hex = hasher.finalize().toLowercaseHex();
    return "sha256:" + std::string(hex.data(), hex.size());
}

namespace {

// The render and export destination contract (#388): a file path with the preset's extension, a
// folder (the file is then frame.<ext>, or render.<ext> for media), or a sequence pattern with
// one #### or {frame} where the frame number goes. A path without an extension gets the preset's.
struct RenderDestination final {
    std::filesystem::path path;
    // A range's file name pattern for the export queue's naming (#### or {frame}); absent means
    // the default <base>.NNNN.<ext>.
    std::optional<std::string> pattern;
};

std::string_view presetExtension(const std::string_view preset) {
    if (preset == "PngRgba8SrgbV1")
        return "png";
    if (preset == "FlatExrRgba32fLinRec709SceneV1")
        return "exr";
    if (preset == "TiffRgba16SrgbV1")
        return "tiff";
    if (preset == "ProResMovV1")
        return "mov";
    if (preset == "DnxhrMxfV1")
        return "mxf";
    return "wav";
}

std::string lowered(std::string text) {
    for (auto& c : text)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    return text;
}

RenderDestination renderDestination(const std::string& text, const std::string_view preset,
                                    const bool range, const std::uint64_t frame, const bool still,
                                    const bool patternsSupported) {
    const auto extension = std::string(presetExtension(preset));
    std::filesystem::path path(text);
    std::error_code error;
    const bool folder =
        text.ends_with('/') || text.ends_with('\\') || std::filesystem::is_directory(path, error);
    if (folder)
        return {path / ((still ? "frame." : "render.") + extension), std::nullopt};
    auto name = path.filename().string();
    if (path.extension().empty() && name.find("####") == std::string::npos &&
        name.find("{frame}") == std::string::npos) {
        name += "." + extension;
        path.replace_filename(name);
    }
    const auto actual = lowered(path.extension().string());
    if (actual != "." + extension && !(extension == "tiff" && actual == ".tif"))
        throw InvalidInput("destination must end in ." + extension + " for " + std::string(preset) +
                           ", or name an existing folder");
    const auto hashes = name.find("####");
    const auto braces = name.find("{frame}");
    const auto tokens =
        (hashes != std::string::npos ? 1 : 0) + (braces != std::string::npos ? 1 : 0) +
        (hashes != std::string::npos && name.find("####", hashes + 4) != std::string::npos ? 1 : 0);
    if (name.find('#') != std::string::npos && hashes == std::string::npos)
        throw InvalidInput("a frame number pattern is exactly four # characters (####) or {frame}");
    if (tokens == 0)
        return {path, std::nullopt};
    if (tokens > 1 || !still)
        throw InvalidInput(still ? "a destination has at most one #### or {frame}"
                                 : "media destinations are single files; remove #### or {frame}");
    const auto at = hashes != std::string::npos ? hashes : braces;
    const auto width = hashes != std::string::npos ? std::size_t{4} : std::size_t{7};
    if (!range) {
        auto number = std::to_string(frame);
        if (number.size() < 4)
            number.insert(0, 4 - number.size(), '0');
        name.replace(at, width, number);
        path.replace_filename(name);
        return {path, std::nullopt};
    }
    // "seq.####.png" is the default naming of "seq.png"; anything else is a naming pattern.
    const auto before = name.substr(0, at);
    const auto after = name.substr(at + width);
    if (before.ends_with('.') && before.size() > 1 && after == path.extension().string()) {
        path.replace_filename(before.substr(0, before.size() - 1) + after);
        return {path, std::nullopt};
    }
    if (!patternsSupported)
        throw InvalidInput("bloom-mcp names range files <base>.####.<ext>; use a destination "
                           "such as seq.####." +
                           extension);
    path.replace_filename("frame" + path.extension().string());
    auto pattern = name;
    pattern.replace(at, width, "####");
    return {path, pattern};
}

// A projectFile render says which route rendered it, in the vocabulary the other provenance
// fields use (gpuFrames, cpuReferenceFrames, "gpu", "cpu-reference"): a file render never turns
// into a silent CPU render. Frames the provider did not evaluate on the GPU ran the CPU reference
// path, whether no device was available or no provider was given.
void reportFileRoute(Json& out, yyjson_mut_val* value, const McpCall& call,
                     const std::uint64_t frames, const std::uint64_t gpuFrames,
                     const bool withProvenance = true) {
    if (!call.fileSession)
        return;
    const auto gpu = std::min(gpuFrames, frames);
    const auto cpu = frames - gpu;
    out.set(value, "gpuFrames", out.number(gpu));
    out.set(value, "cpuReferenceFrames", out.number(cpu));
    if (withProvenance)
        out.set(value, "provenance", out.text(cpu == 0 && gpu > 0 ? "gpu" : "cpu-reference"));
    if (cpu > 0)
        out.set(value, "routeNote",
                out.text(std::to_string(cpu) + " of " + std::to_string(frames) +
                         " frames rendered on the CPU reference path: no GPU device was "
                         "available to the project file's render, or the scene could not run "
                         "on it."));
}

yyjson_mut_val* renderHeadless(McpServerCore::State& core, Json& out, McpCall& call,
                               yyjson_val* arguments, const bool compositionExport) {
    auto& scheduler = core.dependencies.scheduler;
    auto& session = call.session;
    const auto& gpuExportProvider = call.gpuProvider;
    const auto& stop = call.control.stop;
    members(arguments,
            {"composition", "frame", "first", "last", "preset", "destination", "profile", "audio",
             "sampleRate", "overwrite"},
            {"composition", "preset", "destination"});
    const auto id = integer(member(arguments, "composition"));
    const auto presetName = string(member(arguments, "preset"), 128);
    const auto requested = string(member(arguments, "destination"));
    const bool range = member(arguments, "frame") == nullptr;
    if (requested.empty() || (!range && (member(arguments, "first") || member(arguments, "last"))))
        throw InvalidInput(
            "Specify exactly a frame or inclusive first/last range and a destination");
    const auto first = integer(member(arguments, range ? "first" : "frame"));
    const auto last = range ? integer(member(arguments, "last")) : first;
    if (first > last || last - first >= 10000)
        throw InvalidInput("Range must contain 1..10000 frames");
    const bool stillPreset = presetName == "PngRgba8SrgbV1" ||
                             presetName == "FlatExrRgba32fLinRec709SceneV1" ||
                             presetName == "TiffRgba16SrgbV1";
    const auto destination =
        renderDestination(requested, presetName, range, first, stillPreset, false).path;
    std::optional<output::OutputPresetV1> preset;
    constexpr std::array presets{output::OutputPresetV1::PngRgba8SrgbV1,
                                 output::OutputPresetV1::FlatExrRgba32fLinRec709SceneV1,
                                 output::OutputPresetV1::TiffRgba16SrgbV1,
                                 output::OutputPresetV1::ProResMovV1,
                                 output::OutputPresetV1::DnxhrMxfV1,
                                 output::OutputPresetV1::PcmWavV1};
    for (const auto candidate : presets) {
        const auto identity = output::outputPresetIdentityV1(candidate);
        if (identity && identity->serializedId == presetName)
            preset = candidate;
    }
    if (!preset)
        throw InvalidInput("Unknown preset");
    const bool still = *preset == presets[0] || *preset == presets[1] || *preset == presets[2];
    if (!compositionExport && !still)
        throw InvalidInput("Use export for media presets and encoding settings");
    if (still && (member(arguments, "profile") || member(arguments, "audio") ||
                  member(arguments, "sampleRate")))
        throw InvalidInput("Still presets do not accept media encoding settings");
    // #453: nothing is replaced unless overwrite is "replace". Headless has no confirmation, so
    // "ask" (the default) refuses like "refuse". Every file the render would write is checked: each
    // frame of an image range, or the one file; the publications enforce it again.
    auto* overwrite = member(arguments, "overwrite");
    const auto policy = overwrite ? string(overwrite, 16) : std::string("ask");
    if (policy != "ask" && policy != "replace" && policy != "refuse")
        throw InvalidInput("overwrite is ask, replace or refuse");
    const bool replace = policy == "replace";
    if (!replace) {
        const auto refuseExisting = [](const std::filesystem::path& path) {
            std::error_code error;
            if (std::filesystem::exists(path, error))
                throw ToolFailure(McpToolErrorCode::PreflightBlocked,
                                  path.filename().string() +
                                      " exists. Pass overwrite=replace to replace it.");
        };
        if (still && range)
            for (auto index = first;; ++index) {
                refuseExisting(
                    host::FrameRangeRunnerV1::sequenceFramePath(destination, index, last));
                if (index == last)
                    break;
            }
        else
            refuseExisting(destination);
    }
    const auto source = session.renderSourceSnapshot();
    const auto& snapshot = source.snapshot;
    const auto* composition =
        snapshot.project().findComposition(document::CompositionId::fromRaw(id));
    if (!composition)
        throw InvalidInput("Composition does not exist");
    const auto colorIntent = scripting::captureRenderColorIntent(source, *composition);
    if (!colorIntent)
        throw std::runtime_error("Project color settings are unavailable for export");
    runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
    std::uint64_t frames = 0;
    std::uint64_t gpuFrames = 0;
    std::string report;
    if (still) {
        const auto result = scripting::Render::run(
            session, scheduler, compiler,
            {.composition = composition->id(),
             .frame = range ? std::nullopt : std::optional(first),
             .range = range ? std::optional(std::pair(first, last)) : std::nullopt,
             .preset = *preset,
             .destination = destination,
             .cancelled = [stop] { return stop.stop_requested(); },
             .replaceExisting = replace},
            {}, gpuExportProvider);
        if (!result.succeeded)
            throw std::runtime_error(result.diagnostic);
        frames = result.publishedFrames;
        gpuFrames = result.gpuEvaluatedFrames;
        report = result.preservationReport;
    } else {
        const auto profile = member(arguments, "profile") ? string(member(arguments, "profile"), 32)
                             : *preset == output::OutputPresetV1::PcmWavV1 ? "pcm_s16le"
                                                                           : "hq";
        const auto rate =
            member(arguments, "sampleRate") ? integer(member(arguments, "sampleRate")) : 48000;
        if (rate < 8000 || rate > 192000)
            throw InvalidInput("Audio sample rate is outside 8000..192000");
        const auto retirement = host::SequenceExportRetirementOwnerV1::create();
        output::ExportResourceLedgerV1 ledger;
        // Headless calls may leave this scope through an exception. Drain after the runner has
        // released its ticket but before the borrowed ledger and compiler are destroyed.
        struct RetirementDrain final {
            std::shared_ptr<host::SequenceExportRetirementOwnerV1> owner;
            ~RetirementDrain() {
                owner->beginShutdown();
                while (!owner->retirementComplete())
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                while (!owner->collect())
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        } drain{retirement};
        // The server-lifetime GPU final-render provider is reused here; the sequence runner copies
        // it into every per-frame attempt and defers the first evaluation until its bootstrap is
        // terminal, so the first frame is genuinely GPU when a device exists. The unchanged CPU
        // reference path is the fallback otherwise.
        host::SequenceExportRunnerV1 runner(
            scheduler, compiler, *session.publicationCoordinator(), *session.artifactCoordinator(),
            ledger,
            {.composition = {snapshot, composition->id()},
             .range = {.destination = destination,
                       .firstFrame = first,
                       .lastFrame = last,
                       .frameRate = composition->format().frameRate(),
                       .duration = composition->duration()},
             .preset = *preset,
             .profile = profile,
             .audio = member(arguments, "audio") ? boolean(member(arguments, "audio")) : true,
             .sampleRate = static_cast<std::uint32_t>(rate),
             .pcmCodec = *preset == output::OutputPresetV1::PcmWavV1 ? profile : "pcm_s16le",
             .bwfDescription = {},
             .assetBaseDirectory = source.assetBaseDirectory,
             .worker = {},
             .workingColorSpaceId = std::string(colorIntent->workingColorSpaceId),
             .ocioConfigRevision = colorIntent->ocioConfigRevision,
             .ocioConfigUri = std::string(colorIntent->ocioConfigUri),
             .gpuProvider = gpuExportProvider,
             .overwritePolicy = replace ? platform::ArtifactOverwritePolicy::CreateOrReplace
                                        : platform::ArtifactOverwritePolicy::CreateOnly},
            *retirement);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(10);
        while (!runner.result()) {
            runner.poll();
            if (stop.stop_requested() || std::chrono::steady_clock::now() > deadline)
                runner.cancel();
            if (runner.stage() == host::SequenceExportStageV1::AwaitingApproval) {
                const auto* analysis = runner.analysis();
                const auto digest = runner.frameApprovalDigest();
                if (!analysis || !digest || !runner.approve(analysis->digest, *digest))
                    runner.cancel();
                else
                    report = analysis->implementationNote;
            }
            if (!runner.result())
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (!runner.result()->published()) {
            if (runner.result()->failureStage == host::SequenceExportFailureStageV1::Destination)
                throw ToolFailure(McpToolErrorCode::PreflightBlocked,
                                  destination.filename().string() +
                                      " exists or changed. Pass overwrite=replace to replace it.");
            throw std::runtime_error(runner.result()->failure ? runner.result()->failure->detail
                                                              : "Export was not published");
        }
        frames = runner.result()->encodedFrames;
        gpuFrames = runner.result()->gpuEvaluatedFrames;
    }
    auto* result = out.object();
    out.set(result, "publishedFrames", out.number(frames));
    reportFileRoute(out, result, call, frames, gpuFrames);
    out.set(result, "preservationReport", out.text(report));
    auto* files = out.array();
    const auto add = [&](const std::filesystem::path& path) {
        auto* file = out.object();
        out.set(file, "path", out.text(path.string()));
        out.set(file, "digest", out.text(digestFile(path)));
        out.append(files, file);
    };
    if (still && range) {
        for (auto index = first;; ++index) {
            add(host::FrameRangeRunnerV1::sequenceFramePath(destination, index, last));
            if (index == last)
                break;
        }
    } else
        add(destination);
    out.set(result, "files", files);
    return result;
}

// The live host: the same request shape as a transient export queue job (ADR 0033 section 13).
yyjson_mut_val* renderLive(McpServerCore::State& core, Json& out, McpCall& call,
                           yyjson_val* arguments, const bool compositionExport) {
    members(arguments,
            {"composition", "frame", "first", "last", "preset", "destination", "profile", "audio",
             "sampleRate", "overwrite"},
            {"composition", "preset", "destination"});
    const auto id = integer(member(arguments, "composition"));
    const auto presetName = string(member(arguments, "preset"), 128);
    const auto requested = string(member(arguments, "destination"));
    const bool range = member(arguments, "frame") == nullptr;
    if (requested.empty() || (!range && (member(arguments, "first") || member(arguments, "last"))))
        throw InvalidInput(
            "Specify exactly a frame or inclusive first/last range and a destination");
    const auto first = integer(member(arguments, range ? "first" : "frame"));
    const auto last = range ? integer(member(arguments, "last")) : first;
    if (first > last || last - first >= core.dependencies.limits.renderFrames)
        throw InvalidInput("Range must contain 1..10000 frames");
    struct Mapping {
        std::string_view preset;
        std::string_view builtIn;
        bool still;
    };
    constexpr std::array<Mapping, 6> mappings{{
        {"PngRgba8SrgbV1", "builtin:png-sequence", true},
        {"FlatExrRgba32fLinRec709SceneV1", "builtin:exr-sequence", true},
        {"TiffRgba16SrgbV1", "builtin:tiff-sequence", true},
        {"ProResMovV1", "builtin:prores-422hq-mov", false},
        {"DnxhrMxfV1", "builtin:dnxhr-hq-mxf", false},
        {"PcmWavV1", "builtin:wav", false},
    }};
    const auto found = std::ranges::find(mappings, presetName, &Mapping::preset);
    if (found == mappings.end())
        throw InvalidInput("Unknown preset");
    if (!compositionExport && !found->still)
        throw InvalidInput("Use export for media presets and encoding settings");
    if (found->still && (member(arguments, "profile") || member(arguments, "audio") ||
                         member(arguments, "sampleRate")))
        throw InvalidInput("Still presets do not accept media encoding settings");
    auto preset = host::builtInExportPreset(found->builtIn);
    if (!preset)
        throw std::runtime_error("The export preset is not available");
    host::ExportTransientJobRequest request;
    request.composition = document::CompositionId::fromRaw(id);
    request.range = range ? document::ExportRange{document::ExportRangeKind::Custom, first, last}
                          : document::ExportRange{document::ExportRangeKind::Frame, first, first};
    request.settings = preset->settings;
    request.presetId = preset->id;
    request.name = "MCP " + std::string(compositionExport ? "export" : "render");
    if (auto* profile = member(arguments, "profile")) {
        auto text = string(profile, 32);
        if (preset->settings.format == document::ExportFormat::Wav)
            text = text == "pcm_s16le" ? "s16" : text == "pcm_s24le" ? "s24" : text;
        request.settings.codecProfile = text;
    }
    if (auto* audio = member(arguments, "audio"))
        request.settings.includeAudio = boolean(audio);
    if (auto* rate = member(arguments, "sampleRate")) {
        const auto value = integer(rate);
        if (value < 8000 || value > 192000)
            throw InvalidInput("Audio sample rate is outside 8000..192000");
        request.settings.audioSampleRate = static_cast<std::uint32_t>(value);
    }
    const auto destination =
        renderDestination(requested, presetName, range, first, found->still, true);
    if (destination.pattern)
        request.settings.naming.pattern = *destination.pattern;
    request.destination =
        transientDestination(destination.path.string(), member(arguments, "overwrite"));
    const auto result = runTransientJob(core, call, std::move(request));
    auto* value = out.object();
    out.set(value, "publishedFrames", out.number(result.frames));
    out.set(value, "preservationReport", out.text(result.report));
    out.set(value, "job", out.text(std::to_string(result.job)));
    out.set(value, "log", logEntries(out, result.log));
    auto* files = out.array();
    for (const auto& path : result.files) {
        auto* file = out.object();
        out.set(file, "path", out.text(path.string()));
        out.set(file, "digest", out.text(digestFile(path)));
        out.append(files, file);
    }
    out.set(value, "files", files);
    return value;
}

// Keeps the newest kMcpRetainedCaptures captures (names sort by time).
void pruneCaptures(const std::filesystem::path& folder) {
    std::error_code error;
    std::vector<std::filesystem::path> captures;
    for (const auto& entry : std::filesystem::directory_iterator(folder, error)) {
        const auto name = entry.path().filename().string();
        if (name.starts_with("capture-") && entry.path().extension() == ".png")
            captures.push_back(entry.path());
    }
    std::ranges::sort(captures);
    while (captures.size() > kMcpRetainedCaptures) {
        std::filesystem::remove(captures.front(), error);
        captures.erase(captures.begin());
    }
}

std::filesystem::path nextCapturePath(McpServerCore::State& core) {
    const auto& folder = core.dependencies.captureDirectory;
    if (folder.empty())
        throw ToolFailure(McpToolErrorCode::ExportUnavailable,
                          "Bloom can't find a folder for captures.");
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    std::uint64_t counter = 0;
    {
        const std::lock_guard lock(core.captureMutex);
        counter = ++core.captureCounter;
    }
    // Fixed-width fields so lexical order is chronological order.
    auto stamp = std::to_string(millis);
    stamp.insert(0, stamp.size() < 16 ? 16 - stamp.size() : 0, '0');
    auto sequence = std::to_string(counter % 1000000U);
    sequence.insert(0, 6 - sequence.size(), '0');
    return folder / ("capture-" + stamp + "-" + sequence + ".png");
}

} // namespace

yyjson_mut_val* renderTool(McpServerCore::State& core, Json& out, McpCall& call,
                           yyjson_val* arguments, const bool compositionExport) {
    return call.live != nullptr ? renderLive(core, out, call, arguments, compositionExport)
                                : renderHeadless(core, out, call, arguments, compositionExport);
}

namespace {

yyjson_mut_val* channelJson(Json& out, const ChannelStatistics& channel) {
    auto* value = out.object();
    out.set(value, "mean", out.real(channel.mean));
    out.set(value, "min", out.real(channel.min));
    out.set(value, "max", out.real(channel.max));
    auto* histogram = out.array();
    for (const auto count : channel.histogram)
        out.append(histogram, out.number(count));
    out.set(value, "histogram", histogram);
    out.set(value, "below", out.number(channel.below));
    out.set(value, "above", out.number(channel.above));
    return value;
}

yyjson_mut_val* statisticsJson(Json& out, const FrameStatistics& statistics) {
    auto* value = out.object();
    out.set(value, "pixels", out.number(statistics.pixels));
    out.set(value, "luma", channelJson(out, statistics.luma));
    out.set(value, "red", channelJson(out, statistics.red));
    out.set(value, "green", channelJson(out, statistics.green));
    out.set(value, "blue", channelJson(out, statistics.blue));
    out.set(value, "alphaMean", out.real(statistics.alphaMean));
    return value;
}

// Renders the frames to a scratch Flat OpenEXR sequence through the production still route (the
// working colour space, written without a display transform), reads them back and measures them.
yyjson_mut_val* statisticsCapture(McpServerCore::State& core, Json& out, McpCall& call,
                                  yyjson_val* arguments, const document::Composition& composition,
                                  const std::uint64_t first) {
    const auto last = member(arguments, "last") ? integer(member(arguments, "last")) : first;
    if (first > last || last - first >= core.dependencies.limits.renderFrames)
        throw InvalidInput("frame..last must contain 1..10000 frames");
    const auto bins = member(arguments, "bins") ? integer(member(arguments, "bins")) : 32U;
    if (bins < 2 || bins > 256)
        throw InvalidInput("bins is 2..256");
    const auto width = composition.format().width();
    const auto height = composition.format().height();
    std::uint64_t regionX = 0;
    std::uint64_t regionY = 0;
    std::uint64_t regionWidth = width;
    std::uint64_t regionHeight = height;
    if (auto* region = member(arguments, "region")) {
        if (!yyjson_is_arr(region) || yyjson_arr_size(region) != 4)
            throw InvalidInput("region is [x, y, width, height] in pixels from the top-left");
        std::array<std::uint64_t, 4> values{};
        for (std::size_t index = 0; index < 4; ++index)
            values[index] = integer(yyjson_arr_get(region, index));
        regionX = values[0];
        regionY = values[1];
        regionWidth = values[2];
        regionHeight = values[3];
        if (regionWidth == 0 || regionHeight == 0 || regionX + regionWidth > width ||
            regionY + regionHeight > height)
            throw InvalidInput("region must be inside the " + std::to_string(width) + " x " +
                               std::to_string(height) + " frame and not empty");
    }
    const auto source = call.session.renderSourceSnapshot();
    const auto colorIntent = scripting::captureRenderColorIntent(source, composition);
    if (!colorIntent)
        throw std::runtime_error("Project color settings are unavailable");

    const auto scratch = nextCapturePath(core).replace_extension("");
    struct Scratch final {
        std::filesystem::path folder;
        ~Scratch() {
            std::error_code error;
            std::filesystem::remove_all(folder, error);
        }
    } cleanup{scratch};
    std::error_code error;
    std::filesystem::create_directories(scratch, error);
    const auto destination = scratch / "stats.exr";
    const bool range = last != first;
    std::optional<TransientResult> transient;
    std::string provenance = "cpu-reference";
    std::uint64_t routeFrames = 0;
    std::uint64_t routeGpuFrames = 0;
    if (call.live != nullptr) {
        auto preset = host::builtInExportPreset("builtin:exr-sequence");
        if (!preset)
            throw std::runtime_error("The OpenEXR preset is not available");
        host::ExportTransientJobRequest request;
        request.composition = composition.id();
        request.range = range ? document::ExportRange{document::ExportRangeKind::Custom, first, last}
                              : document::ExportRange{document::ExportRangeKind::Frame, first, first};
        request.settings = preset->settings;
        request.presetId = preset->id;
        request.name = "MCP capture statistics";
        request.destination = transientDestination(destination.string(), nullptr);
        transient = runTransientJob(core, call, std::move(request));
        provenance = "export-queue";
    } else {
        const runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
        const auto& stop = call.control.stop;
        const auto result = scripting::Render::run(
            call.session, core.dependencies.scheduler, compiler,
            {.composition = composition.id(),
             .frame = range ? std::nullopt : std::optional(first),
             .range = range ? std::optional(std::pair(first, last)) : std::nullopt,
             .preset = output::OutputPresetV1::FlatExrRgba32fLinRec709SceneV1,
             .destination = destination,
             .cancelled = [stop] { return stop.stop_requested(); }},
            {}, call.gpuProvider);
        if (!result.succeeded)
            throw std::runtime_error(result.diagnostic);
        routeFrames = result.publishedFrames;
        routeGpuFrames = result.gpuEvaluatedFrames;
        if (call.fileSession && routeGpuFrames >= routeFrames && routeGpuFrames > 0)
            provenance = "gpu";
    }
    StatisticsAccumulator all(bins);
    auto* frames = out.array();
    for (auto index = first;; ++index) {
        const auto path = range ? host::FrameRangeRunnerV1::sequenceFramePath(destination, index, last)
                                : destination;
        StatisticsAccumulator one(bins);
        const auto readback = output::visitExportedFrame(
            path, {regionX, regionY, regionWidth, regionHeight},
            [&](const float red, const float green, const float blue, const float alpha) {
                one.addPixel(red, green, blue, alpha);
                all.addPixel(red, green, blue, alpha);
            });
        if (!readback.succeeded)
            throw std::runtime_error("Frame " + std::to_string(index) +
                                     " could not be read back: " + readback.diagnostic);
        if (range) {
            const auto result = one.result();
            auto* row = out.object();
            out.set(row, "frame", out.number(index));
            out.set(row, "lumaMean", out.real(result.luma.mean));
            out.set(row, "lumaMin", out.real(result.luma.min));
            out.set(row, "lumaMax", out.real(result.luma.max));
            out.set(row, "redMean", out.real(result.red.mean));
            out.set(row, "greenMean", out.real(result.green.mean));
            out.set(row, "blueMean", out.real(result.blue.mean));
            out.append(frames, row);
        }
        if (index == last)
            break;
    }
    call.summary = "statistics of composition " + std::to_string(composition.id().value()) +
                   " frames " + std::to_string(first) + ".." + std::to_string(last);
    auto* value = out.object();
    out.set(value, "composition", out.number(composition.id().value()));
    out.set(value, "first", out.number(first));
    out.set(value, "last", out.number(last));
    auto* regionJson = out.array();
    for (const auto item : {regionX, regionY, regionWidth, regionHeight})
        out.append(regionJson, out.number(item));
    out.set(value, "region", regionJson);
    out.set(value, "colorSpace", out.text(std::string(colorIntent->workingColorSpaceId)));
    out.set(value, "luma", out.text("Rec.709 weights (0.2126, 0.7152, 0.0722) of the working-space "
                                    "red, green and blue"));
    out.set(value, "route", out.text("production still export to a scratch OpenEXR, working space"));
    out.set(value, "provenance", out.text(provenance));
    reportFileRoute(out, value, call, routeFrames, routeGpuFrames, false);
    out.set(value, "statistics", statisticsJson(out, all.result()));
    if (range)
        out.set(value, "frames", frames);
    if (transient) {
        out.set(value, "job", out.text(std::to_string(transient->job)));
        out.set(value, "log", logEntries(out, transient->log));
    }
    return value;
}

} // namespace

yyjson_mut_val* captureTool(McpServerCore::State& core, Json& out, McpCall& call,
                            yyjson_val* arguments) {
    if (arguments != nullptr)
        members(arguments, {"composition", "frame", "statistics", "last", "region", "bins"});
    const bool statistics = arguments && member(arguments, "statistics") &&
                            boolean(member(arguments, "statistics"));
    if (!statistics && arguments &&
        (member(arguments, "last") || member(arguments, "region") || member(arguments, "bins")))
        throw InvalidInput("last, region and bins need statistics=true");
    const auto snapshot = call.session.snapshot();
    const auto compositionId = arguments && member(arguments, "composition")
                                   ? integer(member(arguments, "composition"))
                                   : call.context.composition.value_or(0);
    const auto* composition =
        snapshot.project().findComposition(document::CompositionId::fromRaw(compositionId));
    if (composition == nullptr)
        throw InvalidInput("Composition does not exist");
    std::uint64_t frame = 0;
    if (arguments && member(arguments, "frame")) {
        frame = integer(member(arguments, "frame"));
    } else if (call.live != nullptr && call.context.time) {
        const auto rate = composition->format().frameRate();
        const auto mapping = core::FrameTimeMapping::create(composition->duration(),
                                                            rate.numerator(), rate.denominator());
        if (mapping)
            frame = mapping.value()->nearestFrameIndex(*call.context.time);
    }
    if (statistics)
        return statisticsCapture(core, out, call, arguments, *composition, frame);
    const auto path = nextCapturePath(core);
    // The live capture's transient job (job id and run log, as render and export carry).
    std::optional<TransientResult> transient;
    std::uint64_t routeFrames = 0;
    std::uint64_t routeGpuFrames = 0;
    call.summary =
        "composition " + std::to_string(compositionId) + " frame " + std::to_string(frame);
    if (call.live != nullptr) {
        auto preset = host::builtInExportPreset("builtin:png-sequence");
        if (!preset)
            throw std::runtime_error("The PNG preset is not available");
        host::ExportTransientJobRequest request;
        request.composition = composition->id();
        request.range = {document::ExportRangeKind::Frame, frame, frame};
        request.settings = preset->settings;
        // The Viewer's display and view; empty keeps the configuration's defaults.
        request.settings.displayName = call.live->viewerDisplay;
        request.settings.viewName = call.live->viewerView;
        request.presetId = preset->id;
        request.name = "MCP capture";
        request.destination = transientDestination(path.string(), nullptr);
        transient = runTransientJob(core, call, std::move(request));
    } else {
        const runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
        const auto& stop = call.control.stop;
        const auto result =
            scripting::Render::run(call.session, core.dependencies.scheduler, compiler,
                                   {.composition = composition->id(),
                                    .frame = frame,
                                    .range = std::nullopt,
                                    .preset = output::OutputPresetV1::PngRgba8SrgbV1,
                                    .destination = path,
                                    .cancelled = [stop] { return stop.stop_requested(); }},
                                   {}, call.gpuProvider);
        if (!result.succeeded)
            throw std::runtime_error(result.diagnostic);
        routeFrames = result.publishedFrames;
        routeGpuFrames = result.gpuEvaluatedFrames;
    }
    pruneCaptures(path.parent_path());
    std::error_code error;
    const auto bytes = std::filesystem::file_size(path, error);
    if (error)
        throw std::runtime_error("The capture was not published");
    auto* value = out.object();
    out.set(value, "composition", out.number(compositionId));
    out.set(value, "frame", out.number(frame));
    out.set(value, "path", out.text(path.string()));
    out.set(value, "bytes", out.number(bytes));
    out.set(value, "digest", out.text(digestFile(path)));
    reportFileRoute(out, value, call, routeFrames, routeGpuFrames);
    out.set(value, "width", out.number(composition->format().width()));
    out.set(value, "height", out.number(composition->format().height()));
    if (transient) {
        out.set(value, "job", out.text(std::to_string(transient->job)));
        out.set(value, "log", logEntries(out, transient->log));
    }
    const auto uri = core::fileUriFromAbsolutePath(std::filesystem::absolute(path));
    if (bytes <= kMcpInlineImageBytes) {
        std::ifstream input(path, std::ios::binary);
        std::vector<char> data(static_cast<std::size_t>(bytes));
        input.read(data.data(), static_cast<std::streamsize>(data.size()));
        if (!input)
            throw std::runtime_error("The capture could not be read");
        auto* image = out.object();
        out.set(image, "type", out.text("image"));
        out.set(image, "data", out.text(base64(std::as_bytes(std::span(data)))));
        out.set(image, "mimeType", out.text("image/png"));
        call.content.push_back(image);
        out.set(value, "inline", out.boolean(true));
    } else {
        auto* link = out.object();
        out.set(link, "type", out.text("resource_link"));
        out.set(link, "uri", out.text(uri.value_or(path.string())));
        out.set(link, "name", out.text(path.filename().string()));
        out.set(link, "mimeType", out.text("image/png"));
        out.set(link, "size", out.number(bytes));
        call.content.push_back(link);
        out.set(value, "inline", out.boolean(false));
        auto* diagnostic = out.object();
        out.set(diagnostic, "code",
                out.text(toolErrorCode(McpToolErrorCode::CaptureTooLargeForInline)));
        out.set(diagnostic, "message",
                out.text("The capture is too large to embed; it is linked instead."));
        out.set(value, "diagnostic", diagnostic);
        call.notice = McpToolErrorCode::CaptureTooLargeForInline;
    }
    return value;
}

} // namespace bloom::mcp
