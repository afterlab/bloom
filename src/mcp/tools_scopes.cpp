// query kind=scopes (#724, section 8b): the Scopes panel's count tables, a summary of them, or a
// picture of a scope, for one frame or a range, in any session with no panel open. The measure is
// scripting/scope_read.hpp's, the same one the Python API calls; this file parses the MCP
// arguments and renders the result as JSON.
#include "core_internal.hpp"

#include <bloom/core/file_uri.hpp>
#include <bloom/scripting/scope_read.hpp>

#include <cmath>
#include <fstream>

namespace bloom::mcp {
namespace {

using scripting::ScopeReadEncoding;
using scripting::ScopeReadOutput;

constexpr std::array<std::string_view, 3> kResults{"tables", "summary", "image"};
constexpr std::array<std::string_view, 2> kSignals{"display", "working"};
constexpr std::array<std::string_view, 2> kEncodings{"dense", "sparse"};
constexpr std::array<std::string_view, 3> kResolutions{"full", "half", "quarter"};
constexpr std::array<std::string_view, 2> kWaveformChannels{"rgb", "luma"};
constexpr std::array<std::string_view, 3> kHistogramChannels{"rgb", "luma", "rgb-luma"};
constexpr std::array<std::string_view, 2> kHistogramScales{"linear", "logarithmic"};

// The channel names of the tables, in table order.
constexpr std::array<std::string_view, 4> kChannelNames{"red", "green", "blue", "luma"};

[[nodiscard]] std::string quoted(const std::span<const std::string_view> names) {
    std::string text;
    for (const auto name : names)
        text += (text.empty() ? "\"" : ", \"") + std::string(name) + "\"";
    return text;
}

// The index of the named choice, or nullopt when the argument is absent.
[[nodiscard]] std::optional<std::size_t> choice(yyjson_val* arguments, const char* name,
                                                const std::span<const std::string_view> names) {
    auto* value = member(arguments, name);
    if (value == nullptr)
        return std::nullopt;
    if (yyjson_is_str(value)) {
        const std::string_view text(yyjson_get_str(value), yyjson_get_len(value));
        for (std::size_t index = 0; index < names.size(); ++index)
            if (names[index] == text)
                return index;
    }
    throw InvalidInput(std::string(name) + " is one of " + quoted(names));
}

[[nodiscard]] std::uint32_t whole(yyjson_val* value, const char* name) {
    if (!yyjson_is_uint(value) && !yyjson_is_sint(value))
        throw InvalidInput(std::string(name) + " is a whole number");
    const auto number = yyjson_get_sint(value);
    if (number < 0 || number > 1'000'000)
        throw InvalidInput(std::string(name) + " is a whole number from 0 up");
    return static_cast<std::uint32_t>(number);
}

yyjson_mut_val* numbers(Json& out, const std::vector<std::uint32_t>& values) {
    auto* array = out.array();
    for (const auto value : values)
        out.append(array, out.number(value));
    return array;
}

yyjson_mut_val* point(Json& out, const std::array<double, 2>& value) {
    auto* array = out.array();
    out.append(array, out.real(value[0]));
    out.append(array, out.real(value[1]));
    return array;
}

yyjson_mut_val* tableJson(Json& out, const scripting::ScopeTableData& table,
                          const ScopeReadEncoding encoding, const bool clips) {
    auto* value = out.object();
    out.set(value, "shape", numbers(out, table.shape));
    out.set(value, "order", out.text(table.order));
    out.set(value, "encoding",
            out.text(encoding == ScopeReadEncoding::Dense
                         ? "dense: values holds every cell in that order"
                         : "sparse: values is [index, count] pairs of the non-zero cells, index "
                           "into the dense order"));
    out.set(value, "values", numbers(out, table.values));
    if (clips) {
        out.set(value, "underOverShape", numbers(out, table.underOverShape));
        out.set(value, "under", numbers(out, table.under));
        out.set(value, "over", numbers(out, table.over));
    }
    return value;
}

yyjson_mut_val* channelSummary(Json& out, const runtime::ScopeChannelSummary& summary) {
    auto* value = out.object();
    if (!summary.available)
        return out.null();
    out.set(value, "min", out.number(summary.minimum));
    out.set(value, "max", out.number(summary.maximum));
    out.set(value, "mean", out.real(summary.mean));
    out.set(value, "p1", out.number(summary.percentile1));
    out.set(value, "p50", out.number(summary.percentile50));
    out.set(value, "p99", out.number(summary.percentile99));
    out.set(value, "clippedUnder", out.number(summary.clippedUnder));
    out.set(value, "clippedOver", out.number(summary.clippedOver));
    return value;
}

yyjson_mut_val* settingsJson(Json& out, const scripting::ScopeReadRequest& request) {
    auto* settings = out.object();
    auto* names = out.array();
    if (request.output != ScopeReadOutput::Image)
        for (const auto& name : scripting::scopeNamesForTables(request.scopes))
            out.append(names, out.text(name));
    out.set(settings, "scopes", names);
    out.set(settings, "signal",
            out.text(request.signal == runtime::ScopeSignal::Display ? "display" : "working"));
    out.set(settings, "axisMaximum",
            request.signal == runtime::ScopeSignal::Working ? out.number(request.axisMaximum)
                                                            : out.null());
    if (request.region) {
        auto* region = out.array();
        out.append(region, out.signedNumber(request.region->x));
        out.append(region, out.signedNumber(request.region->y));
        out.append(region, out.number(request.region->width));
        out.append(region, out.number(request.region->height));
        out.set(settings, "region", region);
    } else {
        out.set(settings, "region", out.null());
    }
    out.set(settings, "ignoreTransparent", out.boolean(request.ignoreTransparent));
    out.set(settings, "resolution", out.text(scripting::scopeResolutionName(request.resolution)));
    out.set(settings, "display",
            request.signal == runtime::ScopeSignal::Display ? out.text(request.display)
                                                            : out.null());
    out.set(settings, "view",
            request.signal == runtime::ScopeSignal::Display ? out.text(request.view) : out.null());
    out.set(settings, "result", out.text(kResults[static_cast<std::size_t>(request.output)]));
    if (request.output == ScopeReadOutput::Tables)
        out.set(settings, "encoding",
                out.text(kEncodings[static_cast<std::size_t>(request.encoding)]));
    if (request.output == ScopeReadOutput::Image) {
        out.set(settings, "image", out.text(scripting::scopeImageKindName(request.image)));
        out.set(settings, "waveformChannels",
                out.text(
                    kWaveformChannels[static_cast<std::size_t>(request.drawing.waveformChannels)]));
        out.set(
            settings, "histogramChannels",
            out.text(
                kHistogramChannels[static_cast<std::size_t>(request.drawing.histogramChannels)]));
        out.set(
            settings, "histogramScale",
            out.text(kHistogramScales[static_cast<std::size_t>(request.drawing.histogramScale)]));
        out.set(settings, "brightness", out.real(static_cast<double>(request.drawing.brightness)));
    }
    return settings;
}

} // namespace

yyjson_mut_val* scopesQuery(McpServerCore::State& core, Json& out, McpCall& call,
                            yyjson_val* arguments) {
    members(arguments,
            {"kind",
             "composition",
             "frame",
             "time",
             "lastFrame",
             "scopes",
             "signal",
             "axisMaximum",
             "region",
             "ignoreTransparent",
             "resolution",
             "display",
             "view",
             "result",
             "encoding",
             "image",
             "width",
             "height",
             "waveformChannels",
             "histogramChannels",
             "histogramScale",
             "brightness"},
            {"kind"});
    const auto snapshot = call.session.snapshot();
    scripting::ScopeReadRequest request;
    const auto compositionId = member(arguments, "composition")
                                   ? integer(member(arguments, "composition"))
                                   : call.context.composition.value_or(0);
    request.composition = document::CompositionId::fromRaw(compositionId);

    // The frames: `frame`, or `time` (a whole frame or exact seconds), else the playhead, else 0.
    if (member(arguments, "frame") && member(arguments, "time"))
        throw InvalidInput("give frame or time, not both; frame is a whole frame number and time "
                           "is a frame or [numerator, denominator] seconds");
    if (member(arguments, "frame")) {
        request.firstFrame = integer(member(arguments, "frame"));
    } else {
        core::RationalTime time = call.context.time.value_or(core::RationalTime::fromInteger(0));
        if (auto* requested = member(arguments, "time"))
            time = frameOrSecondsArgument(requested, snapshot, compositionId);
        const auto frame = scripting::scopeFrameForTime(snapshot, request.composition, time);
        // An unknown composition is the read's own refusal below.
        request.firstFrame = frame.value_or(0);
    }
    request.lastFrame = member(arguments, "lastFrame") ? integer(member(arguments, "lastFrame"))
                                                       : request.firstFrame;

    // What is measured.
    if (auto* names = member(arguments, "scopes")) {
        if (!yyjson_is_arr(names))
            throw InvalidInput("scopes is a list of scope names: " +
                               quoted(scripting::kScopeNames));
        std::vector<std::string> list;
        std::size_t index = 0;
        std::size_t count = 0;
        yyjson_val* item = nullptr;
        yyjson_arr_foreach(names, index, count, item) {
            if (!yyjson_is_str(item))
                throw InvalidInput("scopes is a list of scope names: " +
                                   quoted(scripting::kScopeNames));
            list.push_back(string(item, 32));
        }
        std::string problem;
        const auto tables = scripting::scopeTablesForNames(list, problem);
        if (!tables)
            throw InvalidInput(problem);
        request.scopes = *tables;
    }
    if (const auto signal = choice(arguments, "signal", kSignals))
        request.signal =
            *signal == 0 ? runtime::ScopeSignal::Display : runtime::ScopeSignal::Working;
    if (auto* value = member(arguments, "axisMaximum"))
        request.axisMaximum = whole(value, "axisMaximum");
    if (auto* region = member(arguments, "region")) {
        if (!yyjson_is_arr(region) || yyjson_arr_size(region) != 4)
            throw InvalidInput("region is [x, y, width, height] in composition pixels from the "
                               "frame's top-left corner");
        std::array<std::uint32_t, 4> values{};
        for (std::size_t index = 0; index < 4; ++index)
            values[index] = whole(yyjson_arr_get(region, index), "region");
        request.region =
            runtime::ScopeRegion{static_cast<std::int32_t>(values[0]),
                                 static_cast<std::int32_t>(values[1]), values[2], values[3]};
    }
    if (member(arguments, "ignoreTransparent"))
        request.ignoreTransparent = boolean(member(arguments, "ignoreTransparent"));
    if (const auto resolution = choice(arguments, "resolution", kResolutions))
        request.resolution = static_cast<scripting::ScopeReadResolution>(*resolution);
    if (member(arguments, "display"))
        request.display = string(member(arguments, "display"), 256);
    if (member(arguments, "view"))
        request.view = string(member(arguments, "view"), 256);

    // What is returned.
    if (const auto result = choice(arguments, "result", kResults))
        request.output = static_cast<ScopeReadOutput>(*result);
    if (const auto encoding = choice(arguments, "encoding", kEncodings)) {
        if (request.output != ScopeReadOutput::Tables)
            throw InvalidInput("encoding applies to result \"tables\"; a summary and an image have "
                               "no tables to encode");
        request.encoding = static_cast<ScopeReadEncoding>(*encoding);
    }
    const bool imageArguments =
        member(arguments, "image") || member(arguments, "width") || member(arguments, "height") ||
        member(arguments, "waveformChannels") || member(arguments, "histogramChannels") ||
        member(arguments, "histogramScale") || member(arguments, "brightness");
    if (imageArguments && request.output != ScopeReadOutput::Image)
        throw InvalidInput("image, width, height, waveformChannels, histogramChannels, "
                           "histogramScale and brightness draw a picture; set result to "
                           "\"image\" or leave them out");
    if (auto* image = member(arguments, "image")) {
        const auto parsed = yyjson_is_str(image)
                                ? scripting::scopeImageKindFromName(string(image, 32))
                                : std::nullopt;
        if (!parsed)
            throw InvalidInput("image is one of \"waveform\", \"parade\", \"vectorscope\", "
                               "\"histogram\", \"chromaticity\" or \"all\"");
        request.image = *parsed;
    }
    if (member(arguments, "width"))
        request.width = static_cast<int>(whole(member(arguments, "width"), "width"));
    if (member(arguments, "height"))
        request.height = static_cast<int>(whole(member(arguments, "height"), "height"));
    if (const auto channels = choice(arguments, "waveformChannels", kWaveformChannels))
        request.drawing.waveformChannels =
            static_cast<runtime::scopes::WaveformChannels>(*channels);
    if (const auto channels = choice(arguments, "histogramChannels", kHistogramChannels))
        request.drawing.histogramChannels =
            static_cast<runtime::scopes::HistogramChannels>(*channels);
    if (const auto scale = choice(arguments, "histogramScale", kHistogramScales))
        request.drawing.histogramScale = static_cast<runtime::scopes::HistogramScale>(*scale);
    if (auto* brightness = member(arguments, "brightness")) {
        if (!yyjson_is_num(brightness))
            throw InvalidInput("brightness is a number from 0.1 to 16 (1 is the panel's default)");
        request.drawing.brightness = static_cast<float>(yyjson_get_num(brightness));
    }
    if (request.output == ScopeReadOutput::Image &&
        request.lastFrame - request.firstFrame >= kMcpRetainedCaptures &&
        request.lastFrame >= request.firstFrame)
        throw InvalidInput("An image read keeps at most " + std::to_string(kMcpRetainedCaptures) +
                           " pictures like capture does; ask for " +
                           std::to_string(kMcpRetainedCaptures) +
                           " frames or fewer, or read tables or a summary of the range");

    // Where frames are measured: the application's routes and warm sidecars on the live host, the
    // session's own GPU provider (or the CPU reference) on a headless one.
    const auto& stop = call.control.stop;
    scripting::ScopeReadPorts ports;
    ports.scheduler = &core.dependencies.scheduler;
    ports.cancelled = [stop] { return stop.stop_requested(); };
    const auto& ui = core.dependencies.ui;
    if (call.live != nullptr) {
        if (ui.scopeRoutes) {
            if (!core.dependencies.exports.run(
                    [&](host::ExportQueueService*, std::uint64_t) {
                        ports.routes = ui.scopeRoutes();
                    },
                    stop))
                throw ToolFailure(McpToolErrorCode::Cancelled, "The request was cancelled.");
        }
        if (ui.warmScopes) {
            ports.warm = [&core, &ui, stop](const scripting::ScopeWarmQuery& query) {
                std::optional<scripting::ScopeWarmHit> hit;
                if (!core.dependencies.exports.run(
                        [&](host::ExportQueueService*, std::uint64_t) {
                            hit = ui.warmScopes(query);
                        },
                        stop))
                    return std::optional<scripting::ScopeWarmHit>{};
                return hit;
            };
        }
    } else {
        ports.routes = scripting::headlessScopeRoutes(call.gpuProvider, core.dependencies.scheduler,
                                                      ports.cancelled);
    }
    if (call.control.progress)
        ports.progress = [&call](const std::uint64_t done, const std::uint64_t total) {
            call.control.progress({static_cast<double>(done), static_cast<double>(total),
                                   "Measured scopes of frame " + std::to_string(done) + " of " +
                                       std::to_string(total)});
        };

    const auto read = scripting::readScopes(call.session, request, ports);
    switch (read.error.status) {
    case scripting::ScopeReadStatus::Ok:
        break;
    case scripting::ScopeReadStatus::Invalid:
        throw InvalidInput(read.error.message);
    case scripting::ScopeReadStatus::Cancelled:
        throw ToolFailure(McpToolErrorCode::Cancelled, "The request was cancelled.");
    case scripting::ScopeReadStatus::Failed:
        throw std::runtime_error(read.error.message);
    }
    const auto& settings = read.settings;
    call.summary = "scopes of composition " + std::to_string(compositionId) + " frames " +
                   std::to_string(settings.firstFrame) + ".." + std::to_string(settings.lastFrame);

    // The tables' size, checked before anything is built.
    std::vector<scripting::ScopeTablesData> tables;
    if (settings.output == ScopeReadOutput::Tables) {
        std::size_t bytes = 0;
        for (const auto& frame : read.frames) {
            tables.push_back(scripting::describeScopeTables(
                frame.empty ? runtime::ScopeCounts{} : frame.counts, settings.encoding));
            bytes += tables.back().jsonBytes;
        }
        if (bytes > scripting::kScopeTablesJsonBudget)
            throw InvalidInput(
                "The tables are about " + std::to_string(bytes / 1024) + " KiB as JSON, over the " +
                std::to_string(scripting::kScopeTablesJsonBudget / 1024) +
                " KiB one answer can carry. Ask for fewer scopes (scopes), the sparse encoding "
                "(encoding \"sparse\" lists only the non-zero cells), a smaller region, fewer "
                "frames, a summary (result \"summary\") or a picture (result \"image\")");
    }

    auto* result = out.object();
    out.set(result, "revision", out.number(read.revision));
    out.set(result, "composition", out.number(compositionId));
    auto* size = out.object();
    out.set(size, "width", out.number(read.compositionWidth));
    out.set(size, "height", out.number(read.compositionHeight));
    out.set(result, "compositionSize", size);
    out.set(result, "settings", settingsJson(out, settings));
    out.set(result, "axis",
            out.text(settings.signal == runtime::ScopeSignal::Display
                         ? "Code values 0 to 1023 span the display signal 0 to 1."
                         : "Code values 0 to 1023 span the working-space value 0 to axisMaximum "
                           "(linear, scene-referred)."));
    auto* luma = out.object();
    out.set(luma, "kr", out.real(static_cast<double>(read.color.luma.red)));
    out.set(luma, "kg", out.real(static_cast<double>(read.color.luma.green)));
    out.set(luma, "kb", out.real(static_cast<double>(read.color.luma.blue)));
    out.set(result, "luma", luma);
    auto* primaries = out.object();
    out.set(primaries, "colorSpace", out.text(read.color.colorSpace));
    out.set(primaries, "r", point(out, read.color.red));
    out.set(primaries, "g", point(out, read.color.green));
    out.set(primaries, "b", point(out, read.color.blue));
    out.set(primaries, "w", point(out, read.color.white));
    out.set(result, "primaries", primaries);

    auto* frames = out.array();
    std::size_t index = 0;
    for (const auto& frame : read.frames) {
        auto* row = out.object();
        out.set(row, "frame", out.number(frame.frame));
        auto* time = out.array();
        out.append(time, out.signedNumber(frame.time.numerator()));
        out.append(time, out.signedNumber(frame.time.denominator()));
        out.set(row, "time", time);
        out.set(row, "route", out.text(frame.route == runtime::ScopeRoute::Gpu ? "gpu" : "cpu"));
        out.set(row, "routeNote", frame.routeNote.empty() ? out.null() : out.text(frame.routeNote));
        out.set(row, "fromCache", out.boolean(frame.fromCache));
        out.set(row, "empty", out.boolean(frame.empty));
        if (frame.empty) {
            out.set(row, "measuredArea", out.null());
            out.set(row, "counted", out.number(std::uint64_t{0}));
            out.set(row, "note",
                    out.text("The measured area is empty: the region lies outside the frame, so "
                             "nothing was counted. Move the region inside the " +
                             std::to_string(read.compositionWidth) + " x " +
                             std::to_string(read.compositionHeight) + " frame."));
        } else {
            auto* area = out.object();
            out.set(area, "x", out.signedNumber(frame.area.x));
            out.set(area, "y", out.signedNumber(frame.area.y));
            out.set(area, "width", out.number(frame.area.width));
            out.set(area, "height", out.number(frame.area.height));
            out.set(row, "measuredArea", area);
            out.set(row, "counted", out.number(frame.counted));
        }
        if (settings.output == ScopeReadOutput::Tables && !frame.empty) {
            const auto& counts = frame.counts;
            const auto& layout = counts.layout;
            auto* counters = out.object();
            out.set(counters, "notFinite", out.number(counts.notFinite()));
            out.set(counters, "noChromaticity",
                    layout.has(render::kScopeTableChromaticity)
                        ? out.number(counts.noChromaticity())
                        : out.null());
            auto* outside = out.object();
            out.set(outside, "vectorscope",
                    layout.has(render::kScopeTableVectorscope)
                        ? out.number(counts.vectorscopeOutside())
                        : out.null());
            out.set(outside, "chromaticity",
                    layout.has(render::kScopeTableChromaticity)
                        ? out.number(counts.chromaticityOutside())
                        : out.null());
            out.set(counters, "outside", outside);
            out.set(row, "counters", counters);
            const auto& data = tables[index];
            auto* list = out.object();
            if (data.histogram) {
                auto* table = tableJson(out, *data.histogram, settings.encoding, true);
                auto* channels = out.array();
                for (const auto name : kChannelNames)
                    out.append(channels, out.text(name));
                out.set(table, "channels", channels);
                out.set(list, "histogram", table);
            }
            if (data.waveform) {
                auto* table = tableJson(out, *data.waveform, settings.encoding, true);
                auto* channels = out.array();
                for (const auto name : kChannelNames)
                    out.append(channels, out.text(name));
                out.set(table, "channels", channels);
                out.set(table, "columns", out.number(layout.columns));
                out.set(list, "waveform", table);
            }
            if (data.vectorscope)
                out.set(list, "vectorscope",
                        tableJson(out, *data.vectorscope, settings.encoding, false));
            if (data.chromaticity)
                out.set(list, "chromaticity",
                        tableJson(out, *data.chromaticity, settings.encoding, false));
            out.set(row, "tables", list);
        } else if (settings.output == ScopeReadOutput::Summary && frame.summary) {
            const auto& summary = *frame.summary;
            auto* value = out.object();
            auto* channels = out.object();
            for (std::size_t channel = 0; channel < kChannelNames.size(); ++channel)
                out.set(channels, kChannelNames[channel],
                        channelSummary(out, summary.channels[channel]));
            out.set(value, "codes",
                    out.text("min, max, p1, p50 and p99 are code values 0 to 1023; mean is the "
                             "count-weighted mean code. clippedUnder and clippedOver count pixels "
                             "below code 0 and above code 1023. A channel is null when its "
                             "table was not measured or counted nothing."));
            out.set(value, "channels", channels);
            if (summary.vectorscopeAvailable) {
                auto* vector = out.object();
                out.set(vector, "meanSaturation", out.real(summary.meanSaturation));
                out.set(vector, "meanHueDegrees", out.real(summary.meanHueDegrees));
                out.set(vector, "units",
                        out.text("meanSaturation is the mean distance from the center as a "
                                 "fraction of the graticule circle's radius; meanHueDegrees is "
                                 "the angle of the mean color, counter-clockwise from +Cb, 0 to "
                                 "360."));
                out.set(value, "vectorscope", vector);
            } else {
                out.set(value, "vectorscope", out.null());
            }
            out.set(row, "summary", value);
        } else if (settings.output == ScopeReadOutput::Image) {
            const auto path = nextScopePicturePath(core);
            {
                std::ofstream file(path, std::ios::binary);
                file.write(reinterpret_cast<const char*>(frame.png.data()),
                           static_cast<std::streamsize>(frame.png.size()));
                if (!file)
                    throw std::runtime_error("The scope picture could not be written");
            }
            pruneScopePictures(path.parent_path());
            auto* image = out.object();
            out.set(image, "path", out.text(path.string()));
            out.set(image, "bytes", out.number(frame.png.size()));
            out.set(image, "digest", out.text(digestFile(path)));
            out.set(image, "width", out.number(static_cast<std::uint64_t>(frame.imageWidth)));
            out.set(image, "height", out.number(static_cast<std::uint64_t>(frame.imageHeight)));
            const bool inlined =
                read.frames.size() == 1 && frame.png.size() <= kMcpInlineImageBytes;
            out.set(image, "inline", out.boolean(inlined));
            if (read.frames.size() == 1) {
                if (inlined) {
                    auto* block = out.object();
                    out.set(block, "type", out.text("image"));
                    out.set(block, "data", out.text(base64(std::as_bytes(std::span(frame.png)))));
                    out.set(block, "mimeType", out.text("image/png"));
                    call.content.push_back(block);
                } else {
                    const auto uri = core::fileUriFromAbsolutePath(std::filesystem::absolute(path));
                    auto* link = out.object();
                    out.set(link, "type", out.text("resource_link"));
                    out.set(link, "uri", out.text(uri.value_or(path.string())));
                    out.set(link, "name", out.text(path.filename().string()));
                    out.set(link, "mimeType", out.text("image/png"));
                    out.set(link, "size", out.number(frame.png.size()));
                    call.content.push_back(link);
                    call.notice = McpToolErrorCode::CaptureTooLargeForInline;
                }
            }
            out.set(row, "image", image);
        }
        out.append(frames, row);
        ++index;
    }
    out.set(result, "frames", frames);
    return result;
}

} // namespace bloom::mcp
