# Scopes

The Scopes panel (`bloom.scopes`, issue #724) shows five video scopes of the picture a Viewer shows:
Waveform, Parade, Vectorscope, Histogram and CIE chromaticity. This document is the specification
of what is measured and how, and it is the owning document of the coverage contract entry
`prep.scopes.result_readback`.

## What is measured

One definition, whatever way a frame was reached (paused, scrubbed, played, cached or not, on the
device or on the CPU). A measure counts the pixels of a measured area of one float picture, the
signal, into count tables. Every cell of a table is a whole number of pixels.

### The signal

| Signal | The picture that is counted |
|---|---|
| Display (default) | Every pixel's straight color through the Viewer's display and view, then the Viewer's exposure and gamma when they are not neutral, then multiplied by the pixel's alpha again |
| Working | The scene-linear process picture as it is stored: its color over black and its alpha |

The signal is taken over black: red, green and blue are the color times alpha. With a neutral
exposure and gamma the display transform's float result is kept as it is, so a value the display
encoding leaves below 0 or above 1 is counted as under or over. An exposure or gamma that is not
neutral clamps to 0..1, as the Viewer shows it. A pixel with no alpha is stored as it is.

The Display axis is 10-bit code values, 0 to 1023. The Working axis has a maximum M chosen from 1,
2, 4, 8 and 16; changing M measures again.

### The measured area

The picture's display window, cut to the frame's region of interest when it has one and to the
scope region when one is set (`scopeMeasuredArea`, `scope_measure.hpp`). The scope region is in
composition pixels and is scaled to the picture outward (its first pixel floored, its end raised),
so a region drawn at full resolution measures the same part of a half or quarter resolution
picture. A pixel of the area outside the picture's data window is transparent black. A region
outside the picture has nothing to count, and the result says so.

### Per pixel

All arithmetic is binary32, in the order written, without contraction.

1. Any of red, green, blue or alpha not finite: the pixel is counted in no table and adds 1 to
   `notFinite`.
2. With Ignore Transparent Pixels, alpha 0: the pixel is not counted at all.
3. Otherwise the pixel adds 1 to `counted` and to one cell of every table asked for.

Luma is `y = (Kr r + Kg g) + Kb b`. The coefficients are the Y of the unit red, green and blue of
the signal's color space, normalized to sum 1, measured through the same OCIO transform the CIE
scope uses (`ScopeXyzProcessor`, `scope_colorimetry.hpp`), never taken from a table chosen by name.
Rec. 709 primaries give 0.2126, 0.7152, 0.0722.

The code of a component `v` is `c = floor((v * (1 / M)) * 1023 + 0.5)`. Below 0, and a value that is
not a number, is `under`; above 1023 is `over`.

| Table | Cells | Where a pixel goes |
|---|---|---|
| Histogram | 4 channels (red, green, blue, luma) of 1024 bins, under and over | Bin `c` of each channel |
| Waveform (and Parade) | 4 channels of `columns` columns of `512 >> d` levels, under and over; `columns = min(512 >> d, W)` for a measured width `W` | Column `floor(x * columns / W)`, level `(c >> 1) >> d` |
| Vectorscope | `512 >> d` by `512 >> d` | `cb = (b - y) / (2 (1 - Kb))`, `cr = (r - y) / (2 (1 - Kr))`; full cell `(floor((cb + 0.5) * 512), floor((cr + 0.5) * 512))`; outside 0..511 adds 1 to `vectorscopeOutside`, otherwise the pixel adds to the full cell shifted right by `d` on each axis |
| CIE chromaticity | `512 >> d` by `512 >> d` | `(X, Y, Z)` is the signal converted to CIE XYZ with a D65 white; `s = (X + Y) + Z`; not finite or `s <= 0` adds 1 to `noChromaticity`; otherwise `x = X / s`, `y = Y / s`, full cell `(floor(x / 0.8 * 512), floor(y / 0.9 * 512))`; outside 0..511 adds 1 to `chromaticityOutside`, otherwise the pixel adds to the full cell shifted right by `d` on each axis |

`d` is the measure's detail, 0, 1 or 2 (`ScopeMeasureSettings::detail`, `render::kScopeMaxDetail`);
0 is full detail and every statement above is for it. A pixel's cell on an axis is always decided at
full detail (0..511, or outside) and then shifted right by `d`, so "outside" never depends on the
detail. At detail `d`, the Vectorscope and CIE tables equal the full tables with each `2^d x 2^d`
block of cells summed, and each Waveform column's levels equal sums of `2^d` adjacent full-detail
levels (of a Waveform measured with the same number of columns; the column count is
`min(512 >> d, W)`, so a detail of 1 or 2 has at most 256 or 128 columns). The eight counters and
the Histogram (1024 bins, under and over) are the same at every detail, so the percentiles, means
and clip counts of a summary come from the same numbers; a summary's Vectorscope mean saturation
and hue are taken from the centers of the detail's cells, which moves a pixel by at most half a
cell: the saturation differs from full detail's by at most `sqrt(2) * 2^d / 512` (as a fraction of
the graticule circle's radius). The live panel measures at detail 0; MCP and Python reads always
measure at detail 0 and take no detail argument.

The Parade draws the Waveform table's red, green and blue channels side by side; it has no table
of its own. The two Vectorscope denominators are rounded once to binary32 on the host, so both
routes divide by the same number. The layout of the tables as 32-bit words is
`render/scope_table_layout.hpp`.

Gray and the fully saturated primaries sit exactly on the edge between two Vectorscope cells (the
center is the corner of cells 255 and 256), so which of the two cells a gray pixel lands in is
decided by the last bit of its value. A test asserts the count of a neighborhood, never one cell.

## Two routes, one result

| Route | How |
|---|---|
| Device | A job of the final-render evaluator (`GpuProcessFrameEvaluator::measureScopes`, `gpu_scope_measure.hpp`). The frame's scene runs on the device as a final frame does. The resident picture is converted to the signal and, for the CIE table, to XYZ by two OCIO programs, counted by the fixed embedded kernel `ScopeTablesV1` (`render::GpuScopeTables`), and only the tables are read back |
| CPU reference | The frame is evaluated by the CPU evaluator and counted by `measureScopesCpu` (`scope_measure.hpp`) |

`measureScopeFrame` (`scope_frame_measure.hpp`) is the one call for a frame. It measures on the
device when the frame's route is the device and the device can, and with the CPU reference
otherwise. A result always names the route that counted it. A frame the device could not count (no
device, a scene outside the prepared-GPU subset, no room, a pixel that is not finite, which the
device's color step refuses) is counted by the CPU reference, labeled CPU, with the reason. A scope
is never silently missing.

The kernel adds whole numbers with `atomicAdd` on an unsigned storage buffer for the counters, the
Histogram, the Vectorscope and the CIE table; integer adds commute, so the result does not depend
on how the device schedules the work. The Waveform needs no atomics between columns: one invocation
owns one column of the table and walks the pixels that fall in it.

### Exactness

For the same float picture the two routes give equal tables, cell for cell. The kernel runs the
CPU reference's operations in the same order with `precise` arithmetic, writes constants that are
not exact in decimal as bits, and corrects every device division to the correctly rounded quotient
the CPU computes (a device's own float division is not required to be correctly rounded, and is
not on every device). `bloom.runtime.scope_measure_gpu_parity` proves this on a real device with
pictures that put pixels on cell edges.

The display transform and the conversion to XYZ are OCIO's CPU and GPU processors, which round
differently. Through those steps a pixel may land in the neighboring cell: every pixel is still
counted, and along every table axis no pixel is more than one cell from where the CPU reference
puts it (`bloom.runtime.scope_frame_measure_native`). The Working signal's Histogram, Waveform and
Vectorscope pass through no OCIO step and are exact.

The device route measures the picture the final-render evaluator computes, not a readback of the
Viewer's own device picture: the scene of a measured frame therefore runs on that evaluator's
device as well as on the Viewer's.

## The scope picture

A scope has one Qt-free definition of what it looks like, in `bloom/runtime/scope_picture.hpp`. The
Scopes panel and the headless scope image (MCP and scripting sessions have no Qt) both draw from it,
so they cannot drift.

Shared by the panel and the headless image:

- **The trace mapping.** Count tables to a premultiplied 8-bit image for the Waveform, Parade,
  Vectorscope, Histogram (linear and logarithmic) and CIE chromaticity: the brightness
  `log2(1 + n) / log2(1 + m)` with `core::specifiedLog2`, the visible floor, the clip summary and
  the cancellation checks. The panel wraps the buffer in a `QImage` and maps on a task worker; the
  headless image maps in place.
- **The graticule plan.** From the scope kind, a logical size, a device pixel ratio, the settings
  that affect drawing (signal, axis maximum, channels, scale), the luma coefficients, the
  colorimetry and a text-measuring callback, one function returns everything a scope shows besides
  its trace, in logical coordinates: the plot and trace rectangles, hairlines, polylines (the
  spectral locus, the gamut triangle), circles (the Vectorscope ring, the white point), filled clip
  markers and labels, each with a color role and, for a primitive, whether it sits under or over the
  trace. The geometry is the panel's own; the panel computes none of it.
- **The palette.** The color roles a scope uses: canvas, graticule quiet, stronger and strongest,
  label, faint, warning and the four channel colors. The panel fills it from the interface tokens.
  The headless image uses `defaultScopePalette()`, whose values `bloom.ui.scopes-picture` ties to
  those tokens one by one. The spacing and tick sizes the geometry uses (`ScopeMetrics`) are tied
  the same way.

What differs:

- The panel paints the plan as vector drawing with `QPainter` and the interface fonts, crisp at
  every scale. The headless image rasterizes it: lines are one device pixel, antialiased where the
  plan says so, and labels are rasterized with the embedded Inter Medium face (the embedded faces
  have no monospaced one, so numbers use the same face as names), by `renderScopePicture` for one
  scope and `renderAllScopesPicture` for the five stacked in the panel's order, each as tall as the
  panel's `heightForWidth` gives for that width.
- The panel's geometry reads its own font metrics; the headless image's reads the embedded face's.
  A label may therefore sit a pixel or two differently, and the plot may be a few pixels wider or
  narrower. A caller that needs the panel's geometry in a headless picture passes the panel's text
  measure in `PictureOptions`.
- The panel dims and marks a scope "Behind" while it shows another frame's tables; a picture always
  shows the tables it is given.

`encodeScopePng` turns the buffer into PNG bytes (8-bit RGBA, an sRGB chunk, one IDAT stream,
straight alpha) with the qualified zlib, independent of any project or export job. Every picture
call takes a cancellation token and a byte limit and returns an error value, never an exception.

## The table readback

`prep.scopes.result_readback` is a named, bounded readback in the audited host-preparation list of
the coverage contract (`gpuApprovedCpuPreparation()` in `gpu_coverage_contract.hpp`). It covers
only this: the count tables of one scope measure, read back once per measure.

- What is read back is at most 1,581,072 unsigned 32-bit words (8 counters, the Histogram's 4,104,
  the Waveform's 1,052,672 at 512 columns, and 262,144 each for the Vectorscope and the CIE table):
  6,324,288 bytes, whatever the size of the picture. This is the contract entry's bound, the
  full-detail worst case. A measure that asks for fewer scopes reads back fewer, and a measure at
  detail 1 or 2 counts into smaller tables on the device and reads back the smaller tables (the
  table buffer, its staging copy and the host copy are sized from the detail's layout, and the
  request's budget is the detail's, not the full tables'). Only the tables asked for are
  allocated, counted and read back; a measure without the CIE table runs no XYZ color program and
  binds no XYZ picture. `ScopeFrameMeasure::tableReadbackBytes` reports the bytes a measure read
  back (the layout's bytes; 0 for the CPU reference).

Bytes read back by one measure, on a picture at least 512 wide (the Waveform has `min(512 >> d, W)`
columns, so a narrower picture reads back less; the counters are 32 bytes):

| Tables asked for | Detail 0 | Detail 1 | Detail 2 |
|---|---:|---:|---:|
| Histogram | 16,448 | 16,448 | 16,448 |
| Waveform | 4,210,720 | 1,056,800 | 266,272 |
| Vectorscope or CIE | 1,048,608 | 262,176 | 65,568 |
| All four | 6,324,288 | 1,597,504 | 413,760 |

Passes of one measure:

| Pass | Histogram alone | Every scope |
|---|---|---|
| The frame's scene | 1 | 1 |
| The display color program (the Display signal only) | 1 | 1 |
| The XYZ color program (only with the CIE table) | 0 | 1 |
| The count kernel's pixel pass | 1 | 1 |
| The count kernel's column pass (only with the Waveform) | 0 | 1 |
| Table readback | 1 | 1 |

The Working signal runs no display color program. The passes do not depend on the detail; the
detail changes only the bytes of the one table readback and the table memory.
- The picture is never read back, and no per-node or per-operation full-frame roundtrip exists.
  The scene, both color steps and the counting are device work; the measure reports its picture
  readbacks, which are always zero, and the test asserts it.
- It is not an exemption of any pixel operation, feature or route: every operation of the measured
  frame stays Required device work.

The entry is the decision O1 of the Scopes specification of issue #724. Like
`prep.statistics.result_readback` it needs the owner's approval; it lands only with that approval.

## Real-time cost

The owner allows the tables' readback if updating them in real time is cheap. It is shown by
structure and by tests of that structure; no timing is measured or asserted anywhere.

**What runs for one shown frame.** The frame is measured by one cancellable task on a task worker,
never on the interface thread, in two steps: at the live detail (detail 1) first and, when that
frame is still the one shown, once more at full detail (detail 0).

| Step | Device route | CPU route |
|---|---|---|
| The frame's scene | Run again on the final-render evaluator (the shown picture was prepared by the Viewer's own service; the measure does not reuse it) | Evaluated again by the CPU evaluator |
| Color programs | The display program (Display signal), the XYZ program (only with the CIE table) | Per pixel on the worker |
| Counting | The pixel pass and, with the Waveform, the column pass of the kernel | `measureScopesCpu` |
| Readback | One table readback, the layout's bytes at the measure's detail; no picture | None |
| After | Packing and the trace mapping on task workers | The same |

On the device route a measured frame's scene therefore runs a second time, on the final-render
evaluator's device, beside the run that prepared the Viewer's picture. That is the price of never
reading the picture back and of never delaying the Viewer's frame.

**Bytes read back per measure** are the table in "The table readback": for a picture at least 256
wide, every section is 1,597,504 bytes at the live detail and, for a picture at least 512 wide,
6,324,288 at full detail (4,218,944 for a picture 256 wide); one section reads back only its own
table (the Histogram 16,448 bytes at either detail). Only the open sections are measured, so a
collapsed section's table is neither counted nor read back.

**Bounds.**

- One measure is in flight per controller, for the shown frame and the RAM preview fill together.
  Cancelling it does not free the slot: the slot is free when its task has finished. The device
  side agrees: the kernel's table slot is one per process, and a second measure of the process
  that began while one was in flight would be refused as busy (`gpu_scope_tables.cpp`).
- Latest wins. While a measure is in flight a newer shown frame, a settings change or a view
  adjustment only replaces the one waiting request; the frames in between are never measured
  (`measuresSkipped` counts them). A measure in flight for a frame or identity that is no longer
  wanted is cancelled and observes that between its steps; its result is neither shown nor cached.
  Nothing queues beyond the one waiting request.
- A frame replaced before its live tables arrive is never measured at full detail, so playback and
  scrubbing read back live tables only; a frame that stays ends at full detail without any playback
  signal.
- A RAM preview fill measures retained frames at full detail, one at a time, in the same slot and
  only when no shown-frame request waits or is in flight. A fill measure already running when a
  shown frame arrives is finished first (it is for a frame still wanted); the shown frame's request
  waits for it.

**What never happens.** No wait on the Viewer's frame, fence or presentation. Nothing on the
interface thread: the measure, the packing and the mapping run on task workers and the controller
only submits, polls and publishes. No picture readback.

**Code facts for the device route** (read from the code; the Viewer's wait-free property is shown
by structure, not timed).

- (a) The Viewer and the measure use different devices and queues. The Viewer's frames come from
  the preview display service, which creates its own `GpuDevice` on its own service thread
  (`gpu_preview_display_service_jobs.cpp:257`, thread started at `gpu_preview_display_service.cpp:565`;
  the application runs it with presentation `Disabled`, `apps/bloom/main.cpp:372-373`, so it presents
  on no Vulkan queue). The measure runs on the final-render evaluator, which the application's
  `GpuExportProvider` creates (`apps/bloom/main.cpp:609`, `gpu_export_provider.cpp:309`); its owner
  thread creates another `GpuDevice` (`gpu_process_frame.cpp:692`). Each device gets its own
  `VkDevice` and its own compute queue (`gpu_device_bootstrap.cpp:661-665`), so no queue, fence or
  command pool is shared. What the two share is the memory accounting of the processing-phase
  policy (`gpu_processing_phase_policy.cpp`), which ranks the Viewer first (`viewerPriority`); a
  measure that finds no room is counted by the CPU reference instead (`AwaitingCapacity` maps to
  Unavailable, `gpu_process_frame.cpp:262-266` and `gpu_process_frame_scope.cpp:397`),
  and never makes the Viewer wait.
- (b) The table readback's fence is polled on the evaluator's owner thread
  (`gpu_scope_tables.cpp:785` refuses a poll from any other thread; `runScopeMeasure` is called
  from the owner's request run, `gpu_process_frame.cpp:483`). The task worker that asked for the
  measure waits on its own request's condition variable only (`gpu_process_frame.cpp:1261-1271`),
  and that worker is a scheduler thread of the measure's task (`scopes_controller.cpp:282`), neither
  the interface thread nor a thread the Viewer's service waits on. The one mutex around the table
  slot (`gpu_scope_tables.cpp:147`, taken at 322, 338, 431 and 526) guards a state flip only and is
  never held across the submit or the fence poll; the Viewer's service never touches it.
- (c) The readback is asynchronous: the copy is recorded and submitted with a fence
  (`vkQueueSubmit`, `gpu_scope_tables.cpp:746`) and the owner then polls the fence's status
  (`vkGetFenceStatus`, `gpu_scope_tables.cpp:795`, in the loop of `gpu_process_frame_scope.cpp:325-340`
  that sleeps in 50 microsecond steps and observes cancellation and the deadline). No device call
  blocks on the copy; there is no `vkWaitForFences` in the scope tables code.

**Tests of the structure.** `bloom.ui.scopes-controller`: no second measure starts while the first
is held, the greatest number in flight is 1, four of five frames are skipped, the newest frame's
live result is shown and cached and the held frame's is neither, the newest frame then ends at full
detail; live then full for a frame left alone (the full tables equal a direct full measure, the
live ones equal them pooled), a frame replaced while held never gets a full measure, a detail-0
sidecar is shown with no measure and a detail-1 sidecar is shown at once with one full measure
following. `bloom.ui.scopes-realtime`: with a visible panel and the measure held, a session
stepped through several frames is published and shown by the preview and the Viewer each time, the
held measure ran on a worker, the interface thread's event loop kept turning, and one measure was
in flight throughout. It runs the CPU routes and so cannot show the device; (a) to (c) above and
the bytes asserted by `bloom.runtime.scope_frame_measure_native` cover that side. A test that a
second device completes a frame while a measure is in flight on the first would need a seam that
holds the readback in flight, which the production code does not have, and is not built.

## Reading scopes from MCP and Python

`scripting/scope_read.hpp` is the one Qt-free read behind MCP's `query` kind `scopes` and Python's
`bloom.scopes`. It validates the request on the calling thread, then runs one task on the caller's
scheduler that resolves the color configuration, chooses the display and view (the configuration's
own defaults, never the Viewer's inspection settings), compiles the plan once and measures each frame
through `measureScopeFrame` on the same routes and with the same CPU/GPU parity as the panel. A
frame whose tables the desktop application's preview cache already holds under the same frame
identity and the same measure settings is taken from that sidecar instead of measured (the
`McpLiveUiBinding` port answers only for a Viewer state of no region of interest, isolation or
channel view). The caller's cancellation is polled while the task runs and reaches inside a frame.
Tables are described as flat arrays (dense, or the non-zero cells as index and count pairs), a summary
is `summarizeScopes` of the tables, and a picture is `renderScopePicture` or `renderAllScopesPicture`
encoded with `encodeScopePng`, so a picture equals what the panel draws.

## Sidecars and identity

A frame's count tables are kept beside the frame in the preview cache as a sidecar
(`PreviewScopeSidecar`, `preview_frame_cache.hpp`): the packed tables, the measured area, the route
that counted and, for a frame the device could not count, the reason.

- The sidecar's identity is the frame's `PreviewFrameCacheKey` plus `PreviewScopeIdentity`: the
  measure settings (which scopes, the signal and its axis maximum, the region, Ignore Transparent
  Pixels and the detail) and the Viewer's exposure and gamma. A frame has at most one sidecar.
- The panel's measure has two details of one frame: the live one (detail 1) and the full one
  (detail 0). The panel's settings carry detail 0; the controller measures the shown frame at the
  live detail first and replaces the live sidecar by the full one when it arrives. A sidecar of the
  shown frame at detail 0 is shown at once with no measure; one at detail 1 is shown at once and the
  full measure follows.
- A scope setting replaces the sidecar and leaves the frame's entry alone. An edit of the document
  gives the frame another cache key, so the frame and its sidecar are both new. A sidecar leaves
  the cache with its frame.
- A sidecar's bytes are charged to the cache's byte budget and reported with the frame bytes
  (`scopeSidecarBytes`, part of `hostBytes`). Tables are packed to the narrowest whole-number
  width that holds their largest count (`ScopeTables`). A sidecar that does not fit is not kept.
- A cached frame without a matching sidecar is measured again from the frame's own plan and
  request through `measureScopeFrame`, never from the packed 8-bit picture, and gives the tables a
  cold frame gives. A sidecar of another detail is another identity: MCP and Python reads measure
  at detail 0 and are never served a sidecar of another detail.

The sidecar is measured by a cancellable task of its own after the frame is shown or retained,
not inside the frame's preparation: a frame is never delayed by its scopes, and a hidden panel
costs nothing. The device route therefore runs the frame's scene again on the final-render
evaluator.

## The panel

`ScopesController` (`scopes_controller.hpp`) is told which frame the followed Viewer shows and
which measure the panel wants. It takes the tables from the frame's sidecar when the identity
matches, and otherwise measures on a task worker and attaches the result. One measure is in flight
per controller and the newest request replaces the one waiting ("Real-time cost"); a superseded
measure is cancelled and its result is neither shown nor cached. The shown frame is measured at the
live detail (`kLiveDetail`, detail 1) first and, when it is still the shown frame under the same
settings, once more at full detail, whose tables replace the live ones in the reading and in the
frame's sidecar. `ScopesReading::detail` says which tables are on show, and the status line says
"Live detail" while it is not 0. The panel's settings keep detail 0: the detail is the
controller's choice. While another frame's tables are on show the reading is marked behind and
names the frame that was measured. During a RAM preview fill with a panel open, each frame the
cache retains is measured at full detail in the background, one at a time and only when no
shown-frame request waits or is in flight; frames still waiting when the fill ends are measured
too, and hiding the panel or closing every scope drops them.

`ScopesEditor` (`bloom.scopes`) is the panel: the header, the status line and five collapsible
sections whose collapse state is kept per panel area. A hidden panel and a collapsed section ask
for no measure. Mapping tables to trace images runs on a task worker (`ScopesTraceRenderer`); a
Brightness change maps again and does not measure again. Nothing of the measure or the mapping
runs on the interface thread.

## The scope region

`ScopesRegionStore` holds one region per project and composition, the armed flag of the Region
toggle and the count of visible Scopes panels. It is session state: it holds no reference to the
document, is not saved, is not an undo step and does not change rendering, export or the Viewer's
region of interest. The region is whole composition pixels clipped to the frame;
`scopeMeasuredArea` scales it to the resolution measured.

Every Viewer of the composition draws the region as an overlay labeled "Scopes" while a Scopes
panel is visible, on the widget and on the device presentation alike. The Viewer takes input for
it only while the toggle is armed (draw a rectangle, or move the region by its body) or when the
press is on one of its handles (resize), so layer moves, marquee selection and the region of
interest tool are unaffected. The store is written when a drag ends.

## Budget, cancellation and retirement

The measure is part of its frame's request. The tables, their staging copy and their host copy are
in the request's output envelope, the color programs are prepared on the calling worker and never
on the device owner, the job observes the request's cancellation between its steps, and nothing is
published after a cancel. Every native resource is kept until its submission's fence is proven
retired; a submission that could not be proven is retired by the evaluator's idle owner as the
other stages' are. The measure holds no state between frames.

## Tests

| Test | What it proves |
|---|---|
| `bloom.runtime.scope_measure` | The CPU reference: known answers for every table, the counters, areas, columns, the axis maximum, the packed form, the summary and the measured area; at detail 1 and 2 the Vectorscope, CIE and Waveform tables equal the full tables pooled, the counters and the Histogram are equal, the layout's exact byte counts, and detail 3 is refused |
| `bloom.render.gpu_scope_tables` | The kernel on a real device at every detail: every count against a host count of the same pictures, the bytes read back equal to the layout's, and its refusals (no XYZ picture, a detail above 2, a budget below the detail's tables, a budget that fits a detail but not full detail) |
| `bloom.runtime.scope_measure_gpu_parity` | CPU and device tables equal for the same float pictures at every detail, cell edges included |
| `bloom.runtime.scope_picture` | The Qt-free picture: a picture from detail-1 and detail-2 tables has the size and graticule of full detail's and a trace over the same region, the plan's facts for each scope (Waveform codes, Vectorscope ticks, targets and skin line, CIE grid, labels, triangle and printed lines, Parade panes, Histogram rows and clip markers), trace pixels equal to the mapping over the canvas, the visible floor, Histogram heights, the picture of all five, PNG bytes decoded back, and the cancelled, byte-limit and size refusals |
| `bloom.ui.scopes-picture` | The panel and the picture agree (hover readouts at detail 1 report the detail cell's count): palette and metrics equal the interface tokens, an offscreen grab and the picture agree on the trace rectangle of each scope, and the plan's hairlines are where the grab has graticule pixels |
| `bloom.color.scope-colorimetry` | The XYZ transform, the luma coefficients and the primaries of the built-in configs |
| `bloom.runtime.scope_frame_measure_native` | A frame through the production device route against the CPU reference: exact for the Working signal, within one cell through the OCIO steps, one table readback and no picture readback, the route named; a measure at detail 1 reads back the layout's bytes, and a Histogram-only measure its 16,448 bytes and runs no XYZ program; the bytes of one section and of every section at the live and at full detail |
| `bloom.scripting.scope-read` | The headless read: tables equal the CPU reference cell for cell for both signals, counters, luma and primaries, sums, sparse equal to dense, the summary, pictures, a range with progress and a deterministic cancel, the warm-sidecar port, region, transparency, resolution, display and view, and every refusal with its remedy |
| `bloom.mcp.scope-query` | `query` kind `scopes` on the shared core: the tables equal the read's, sparse, summary, image and range answers, and the refusals |
| `bloom.scripting.python-scopes` | `bloom.scopes.read` and `image` from Python: a flat color's tables, sparse equal to dense, summary, range and time, written and replaced files, argument errors |
| `bloom.ui.scopes-mcp-warm` | The MCP read against a real preview and cache: a sidecar under the read's own settings is answered from the cache with no measure; every setting (a sidecar of another detail included), region, isolation, look or composition that differs is measured |
| `bloom.scripting.scope-read-native` | The headless read on a real device: route Gpu, Working tables equal the CPU reference, CIE and Display tables within one cell; a disabled provider reads with the CPU reference and says why |
| `bloom.color.scope-colorimetry-external` | The XYZ transform of an external config through the helper process equals the built-in's; spaces with no XYZ interchange are refused with their reason |
| `bloom.ui.scopes-controller` | The controller over CPU frames: one measure in flight, latest wins with skipped frames counted, live then full, a replaced frame never measured in full, sidecars of both details, cancellation, the fill |
| `bloom.ui.scopes-realtime` | A real preview, Viewer and panel with the measure held: every stepped frame is published and shown, the measure runs on a worker, the interface thread's loop keeps turning, one measure in flight (CPU routes) |
| `bloom.ui.scopes-region` | Includes a project saved with a scope region equal, byte for byte, to one saved without |
| `bloom.ui.viewer_resident_scopes_region_native` | The scope region and its handles are drawn over the device-presented Viewer picture (needs a window system and a device) |

The device tests skip without a device and fail under `--require-device`.
