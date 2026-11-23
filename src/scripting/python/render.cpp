#include "native.hpp"

#include <bloom/core/sha256.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <nanobind/stl/string.h>

#include <array>
#include <fstream>
#include <stdexcept>

namespace bloom::scripting::python {
namespace {

std::string digestFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    core::Sha256Hasher hasher;
    std::array<char, 65536> buffer{};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        if (!hasher.update(std::as_bytes(
                std::span(buffer.data(), static_cast<std::size_t>(input.gcount()))))) {
            throw std::runtime_error("Rendered file exceeds digest limits");
        }
    }
    if (!input.eof()) {
        throw std::runtime_error("Could not read rendered file");
    }
    const auto hex = hasher.finalize().toLowercaseHex();
    return "sha256:" + std::string(hex.data(), hex.size());
}

// bloom.render hands over {"sidecar": [format, ...], "embed": bool, "nodes": [id, ...] | None}
// (render.py normalizes and checks the user's dict); a malformed one is a ValueError here too.
std::optional<document::ExportSubtitles> subtitleSettings(const nb::object& given) {
    if (given.is_none())
        return std::nullopt;
    if (!nb::isinstance<nb::dict>(given))
        throw nb::value_error("subtitles is a dict");
    const auto dict = nb::cast<nb::dict>(given);
    document::ExportSubtitles parsed;
    if (dict.contains("sidecar"))
        for (const auto& item : nb::cast<nb::list>(dict["sidecar"])) {
            const auto format = document::parseExportSubtitleFormat(nb::cast<std::string>(item));
            if (!format)
                throw nb::value_error("subtitles sidecar formats are srt, vtt and ttml");
            parsed.sidecars.push_back(*format);
        }
    if (dict.contains("embed"))
        parsed.embed = nb::cast<bool>(dict["embed"]);
    if (dict.contains("nodes") && !dict["nodes"].is_none()) {
        std::vector<document::NodeId> nodes;
        for (const auto& item : nb::cast<nb::list>(dict["nodes"]))
            nodes.push_back(document::NodeId::fromRaw(nb::cast<std::uint64_t>(item)));
        parsed.nodes = std::move(nodes);
    }
    return parsed;
}

const char* diagnosticCode(const RenderDiagnosticCode code) {
    switch (code) {
    case RenderDiagnosticCode::None:
        return "";
    case RenderDiagnosticCode::MissingProcessingRoot:
        return "missing-processing-root";
    case RenderDiagnosticCode::Cancelled:
        return "cancelled";
    case RenderDiagnosticCode::DeviceRequired:
        return "device-required";
    case RenderDiagnosticCode::MediaSourceUnavailable:
        return "media-source-unavailable";
    }
    return "";
}

} // namespace

nb::dict NativeHost::render(const std::uint64_t composition,
                            const std::optional<std::uint64_t> first,
                            const std::optional<std::uint64_t> last, const bool range, const std::string& preset,
                            const std::string& destination, const std::uint64_t cancellationTask,
                            const bool replace, const bool requireDevice,
                            const nb::object& subtitles, const std::string& background) {
    // #760: "transparent" (the default) or "composition".
    const auto chosenBackground = document::parseExportBackground(background);
    if (!chosenBackground)
        throw nb::value_error("background is \"transparent\" or \"composition\"");
    output::OutputPresetV1 selected;
    if (preset == "PngRgba8SrgbV1") {
        selected = output::OutputPresetV1::PngRgba8SrgbV1;
    } else if (preset == "FlatExrRgba32fLinRec709SceneV1") {
        selected = output::OutputPresetV1::FlatExrRgba32fLinRec709SceneV1;
    } else {
        throw nb::value_error("Unknown render preset");
    }
    // #766: a range may leave an end open; the render then takes the composition's own end.
    if ((!range && !first) || (first && last && (*first > *last || *last - *first >= 10000)) ||
        destination.empty()) {
        throw nb::value_error("Invalid frame range or destination");
    }
    const auto subtitleSettingsToRender = subtitleSettings(subtitles);
    auto call = retirement_->acquire(); // Covers session-lock wait and every GIL-released use.
    auto& scheduler = call.scheduler();
    const auto access = lockSession();
    RenderResult result;
    std::vector<std::pair<std::string, std::string>> files;
    {
        const nb::gil_scoped_release release;
        runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
        RenderRequest request{
            .composition = document::CompositionId::fromRaw(composition),
            .frame = range ? std::nullopt : first,
            .range = range && first && last ? std::optional(std::pair(*first, *last)) : std::nullopt,
            .rangeFirst = range && !(first && last) ? first : std::nullopt,
            .rangeLast = range && !(first && last) ? last : std::nullopt,
            .preset = selected,
            .destination = destination,
            .cancelled =
                [this, cancellationTask, &call, &scheduler] {
                    if (call.closing() || cancellationRequested() || !session_->isValid())
                        return true;
                    if (cancellationTask == 0)
                        return false;
                    // The parent Python task's ticket runs until that task finishes, which is
                    // after this render returns. A cancelled ticket ends within one of its own
                    // polls, after which its snapshot no longer reports the request and later
                    // leaves the terminal history, so a ticket that is no longer running can only
                    // have been cancelled.
                    const auto task =
                        scheduler.snapshot(runtime::TaskId::fromRaw(cancellationTask));
                    return !task || runtime::isTerminal(task->state) || task->cancellationRequested;
                },
            .replaceExisting = replace,
            .requireDevice = requireDevice,
            .workerLimits = {},
            .loudness = std::nullopt,
            .encoder = document::ExportEncoderPreference::Automatic,
            .subtitles = subtitleSettingsToRender,
            .background = *chosenBackground};
        result =
            Render::run(*session_, scheduler, compiler, std::move(request), {}, gpuExportProvider_);
        if (result.succeeded) {
            if (range) {
                // An open end was resolved by the render: the frames published say where it ended.
                const auto firstFrame = first.value_or(0);
                const auto lastFrame = last.value_or(firstFrame + result.publishedFrames - 1);
                for (auto index = firstFrame; index <= lastFrame; ++index) {
                    const auto path =
                        host::FrameRangeRunnerV1::sequenceFramePath(destination, index, lastFrame);
                    files.emplace_back(path.string(), digestFile(path));
                }
            } else {
                files.emplace_back(destination, digestFile(destination));
            }
        }
    }
    nb::dict row;
    row["succeeded"] = result.succeeded;
    // #452: which device evaluated the frames, on success and failure alike.
    row["gpu_frames"] = result.gpuEvaluatedFrames;
    row["cpu_reference_frames"] = result.cpuReferenceFrames;
    if (!result.succeeded) {
        // bloom.render raises bloom.CancelledError for "cancelled" and RuntimeError otherwise.
        row["diagnostic"] = result.diagnostic;
        row["diagnostic_code"] = diagnosticCode(result.diagnosticCode);
        return row;
    }
    row["published_frames"] = result.publishedFrames;
    row["preservation_report"] = result.preservationReport;
    nb::list paths;
    for (const auto& [path, digest] : files) {
        nb::dict file;
        file["path"] = path;
        file["digest"] = digest;
        paths.append(file);
    }
    // Each sidecar is a published file too.
    nb::list tracks;
    for (const auto& track : result.subtitles) {
        nb::dict item;
        item["node"] = track.node.value();
        item["language"] = track.language;
        item["title"] = track.title;
        item["format"] = track.sidecar
                             ? std::string(document::exportSubtitleFormatToken(*track.sidecar))
                             : std::string("embedded");
        item["path"] = track.path.string();
        item["cues"] = track.cues;
        item["bytes"] = track.bytes;
        std::string sha256;
        if (track.digest) {
            const auto hex = track.digest->toLowercaseHex();
            sha256 = "sha256:" + std::string(hex.data(), hex.size());
            nb::dict file;
            file["path"] = track.path.string();
            file["digest"] = sha256;
            paths.append(file);
        }
        item["sha256"] = sha256;
        tracks.append(item);
    }
    row["files"] = paths;
    row["subtitles"] = tracks;
    // #761: a movie's or WAV's streams as the verifying reopen read them from the file.
    nb::list streams;
    for (const auto& stream : result.streams) {
        nb::dict item;
        const bool video = stream.kind == host::ExportStreamKind::Video;
        item["kind"] = video ? "video" : "audio";
        item["codec"] = stream.codec;
        item["profile"] = stream.profile;
        if (video) {
            item["width"] = stream.width;
            item["height"] = stream.height;
            nb::list rate;
            rate.append(stream.frameRate.numerator);
            rate.append(stream.frameRate.denominator);
            item["frame_rate"] = rate;
        } else {
            item["channels"] = stream.channels;
            item["sample_rate"] = stream.sampleRate;
        }
        item["bitrate"] = stream.bitrate;
        streams.append(item);
    }
    row["streams"] = streams;
    return row;
}

} // namespace bloom::scripting::python
