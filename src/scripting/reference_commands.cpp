// The reference entries of the query kinds, the bloom-cli commands and the Python entry points
// (the MCP tools and shared concepts are in reference_surfaces.cpp).

#include "reference_internal.hpp"

#include <string>
#include <utility>

namespace bloom::scripting::detail {

// ---------------------------------------------------------------------------------------------
// Query kinds

void addQueryKindEntries(std::vector<ReferenceEntry>& out) {
    const auto kind = [&out](std::string name, std::string summary, std::string description,
                             std::string result, std::initializer_list<ArgumentSpec> arguments,
                             std::string_view example) {
        auto value = makeEntry(ReferenceKind::QueryKind, "query:" + name, "document",
                               std::move(summary), std::move(description));
        value.effects = readOnlyEffects();
        addArguments(value, {{.name = "kind",
                              .type = "string",
                              .required = true,
                              .summary = "This kind.",
                              .choices = {}}});
        value.arguments.back().choices.push_back({.value = jsonQuote(name), .label = name});
        addArguments(value, arguments);
        value.result = std::move(result);
        value.examples.push_back({.surface = "mcp", .text = std::string(example)});
        value.related.emplace_back("query");
        out.push_back(std::move(value));
    };
    constexpr ArgumentSpec composition{.name = "composition",
                                       .type = "integer",
                                       .required = true,
                                       .summary = "The composition.",
                                       .unit = "composition id (query:compositions)",
                                       .range = ">= 1"};
    constexpr ArgumentSpec page{.name = "pageSize",
                                .type = "integer",
                                .summary = "Records per page.",
                                .range = "1..64",
                                .defaultValue = "64"};
    constexpr ArgumentSpec cursor{.name = "cursor",
                                  .type = "string",
                                  .summary = "The previous page's nextCursor; stale after any "
                                             "edit."};
    constexpr std::string_view time = "{frames, seconds:[n, d]}";
    // #575: any query that returns records can name the members it wants.
    constexpr ArgumentSpec fields{.name = "fields",
                                  .type = "array of strings",
                                  .summary = "Return only these members of every record, such as "
                                             "[\"id\", \"name\"]. A name the records do not "
                                             "have is refused, listing the ones they do. Default: "
                                             "every member.",
                                  .unit = "member names of the records this kind returns"};
    constexpr ArgumentSpec layerFilter{
        .name = "layer",
        .type = "integer",
        .summary = "Only this layer: its record and Merge entries (layers), or its Layer Output "
                   "node (or its entry's source) and every node upstream of it, with the links "
                   "and groups among them (nodes). Default: every layer.",
        .unit = "layer id (query:layers .id)",
        .range = ">= 1"};

    kind("project", "The project's id and name.", {}, "{revision, project:{id, name}}", {},
         R"({"name":"query","arguments":{"kind":"project"}})");
    kind("compositions",
         "Every composition with its size, frame rate, duration, work area and settings.",
         "Times are reported as {frames, seconds:[n, d]} (concept:time), in the composition's "
         "own frame rate.",
         "{revision, records:[{id, name, width, height, frameRate:[n, d], fps, "
         "frameRateNumerator, frameRateDenominator, pixelAspect:[n, d], duration:" +
             std::string(time) + ", start:" + std::string(time) +
             " (the timecode label of frame 0), dropFrame, workArea:{start, end}|null (end "
             "exclusive), backgroundColor:[r, g, b, a] (display-only), workingColorSpace|null, "
             "safeAreas:{action, title}, layerCount, folder|null}]}",
         {fields}, R"({"name":"query","arguments":{"kind":"compositions"}})");
    kind("layers", "A composition's layers, topmost first, with timing, masks and parameter ids.",
         "Each layer's `parameters` maps its transform roles (position, anchor, scale, rotation, "
         "opacity, blendMode, timeRemap) to parameter ids; `sourceParameters` maps the source "
         "node's roles (a solid's color, a text layer's text, size, font, ...) the same way. "
         "Pass those ids to bloom.parameter.set-source or the animation operations. Every Merge "
         "entry is a layer (#502): `entries` lists each one, output Merge first and each Merge "
         "top to bottom, with its layer -- a Layer node (layerKind node), the entry's own layer "
         "(entry) or none yet (none, reporting the defaults it renders with). Address an entry "
         "with merge and slot: every layer operation takes them instead of layer, and "
         "bloom.merge.set-entry-parameter sets a control by role.",
         "{revision, composition, records:[{id, name, index (0 is topmost), slot, merge, "
         "layerOutputNode, sourceNode|null, sourceType|null, enabled, solo, locked, shy, "
         "nameFollowsSource, sourceName?, "
         "labelColor:[r, g, b]|null, parent|null, start:" +
             std::string(time) + ", end:" + std::string(time) +
             " (exclusive), timing:{startTime, speed:[n, d], before, after, extent}, "
             "masks:[{id, name, mode, inverted, path, feather, expansion, opacity}], "
             "trackMatte:{source, channel, inverted}|null, parameters:{role: id}, "
             "sourceParameters:{role: id}, layerKind (node|entry), entryLayer, pivot "
             "(content|frame), entries:[{merge, slot}], shared, adjustment|null, "
             "values:{role: value|null}}], entries:[{merge, slot, index, outputMerge, "
             "resolvedLayer|null, layerKind (node|entry|none), entryLayer, layerNode|null, "
             "sourceNode|null, sourceType|null, name|null, enabled, solo, locked, shy, pivot, "
             "adjustment|null, shared, nestedMerge|null, parameters:{role: id}, "
             "values:{role: value|null}}]}",
         {composition, layerFilter, fields},
         R"({"name":"query","arguments":{"kind":"layers","composition":1,"fields":["id","name"]}})");
    kind("nodes", "A composition's node graph: nodes, ports, links, card positions and groups.",
         "Input ports list what feeds them; output ports list where they lead. Merge nodes also "
         "list their layer stack, each row with the same entry fields as query:layers "
         "`entries`. `links` repeats every edge once.",
         "{revision, composition, records:[{id, typeId, name, category, parameters:{role: id}, "
         "position:[x, y], width, collapsed, muted, group|null, inputs:[{port, kind, required, "
         "source:{node, port}|null}], outputs:[{port, kind, targets:[{node, port}|{node, role, "
         "slot}]}], stack?:[{slot, layer, index, resolvedLayer, layerKind, entryLayer, layerNode, "
         "sourceNode, sourceType, name, enabled, solo, locked, shy, pivot, adjustment, shared, "
         "nestedMerge, parameters, values}]}], links:[{id, source:{node, port}, "
         "target:{node, port}|{node, role, slot}}], groups:[{id, name, members}]}",
         {composition, layerFilter, fields},
         R"({"name":"query","arguments":{"kind":"nodes","composition":1,"layer":2}})");
    kind("node-types", "Every node type bloom.node.add accepts, or one type in full.",
         "Without nodeType: every type with its category, ports and parameter roles. With "
         "nodeType: the type's ports and every parameter with value kind, default, unit, range, "
         "accepted values and whether it can be animated. bloom://reference/nodes has the same "
         "content as Markdown.",
         "{revision, records:[{id, name, category, schemaVersion, cardinality, inputs:[{port, "
         "kind, required}], outputs:[{port, kind}], parameters:[{role, schemaKey, valueKind, "
         "animatable, default, unit?, minimum?, maximum?, choices?}], layerSlots?}]} or, with "
         "nodeType, {revision, record}",
         {{.name = "nodeType",
           .type = "string",
           .summary = "One type to describe in full.",
           .unit = "node type id such as bloom.solid-source"},
          fields},
         R"({"name":"query","arguments":{"kind":"node-types","nodeType":"bloom.solid-source"}})");
    kind("parameters", "A composition's parameters: full records, or summary pages.",
         "Without pageSize or cursor: every full record. With them: bounded summaries whose "
         "constant values are omitted (valueOmitted:true) and a nextCursor until it is null.",
         "{revision, records:[{id, schemaKey, source:\"constant\"|\"curve\"|\"driver\", value?, "
         "label? (an enumeration member's), curveId?, nodeId? and port? (a driver)}], nextCursor?, "
         "totalRecords?}",
         {composition, page, cursor, fields},
         R"({"name":"query","arguments":{"kind":"parameters","composition":1,"pageSize":64}})");
    kind("parameter",
         "One parameter's record and its metadata: value kind, unit, range, default, accepted "
         "values and owner.",
         "Write values in stored units, the units minimum, maximum and default are in. `unit` "
         "and `displayScale` are how the Properties panel shows the value (displayed = stored x "
         "displayScale: an opacity stored as 0.5 shows as 50 %). Enumerations report `choices` "
         "as {value, label}: bloom.parameter.set-source takes either the value or the "
         "label (case-insensitive).",
         "{revision, composition, parameter, record:{id, schemaKey, source, value|curveId|...}, "
         "metadata:{valueKind, animatable, default, unit, displayScale, minimum, maximum, "
         "softMinimum, softMaximum, choices:[{value, label}], owner:{node, typeId, role}|{layer, "
         "mask, role}|null}}",
         {composition,
          {.name = "parameter",
           .type = "integer",
           .required = true,
           .summary = "The parameter.",
           .unit = "parameter id",
           .range = ">= 1"},
          {.name = "expectedRevision",
           .type = "integer",
           .summary = "Refuse when the project moved on.",
           .range = ">= 0"}},
         R"({"name":"query","arguments":{"kind":"parameter","composition":1,"parameter":5}})");
    kind(
        "parameter-value", "A long constant string parameter, in UTF-8 chunks.",
        "Start at offset 0 and pass each nextOffset until complete is true. Chunks end on a "
        "UTF-8 boundary.",
        "{revision, composition, parameter, offset, nextOffset, totalBytes, complete, chunk}",
        {composition,
         {.name = "parameter",
          .type = "integer",
          .required = true,
          .summary = "The parameter.",
          .unit = "parameter id",
          .range = ">= 1"},
         {.name = "expectedRevision",
          .type = "integer",
          .required = true,
          .summary = "The revision the chunks must come from.",
          .range = ">= 0"},
         {.name = "offset",
          .type = "integer",
          .summary = "Byte offset.",
          .unit = "UTF-8 bytes",
          .range = ">= 0",
          .defaultValue = "0"},
         {.name = "limit",
          .type = "integer",
          .summary = "Most bytes.",
          .unit = "UTF-8 bytes",
          .range = "4..131072",
          .defaultValue = "131072"}},
        R"({"name":"query","arguments":{"kind":"parameter-value","composition":1,"parameter":9,"expectedRevision":4}})");
    kind("keyframes", "An animated parameter's keys: time, value and interpolation, paged.",
         "Address the curve by `parameter` (an animated parameter) or by `curve`. Vector and "
         "colour curves list one row per component key, with `component` naming the axis or "
         "channel. Times are {frames, seconds:[n, d]} at the composition's rate (concept:time); "
         "values are in the parameter's own units.",
         "{revision, composition, parameter|null, curve, valueKind, frameRate:[n, d], "
         "records:[{keyframe, component|null, time:" +
             std::string(time) +
             ", value, interpolation (a token from query enums: hold, linear, ease-in-out, "
             "sine, quad, ... elastic), interpolationId, outgoingHandle:{time, value}, "
             "incomingHandle:{time, value}, ease, back, amplitude, period, handleMode, "
             "handleModeId}], totalRecords, "
             "nextCursor|null}",
         {composition,
          {.name = "parameter",
           .type = "integer",
           .summary = "The animated parameter (or give curve).",
           .unit = "parameter id",
           .range = ">= 1"},
          {.name = "curve",
           .type = "integer",
           .summary = "The curve (or give parameter).",
           .unit = "animation curve id",
           .range = ">= 1"},
          page,
          cursor,
          fields},
         R"({"name":"query","arguments":{"kind":"keyframes","composition":1,"parameter":5}})");
    kind("enums", "The keyframe vocabularies with their tokens and stable ids.",
         "interpolation, ease, handleMode and easyEase: each value has an id, a token the "
         "animation operations take, and a label.",
         "{revision, records:[{enum, values:[{id, token, label}]}]}", {fields},
         R"({"name":"query","arguments":{"kind":"enums"}})");
    kind("assets", "Every asset: kind, file, size, folder and tags.", {},
         "{revision, records:[{id, name, kind:\"image\"|\"sequence\"|\"audio\"|\"font\"|\"video\""
         "|\"lut\", path, width, height, frames, digest, folder|null, tags}], folders:[{id, "
         "name, parent|null}]}",
         {fields}, R"({"name":"query","arguments":{"kind":"assets"}})");
    kind("operations", "Every operation schema transact accepts.",
         "The same schemas help describes, as data. The full list is large (hundreds of "
         "thousands of characters); narrow it with idPrefix and fields.",
         "{revision, records:[{id, description, arguments:[{name, kind, required, contextual, "
         "context, summary, unit?, minimum?, maximum?, default?, choices?}], example, "
         "outputs:[{name, kind, repeated, note?}] (the ids the transaction result carries), "
         "sideEffects:[text] (what the call changes beyond its target)}]}",
         {{.name = "idPrefix",
           .type = "string",
           .summary = "Only operations whose id starts with this text, such as "
                      "\"bloom.animation.\". Default: every operation.",
           .unit = "operation id prefix"},
          fields},
         R"({"name":"query","arguments":{"kind":"operations","idPrefix":"bloom.animation.","fields":["id","example"]}})");
    kind("text-metrics", "The advance and ink bounds of a piece of text, without rendering.",
         "Lays the text out as every render does: one left-aligned block of point text, no "
         "wrapping, line height 1.0 and no letter spacing, with kerning. All values are layout "
         "pixels at 1:1: x to the right of the text origin (the left edge of the first glyph's "
         "advance), y down from the top of the first line. `advance` is the widest line's width; "
         "`ink` is the union of every glyph's ink in whole pixels (null for text without ink). "
         "`font` is a font ID or a built-in face's name. A font ID is measured in the face a render "
         "would use now: `font.fallback` says when that is a stand-in, and `font.downloading` when "
         "the font is on its way, so ask again.",
         "{revision, font:{value, label} or {id, usedFamily, usedStyle, fallback, downloading, "
         "diagnostic}, size, unit:\"pixels\", space, advance, lineHeight, "
         "lines:[{index, x, y, advance, height, byteBegin, byteEnd}], "
         "ink:{x, y, width, height}|null}",
         {{.name = "text",
           .type = "string",
           .required = true,
           .summary = "The text; a newline starts a new line.",
           .unit = "UTF-8 text",
           .range = "at most 4096 bytes"},
          {.name = "font",
           .type = "string or integer",
           .summary = "A built-in face by label, stored value 0 to 3, or font reference.",
           .unit = "built-in face",
           .defaultValue = "DejaVu Sans"},
          {.name = "size",
           .type = "number",
           .required = true,
           .summary = "The text size.",
           .unit = "pixels per em",
           .range = "above 0 to 4096"}},
         R"({"name":"query","arguments":{"kind":"text-metrics","text":"Hello","font":"Inter Regular","size":48}})");
    kind("layer-bounds", "A layer's evaluated bounds at a time, without rendering.",
         "Evaluates the composition at `time` on the CPU reference path (heavy: a compile and "
         "one frame) and reads the layer's bounds. `local` is the layer's content rectangle "
         "before its transform: pixel edges, origin at the top-left of the layer's content (a "
         "solid's own size, a text block's laid-out box). `corners` are that rectangle's four "
         "corners after the layer's transform and its parents', in composition pixels (x to the "
         "right, y down, origin at the frame's top-left corner), in the order top-left, "
         "top-right, bottom-right, bottom-left; `bounds` is the axis-aligned box around them and "
         "`anchor` the layer's anchor point, both in composition pixels and not clipped to the "
         "frame. A layer outside its range, disabled or empty at that time answers "
         "visible:false with a reason.",
         "{revision, composition, layer, time:" + std::string(time) +
             ", visible, status? (not-visible|unavailable), reason?, space, local:{left, top, "
             "right, bottom}, bounds:{left, top, right, bottom}, corners:[[x, y] x 4], "
             "anchor:[x, y]}",
         {composition,
          {.name = "layer",
           .type = "integer",
           .required = true,
           .summary = "The layer.",
           .unit = "layer id (query:layers .id)",
           .range = ">= 1"},
          {.name = "time",
           .type = "integer or [numerator, denominator]",
           .summary = "An integer is a whole frame of the composition at its frame rate; "
                      "[numerator, denominator] is exact seconds (concept:time).",
           .unit = "composition time",
           .defaultValue = "the playhead (frame 0 headless)"}},
         R"({"name":"query","arguments":{"kind":"layer-bounds","composition":1,"layer":2,"time":24}})");
    kind("history", "The undo history: every entry's label and where the project is in it.",
         "Entries are oldest first. The first `position` entries are applied; the ones after "
         "that are the redo branch. `undoLabel` is the step the history tool's undo would take "
         "and `redoLabel` its redo. A transact entry carries the call's `label` (\"Agent edit\" "
         "in the desktop application when none was given). The history is bounded, so the oldest "
         "entries retire.",
         "{revision, canUndo, canRedo, undoLabel|null, redoLabel|null, position, "
         "entries:[{index, label, applied}]}",
         {}, R"({"name":"query","arguments":{"kind":"history"}})");
    kind("viewer", "The Viewer's display settings: what viewer.set writes, read back.",
         "Desktop application only. These are display settings; none reaches a render or the "
         "project. `display` and `view` are empty while the project default applies. `zoom` is "
         "\"fit\" or the factor (1.0 is 100 percent). `available` is false when no Viewer is "
         "showing; show one with editor.reveal. `level` (entered group layers, outermost first) "
         "and `points` (the shape in point editing) are reported even then.",
         "{revision, available, level, points|null, display, view, exposure (stops), gamma, "
         "channel (rgba|rgb|red|green|blue|alpha), zoom (\"fit\"|factor), resolution "
         "(auto|full|half|quarter), background (solid|gray|black|white), transparencyGrid, look, "
         "regionOfInterest:{x, y, width, height}|null (composition pixels)}",
         {}, R"({"name":"query","arguments":{"kind":"viewer"}})");
    kind("preview-cache", "RAM Preview: the filled range, fill rate, budget and why it stopped.",
         "Desktop application only. A value the application does not know reads null, never a "
         "made-up number: `fillRate` is null until the Profiler has sampled a fill, and "
         "`stopReason` is null while a fill runs and before any fill has ended. `filledRange` is "
         "the longest run of consecutive cached frames (composition frame indices, `endFrame` "
         "exclusive). `budgetBytes` is the Preview frames pool's ceiling and `heldBytes` what it "
         "holds. `stopReason` is complete (the whole range is cached), budget (the range "
         "outgrew the memory budget; the frames that fit stay cached), cancelled or failed.",
         "{revision, available, state (filling|idle)|null, cachedFrames|null, "
         "totalFrames|null, filledRange:{firstFrame, endFrame, frames}|null, "
         "fillRate (frames per second)|null, budgetBytes|null, heldBytes|null, "
         "stopReason (complete|budget|cancelled|failed)|null}",
         {}, R"({"name":"query","arguments":{"kind":"preview-cache"}})");
    kind("jobs", "Background jobs that are not exports: kind, state, progress and failure reason.",
         "Desktop application only. The Jobs panel's rows without the export runs (export.status "
         "reads those): imports, previews, thumbnails, cache purges and other background work, "
         "newest first, including the ones that have finished. `kind` is the part of Bloom that "
         "owns the job: application, project, composition or panel-request. `state` is queued, "
         "running, succeeded, cancelled or failed. `progress.completed` and `progress.total` read "
         "null while the job reports none (an indeterminate job). `failure` is the reason a "
         "failed job recorded and is null for every other job, and for a failed job that "
         "recorded none. jobs.cancel cancels a job by its id.",
         "{revision, jobs:[{id, name, kind|null, state|null, progress:{phase, completed|null, "
         "total|null}, failure|null, canCancel, cancellationRequested}]}",
         {}, R"({"name":"query","arguments":{"kind":"jobs"}})");
    kind("pixels", "Pixel values of a frame at a time, read from the production render.",
         "Renders the frame exactly as a still export renders it, to a scratch OpenEXR in the "
         "working color space that is deleted before the answer returns, and reads the numbers "
         "from it: they match what an export writes. Nothing is exported, saved or kept, and the "
         "project is untouched. It evaluates the whole frame (heavy), on the request's worker "
         "and not on the application's interface, and it stops when the request is cancelled. "
         "`pixels` is [red, green, blue, alpha] per pixel, row by row from the region's top-left "
         "corner. The values are scene-linear in the `colorSpace` the answer names, with no "
         "display transform and no clamping (they can be negative or above 1), and red, green "
         "and blue are premultiplied by alpha. At most 256 pixels per call (a point is "
         "[x, y, 1, 1]; 16 by 16 is the largest square); for a larger area use capture with "
         "statistics. `gpuFrames`, `cpuReferenceFrames` and `provenance` say which route "
         "rendered it; `routeNote` says why when it ran on the CPU reference path. They read "
         "null when the render recorded no route.",
         "{revision, composition, time:[n, d] (seconds), frame, region:[x, y, width, height], "
         "colorSpace, encoding, order, route, gpuFrames|null, cpuReferenceFrames|null, "
         "provenance (gpu|cpu-reference)|null, routeNote?, width, height, pixels:[[r, g, b, a] "
         "x width*height], job?, log?}",
         {{.name = "composition",
           .type = "integer",
           .summary = "The composition.",
           .unit = "composition id (query:compositions)",
           .range = ">= 1",
           .defaultValue = "the current composition"},
          {.name = "time",
           .type = "integer or [numerator, denominator]",
           .summary = "An integer is a whole frame of the composition at its frame rate; "
                      "[numerator, denominator] is exact seconds (concept:time). The frame "
                      "nearest to it is read. A time outside the composition is refused.",
           .unit = "composition time",
           .defaultValue = "the playhead (frame 0 headless)"},
          {.name = "region",
           .type = "array of 4 integers",
           .required = true,
           .summary = "[x, y, width, height]: the pixels to read.",
           .unit = "composition pixels from the frame's top-left corner",
           .range = "inside the frame; width * height at most 256"}},
         R"({"name":"query","arguments":{"kind":"pixels","time":24,"region":[960,540,1,1]}})");
    kind(
        "scopes",
        "The Scopes panel's count tables, a summary of them, or a picture of a scope, for a frame "
        "or a range.",
        "Measures the frame exactly as the Scopes panel does (the same definitions, the same GPU "
        "and CPU routes) with no panel open, in any session, on the request's worker and not on "
        "the application's interface; it stops when the request is cancelled. The Display signal "
        "goes through the color configuration's default display and view (the pair capture "
        "uses) unless `display` and `view` name another, never through an open Viewer's exposure, "
        "gamma, channel or isolation. The Working signal is the scene-linear picture; "
        "`axisMaximum` M sets the largest linear value the axis shows. `region` is in composition "
        "pixels; one outside the frame answers empty:true. `result` picks what comes back. "
        "tables: the count tables of the scopes asked for, every cell an exact pixel count: "
        "histogram (4 channels x 1024 codes, with under and over per channel), waveform (4 "
        "channels x columns x 512 levels, with under and over per column; columns is the "
        "measured width, at most 512; the parade draws this table's red, green and blue), "
        "vectorscope and chromaticity (512 x 512 each); `shape` and `order` say how `values` "
        "reads, and encoding sparse lists only the non-zero cells as [index, count] pairs. "
        "`counted` is the pixels counted; `counters` has notFinite, noChromaticity and the "
        "pixels outside the vectorscope and chromaticity tables. summary: per channel (red, "
        "green, blue, luma) min, max, mean, p1, p50 and p99 code values and the clipped counts, "
        "and for the vectorscope the mean saturation and hue. image: a PNG of one scope or all "
        "five, drawn as the panel draws them, at `width` (and `height` for one scope); it is "
        "returned like capture returns a picture (embedded when small, else linked) and "
        "written to the capture folder. Every frame reports the `route` that counted it (gpu or "
        "cpu) and why the CPU reference counted a frame the device could not; with the "
        "application's RAM Preview, `fromCache` says a frame's tables came from its warm "
        "sidecar. A read covers one frame, or frame to lastFrame (at most 1000 frames; an image "
        "range at most 16), and reports progress per frame. An answer is limited to about 1.7 "
        "MiB of table numbers: ask for fewer scopes, the sparse encoding, a smaller region, a "
        "summary or an image instead.",
        "{revision, composition, compositionSize:{width, height}, settings:{scopes, signal, "
        "axisMaximum|null, region|null, ignoreTransparent, resolution, display|null, view|null, "
        "result, encoding?, image?, waveformChannels?, histogramChannels?, histogramScale?, "
        "brightness?}, axis, luma:{kr, kg, kb}, primaries:{colorSpace, r:[x, y], g, b, w}, "
        "frames:[{frame, time:[n, d], route (gpu|cpu), routeNote|null, fromCache, empty, "
        "measuredArea:{x, y, width, height}|null, counted, note?, counters?:{notFinite, "
        "noChromaticity|null, outside:{vectorscope|null, chromaticity|null}}, tables?:{histogram?:"
        "{channels, shape, order, encoding, values, underOverShape, under, over}, waveform?:{... "
        "columns}, vectorscope?:{shape, order, encoding, values}, chromaticity?:{...}}, "
        "summary?:{codes, channels:{red|green|blue|luma:{min, max, mean, p1, p50, p99, "
        "clippedUnder, clippedOver}|null}, vectorscope:{meanSaturation, meanHueDegrees, "
        "units}|null}, image?:{path, bytes, digest, width, height, inline}}]}",
        {{.name = "composition",
          .type = "integer",
          .summary = "The composition.",
          .unit = "composition id (query:compositions)",
          .range = ">= 1",
          .defaultValue = "the current composition"},
         {.name = "frame",
          .type = "integer",
          .summary = "The first (or only) frame to read.",
          .unit = "whole frames of the composition at its frame rate",
          .range = "inside the composition",
          .defaultValue = "the playhead's frame (frame 0 headless)"},
         {.name = "time",
          .type = "integer or [numerator, denominator]",
          .summary = "Instead of frame: an integer is a whole frame; [numerator, denominator] is "
                     "exact seconds (concept:time). The frame nearest to it is read.",
          .unit = "composition time"},
         {.name = "lastFrame",
          .type = "integer",
          .summary = "The last frame of a range, inclusive; the answer has one entry per frame.",
          .unit = "whole frames",
          .range = "frame to frame + 999 (image: at most 16 frames)",
          .defaultValue = "the first frame"},
         {.name = "scopes",
          .type = "array of strings",
          .summary = "The scopes whose tables are measured: waveform, parade (the waveform "
                     "table), vectorscope, histogram, chromaticity. A summary reads the "
                     "histogram and the vectorscope; an image measures what it shows.",
          .unit = "scope names",
          .defaultValue = "all five"},
         {.name = "signal",
          .type = "string",
          .summary = "display: the picture after the display and view. working: the "
                     "scene-linear picture before them.",
          .range = "\"display\" or \"working\"",
          .defaultValue = "display"},
         {.name = "axisMaximum",
          .type = "integer",
          .summary = "The working signal's axis maximum M: code 1023 is the value M.",
          .unit = "linear value",
          .range = "1, 2, 4, 8 or 16",
          .defaultValue = "1"},
         {.name = "region",
          .type = "array of 4 integers",
          .summary = "[x, y, width, height]: measure only this part of the frame.",
          .unit = "composition pixels from the frame's top-left corner",
          .range = "width and height at least 1; the part outside the frame is cut",
          .defaultValue = "the whole frame"},
         {.name = "ignoreTransparent",
          .type = "boolean",
          .summary = "Leave out pixels whose alpha is 0.",
          .defaultValue = "false"},
         {.name = "resolution",
          .type = "string",
          .summary = "The resolution the frame is evaluated at; a region is scaled to it.",
          .range = "\"full\", \"half\" or \"quarter\"",
          .defaultValue = "full"},
         {.name = "display",
          .type = "string",
          .summary = "The display signal's OCIO display. A name the configuration lacks is "
                     "refused with the valid ones.",
          .unit = "display name of the project's color configuration",
          .defaultValue = "the configuration's default"},
         {.name = "view",
          .type = "string",
          .summary = "The display signal's OCIO view.",
          .unit = "view name of the display",
          .defaultValue = "the configuration's default"},
         {.name = "result",
          .type = "string",
          .summary = "tables, a summary of them, or a picture.",
          .range = "\"tables\", \"summary\" or \"image\"",
          .defaultValue = "tables"},
         {.name = "encoding",
          .type = "string",
          .summary = "For tables: dense lists every cell; sparse lists [index, count] pairs of the "
                     "non-zero cells.",
          .range = "\"dense\" or \"sparse\"",
          .defaultValue = "dense"},
         {.name = "image",
          .type = "string",
          .summary = "For an image: which scope to draw.",
          .range = "\"waveform\", \"parade\", \"vectorscope\", \"histogram\", \"chromaticity\" "
                   "or \"all\" (the five stacked in the panel's order)",
          .defaultValue = "all"},
         {.name = "width",
          .type = "integer",
          .summary = "For an image: the picture's width.",
          .unit = "pixels",
          .range = "128..2048",
          .defaultValue = "640"},
         {.name = "height",
          .type = "integer",
          .summary = "For an image of one scope: the picture's height. All five take their "
                     "height from the width.",
          .unit = "pixels",
          .range = "128..2048",
          .defaultValue = "the panel's height for the width"},
         {.name = "waveformChannels",
          .type = "string",
          .summary = "For an image: the channels the waveform draws.",
          .range = "\"rgb\" or \"luma\"",
          .defaultValue = "rgb"},
         {.name = "histogramChannels",
          .type = "string",
          .summary = "For an image: the rows the histogram draws.",
          .range = "\"rgb\", \"luma\" or \"rgb-luma\"",
          .defaultValue = "rgb"},
         {.name = "histogramScale",
          .type = "string",
          .summary = "For an image: the histogram's height scale.",
          .range = "\"linear\" or \"logarithmic\"",
          .defaultValue = "linear"},
         {.name = "brightness",
          .type = "number",
          .summary = "For an image: the trace brightness gain (the panel's Brightness).",
          .unit = "gain",
          .range = "0.1..16",
          .defaultValue = "1"}},
        R"({"name":"query","arguments":{"kind":"scopes","composition":1,"frame":24,"result":"summary"}})");
    {
        auto& scopes = out.back();
        scopes.examples.push_back(
            {.surface = "mcp",
             .text =
                 R"({"name":"query","arguments":{"kind":"scopes","frame":24,"result":"image","image":"all","width":640}})"});
        scopes.examples.push_back(
            {.surface = "mcp",
             .text =
                 R"({"name":"query","arguments":{"kind":"scopes","frame":0,"lastFrame":47,"signal":"working","axisMaximum":4,"scopes":["histogram"],"result":"summary"}})"});
        scopes.related.emplace_back("bloom.scopes");
        scopes.errors = {
            makeError("-32602", "composition does not exist, or frame is outside it",
                      "pick a composition and frame the error lists (query:compositions)"),
            makeError("-32602", "a scope name, signal, resolution or result that is not valid",
                      "use one of the names the error lists"),
            makeError("-32602", "display or view the color configuration lacks",
                      "choose one of the valid pairs the error lists, or leave both out"),
            makeError("-32602",
                      "width or height outside 128..2048, height with image all, or "
                      "more frames than a read allows",
                      "use the range the error names, or split the read"),
            makeError("-32602", "the tables would not fit one answer",
                      "ask for fewer scopes, encoding sparse, a smaller region, fewer frames, a "
                      "summary or an image"),
            makeError("bloom.mcp.cancelled", "the request was cancelled",
                      "ask again; nothing was changed", true)};
    }
    kind("render-route", "Which route produced the Viewer's frame: GPU or CPU, and why.",
         "Desktop application only. Reads what the displayed frame itself recorded: `scene` is "
         "where the composition was evaluated and `display` where the display transform ran, "
         "each gpu or cpu. A frame is never reported as GPU unless it recorded a GPU route, and "
         "what it did not record reads null. `provider` names the display arm (cpu-reference, "
         "cpu-ocio, gpu-neutral, gpu-resident or gpu-prepared-packed). `why` is the reason the "
         "frame itself recorded for running part of it on the CPU, and null when it recorded "
         "none. `cpuFallbacks` counts every preview request since the application started that "
         "took the CPU path, by the reason the GPU preview service recorded (gpu-unavailable, "
         "non-neutral-request, not-worthwhile, ineligible, unsupported-gpu-subset, "
         "native-failure, native-timeout); it is a running total, not this frame's reason, and "
         "reads null without a GPU preview service. Renders, exports and captures report their "
         "own route: render, export and capture carry `gpuFrames`, `cpuReferenceFrames`, "
         "`provenance` and `routeNote`. A capture, render or export does not change the "
         "Viewer's frame, so `lastJob` answers for the newest of them: its frame counts and, "
         "for a CPU route, the reason its log recorded. `composition` limits `lastJob` to a "
         "job of that composition.",
         "{revision, available, hasFrame, freshness (current|stale|none), time:[n, d]|null, "
         "scene (gpu|cpu)|null, display (gpu|cpu)|null, provider|null, label|null, why|null, "
         "cpuFallbacks:{total, byReason:{reason: count|null}}|null, "
         "lastJob:{composition, job, gpuFrames, cpuReferenceFrames, provenance, why|null}|null}",
         {{.name = "composition",
           .type = "integer",
           .summary = "Limits lastJob to a job of this composition.",
           .unit = "composition id (query:compositions)",
           .range = ">= 1",
           .defaultValue = "the newest job of any composition"}},
         R"({"name":"query","arguments":{"kind":"render-route"}})");
    kind("context", "The editor context omitted contextual arguments fall back to.",
         "In the desktop application: the Viewer's composition, the playhead, the selection, "
         "the Viewer's display and view and its drill level (the entered groups, outermost first, "
         "and the shape in point editing). On stdio: the session defaults (concept:context).",
         "{revision, live, project:{id, name}, composition|null, selectedNodes, selectedLayers, "
         "level, points|null, time:[n, d] (seconds), display, view, readOnly}",
         {}, R"({"name":"query","arguments":{"kind":"context"}})");
}

// ---------------------------------------------------------------------------------------------
// bloom-cli commands

void addCliCommandEntries(std::vector<ReferenceEntry>& out) {
    const auto command = [&out](std::string name, std::string summary, std::string description,
                                std::initializer_list<ArgumentSpec> arguments, std::string result,
                                std::string_view example, const ReferenceEffects effects) {
        auto value = makeEntry(ReferenceKind::CliCommand, "bloom-cli " + name, "bloom-cli",
                               std::move(summary), std::move(description));
        addArguments(value, arguments);
        value.result = std::move(result);
        value.effects = effects;
        value.examples.push_back({.surface = "cli", .text = std::string(example)});
        out.push_back(std::move(value));
    };
    command("help", "Show the reference index, or one entry in full.",
            "The same reference the MCP help tool and bloom.help() print. --json prints it as "
            "JSON.",
            {{.name = "name", .type = "string", .summary = "The entry; omit for the index."},
             {.name = "--json", .type = "flag", .summary = "Print JSON instead of text."}},
            "The reference text on stdout; exit status 1 when the name matches nothing (the "
            "closest names are printed).",
            "bloom-cli help bloom.layer.add-solid", readOnlyEffects());
    command("new", "Create an empty project file.", {},
            {{.name = "file", .type = "path", .required = true, .summary = "The .bloom file."}},
            "\"created <file>\"; exit status 2 or 3 on failure.", "bloom-cli new scene.bloom",
            fileEffects());
    command("open", "Open a project and print its revision (a validity check).", {},
            {{.name = "file", .type = "path", .required = true, .summary = "The .bloom file."}},
            "\"opened <file> revision=N\"; exit status 2 when it cannot be opened.",
            "bloom-cli open scene.bloom", readOnlyEffects());
    command(
        "run", "Run a JSON operation script or a Python script.",
        "A .json script is an array of {\"op\", \"args\"} objects; each runs as its own "
        "transaction, in a new empty project that is not saved, and export.* calls drive a "
        "headless export queue. A .py script runs in the embedded interpreter, where "
        "bloom.Host(project) opens and saves projects.",
        {{.name = "script",
          .type = "path",
          .required = true,
          .summary = "A .json operation script or a .py Python script."}},
        "\"executed N transaction(s), revision=R\" for JSON; exit status 4 (invalid "
        "operation), 5 (rejected) or 6 (export call refused) names the failing operation.",
        R"(bloom-cli run edits.json   # [{"op":"bloom.composition.add","args":{"name":"Scene"}}])",
        {});
    command("python", "Start an interactive Python prompt with the bloom package.", {}, {},
            "An interactive interpreter.", "bloom-cli python", {});
    command("render", "Render one frame or a frame range of a project's first composition.",
            "Output keeps alpha; the composition background colour is not rendered. Files are "
            "written as <out>/frame.png or frame.exr (a range numbers them).",
            {{.name = "--project",
              .type = "path",
              .summary = "The project; omit to render a new empty project."},
             {.name = "--frame",
              .type = "integer",
              .summary = "One frame (or --range).",
              .unit = "composition frame index"},
             {.name = "--range",
              .type = "A-B",
              .summary = "An inclusive frame range (or --frame).",
              .unit = "composition frame indices"},
             {.name = "--preset",
              .type = "string",
              .required = true,
              .summary = "The image format.",
              .choices = {{"PngRgba8SrgbV1", "PNG, 8-bit RGBA, sRGB"},
                          {"FlatExrRgba32fLinRec709SceneV1",
                           "OpenEXR, 32-bit float RGBA, linear Rec.709 scene"}}},
             {.name = "--out",
              .type = "path",
              .required = true,
              .summary = "The output folder; created when missing."}},
            "The preservation report and \"rendered N frame(s) to <out>\"; exit status 5 on a "
            "render failure.",
            "bloom-cli render --project scene.bloom --range 0-47 --preset PngRgba8SrgbV1 --out "
            "frames",
            fileEffects());
    command("export", "Render a saved project's export queue jobs headlessly.",
            "Exits non-zero unless every selected job succeeds and prints the verification and "
            "import results as JSON. Output keeps alpha; the composition background colour is "
            "not rendered.",
            {{.name = "--project", .type = "path", .required = true, .summary = "The project."},
             {.name = "--all", .type = "flag", .summary = "Every enabled job (or --job)."},
             {.name = "--job",
              .type = "integer",
              .summary = "One job id; repeat for several.",
              .unit = "export job id"},
             {.name = "--replace",
              .type = "flag",
              .summary = "Confirm replacing every existing destination that asks."},
             {.name = "--save",
              .type = "flag",
              .summary = "Write the imports of verified results back to the project."}},
            "Export events as JSON; exit status non-zero when a job fails.",
            "bloom-cli export --project scene.bloom --all --replace", fileEffects());
}

// ---------------------------------------------------------------------------------------------
// Python

void addPythonEntries(std::vector<ReferenceEntry>& out) {
    const auto api = [&out](std::string name, std::string summary, std::string description,
                            std::string example) {
        auto value = makeEntry(ReferenceKind::Python, std::move(name), "python", std::move(summary),
                               std::move(description));
        value.examples.push_back({.surface = "python", .text = std::move(example)});
        out.push_back(std::move(value));
    };
    api("bloom.help", "Print the reference index, or one entry in full.",
        "bloom.help(name=None, *, json=False). With json=True it returns the entry as a dict "
        "instead of printing.",
        "bloom.help(\"query:layers\")");
    api("bloom.ops", "Every operation as a Python function: bloom.ops.<domain>.<verb>.",
        "bloom.ops.layer.add_solid is bloom.layer.add-solid. Arguments bind positionally or by "
        "keyword; contextual arguments are keyword-only and default to bloom.context. Outside a "
        "transaction group each call is its own undo step and returns the result mapping.",
        "bloom.ops.layer.add_solid(name=\"Red\", color=(1, 0, 0, 1))");
    api("bloom.transactions.group", "Group operation calls into one atomic undo step.",
        "with bloom.transactions.group(label, expected_revision=None): ... The calls inside are "
        "queued and applied together when the block exits; `result` holds the transaction "
        "result. bloom.transactions.undo() and redo() step the history.",
        "with bloom.transactions.group(\"Title\") as edit:\n"
        "    bloom.ops.layer.add_text(name=\"Title\", text=\"Hello\")");
    api("bloom.context",
        "The editor context: project, composition, selection, layers, time, Viewer level.",
        "Read-only. time is a Fraction of seconds. level is the Viewer's entered group layers, "
        "outermost first (empty at the top level), and points the shape layer in point editing "
        "or None; both are empty headless.",
        "bloom.context.composition");
    api("bloom.viewer.set_level", "Set the desktop Viewer's drill level, as a double-click does.",
        "bloom.viewer.set_level(layers, points=None): layers are group layers from the top, "
        "outermost first ([] is the top level); points is a shape layer of that level whose "
        "points are edited. Session state: never saved, never undone. Raises OperationError with "
        "unknown-target, invalid-argument or, headless, not-live.",
        "bloom.viewer.set_level([group, inner])");
    api("bloom.data", "Read the project: compositions, assets, layers, nodes, parameters, curves.",
        "bloom.data.layers(composition), bloom.data.curves(composition) and the other "
        "collections return proxies whose attributes read the current snapshot. "
        "bloom.data.merge_entries(composition) lists every Merge entry (#502) with its layer "
        "(layer_kind node, entry or none), parameter ids, values and adjustment, shared and "
        "nested-Merge markers.",
        "bloom.data.layers(bloom.context.composition)");
    api("bloom.render", "Render a frame or a range to image files.",
        "bloom.render.frame(number, *, out, composition=None, preset=...) and "
        "bloom.render.range(first, last, *, out, ...). Output keeps alpha; the composition "
        "background colour is not rendered.",
        "bloom.render.frame(0, out=\"frames\")");
    api("bloom.scopes", "Read the Scopes panel's tables, a summary or a picture of a scope.",
        "bloom.scopes.read(frame=None, *, last_frame=None, composition=None, time=None, "
        "scopes=None, signal=\"display\", axis_maximum=1, region=None, ignore_transparent=False, "
        "resolution=\"full\", display=None, view=None, result=\"tables\", encoding=\"dense\") "
        "measures the frames exactly as query:scopes does (the arguments are the same, in "
        "snake_case) and returns the same answer as a dict with snake_case keys: settings, luma, "
        "primaries and frames, each frame with its route, counted, counters and tables or summary. "
        "bloom.scopes.image(out, frame=None, *, image=\"all\", width=640, height=None, ...) "
        "writes the PNG to the path `out` (for a range, one numbered file per frame, as "
        "bloom.render.range names them) and returns the same dict with each frame's path. "
        "A read inside bloom.tasks.submit reports progress per frame and stops when the task is "
        "cancelled (CancelledError). A bad argument raises ValueError with the remedy; a project "
        "that cannot be measured raises RuntimeError.",
        "bloom.scopes.read(24, result=\"summary\")[\"frames\"][0][\"summary\"]");
    api("bloom.events", "Subscribe to command events.",
        "bloom.events.subscribe(callback, kinds=...) and bloom.events.pump().",
        "bloom.events.subscribe(print)");
    api("bloom.tasks", "Run cancellable background work with progress.",
        "bloom.tasks.submit(function, *args) returns a TaskHandle with progress, cancel() and "
        "result().",
        "bloom.tasks.submit(work)");
}

} // namespace bloom::scripting::detail
