#include "native.hpp"

#include <bloom/core/sha256.hpp>
#include <bloom/scripting/scope_read.hpp>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>

#include <array>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>

// #724 Scopes, section 8b: bloom.scopes.read and bloom.scopes.image. The measure is
// scripting/scope_read.hpp's, the one query kind=scopes calls; this file turns the Python
// arguments into a request and the result into dicts (snake_case keys, the MCP answer's members).
namespace bloom::scripting::python {
namespace {

template <typename Value> Value required(const nb::dict& options, const char* key) {
    if (!options.contains(key))
        throw nb::type_error((std::string("scopes: missing option ") + key).c_str());
    return nb::cast<Value>(options[key]);
}

[[nodiscard]] std::string digestOf(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    core::Sha256Hasher hasher;
    std::array<char, 65536> buffer{};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        if (!hasher.update(
                std::as_bytes(std::span(buffer.data(), static_cast<std::size_t>(input.gcount())))))
            throw std::runtime_error("The picture exceeds the digest limits");
    }
    const auto hex = hasher.finalize().toLowercaseHex();
    return "sha256:" + std::string(hex.data(), hex.size());
}

[[nodiscard]] nb::list numbers(const std::vector<std::uint32_t>& values) {
    nb::list list;
    for (const auto value : values)
        list.append(value);
    return list;
}

[[nodiscard]] nb::list pair(const std::array<double, 2>& value) {
    nb::list list;
    list.append(value[0]);
    list.append(value[1]);
    return list;
}

constexpr std::array<const char*, 4> kChannelNames{"red", "green", "blue", "luma"};
constexpr std::array<const char*, 3> kResultNames{"tables", "summary", "image"};
constexpr std::array<const char*, 2> kEncodingNames{"dense", "sparse"};

[[nodiscard]] nb::dict tableDict(const ScopeTableData& table, const ScopeReadEncoding encoding,
                                 const bool clips) {
    nb::dict row;
    row["shape"] = numbers(table.shape);
    row["order"] = table.order;
    row["encoding"] = encoding == ScopeReadEncoding::Dense ? "dense" : "sparse";
    row["values"] = numbers(table.values);
    if (clips) {
        row["under_over_shape"] = numbers(table.underOverShape);
        row["under"] = numbers(table.under);
        row["over"] = numbers(table.over);
    }
    return row;
}

[[nodiscard]] nb::object channelSummary(const runtime::ScopeChannelSummary& summary) {
    if (!summary.available)
        return nb::none();
    nb::dict row;
    row["min"] = summary.minimum;
    row["max"] = summary.maximum;
    row["mean"] = summary.mean;
    row["p1"] = summary.percentile1;
    row["p50"] = summary.percentile50;
    row["p99"] = summary.percentile99;
    row["clipped_under"] = summary.clippedUnder;
    row["clipped_over"] = summary.clippedOver;
    return std::move(row);
}

[[nodiscard]] nb::dict settingsDict(const ScopeReadRequest& request) {
    nb::dict settings;
    nb::list names;
    if (request.output != ScopeReadOutput::Image)
        for (const auto& name : scopeNamesForTables(request.scopes))
            names.append(name);
    settings["scopes"] = names;
    settings["signal"] = request.signal == runtime::ScopeSignal::Display ? "display" : "working";
    settings["axis_maximum"] = request.signal == runtime::ScopeSignal::Working
                                   ? nb::cast(request.axisMaximum)
                                   : nb::none();
    if (request.region) {
        nb::list region;
        region.append(request.region->x);
        region.append(request.region->y);
        region.append(request.region->width);
        region.append(request.region->height);
        settings["region"] = region;
    } else {
        settings["region"] = nb::none();
    }
    settings["ignore_transparent"] = request.ignoreTransparent;
    settings["resolution"] = std::string(scopeResolutionName(request.resolution));
    const bool display = request.signal == runtime::ScopeSignal::Display;
    settings["display"] = display ? nb::cast(request.display) : nb::none();
    settings["view"] = display ? nb::cast(request.view) : nb::none();
    settings["result"] = kResultNames[static_cast<std::size_t>(request.output)];
    if (request.output == ScopeReadOutput::Tables)
        settings["encoding"] = kEncodingNames[static_cast<std::size_t>(request.encoding)];
    if (request.output == ScopeReadOutput::Image) {
        settings["image"] = std::string(scopeImageKindName(request.image));
        settings["waveform_channels"] =
            request.drawing.waveformChannels == runtime::scopes::WaveformChannels::Rgb ? "rgb"
                                                                                       : "luma";
        constexpr std::array<const char*, 3> histogramChannels{"rgb", "luma", "rgb-luma"};
        settings["histogram_channels"] =
            histogramChannels[static_cast<std::size_t>(request.drawing.histogramChannels)];
        settings["histogram_scale"] =
            request.drawing.histogramScale == runtime::scopes::HistogramScale::Linear
                ? "linear"
                : "logarithmic";
        settings["brightness"] = static_cast<double>(request.drawing.brightness);
    }
    return settings;
}

template <std::size_t Count>
[[nodiscard]] std::size_t choiceIndex(const nb::dict& options, const char* key,
                                      const std::array<std::string_view, Count>& names) {
    const auto text = required<std::string>(options, key);
    for (std::size_t index = 0; index < names.size(); ++index)
        if (names[index] == text)
            return index;
    std::string choices;
    for (const auto name : names)
        choices += (choices.empty() ? "'" : ", '") + std::string(name) + "'";
    throw nb::value_error((std::string(key) + " is one of " + choices).c_str());
}

} // namespace

nb::dict NativeHost::scopes(const nb::dict& options, const std::uint64_t cancellationTask,
                            const nb::object& progress) {
    ScopeReadRequest request;
    request.composition =
        document::CompositionId::fromRaw(required<std::uint64_t>(options, "composition"));
    request.firstFrame = required<std::uint64_t>(options, "first_frame");
    request.lastFrame = required<std::uint64_t>(options, "last_frame");
    if (!options["scopes"].is_none()) {
        std::string problem;
        const auto tables =
            scopeTablesForNames(required<std::vector<std::string>>(options, "scopes"), problem);
        if (!tables)
            throw nb::value_error(problem.c_str());
        request.scopes = *tables;
    }
    constexpr std::array<std::string_view, 2> signals{"display", "working"};
    request.signal = choiceIndex(options, "signal", signals) == 0 ? runtime::ScopeSignal::Display
                                                                  : runtime::ScopeSignal::Working;
    request.axisMaximum = required<std::uint32_t>(options, "axis_maximum");
    if (!options["region"].is_none()) {
        const auto region = required<std::vector<std::int64_t>>(options, "region");
        if (region.size() != 4)
            throw nb::value_error("region is (x, y, width, height) in composition pixels from the "
                                  "frame's top-left corner");
        for (const auto value : region)
            if (value < -1'000'000'000 || value > 1'000'000'000)
                throw nb::value_error("region values are within a billion pixels");
        if (region[2] < 0 || region[3] < 0)
            throw nb::value_error("region's width and height must be at least 1; leave region "
                                  "out to measure the whole frame");
        request.region = runtime::ScopeRegion{
            static_cast<std::int32_t>(region[0]), static_cast<std::int32_t>(region[1]),
            static_cast<std::uint32_t>(region[2]), static_cast<std::uint32_t>(region[3])};
    }
    request.ignoreTransparent = required<bool>(options, "ignore_transparent");
    constexpr std::array<std::string_view, 3> resolutions{"full", "half", "quarter"};
    request.resolution =
        static_cast<ScopeReadResolution>(choiceIndex(options, "resolution", resolutions));
    request.display = required<std::string>(options, "display");
    request.view = required<std::string>(options, "view");
    constexpr std::array<std::string_view, 3> results{"tables", "summary", "image"};
    request.output = static_cast<ScopeReadOutput>(choiceIndex(options, "result", results));
    constexpr std::array<std::string_view, 2> encodings{"dense", "sparse"};
    request.encoding = static_cast<ScopeReadEncoding>(choiceIndex(options, "encoding", encodings));
    constexpr std::array<std::string_view, 6> images{"waveform",  "parade",       "vectorscope",
                                                     "histogram", "chromaticity", "all"};
    request.image = static_cast<ScopeReadImageKind>(choiceIndex(options, "image", images));
    request.width = required<int>(options, "width");
    request.height = required<int>(options, "height");
    constexpr std::array<std::string_view, 2> waveformChannels{"rgb", "luma"};
    request.drawing.waveformChannels = static_cast<runtime::scopes::WaveformChannels>(
        choiceIndex(options, "waveform_channels", waveformChannels));
    constexpr std::array<std::string_view, 3> histogramChannels{"rgb", "luma", "rgb-luma"};
    request.drawing.histogramChannels = static_cast<runtime::scopes::HistogramChannels>(
        choiceIndex(options, "histogram_channels", histogramChannels));
    constexpr std::array<std::string_view, 2> histogramScales{"linear", "logarithmic"};
    request.drawing.histogramScale = static_cast<runtime::scopes::HistogramScale>(
        choiceIndex(options, "histogram_scale", histogramScales));
    request.drawing.brightness = static_cast<float>(required<double>(options, "brightness"));
    const auto out =
        options["out"].is_none() ? std::string() : required<std::string>(options, "out");
    const bool replace = required<bool>(options, "replace");
    if (request.output == ScopeReadOutput::Image && out.empty())
        throw nb::value_error("An image read writes a PNG: give out, the file to write (a range "
                              "writes one numbered file per frame)");
    if (request.output != ScopeReadOutput::Image && !out.empty())
        throw nb::value_error("out names the PNG of an image read; set result to 'image' or leave "
                              "out out");

    auto call = retirement_->acquire();
    auto& scheduler = call.scheduler();
    const auto access = lockSession();
    ScopeReadResult read;
    std::optional<nb::python_error> callbackError;
    {
        const nb::gil_scoped_release release;
        ScopeReadPorts ports;
        ports.scheduler = &scheduler;
        ports.cancelled = [this, cancellationTask, &call, &scheduler, &callbackError] {
            if (callbackError || call.closing() || cancellationRequested() || !session_->isValid())
                return true;
            if (cancellationTask == 0)
                return false;
            const auto task = scheduler.snapshot(runtime::TaskId::fromRaw(cancellationTask));
            return !task || runtime::isTerminal(task->state) || task->cancellationRequested;
        };
        ports.routes = headlessScopeRoutes(gpuExportProvider_, scheduler, ports.cancelled);
        if (!progress.is_none())
            ports.progress = [&progress, &callbackError](const std::uint64_t done,
                                                         const std::uint64_t total) {
                if (callbackError)
                    return;
                const nb::gil_scoped_acquire acquire;
                try {
                    progress(done, total);
                } catch (nb::python_error& error) {
                    callbackError.emplace(std::move(error));
                }
            };
        read = readScopes(*session_, request, ports);
    }
    if (callbackError) {
        // The progress callback raised (a cancelled task raises CancelledError): that is the
        // answer.
        throw std::move(*callbackError);
    }
    switch (read.error.status) {
    case ScopeReadStatus::Ok:
        break;
    case ScopeReadStatus::Invalid:
        throw nb::value_error(read.error.message.c_str());
    case ScopeReadStatus::Failed:
        throw std::runtime_error(read.error.message);
    case ScopeReadStatus::Cancelled: {
        nb::dict row;
        row["succeeded"] = false;
        row["diagnostic"] = read.error.message;
        row["diagnostic_code"] = "cancelled";
        return row;
    }
    }

    const auto& settings = read.settings;
    // The pictures first: nothing is written when one target exists and replace is off.
    std::vector<std::filesystem::path> targets;
    if (settings.output == ScopeReadOutput::Image) {
        for (const auto& frame : read.frames) {
            targets.push_back(read.frames.size() == 1 ? std::filesystem::path(out)
                                                      : host::FrameRangeRunnerV1::sequenceFramePath(
                                                            out, frame.frame, settings.lastFrame));
            std::error_code error;
            if (!replace && std::filesystem::exists(targets.back(), error))
                throw nb::value_error(("A file exists at " + targets.back().string() +
                                       "; pass replace=True to overwrite it, or choose another out")
                                          .c_str());
        }
    }

    nb::dict result;
    result["succeeded"] = true;
    result["revision"] = read.revision;
    result["composition"] = settings.composition.value();
    nb::dict size;
    size["width"] = read.compositionWidth;
    size["height"] = read.compositionHeight;
    result["composition_size"] = size;
    result["settings"] = settingsDict(settings);
    result["axis"] = settings.signal == runtime::ScopeSignal::Display
                         ? "Code values 0 to 1023 span the display signal 0 to 1."
                         : "Code values 0 to 1023 span the working-space value 0 to axis_maximum "
                           "(linear, scene-referred).";
    nb::dict luma;
    luma["kr"] = static_cast<double>(read.color.luma.red);
    luma["kg"] = static_cast<double>(read.color.luma.green);
    luma["kb"] = static_cast<double>(read.color.luma.blue);
    result["luma"] = luma;
    nb::dict primaries;
    primaries["color_space"] = read.color.colorSpace;
    primaries["r"] = pair(read.color.red);
    primaries["g"] = pair(read.color.green);
    primaries["b"] = pair(read.color.blue);
    primaries["w"] = pair(read.color.white);
    result["primaries"] = primaries;

    nb::list frames;
    std::size_t index = 0;
    for (const auto& frame : read.frames) {
        nb::dict row;
        row["frame"] = frame.frame;
        row["time"] = nb::make_tuple(frame.time.numerator(), frame.time.denominator());
        row["route"] = frame.route == runtime::ScopeRoute::Gpu ? "gpu" : "cpu";
        row["route_note"] = frame.routeNote.empty() ? nb::none() : nb::cast(frame.routeNote);
        row["from_cache"] = frame.fromCache;
        row["empty"] = frame.empty;
        row["counted"] = frame.counted;
        if (frame.empty) {
            row["measured_area"] = nb::none();
            row["note"] = "The measured area is empty: the region lies outside the frame, so "
                          "nothing was counted. Move the region inside the " +
                          std::to_string(read.compositionWidth) + " x " +
                          std::to_string(read.compositionHeight) + " frame.";
        } else {
            nb::dict area;
            area["x"] = frame.area.x;
            area["y"] = frame.area.y;
            area["width"] = frame.area.width;
            area["height"] = frame.area.height;
            row["measured_area"] = area;
        }
        if (settings.output == ScopeReadOutput::Tables && !frame.empty) {
            const auto& counts = frame.counts;
            const auto& layout = counts.layout;
            nb::dict counters;
            counters["not_finite"] = counts.notFinite();
            counters["no_chromaticity"] = layout.has(render::kScopeTableChromaticity)
                                              ? nb::cast(counts.noChromaticity())
                                              : nb::none();
            nb::dict outside;
            outside["vectorscope"] = layout.has(render::kScopeTableVectorscope)
                                         ? nb::cast(counts.vectorscopeOutside())
                                         : nb::none();
            outside["chromaticity"] = layout.has(render::kScopeTableChromaticity)
                                          ? nb::cast(counts.chromaticityOutside())
                                          : nb::none();
            counters["outside"] = outside;
            row["counters"] = counters;
            const auto data = describeScopeTables(counts, settings.encoding);
            nb::dict tables;
            nb::list channels;
            for (const auto* name : kChannelNames)
                channels.append(name);
            if (data.histogram) {
                auto table = tableDict(*data.histogram, settings.encoding, true);
                table["channels"] = channels;
                tables["histogram"] = table;
            }
            if (data.waveform) {
                auto table = tableDict(*data.waveform, settings.encoding, true);
                table["channels"] = channels;
                table["columns"] = layout.columns;
                tables["waveform"] = table;
            }
            if (data.vectorscope)
                tables["vectorscope"] = tableDict(*data.vectorscope, settings.encoding, false);
            if (data.chromaticity)
                tables["chromaticity"] = tableDict(*data.chromaticity, settings.encoding, false);
            row["tables"] = tables;
        } else if (settings.output == ScopeReadOutput::Summary && frame.summary) {
            const auto& summary = *frame.summary;
            nb::dict value;
            value["codes"] = "min, max, p1, p50 and p99 are code values 0 to 1023; mean is the "
                             "count-weighted mean code. clipped_under and clipped_over count "
                             "pixels below code 0 and above code 1023. A channel is None when its "
                             "table was not measured or counted nothing.";
            nb::dict channels;
            for (std::size_t channel = 0; channel < kChannelNames.size(); ++channel)
                channels[kChannelNames[channel]] = channelSummary(summary.channels[channel]);
            value["channels"] = channels;
            if (summary.vectorscopeAvailable) {
                nb::dict vector;
                vector["mean_saturation"] = summary.meanSaturation;
                vector["mean_hue_degrees"] = summary.meanHueDegrees;
                vector["units"] =
                    "mean_saturation is the mean distance from the center as a fraction of the "
                    "graticule circle's radius; mean_hue_degrees is the angle of the mean color, "
                    "counter-clockwise from +Cb, 0 to 360.";
                value["vectorscope"] = vector;
            } else {
                value["vectorscope"] = nb::none();
            }
            row["summary"] = value;
        } else if (settings.output == ScopeReadOutput::Image) {
            const auto& path = targets[index];
            std::error_code error;
            if (!path.parent_path().empty())
                std::filesystem::create_directories(path.parent_path(), error);
            {
                std::ofstream file(path, std::ios::binary | std::ios::trunc);
                file.write(reinterpret_cast<const char*>(frame.png.data()),
                           static_cast<std::streamsize>(frame.png.size()));
                if (!file)
                    throw std::runtime_error("The scope picture could not be written to " +
                                             path.string());
            }
            nb::dict image;
            image["path"] = path.string();
            image["bytes"] = frame.png.size();
            image["digest"] = digestOf(path);
            image["width"] = frame.imageWidth;
            image["height"] = frame.imageHeight;
            row["image"] = image;
        }
        frames.append(row);
        ++index;
    }
    result["frames"] = frames;
    return result;
}

} // namespace bloom::scripting::python
