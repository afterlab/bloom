# Scopes

The Scopes panel (`bloom.scopes`, issues #724 and #730) shows five video scopes of the picture a
Viewer shows: Waveform, Parade, Vectorscope, Histogram and CIE chromaticity. This document is the
specification of what is measured, where it is computed, how it is cached and how it is drawn, and
it is the owning document of the coverage contract entry `prep.scopes.result_readback`. The drawing
is matched to the scopes of the reference grading application.

## What is measured

One definition, whatever way a frame was reached (paused, scrubbed, played, cached or not, on the
device or on the CPU). A measure counts the pixels of a measured area of one float picture, the
signal, into count tables, and every cell of a table is a whole number of pixels. There is one
measure per frame and no coarser tier: the tables of a frame are always the full tables below.

### The signal

| Signal | The picture that is counted |
|---|---|
| Display (default) | Every pixel's straight color through the frame's display, view and look (the display transform's float result, before any packing to 8 bits), then multiplied by the pixel's alpha again |
| Working | The scene-linear picture as it is stored: its color over black and its alpha |

The signal is taken over black: red, green and blue are the color times alpha. The display
transform's float result is kept as it is, so a value the display encoding leaves below 0 or above
1 is counted as under or over. The Viewer's exposure and gamma are a viewing aid and are not part of
the Display signal: every cached frame is made neutral, and the scopes of a frame are the same
whatever the Viewer's adjustment. A pixel with no alpha is stored as it is.

The axis is 10-bit full range, code values 0 to 1023, at full resolution. The Display signal's
axis is 0..1; the Working signal's has a maximum M chosen from 1, 2, 4, 8 and 16 (Signal > Working
Axis Maximum), and changing M counts the tables again.

### The measured area

The picture's display window, cut to the frame's region of interest when it has one and to the
scope region when one is set (`scopeMeasuredArea`, `scope_measure.hpp`). The scope region is in
composition pixels and is scaled to the picture outward (its first pixel floored, its end raised),
so a region drawn at full resolution measures the same part of a half or quarter resolution
picture. A pixel of the area outside the picture's data window is transparent black. A region
outside the picture has nothing to count: the frame's tables say EmptyArea and every scope draws
its empty graticule.

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
| Waveform (and Parade) | 4 channels of `columns` columns of 1024 levels, under and over; `columns = min(512, W)` for a measured width `W` | Column `floor(x * columns / W)`, level `c` |
| Vectorscope | 640 by 640 counts, and 640 by 640 luma sums | `cb = (b - y) / (2 (1 - Kb))`, `cr = (r - y) / (2 (1 - Kr))`; cell `(floor((cb + 0.625) * 512), floor((cr + 0.625) * 512))`; outside 0..639 adds 1 to `vectorscopeOutside`; otherwise the pixel adds 1 to the cell's count and its luma code, clipped to 0..1023, shifted right by 4 (0..63), to the cell's luma sum |
| CIE chromaticity | 512 by 512 | `(X, Y, Z)` is the signal converted to CIE XYZ with a D65 white; `s = (X + Y) + Z`; not finite or `s <= 0` adds 1 to `noChromaticity`; otherwise `x = X / s`, `y = Y / s`, cell `(floor(x / 0.8 * 512), floor(y / 0.9 * 512))`; outside 0..511 adds 1 to `chromaticityOutside` |

The Vectorscope table covers Cb and Cr from -0.625 to 0.625, a little beyond the graticule's ring
at 0.60, so 100% color bars (whose Cb or Cr reaches 0.5) are inside it and drawn. 0.625 and 512 are
exact in binary32. A cell's luma sum is exact up to 68,174,084 pixels in that one cell. The
Parade draws the Waveform table's red, green and blue channels side by side; it has no table of its
own. The two Vectorscope denominators are rounded once to binary32 on the host, so both routes
divide by the same number. The layout of the tables as 32-bit words is
`render/scope_table_layout.hpp`.

Gray sits exactly on the edge between two Vectorscope cells (the center is the corner of cells 319
and 320), so which of the two cells a gray pixel lands in is decided by the last bit of its value.
A test asserts the count of a neighborhood, never one cell.

A summary (`summarizeScopes`) gives each channel's percentiles, mean and clip counts from the
Histogram and the Vectorscope's mean saturation and hue from the cells' centers; saturation is a
fraction of the ring's radius (0.60), so 100% is a pixel on the ring.

## Computed in the frame pipeline

The scope tables are part of a frame, like its image. While a Scopes panel is visible, every
preview request (the Viewer's, read-ahead's and RAM Preview's) asks the frame's preparation to
count the tables too (`PreviewRequestIdentity::scopes`, `preview_frame_scopes.hpp`); with no panel
visible nothing is asked and nothing is counted. The request carries the session's settings: every
table, the signal, the Working axis maximum, the composition's scope region and Ignore Transparent
Pixels. Collapsing a pane never changes what is counted, so it never invalidates a cached frame.

The frame's own route counts the tables, once, while it prepares the frame:

| Route | How |
|---|---|
| Device | The preview display service's frame job. The worker that prepares the frame's GPU scene also prepares the measure's color programs (`prepareFrameScopeMeasure`). When the frame's display step is ready, the job counts the scene picture it computed: the display color program (Display signal), the XYZ color program (with the CIE table) and the fixed embedded kernel `ScopeTablesV1` (`render::GpuScopeTables`), with its one table readback. Only then does the frame's final transfer begin, and the frame is published carrying its tables (`gpu_preview_display_service_scope.cpp`). No scene runs again and no picture is read back |
| CPU | The worker that evaluated the frame counts its float process picture (`measurePreviewFrameScopesCpu`, `composition_preview_cpu_stage.cpp`) before the display picture is made |

A frame the device cannot count (no shared OCIO context for the color programs, a scene outside the
prepared-GPU subset) takes the CPU route, which counts its tables from its process picture. A frame
whose device measure fails after its scene ran is published with a Failed record for its settings,
so it is not prepared again in a loop; the scopes draw the empty graticule for it.

The tables travel with the frame (`PreparedPreviewFrame::scopes`) to the Viewer and into the
preview frame cache. They are never counted from a cached 8-bit picture, and nothing measures a
frame after it is shown.

A headless read (MCP and Python, "Reading scopes from MCP and Python") has no Viewer frame. It
measures through `measureScopeFrame` (`scope_frame_measure.hpp`) with the same definition: on the
device as a job of the final-render evaluator (`GpuProcessFrameEvaluator::measureScopes`,
`gpu_scope_measure.hpp`) when the frame's route is the device and the device can, with the CPU
reference otherwise, labeled with the route and the reason.

### Exactness

For the same float picture the two routes give equal tables, cell for cell. The kernel runs the
CPU reference's operations in the same order with `precise` arithmetic, writes constants that are
not exact in decimal as bits, and corrects every device division to the correctly rounded quotient
the CPU computes (a device's own float division is not required to be correctly rounded, and is
not on every device). `bloom.runtime.scope_measure_gpu_parity` proves this on a real device with
pictures that put pixels on cell edges.

The kernel adds whole numbers with `atomicAdd` on an unsigned storage buffer for the counters, the
Histogram, the Vectorscope and the CIE table; integer adds commute, so the result does not depend
on how the device schedules the work. The Waveform needs no atomics between columns: one invocation
owns one column of the table and walks the pixels that fall in it.

The display transform and the conversion to XYZ are OCIO's CPU and GPU processors, which round
differently. Through those steps a pixel may land in the neighboring cell: every pixel is still
counted, and along every table axis no pixel is more than one cell from where the CPU reference
puts it (`bloom.runtime.scope_frame_measure_native`, `bloom.runtime.preview_frame_scopes_native`).
The Working signal's Histogram, Waveform and Vectorscope pass through no OCIO step and are exact.

## Cached with the frame

A frame's tables are kept with the frame in the preview frame cache (`PreviewFrameCache`): the
packed tables (`ScopeTables`, each table packed to the narrowest whole-number width that holds its
largest count), the measured area, the route that counted, the settings they were counted under
and, for a Failed record, the reason.

- A frame's tables are matched by the frame's cache key plus the scope settings. A request that
  asks for tables is a miss for a cached frame without tables of the same settings
  (`PreviewFrameCache::take`, `containsWithScopes`): the frame is prepared again through the frame
  pipeline, and its tables with it. A request that asks for none is answered by the frame alone.
- The tables are charged to the cache's host byte budget whatever the frame's pixel storage is
  (`scopeTableBytes`, part of `hostBytes`), and a frame whose tables do not fit is not kept.
- The frame cached again without tables keeps the ones it carries; a frame with tables of other
  settings replaces them. An edit of the document gives the frame another cache key, so the frame
  and its tables are both new. The tables leave the cache with their frame.
- RAM Preview counts the tables as it fills, at the fill's own pace: a frame of the range is covered
  only when it carries tables of the current settings, so a fill with a panel visible prepares the
  frames without them again, and a fill after a settings change prepares the range again.
- Playback and scrubbing only read cached tables: stepping through a cached range prepares nothing
  and measures nothing.

Queued, not on main (#733): opening a Scopes panel after a RAM Preview was cached finds every
cached frame without tables, so the Viewer prepares those frames again. #733 changes the Display
signal to count the frame the Viewer shows at display time, with no Display-signal tables stored
per frame. Everything above describes what is on main today.

## The panel

`ScopesEditor` (`bloom.scopes`) is five collapsible panes, each with only its title and collapse
arrow: no status line, warning, note or other text, no slider, dropdown or button. Its settings are
in the header menus of its editor area (`EditorChromeProvider`, `docs/ux/ui-grammar.md`):

| Menu | Items |
|---|---|
| Signal | Display, Working Space (a radio group); Working Axis Maximum (1, 2, 4, 8, 16; enabled for Working Space); Ignore Transparent Pixels (a check) |
| Region | Draw Scope Region (a check; disabled with no Viewer to draw in); Clear Scope Region |
| Scopes | Waveform: RGB, Luma; Histogram: RGB, Luma, RGB and Luma, then Linear, Logarithmic |

The settings are session state shared by every Scopes panel (`ScopesRegionStore`): a frame carries
one set of tables, so two panels never ask for different ones. Signal, axis maximum, region and
Ignore Transparent Pixels change what a frame counts: the Viewer asks for its frame again and the
frame pipeline counts it. The Scopes menu changes only how the tables are drawn: the panes map the
same tables again.

The panel follows a Viewer and is handed the frame the Viewer shows. When that frame carries
tables of the session's settings, the panel hands them to the panes; a frame without them changes
nothing. Each pane maps its tables to a trace image on a task worker (`ScopesTraceRenderer`): the
newest request waits behind the one running, which is never cancelled, and nothing of the mapping
runs on the interface thread.

**Never dim.** A pane shows its last trace at full strength until the new trace replaces it in one
swap: the new image, the tables it was mapped from and the modes it was mapped with together. The
panel never dims, fades, blanks, flashes or shows a placeholder while new tables arrive or a trace
is mapped, and playback updates the traces continuously. A frame whose tables say EmptyArea or
Failed, and no frame at all, draw the empty graticule, silently. Nothing changes the panel's
layout while frames change: each pane's height depends on its width alone. Hovering a trace shows
the value under the pointer and the count of the cell there, as a tooltip that changes no layout.

## Drawing the counts

A scope has one Qt-free definition of what it looks like, in `bloom/runtime/scope_picture.hpp`. The
Scopes panel and the headless scope image (MCP and scripting sessions have no Qt) both draw from it,
so they cannot drift. The tables are the truth; the drawing is presentation and has no effect on
them.

### The trace

The trace intensity is fixed: light adds linearly and saturates. There is no brightness control, no
floor, no normalization to the frame's largest count and no logarithm.

- **Level spread.** A Waveform level's or a Histogram bin's count is drawn spread over the levels
  c - 3 to c + 3 by a tent one 8-bit step wide on each side: level c + d takes (4 - |d|) / 16 of
  it. Codes 4 apart add up to a flat line, so a picture whose values come in 8-bit steps (a code
  every 4.01) draws a continuous trace instead of a comb of empty rows or bins, as the reference
  grading application does; a 10-bit picture is softened by a couple of levels. Levels beyond the
  axis are mirrored back into it, which keeps a flat spread flat to its ends. The tables keep every
  count where it was counted; values outside the axis are not spread.
- **Waveform and Parade.** Each screen column sums the table columns it shows (area-sum reduction)
  into 1024 levels, values under the axis on level 0 and over it on level 1023 (they are drawn on
  the 0 and 1023 lines; the counters keep them apart). The column is normalized by the pixels its
  own table columns hold, so a column is as bright as its own pixels make it, whatever the rest of
  the picture holds and whether a table column holds one source pixel or two: no stripes appear at
  any measured width. A screen pixel's intensity is `0.25 x density`, where density is the fraction
  of the column's pixels in that pixel times the plot's height in levels per pixel (a uniform spread
  is 1). The channels are drawn in red (1, 1/3, 1/3), green (1/3, 1, 1/3) and blue (1/3, 1/3, 1),
  luma in white, multiplied by the intensity, added and clamped per channel, so overlapping
  channels turn white. The Parade is three equal panes with a gap of 4 pixels.
- **Vectorscope.** Each screen pixel sums the cells it covers. Its intensity is `0.05 x density`,
  where density is the fraction of the area's pixels in it times the plot's area in cells (a
  uniform spread is 1). Its color is the mean color of its pixels: their mean luma (from the cells'
  luma sums) and the screen pixel's own Cb and Cr through the signal's coefficients, at full
  strength over a third of white, so a cell whitens as it gets denser.
- **Histogram.** Each screen column is the mean count of the bins it covers, values outside the axis
  in the end bins. One scale is shared by the channels, with 0.9 of the row as headroom; a bin with
  a count is at least one pixel tall. A channel is a 1-pixel outline in its color, red (0.875,
  0.204, 0.204), green (0, 0.86, 0), blue (0, 0.46, 0.73), luma light gray, over a fill of 22% of
  that color through which the graticule shows. Linear or logarithmic (`log2(1 + n)`).
- **CIE chromaticity.** Presence only: a cell with any pixel is drawn at full brightness in the
  color of its own chromaticity; a cell with none is not drawn.

The gains are fixed constants of the drawing (`kWaveformTraceGain`, `kVectorscopeTraceGain`), chosen
against the reference grading application's scopes of the same pictures.

### The graticule

From the scope kind, a logical size, a device pixel ratio, the settings that affect drawing, the
luma coefficients, the colorimetry and a text-measuring callback, one function returns everything a
scope shows besides its trace, in logical coordinates: the plot and trace rectangles, hairlines,
polylines, circles and labels, each with a color role and whether it sits under or over the trace.

- The canvas is black; hairlines are `kit::Color::BorderHover` and labels `kit::Color::Muted`, about
  twice as bright as the lines; the Bloom component hues name the channels.
- **Waveform and Parade.** A line at codes 0, 128, ..., 896 and 1023, labeled in the left gutter
  (the Working signal's labels are linear values of the axis maximum); minor ticks every 32 codes
  on the left edge; 48 ticks along the bottom (16 per Parade pane), every fourth longer. The Parade
  names its panes R, G and B.
- **Vectorscope.** The plot spans Cb and Cr of -0.625 to 0.625. Full cross hairs; one ring at
  `|CbCr| = 0.60` with 72 equal ticks inward from it, every 5 degrees; six targets at the 75% color
  bars of the signal's coefficients, each four corner brackets, labeled R, M, B, C, G and Y at their
  lower right; the skin tone line, solid and faint, from the center to the ring at 123 degrees.
- **Histogram.** A row per channel, framed, named by its letter; a line at every labeled code and
  minor ticks every 32 codes along the top.
- **CIE chromaticity.** A grid every 0.1, labeled every 0.2, the axes named X and Y; the spectral
  locus with a tick every 10 nm from 460 to 620 nm, the line of purples and the Planckian locus
  from 1667 K to 25000 K; the gamut triangle of the signal's primaries, named at its green corner by
  its gamut (Rec.709, P3, Rec.2020, ACES AP1, ACES AP0, or the color space's own name), the white
  point ring, and in the top-right corner the gamut with its white (D65, D60, D50 or DCI) and the
  primaries and white to four decimals ("Rec.709 (D65)", "R: 0.6400, 0.3300").

### The panel and the headless image

The panel paints the plan as vector drawing with `QPainter` and the interface fonts, crisp at every
scale. The headless image rasterizes it: lines are one device pixel, antialiased where the plan
says so, and labels are rasterized with the embedded Inter Medium face, by `renderScopePicture` for
one scope and `renderAllScopesPicture` for the five stacked in the panel's order, each as tall as
the panel's `heightForWidth` gives for that width. The panel's geometry reads its own font metrics;
the headless image's reads the embedded face's, so a label may sit a pixel or two differently. A
caller that needs the panel's geometry passes the panel's text measure in `PictureOptions`. The
palette `defaultScopePalette()` and the metrics are tied to the interface tokens one by one
(`bloom.ui.scopes-picture`).

`encodeScopePng` turns the buffer into PNG bytes (8-bit RGBA, an sRGB chunk, one IDAT stream,
straight alpha) with the qualified zlib, independent of any project or export job. Every picture
call takes a cancellation token and a byte limit and returns an error value, never an exception.

## The table readback

`prep.scopes.result_readback` is a named, bounded readback in the audited host-preparation list of
the coverage contract (`gpuApprovedCpuPreparation()` in `gpu_coverage_contract.hpp`). It covers
only this: the count tables of one frame's scope measure, read back once per measure. Its id,
disposition and bound are the same whichever device counts: the preview service's device for a
frame a Viewer, read-ahead or RAM Preview asked for, the final-render device for a headless read.

- What is read back is at most 3,186,704 unsigned 32-bit words (8 counters, the Histogram's 4,104,
  the Waveform's 2,101,248 at 512 columns of 1,026 words, the Vectorscope's 819,200 counts and luma
  sums and the CIE table's 262,144): 12,746,816 bytes, whatever the size of the picture. This is
  the contract entry's bound (`render::kScopeTablesMaxBytes`). A narrower area has fewer Waveform
  columns and reads back less; a measure that asks for fewer tables reads back only those, and one
  without the CIE table runs no XYZ color program. `ScopeFrameMeasure::tableReadbackBytes` reports
  the bytes a measure read back (0 for the CPU reference).
- The picture is never read back, and no per-node or per-operation full-frame roundtrip exists. The
  color steps and the counting are device work; the measure reports its picture readbacks, which
  are always zero, and the tests assert it.
- It is not an exemption of any pixel operation, feature or route: every operation of the measured
  frame stays Required device work.

Bytes read back by one measure, on an area at least 512 wide (the counters are 32 bytes):

| Tables | Bytes |
|---|---:|
| Histogram | 16,448 |
| Waveform | 8,405,024 |
| Vectorscope | 3,276,832 |
| CIE | 1,048,608 |
| All four | 12,746,816 |

Passes of one measure on the device:

| Pass | Histogram alone | Every table |
|---|---|---|
| The display color program (the Display signal only) | 1 | 1 |
| The XYZ color program (only with the CIE table) | 0 | 1 |
| The count kernel's pixel pass | 1 | 1 |
| The count kernel's column pass (only with the Waveform) | 0 | 1 |
| Table readback | 1 | 1 |

The entry is the decision O1 of the Scopes specification of issue #724.

## Cost and scheduling

No timing is measured or asserted anywhere; the cost is shown by structure and by tests of that
structure.

- A frame's measure is part of its own job: it runs on the frame's device between the frame's
  display step and its final transfer, with the frame's own byte allowance, one native submission
  at a time, polled by the service's owner thread and never waited on (`ScopeMeasureStepper`; there
  is no `vkWaitForFences` in the scope tables code). It observes the frame's cancellation and
  deadline between its steps and keeps polling a submission until its fence is proven retired.
- The kernel's table slot is one per process; a frame's count that meets a headless read in flight
  waits for the slot (polled), and a headless read that meets a frame's count is counted by the
  CPU reference, labeled CPU, with the reason. Neither reads the other's tables.
- The count kernel and its color programs are created on first use and released when the service
  is idle; their retained programs are given back first when a frame needs device memory.
- On the CPU route the count runs on the worker that evaluated the frame, never the interface
  thread.
- Playback of a cached range reads cached tables only. Playback of frames being rendered counts
  each frame once, as part of preparing it; nothing is counted after a frame is shown.

What the structure leaves open: the counting pass and the color steps run on the same physical GPU
as the frame's own work and lengthen its job by their device time.

## Reading scopes from MCP and Python

`scripting/scope_read.hpp` is the one Qt-free read behind MCP's `query` kind `scopes` and Python's
`bloom.scopes`. It validates the request on the calling thread, then runs one task on the caller's
scheduler that resolves the color configuration, chooses the display and view (the configuration's
own defaults, never the Viewer's inspection settings), compiles the plan once and measures each
frame through `measureScopeFrame`, with the same CPU/GPU parity as the frame pipeline. A frame
whose tables the desktop application's preview cache already holds, counted by the frame's own
preparation under exactly the read's settings, is answered with those tables instead of measured
(the `McpLiveUiBinding` port answers only for a Viewer state of no region of interest, isolation
or channel view, and tables that lack one the read asks for are never served). The caller's
cancellation is polled while the task runs and reaches inside a frame. Tables are described as flat
arrays (dense, or the non-zero cells as index and count pairs), a summary is `summarizeScopes` of
the tables, and a picture is `renderScopePicture` or `renderAllScopesPicture` encoded with
`encodeScopePng`, so a picture equals what the panel draws.

## The scope region

`ScopesRegionStore` holds one region per project and composition, the armed flag of Draw Scope
Region and the count of visible Scopes panels. It is session state: it holds no reference to the
document, is not saved, is not an undo step and does not change rendering, export or the Viewer's
region of interest. The region is whole composition pixels clipped to the frame;
`scopeMeasuredArea` scales it to the resolution measured.

Every Viewer of the composition draws the region as an overlay labeled "Scopes" while a Scopes
panel is visible, on the widget and on the device presentation alike. The Viewer takes input for
it only while Draw Scope Region is armed (draw a rectangle, or move the region by its body) or when
the press is on one of its handles (resize), so layer moves, marquee selection and the region of
interest tool are unaffected. The store is written when a drag ends.

## Budget, cancellation and retirement

A frame's measure is part of its frame's request: the color outputs, the table buffer, its staging
copy and the host copy are within the frame's byte allowance, and a submission that could not be
proven retired is retired by the service's idle owner as the frame's other stages are. The color
programs are prepared on the worker that prepared the frame's scene and never on the device owner,
the job observes the request's cancellation between its steps, and nothing is published after a
cancel. A headless measure is part of its request on the final-render evaluator in the same way.
The measure holds no picture between frames.

## Tests

| Test | What it proves |
|---|---|
| `bloom.runtime.scope_measure` | The CPU reference: known answers for every table, the counters, areas, columns, the axis maximum, the packed form, the summary and the measured area; the fixed layout's exact byte counts; a 10-bit ramp on every one of the 1024 Waveform levels; the Vectorscope's luma sums; 75% and 100% bars inside the table and the 0.60 ring |
| `bloom.render.gpu_scope_tables` | The kernel on a real device: every count against a host count of the same pictures, an area wider than the Waveform's columns, the bytes read back equal to the layout's, and its refusals (no XYZ picture, a budget below the tables) |
| `bloom.runtime.scope_measure_gpu_parity` | CPU and device tables equal for the same float pictures, cell edges and wide areas included |
| `bloom.runtime.preview_frame_scopes_native` | The frame pipeline on a real preview service: a frame whose request asks for tables is published carrying them, counted by its own job for both signals, equal to (Working) or within one cell of (Display) the CPU reference, one table readback and no picture readback; a region outside the picture is EmptyArea; a request without tables counts nothing |
| `bloom.runtime.scope_picture` | The Qt-free picture: the plan's facts for each scope (Waveform codes and ticks, Vectorscope ring, ticks, cross hairs, targets and skin line, CIE grid, labels, loci, triangle, names and printed lines, Parade panes, Histogram rows); no column stripes at measured widths 640, 1000 and 1920; a 10-bit ramp as one continuous trace and a flat Histogram; a column's intensity independent of other content; values outside the axis on the 0 and 1023 lines and in the end bins; a single pixel visible; Histogram heights, outline and fill; CIE presence in full color; trace pixels equal to the mapping over the canvas; the picture of all five; PNG bytes decoded back; the refusals |
| `bloom.ui.scopes-panel` | The panel over real CPU frames: registration, the header menus and the settings they write, no text, slider, dropdown, check box or button in the panel; a frame's tables shown, a frame without them changing nothing; never dim: until the new trace is mapped every scope paints the previous one exactly, and every picture painted between two valid traces is one of the two; the layout constant across frames; the empty graticule for an empty area; mapping on a worker; the readout; collapse persistence; a hidden panel asking for no tables |
| `bloom.ui.scopes-drawing` | The panel's trace image is the Qt-free mapping's; additive light traces; null for no tables, a missing table and a cancel |
| `bloom.ui.scopes-picture` | The panel and the picture agree: palette and metrics equal the interface tokens, an offscreen grab and the picture agree on the trace rectangle of each scope, the plan's hairlines are where the grab has graticule pixels, and the hover readouts report the cell under the pointer |
| `bloom.ui.ram-preview` | Includes `testRamFillCarriesScopeTables`: a fill with no panel counts nothing; with a panel the frames without tables are prepared again and carry Display tables of all their pixels, charged to the cache's budget; stepping through the cached range prepares nothing; a Working signal fill carries Working tables |
| `bloom.ui.preview-frame-cache-cpu-edge` | Includes a frame's tables: a request for tables misses without them and hits with them, other settings miss, the bytes are charged, a frame without tables keeps the cached ones, other settings replace them |
| `bloom.color.scope-colorimetry` | The XYZ transform, the luma coefficients and the primaries of the built-in configs |
| `bloom.runtime.scope_frame_measure_native` | A frame through the headless device route against the CPU reference: exact for the Working signal, within one cell through the OCIO steps, one table readback of the layout's bytes and no picture readback, the route named; a Histogram-only measure reads 16,448 bytes and runs no color program |
| `bloom.scripting.scope-read` | The headless read: tables equal the CPU reference cell for cell for both signals, counters, luma and primaries, sums, sparse equal to dense, the summary, pictures, a range with progress and a deterministic cancel, the warm port (tables that lack one asked for are never served), region, transparency, resolution, display and view, and every refusal with its remedy |
| `bloom.mcp.scope-query` | `query` kind `scopes` on the shared core: the tables equal the read's, sparse, summary, image and range answers, and the refusals |
| `bloom.scripting.python-scopes` | `bloom.scopes.read` and `image` from Python: a flat color's tables, sparse equal to dense, summary, range and time, written and replaced files, argument errors |
| `bloom.ui.scopes-mcp-warm` | The MCP read against a real preview and cache: a cached frame whose tables were counted under the read's own settings is answered with them and no measure; every setting, resolution, display and view, region of interest, isolation, look or composition that differs is measured |
| `bloom.scripting.scope-read-native` | The headless read on a real device: route Gpu, Working tables equal the CPU reference, CIE and Display tables within one cell; a disabled provider reads with the CPU reference and says why |
| `bloom.color.scope-colorimetry-external` | The XYZ transform of an external config through the helper process equals the built-in's; spaces with no XYZ interchange are refused with their reason |
| `bloom.ui.scopes-region` | Draw Scope Region and Clear Scope Region from the header menu, the gestures, and a region that is no project state: a project saved with a scope region equal, byte for byte, to one saved without |
| `bloom.ui.viewer_resident_scopes_region_native` | The scope region and its handles are drawn over the device-presented Viewer picture (needs a window system and a device) |

The device tests skip without a device and fail under `--require-device`.
