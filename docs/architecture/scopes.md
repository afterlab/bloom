# Scopes

The Scopes panel (`bloom.scopes`, issue #724) shows five video scopes of the picture a Viewer shows:
Waveform, Parade, Vectorscope, Histogram and CIE chromaticity. This document is the specification
of what is measured and how, and it is the owning document of the coverage contract entry
`prep.scopes.result_readback`.

## What is measured

One definition, whatever way a frame was reached (paused, scrubbed, played, cached or not, on the
device or on the CPU). A measure counts the pixels of a measured area of one picture, the signal,
into count tables, and every cell of a table is a whole number of pixels. The picture is a float
picture, except for the one source that counts a retained 8-bit display picture ("The sources of a
panel measure").

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
| Device | Two OCIO programs convert the resident float picture to the signal and, for the CIE table, to XYZ; the fixed embedded kernel `ScopeTablesV1` (`render::GpuScopeTables`) counts it, and only the tables are read back. It counts the scope tap on the Viewer's device, and, for a headless read, a job of the final-render evaluator (`GpuProcessFrameEvaluator::measureScopes`, `gpu_scope_measure.hpp`) that runs the frame's scene |
| CPU reference | `measureScopesCpu` (`scope_measure.hpp`) counts a float picture on a task worker |

A result always names the route that counted it, and a scope is never silently missing.

The kernel adds whole numbers with `atomicAdd` on an unsigned storage buffer for the counters, the
Histogram, the Vectorscope and the CIE table; integer adds commute, so the result does not depend
on how the device schedules the work. The Waveform needs no atomics between columns: one invocation
owns one column of the table and walks the pixels that fall in it.

### The sources of a panel measure

The panel never evaluates a scene: it counts a picture the Viewer already has. For a frame it takes
the first source that measures, in this order (`makeScopesMeasure`, `scopes_controller.cpp`):

| Order | Source | Picture | Route | `ScopeSource` |
|---|---|---|---|---|
| 1 | The scope tap | The scene-linear RGBA32F picture the Viewer's own frame job computed on the Viewer's device (`GpuPreviewDisplayService::measureScopeTap`) | Device | Rendered |
| 2 | The process picture | The Float32 process picture of a frame prepared by the CPU route, which the frame still holds (`measureScopeFrameCpu`) | CPU | Rendered |
| 3 | The retained picture | The packed RGBA8 display picture the preview cache retains (`measureScopeFrameRetained`) | CPU | Retained |

Sources 1 and 2 give both signals. Source 3 gives the Display signal and the CIE table only.
When source 1 is tried and does not measure (skipped, busy or failed), the frame is counted from
the next source that can, so the panel keeps updating.

**The scope tap** (`gpu_preview_display_service_scope.cpp`). The Viewer's frame job computes the
scene-linear picture on the service's device. When the frame is published the service keeps an
alias of that picture. There is one Viewer slot, which holds the latest Viewer-class frame
(Interactive or Visible priority) and stays for repeated measures, and one fill slot for a RAM
preview fill frame submitted with `tapForScopes`, which is measured once ("Real-time cost"). A tap
is named by `GpuScopeTapKey`: the frame's request identity without the request generation and
without the Viewer's adjustment, since the scene picture depends on neither. A request measures
only the picture its key names; a newer Viewer frame replaces the tap and the request ends
Skipped(Superseded). The service releases its taps when it goes idle.

A frame the Viewer shows while the RAM preview fill renders it has the same key in both slots. The
frame is then counted once, from its float picture: a request for the key measures the Viewer's
tap, and one Rendered sidecar is kept beside the frame. The shown-frame request is answered by its
measure and the fill's request by the same tables: the panel lets the fill tap go instead of
measuring again. `dropScopeTap` frees only the fill slot, so the Viewer's tap survives the fill
tap's release, and the freed fill slot lets the next flagged fill frame start.

**The retained-picture measure.** A retained frame is a `PreviewDisplayOnlyFrame`: the packed
straight RGBA8 display picture, with the display transform, the view and the look baked in and the
exposure and gamma the frame was made with (neutral for every cached frame, because the Viewer
derives its adjusted picture from it), clamped and quantized to 8 bits. It carries no float
picture. The measure counts `c / 255` of each channel, applies the measure's exposure and gamma to
that value (in the order of `ViewAdjust::fromEncoded`, clamped to 0..1) when the picture's own
adjustment is neutral and the measure's is not (a picture that holds some other adjustment is
Unavailable, since it cannot be removed), multiplies by alpha, and counts the result with the
CPU reference's counting; the CIE table converts the same signal through the config's transform.
What it cannot give:

- The Working signal: the scene-linear values are gone. The result is Unavailable with a message
  the panel shows.
- Under and over counts for a neutral adjustment: a value the display encoding left outside 0..1
  was clamped when the picture was made, so it is not recoverable.
- Float precision: every channel has 255 steps, so the tables are slightly coarser than those of a
  Rendered measure of the same frame.

A retained measure is counted at full detail only; the CPU counts the same pixels at either detail.

**Why the Viewer's picture is not counted directly.** The application runs the preview service with
the `GpuPreparedPacked` product and presentation `Disabled` (`apps/bloom/main.cpp`). The frame it
delivers is a `PreviewDisplayOnlyFrame` "without the Float32 process image"
(`prepared_preview_frame.hpp`): painted host pixels, with no resident Viewer image and no float
picture once the frame is delivered. The float picture exists only inside the frame job, which is
why the tap takes an alias of it there and why a frame that never had a tap has only its 8-bit
retained picture.

A headless read (MCP and Python, "Reading scopes from MCP and Python") has no shown or retained
frame. It measures through `measureScopeFrame` (`scope_frame_measure.hpp`): on the device when the
frame's route is the device and the device can, with the CPU reference otherwise. A frame the device
could not count (no device, a scene outside the prepared-GPU subset, no room, a pixel that is not
finite, which the device's color step refuses) is counted by the CPU reference, labeled CPU, with
the reason.

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

The tap counts the picture the Viewer's own frame job computed, through the same programs and
kernel, so its Working tables equal the CPU reference's and its Display and CIE tables are within
one cell (`bloom.runtime.scope_tap_native`). The headless evaluator route measures the picture the
final-render evaluator computes, so a headless read runs the frame's scene on that evaluator's
device.

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
only this: the count tables of one scope measure, read back once per measure. Its id, disposition
and bound are the same whichever device counts. For a frame the Viewer shows or a RAM preview fill
retains, the readback is made on the Viewer's device from the tap; a headless read makes it on the
final-render device.

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
| The frame's scene (headless route only; the tap runs none) | 1 | 1 |
| The display color program (the Display signal only) | 1 | 1 |
| The XYZ color program (only with the CIE table) | 0 | 1 |
| The count kernel's pixel pass | 1 | 1 |
| The count kernel's column pass (only with the Waveform) | 0 | 1 |
| Table readback | 1 | 1 |

The Working signal runs no display color program. The passes do not depend on the detail; the
detail changes only the bytes of the one table readback and the table memory.
- The picture is never read back, and no per-node or per-operation full-frame roundtrip exists.
  The color steps and the counting are device work (the scene too, for a headless read); the
  measure reports its picture readbacks, which are always zero, and the tests assert it.
- It is not an exemption of any pixel operation, feature or route: every operation of the measured
  frame stays Required device work.

The entry is the decision O1 of the Scopes specification of issue #724. Like
`prep.statistics.result_readback` it needs the owner's approval; it lands only with that approval.

## Real-time cost

The owner allows the tables' readback if updating them in real time is cheap. It is shown by
structure and by tests of that structure; no timing is measured or asserted anywhere.

**What runs for one shown frame.** The frame is measured by one cancellable task on a task worker,
never on the interface thread, in two steps: at the live detail (detail 1) first and, when that
frame is still the one shown, once more at full detail (detail 0). No step runs the scene. During
playback of frames being rendered that is one counting pass and a table readback of at most the
live bytes per shown frame.

| Step | Device route (the tap) | CPU route |
|---|---|---|
| The frame's scene | None: the picture is the one the Viewer's frame job computed | None: the frame's own process picture is counted |
| Color programs | On the Viewer's device: the display program (Display signal), the XYZ program (only with the CIE table) | Per pixel on the worker |
| Counting | The pixel pass and, with the Waveform, the column pass of the kernel | `measureScopesCpu` |
| Readback | One table readback, the layout's bytes at the measure's detail; no picture | None |
| After | Packing and the trace mapping on task workers | The same |

A shown frame with no tap (a cached frame with no sidecar) is counted from its retained picture on a
task worker, once, at full detail: no device work at all.

**Bytes read back per measure** are the table in "The table readback": for a picture at least 256
wide, every section is 1,597,504 bytes at the live detail and, for a picture at least 512 wide,
6,324,288 at full detail (4,218,944 for a picture 256 wide); one section reads back only its own
table (the Histogram 16,448 bytes at either detail). Only the open sections are measured, so a
collapsed section's table is neither counted nor read back.

**Scheduling of the device measure.**

- The service's owner thread steps the measure (`ScopeMeasureStepper`): the display color step, the
  XYZ color step (only with the CIE table), the count kernel, then one poll of the table readback's
  fence per loop turn. Each is a submission of its own, separate from any frame's.
- A step begins only when no Viewer-class stage (Interactive or Visible priority) is ready, active
  or ahead. The device has one compute queue at one priority, so the order in which work is
  submitted is the only priority there is, and this rule gives the Viewer that order. A measure of a
  RAM preview fill tap yields to Viewer-class stages only.
- Fences are polled by the service loop and never waited on; a measure in flight keeps being polled
  while the Viewer's stages run. The task worker that asked blocks on its own request's condition
  variable, never the interface thread and never the service thread.
- One measure runs per service at a time; a second is Busy. The kernel's table slot is one per
  process, and the panel's measure (the preview service's device) and a headless read (the
  final-render evaluator's device) can meet over it. Neither reads the other's tables, whichever
  holds the slot first (`gpu_scope_tables.cpp`). A headless read that meets a measure in flight is
  refused as busy and is counted by the CPU reference, labeled CPU, with the reason. A panel
  measure that meets a headless read in flight ends Failed with the reason and keeps its tap, so
  the frame is counted from its retained picture.

**Admission and giving memory back.** The measure's memory (two color outputs, the table buffer and
its staging copy) is one `GpuAdmissionClass::Background` processing phase, reserved without
blocking through the processing-phase policy and retained on the device for the measure's life.

- The Viewer's last device and host need stays free beside it, always. The measure asks no cache to
  evict anything for it.
- No room: the measure plus the Viewer's need exceeds the card's usable capacity, or exceeds the
  room free now while nothing transient holds it. The request ends Skipped(NoRoom) at once.
- Wait: the card could hold it, but a Viewer request waits or was admitted within the policy's
  Viewer activity window (`kViewerActivityWindow`, 250 ms), or admitted frames hold the room. The
  request waits at the service holding nothing, only while its tap is alive and no Viewer stage
  needs the device, and ends Skipped(NoRoom) if the tap is given back.
- When a frame is short of device memory the service gives back, in this order: the Viewer tap and
  the fill tap; a measure with nothing in flight (Skipped(NoRoom)); and only then parked storage, the
  scene cache, the OCIO programs and the request to the interface to evict finished frames.
- A measure already in flight is told to stop and is polled until its fence is proven retired; it
  then ends Skipped(NoRoom). A stage that needs the device's one retained processing-phase slot
  parks the measure's phase and does not wait for it. The limit: the memory an in-flight measure
  holds returns only when its fence retires.
- The idle trim releases the taps. The panel counts a Skipped(NoRoom) frame from its retained
  picture.

**RAM preview fill, per filled frame.** While a Scopes panel is visible with a scope open, the
fill's requests carry the flag `tapForScopes` (`ScopeFillTaps`, `scope_fill_taps.hpp`). Each
flagged frame then costs one counting pass (with the color steps) and one full-detail table
readback, 6,324,288 bytes with every section open on a picture at least 512 wide, and no scene run;
its tables are kept beside the retained frame, so every frame of the range has full Rendered tables
for both signals. The fill is paced only as far as one tapped picture needs: the service holds at
most one flagged fill picture, and the next flagged frame starts no scene work until that picture
is measured or dropped. The picture is admitted as Background and is the first thing given back. A
tap that does not fit is refused: the frame is delivered and retained as an unflagged one, the fill
is not held, and the frame is counted from its retained picture when shown (Display and CIE, no
Working signal). On the CPU route the frame's process picture is counted before the cache strips
it. Every flagged frame is answered by a measure or a drop: a hidden or collapsed panel, a settings
change, a frame the cache did not retain, a cancelled fill, a destroyed panel and shutdown end in
one or the other. With no panel visible the fill is unchanged and pays nothing: no flag, no tap,
no measure.

**Playback of frames being rendered.** Each frame is measured at the live detail from the Viewer
slot. A newer Viewer frame can replace the tap before the measure begins; the request then ends
Skipped(Superseded) and the frame is counted from its retained picture (Display and CIE, 8-bit,
no Working signal). A frame that stays shown is tried once more at full detail from the tap and
ends with Rendered tables when that measures.

**Bounds.**

- One measure is in flight per controller, for the shown frame and the RAM preview fill together.
  Cancelling it does not free the slot: the slot is free when its task has finished.
- Latest wins. While a measure is in flight a newer shown frame, a settings change or a view
  adjustment only replaces the one waiting request; the frames in between are never measured
  (`measuresSkipped` counts them). A measure in flight for a frame or identity that is no longer
  wanted is cancelled and observes that between its steps; its result is neither shown nor cached.
  Nothing queues beyond the one waiting request.
- A frame replaced before its live tables arrive is never measured at full detail, so playback and
  scrubbing read back live tables only; a frame that stays ends at full detail without any playback
  signal.
- A RAM preview fill measures its frames at full detail, one at a time, in the same slot and only
  when no shown-frame request waits or is in flight. A fill measure already running when a shown
  frame arrives is finished first (it is for a frame still wanted); the shown frame's request waits
  for it.

**What never happens.** No scene run for a shown, cached or fill frame. No wait on the Viewer's
frame, fence or presentation. Nothing on the interface thread: the measure, the packing and the
mapping run on task workers and the controller only submits, polls and publishes. No picture
readback.

**Code facts** (read from the code; the Viewer's wait-free property is shown by structure, not
timed).

- (a) The measure runs on the Viewer's device. The preview display service creates its own
  `GpuDevice` on its service thread; the application runs it with presentation `Disabled`
  (`apps/bloom/main.cpp`), so it presents on no Vulkan queue. The tap slots, the request and the
  measure's native state are written on that owner thread, and the loop turn steps the measure
  after the frames (`stepScopeMeasure`, `gpu_preview_display_service.cpp`). The measure's memory is
  one Background phase of the same processing-phase policy that ranks the Viewer first
  (`viewerPriority`).
- (b) The tap is kept where a frame is published (`keepScopeTap`) and holds an alias of the image
  the frame job computed, so it adds no copy of the picture; the picture is freed when no holder
  remains.
- (c) The table readback is asynchronous: the copy is recorded and submitted with a fence and the
  stepper polls the fence's status once per loop turn (`ScopeMeasureStepper::step`,
  `gpu_process_frame_scope.cpp`); there is no `vkWaitForFences` in the scope tables code
  (`gpu_scope_tables.cpp`). Cancellation and the deadline request that the submission stop, and the
  stepper keeps polling until its fence is proven retired.
- (d) The measure queues one request; `GpuPreviewDisplayService::measureScopeTap` is called from a
  task worker (`scopes_controller.cpp`, `scope_fill_taps.cpp`), never the interface thread or the
  service thread.

**Tests of the structure.**

- `bloom.runtime.scope_tap_native` (real service, GPU-prepared packed product, presentation
  Disabled, a declared card, no window system): the tap's Working tables equal the CPU reference
  and its Display and CIE tables are within one cell; across a measure the scene counters and the
  preparation count do not move, the device runs only the color programs and the count kernel, no
  picture is read back and one table readback moves the layout's bytes; with the table readback
  held through the service's test seam, further packed requests each complete with a
  `GpuPreparedPacked` frame, the measure stays in flight, no frame takes the CPU fallback, and
  releasing the hold delivers the tables; a second request is Busy and a replaced tap gives
  Skipped(Superseded); a cancel is delivered after the fence is proven retired; a fill frame is
  tapped only when its request asks, the second flagged fill frame waits for the first one's
  measure, and an unflagged fill frame keeps nothing and is never held back; shutdown ends a
  queued or in-flight request; a Viewer tap and a fill tap of one frame (one key) are measured
  once, the fill tap's drop frees the fill slot and leaves the Viewer's tap; a panel measure and a
  headless read that meet over the kernel's table slot, in both orders, each deliver the tables of
  their own picture, and the one refused the slot says why (the headless read counted on the CPU,
  labeled CPU; the panel measure Failed with its tap kept); the declared-card cases wait for the
  service's device reports by their count (`counters.deviceReports`), never by time; on a declared
  card that fits the Viewer's frame but not the frame
  plus the measure the request ends Skipped(NoRoom) with the Viewer's counters unmoved, the taps
  are the first memory given back for a frame that needs room (a larger picture than the frames
  that made the taps, so no parked storage serves it), a fill tap that does not fit is
  refused without holding the fill, and the idle trim releases the tap.
- `bloom.runtime.gpu_admission_pressure` (`backgroundMeasureYieldsToTheViewer`): a Background
  measure phase waits while a Viewer request waits, and its grant leaves the Viewer's last need free.
- `bloom.ui.scopes-controller`: the measure never evaluates a scene, so the scene runs are exactly
  the frames the test built; no second measure starts while the first is held, the greatest number
  in flight is 1, skipped frames are counted, the newest frame's live result is shown and cached
  and the held frame's is neither, and the newest frame then ends at full detail; live then full
  for a frame left alone (the full tables equal a direct full measure, the live ones equal them
  pooled); the source order, the lookup order and a Retained sidecar never replacing a Rendered
  one; the Working signal of a retained frame says it is not available; every flagged fill frame
  is answered on every path.
- `bloom.ui.scopes-realtime`: with a visible panel and the measure held, a session stepped through
  several frames is published and shown by the preview and the Viewer each time, the held measure
  ran on a worker, the interface thread's event loop kept turning, and one measure was in flight
  throughout. It runs the CPU routes and so cannot show the device; (a) to (d) above and the tests
  above cover that side.
- `bloom.ui.scopes-device-route` (real service in the application's configuration, the real preview
  and RAM preview controllers and the production measure, no window system): a shown frame is
  measured through the tap, Rendered on the device route, with Working tables equal to the CPU
  reference; the measures add no scene stage run, no resident graph job and no picture readback; a
  fill with the panel closed flags nothing and taps nothing; a small fill with the panel open leaves
  full-detail Working tables beside every retained frame, again with no scene run added.
- `bloom.ui.ram-preview` (`testRamFillTapsScopes`): the fill with the panel closed pays nothing,
  and with the panel open every retained frame carries full-detail Rendered tables counted from its
  own picture, with every flagged frame answered.

Nothing in these tests, or anywhere, is timed. What the structure and the tests leave open:

- The counting pass and the color steps run on the same physical GPU as the Viewer's own work; the
  rule above only keeps them from starting while the Viewer has a stage waiting.
- The memory of a measure in flight returns only when its fence retires.
- The Working signal is not available for a cached frame that never had a tap.
- A retained-picture measure is at 8-bit precision.

## Reading scopes from MCP and Python

`scripting/scope_read.hpp` is the one Qt-free read behind MCP's `query` kind `scopes` and Python's
`bloom.scopes`. It validates the request on the calling thread, then runs one task on the caller's
scheduler that resolves the color configuration, chooses the display and view (the configuration's
own defaults, never the Viewer's inspection settings), compiles the plan once and measures each frame
through `measureScopeFrame`: the evaluator route, with the same CPU/GPU parity as the panel. A
headless read is the only scope read that evaluates a scene. A frame whose tables the desktop
application's preview cache already holds under the same frame identity and the same measure
settings, with source Rendered, is taken from that sidecar instead of measured (the
`McpLiveUiBinding` port answers only for a Viewer state of no region of interest, isolation or
channel view). A Retained sidecar is never served to a read: its tables count an 8-bit display
picture, which is another definition, so a read with no Rendered sidecar measures. A read that meets a panel measure in flight over the kernel's one table slot is refused the slot, never reads the panel's tables, and counts the frame with the CPU reference, labeled CPU, with the reason. The caller's cancellation is polled while the task runs and reaches inside a frame.
Tables are described as flat arrays (dense, or the non-zero cells as index and count pairs), a summary
is `summarizeScopes` of the tables, and a picture is `renderScopePicture` or `renderAllScopesPicture`
encoded with `encodeScopePng`, so a picture equals what the panel draws.

## Sidecars and identity

A frame's count tables are kept beside the frame in the preview cache as a sidecar
(`PreviewScopeSidecar`, `preview_frame_cache.hpp`): the packed tables, the measured area, the route
that counted and, for a frame the device could not count or one counted from a retained picture,
the reason.

- The sidecar's identity is the frame's `PreviewFrameCacheKey` plus `PreviewScopeIdentity`: the
  measure settings (which scopes, the signal and its axis maximum, the region, Ignore Transparent
  Pixels and the detail), the Viewer's exposure and gamma, and the `ScopeSource` (Rendered or
  Retained) of the picture that was counted. A frame has at most one sidecar, and a sidecar of one
  source never answers a lookup for the other.
- The panel's measure has two details of one frame: the live one (detail 1) and the full one
  (detail 0). The panel's settings carry detail 0; the controller measures the shown frame at the
  live detail first and replaces the live sidecar by the full one when it arrives. A Retained
  measure is made once, at full detail.
- The lookup order for the shown frame is Rendered full, Rendered live, Retained full, Retained
  live. A sidecar at full detail is shown at once with no measure; a Rendered live one is shown at
  once and the full measure follows; a Retained one is shown at once and, when its tap was refused
  for a reason that may pass, the frame is tried once more from the tap at full detail.
  `attachScopeSidecar` never lets a Retained sidecar replace a Rendered one, nor a live one replace
  a full one, so the source never flickers.
- A scope setting replaces the sidecar and leaves the frame's entry alone. An edit of the document
  gives the frame another cache key, so the frame and its sidecar are both new. A sidecar leaves
  the cache with its frame.
- A sidecar's bytes are charged to the cache's byte budget and reported with the frame bytes
  (`scopeSidecarBytes`, part of `hostBytes`). Tables are packed to the narrowest whole-number
  width that holds their largest count (`ScopeTables`). A sidecar that does not fit is not kept.
- A cached frame without a matching sidecar is counted again from the picture it holds: a frame
  the Viewer's device still has a tap for from that tap, otherwise from its retained picture,
  never by running its scene. A sidecar of another detail or source is another identity: MCP and
  Python reads measure at detail 0 and are never served a sidecar of another detail or a Retained
  one.

The sidecar is measured by a cancellable task of its own after the frame is shown or retained,
not inside the frame's preparation: a frame is never delayed by its scopes, and a hidden panel
costs nothing.

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
names the frame that was measured. A frame counted from its retained picture carries the note
"counted from the cached picture", with the reason the tap did not serve it when there is one; the
Working signal of such a frame is the state Failed with the message "The Working signal is not
available for a cached frame. Switch to the Display signal, or show the frame again to measure
it." During a RAM preview fill with a panel open, each flagged frame is measured from its tap at
full detail (`ScopeFillTaps`), and every other frame the cache retains is counted from its
retained picture in the background, one at a time and only when no shown-frame request waits or is
in flight; hiding the panel or closing every scope drops the frames still waiting.

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

A headless measure is part of its frame's request: the tables, their staging copy and their host
copy are in the request's output envelope, and a submission that could not be proven retired is
retired by the evaluator's idle owner as the other stages' are. A tap measure is admitted as its own
Background phase ("Real-time cost"). In both, the color programs are prepared on the calling worker
and never on the device owner, the job observes the request's cancellation between its steps, and
nothing is published after a cancel. Every native resource is kept until its submission's fence is
proven retired. The measure holds no state between frames; the taps are the only picture a service
keeps for the scopes, and they are the first memory it gives back.

## Tests

| Test | What it proves |
|---|---|
| `bloom.runtime.scope_measure` | The CPU reference: known answers for every table, the counters, areas, columns, the axis maximum, the packed form, the summary and the measured area; at detail 1 and 2 the Vectorscope, CIE and Waveform tables equal the full tables pooled, the counters and the Histogram are equal, the layout's exact byte counts, and detail 3 is refused |
| `bloom.render.gpu_scope_tables` | The kernel on a real device at every detail: every count against a host count of the same pictures, the bytes read back equal to the layout's, and its refusals (no XYZ picture, a detail above 2, a budget below the detail's tables, a budget that fits a detail but not full detail) |
| `bloom.runtime.scope_measure_gpu_parity` | CPU and device tables equal for the same float pictures at every detail, cell edges included |
| `bloom.runtime.scope_picture` | The Qt-free picture: a picture from detail-1 and detail-2 tables has the size and graticule of full detail's and a trace over the same region, the plan's facts for each scope (Waveform codes, Vectorscope ticks, targets and skin line, CIE grid, labels, triangle and printed lines, Parade panes, Histogram rows and clip markers), trace pixels equal to the mapping over the canvas, the visible floor, Histogram heights, the picture of all five, PNG bytes decoded back, and the cancelled, byte-limit and size refusals |
| `bloom.ui.scopes-picture` | The panel and the picture agree (hover readouts at detail 1 report the detail cell's count): palette and metrics equal the interface tokens, an offscreen grab and the picture agree on the trace rectangle of each scope, and the plan's hairlines are where the grab has graticule pixels |
| `bloom.color.scope-colorimetry` | The XYZ transform, the luma coefficients and the primaries of the built-in configs |
| `bloom.runtime.scope_frame_measure_native` | A frame through the headless device route (the final-render evaluator) against the CPU reference: exact for the Working signal, within one cell through the OCIO steps, one table readback and no picture readback, the route named; a measure at detail 1 reads back the layout's bytes, and a Histogram-only measure its 16,448 bytes and runs no XYZ program; the bytes of one section and of every section at the live and at full detail |
| `bloom.scripting.scope-read` | The headless read: tables equal the CPU reference cell for cell for both signals, counters, luma and primaries, sums, sparse equal to dense, the summary, pictures, a range with progress and a deterministic cancel, the warm-sidecar port, region, transparency, resolution, display and view, and every refusal with its remedy |
| `bloom.mcp.scope-query` | `query` kind `scopes` on the shared core: the tables equal the read's, sparse, summary, image and range answers, and the refusals |
| `bloom.scripting.python-scopes` | `bloom.scopes.read` and `image` from Python: a flat color's tables, sparse equal to dense, summary, range and time, written and replaced files, argument errors |
| `bloom.ui.scopes-mcp-warm` | The MCP read against a real preview and cache: a Rendered sidecar under the read's own settings is answered from the cache with no measure, a Retained one is never served; every setting (a sidecar of another detail included), region, isolation, look or composition that differs is measured |
| `bloom.scripting.scope-read-native` | The headless read on a real device: route Gpu, Working tables equal the CPU reference, CIE and Display tables within one cell; a disabled provider reads with the CPU reference and says why |
| `bloom.color.scope-colorimetry-external` | The XYZ transform of an external config through the helper process equals the built-in's; spaces with no XYZ interchange are refused with their reason |
| `bloom.ui.scopes-controller` | The controller over CPU frames and a scripted tap: no scene evaluated by any measure, one measure in flight, latest wins with skipped frames counted, live then full, a replaced frame never measured in full, the source order and the lookup order, a Retained sidecar never replacing a Rendered one, the Working signal of a retained frame, cancellation, the fill, and every flagged fill frame answered |
| `bloom.ui.scopes-realtime` | A real preview, Viewer and panel with the measure held: every stepped frame is published and shown, the measure runs on a worker, the interface thread's loop keeps turning, one measure in flight (CPU routes) |
| `bloom.runtime.scope_frame_measure_retained` | The retained-picture measure on the CPU: known pictures to known tables, `c / 255` then exposure, gamma, clamp and alpha in that order, an adjustment already in the picture not applied twice, the CIE table, region, Ignore Transparent Pixels, detail 1 and 2 pooled from detail 0, the Working signal refused with its message, an empty area and cancellation |
| `bloom.runtime.scope_tap_native` | The tap and the device measure on a real service with no window system: parity with the CPU reference, no scene run, one table readback and no picture readback, a held readback while further frames complete, Busy, Superseded, cancel, fill pacing, a Viewer tap and a fill tap of one key, a panel measure and a headless read meeting over the table slot in both orders, shutdown, and the declared-card cases in which the measure yields |
| `bloom.runtime.gpu_admission_pressure` | Includes `backgroundMeasureYieldsToTheViewer`: a Background measure phase waits while a Viewer request waits and leaves the Viewer's last need free |
| `bloom.ui.scopes-device-route` | The panel's device route end to end with the real service, the real preview and RAM preview controllers and the production measure: a shown frame through the tap with no scene run, a fill with the panel closed paying nothing, a fill with the panel open leaving full Working tables beside every frame, and a frame that is both the Viewer's and a fill frame counted once with one Rendered sidecar, both requests answered, the fill slot freed and the Viewer's tap kept |
| `bloom.ui.ram-preview` | Includes `testRamFillTapsScopes`: a fill with the panel closed flags nothing, taps nothing and measures nothing; with the panel open every retained frame has full-detail Rendered tables and every flagged frame is answered |
| `bloom.ui.preview-frame-cache-cpu-edge` | Includes the sidecar identity: a Rendered sidecar never answers a Retained lookup and the reverse |
| `bloom.ui.scopes-region` | Includes a project saved with a scope region equal, byte for byte, to one saved without |
| `bloom.ui.viewer_resident_scopes_region_native` | The scope region and its handles are drawn over the device-presented Viewer picture (needs a window system and a device) |

The device tests skip without a device and fail under `--require-device`.
