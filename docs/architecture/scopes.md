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
| Waveform (and Parade) | 4 channels of `columns` columns of 512 levels, under and over; `columns = min(512, W)` for a measured width `W` | Column `floor(x * columns / W)`, level `c >> 1` |
| Vectorscope | 512 by 512 | `cb = (b - y) / (2 (1 - Kb))`, `cr = (r - y) / (2 (1 - Kr))`; cell `(floor((cb + 0.5) * 512), floor((cr + 0.5) * 512))`; outside 0..511 adds 1 to `vectorscopeOutside` |
| CIE chromaticity | 512 by 512 | `(X, Y, Z)` is the signal converted to CIE XYZ with a D65 white; `s = (X + Y) + Z`; not finite or `s <= 0` adds 1 to `noChromaticity`; otherwise `x = X / s`, `y = Y / s`, cell `(floor(x / 0.8 * 512), floor(y / 0.9 * 512))`; outside 0..511 adds 1 to `chromaticityOutside` |

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
  6,324,288 bytes, whatever the size of the picture. A measure that asks for fewer scopes reads
  back fewer.
- The picture is never read back, and no per-node or per-operation full-frame roundtrip exists.
  The scene, both color steps and the counting are device work; the measure reports its picture
  readbacks, which are always zero, and the test asserts it.
- It is not an exemption of any pixel operation, feature or route: every operation of the measured
  frame stays Required device work.

The entry is the decision O1 of the Scopes specification of issue #724. Like
`prep.statistics.result_readback` it needs the owner's approval; it lands only with that approval.

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
  Pixels) and the Viewer's exposure and gamma. A frame has at most one sidecar.
- A scope setting replaces the sidecar and leaves the frame's entry alone. An edit of the document
  gives the frame another cache key, so the frame and its sidecar are both new. A sidecar leaves
  the cache with its frame.
- A sidecar's bytes are charged to the cache's byte budget and reported with the frame bytes
  (`scopeSidecarBytes`, part of `hostBytes`). Tables are packed to the narrowest whole-number
  width that holds their largest count (`ScopeTables`). A sidecar that does not fit is not kept.
- A cached frame without a matching sidecar is measured again from the frame's own plan and
  request through `measureScopeFrame`, never from the packed 8-bit picture, and gives the tables a
  cold frame gives.

The sidecar is measured by a cancellable task of its own after the frame is shown or retained,
not inside the frame's preparation: a frame is never delayed by its scopes, and a hidden panel
costs nothing. The device route therefore runs the frame's scene again on the final-render
evaluator.

## The panel

`ScopesController` (`scopes_controller.hpp`) is told which frame the followed Viewer shows and
which measure the panel wants. It takes the tables from the frame's sidecar when the identity
matches, and otherwise measures on a task worker and attaches the result. A superseded request is
cancelled and its result is neither shown nor cached. While another frame's tables are on show the
reading is marked behind and names the frame that was measured. During a RAM preview fill with a
panel open, each frame the cache retains is measured in the background.

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
| `bloom.runtime.scope_measure` | The CPU reference: known answers for every table, the counters, areas, columns, the axis maximum, the packed form, the summary and the measured area |
| `bloom.render.gpu_scope_tables` | The kernel on a real device: every count against a host count of the same pictures, and its refusals (no XYZ picture, a budget below the tables) |
| `bloom.runtime.scope_measure_gpu_parity` | CPU and device tables equal for the same float pictures, cell edges included |
| `bloom.runtime.scope_picture` | The Qt-free picture: the plan's facts for each scope (Waveform codes, Vectorscope ticks, targets and skin line, CIE grid, labels, triangle and printed lines, Parade panes, Histogram rows and clip markers), trace pixels equal to the mapping over the canvas, the visible floor, Histogram heights, the picture of all five, PNG bytes decoded back, and the cancelled, byte-limit and size refusals |
| `bloom.ui.scopes-picture` | The panel and the picture agree: palette and metrics equal the interface tokens, an offscreen grab and the picture agree on the trace rectangle of each scope, and the plan's hairlines are where the grab has graticule pixels |
| `bloom.color.scope-colorimetry` | The XYZ transform, the luma coefficients and the primaries of the built-in configs |
| `bloom.runtime.scope_frame_measure_native` | A frame through the production device route against the CPU reference: exact for the Working signal, within one cell through the OCIO steps, one table readback and no picture readback, the route named |
| `bloom.scripting.scope-read` | The headless read: tables equal the CPU reference cell for cell for both signals, counters, luma and primaries, sums, sparse equal to dense, the summary, pictures, a range with progress and a deterministic cancel, the warm-sidecar port, region, transparency, resolution, display and view, and every refusal with its remedy |
| `bloom.mcp.scope-query` | `query` kind `scopes` on the shared core: the tables equal the read's, sparse, summary, image and range answers, and the refusals |
| `bloom.scripting.python-scopes` | `bloom.scopes.read` and `image` from Python: a flat color's tables, sparse equal to dense, summary, range and time, written and replaced files, argument errors |
| `bloom.ui.scopes-mcp-warm` | The MCP read against a real preview and cache: a sidecar under the read's own settings is answered from the cache with no measure; every setting, region, isolation, look or composition that differs is measured |
| `bloom.scripting.scope-read-native` | The headless read on a real device: route Gpu, Working tables equal the CPU reference, CIE and Display tables within one cell; a disabled provider reads with the CPU reference and says why |
| `bloom.color.scope-colorimetry-external` | The XYZ transform of an external config through the helper process equals the built-in's; spaces with no XYZ interchange are refused with their reason |
| `bloom.ui.scopes-region` | Includes a project saved with a scope region equal, byte for byte, to one saved without |
| `bloom.ui.viewer_resident_scopes_region_native` | The scope region and its handles are drawn over the device-presented Viewer picture (needs a window system and a device) |

The device tests skip without a device and fail under `--require-device`.
