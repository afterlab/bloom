# Scopes

The Scopes panel (`bloom.scopes`, issues #724, #730 and #733) shows five video scopes of the picture a
Viewer shows: Waveform, Parade, Vectorscope, Histogram and CIE chromaticity. This document is the
specification of what is measured, where it is computed, how it is cached and how it is drawn, and
it is the owning document of the coverage contract entry `prep.scopes.result_readback`. The drawing
is matched to the scopes of the reference grading application.

## What is measured

One definition, whatever way a frame was reached (paused, scrubbed, played, cached or not, on the
device or on the CPU). A measure counts the pixels of a measured area of one float picture, the
signal, into count tables, and every cell of a table is a whole number of pixels. A table has the
size its pane draws ("Table sizes"); a headless read counts the full tables below.

### The signal

| Signal | The picture that is counted |
|---|---|
| Display (default) | The frame's stored display cells ("The cached display frame"): every pixel's straight color through the frame's display, view and look, packed to 10-bit codes, read as its color over black |
| Working | The scene-linear picture as it is stored: its color over black and its alpha |

The signal is taken over black: red, green and blue are the color times alpha.

The Display code of a channel is read from the stored cell. In a 10:10:10:2 frame it is the stored
10-bit code of an opaque pixel and 0 for a transparent one. In a straight 8:8:8:8 frame it is
`round(c8 * a8 * 1023 / 65025)` in whole numbers. The float a scope counts is the correctly rounded
`code / 1023`, and the alpha is 1 or 0 for a 10:10:10:2 pixel and `a8 / 255` for a straight one
(`displaySignalCodes`, `displaySignalPixel`, `display_pixel_packing.hpp`). A stored code is never
outside 0..1, so the Display tables' under and over cells are 0. What the display encoding left
below 0 or above 1 is the frame's own under and over, which the frame's counters hold ("The cached
display frame"); they count the whole frame and do not follow the scope region or Ignore
Transparent Pixels.

The Viewer's exposure and gamma are a viewing aid and are not part of the Display signal: every
cached frame is made neutral, and the scopes of a frame are the same whatever the Viewer's
adjustment. The Working signal is kept as it is, so a value below 0 or above the axis is counted as
under or over in its tables. A pixel with no alpha is stored as it is.

The axis is 10-bit full range, code values 0 to 1023. The Display signal's axis is 0..1; the
Working signal's has a maximum M chosen from 1, 2, 4, 8 and 16 (Signal > Working Axis Maximum), and
changing M counts the Working tables again.

### Table sizes

Tables are counted at the size the panes draw (`ScopeTableReduction`, `scopeReductionForWidth`,
`scope_table_layout.hpp`). Each table is the full table divided by a power of two on each axis: the
smallest size whose axes are not below the pane's device pixels, and never above the full 10-bit
table. A reduced cell is the exact sum of the full cells it covers, so a reduced table equals the
full table summed. The Histogram is never reduced, and under and over cells are never merged with a
code. A pane that is collapsed or hidden asks for no table. When several panes draw the same table,
it is counted at the largest size any of them draws, and with no pane reporting, every table at the
full size. A headless read counts the full tables.

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
not a number, is `under`; above 1023 is `over`. This is the Working signal's rule. The Display
signal's floats are `code / 1023` of a stored code, which this rule gives back as the same code, so
a Display pixel is never `under`, `over` or not finite; the frame's under, over and not finite of
the display transform's float result are the frame counters ("The cached display frame"), counted
over the whole frame.

| Table | Cells | Where a pixel goes |
|---|---|---|
| Histogram | 4 channels (red, green, blue, luma) of 1024 bins, under and over | Bin `c` of each channel |
| Waveform (and Parade) | 4 channels of `columns` columns of 1024 levels, under and over; `columns = min(512, W)` for a measured width `W` | Column `floor(x * columns / W)`, level `c` |
| Vectorscope | 640 by 640 counts, and 640 by 640 luma sums | `cb = (b - y) / (2 (1 - Kb))`, `cr = (r - y) / (2 (1 - Kr))`; cell `(floor((cb + 0.625) * 512), floor((cr + 0.625) * 512))`; outside 0..639 adds 1 to `vectorscopeOutside`; otherwise the pixel adds 1 to the cell's count and its luma code, clipped to 0..1023, shifted right by 4 (0..63), to the cell's luma sum |
| CIE chromaticity | 512 by 512 | `(X, Y, Z)` is the signal converted to CIE XYZ with a D65 white; `s = (X + Y) + Z`; not finite or `s <= 0` adds 1 to `noChromaticity`; otherwise `x = X / s`, `y = Y / s`, cell `(floor(x / 0.8 * 512), floor(y / 0.9 * 512))`; outside 0..511 adds 1 to `chromaticityOutside` |

The cells above are the full tables. A reduced table ("Table sizes") divides its Waveform columns
(`ceil(columns / 2^s)`), its Waveform levels (`1024 >> s`), its Vectorscope axes (`640 >> s`) or its
CIE axes (`512 >> s`) by a power of two, and a pixel's reduced index is its full index shifted right.

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

## Where each signal is counted

The Display and Working signals are counted in different places, and the measure of both is the
definition above.

| Signal | Where it is counted |
|---|---|
| Display | From the frame the Viewer shows, at display time, for that frame only. No frame and no cache entry stores Display tables |
| Working | By the frame's own preparation, in the frame job, and cached with the frame |
| Either, in a headless read | By the read itself, at the full table size |

### The Display signal of the shown frame

The Display signal of a shown frame is its stored display cells, so its tables need no scene and no
display transform. `ShownFrameScopeMeasurer` (`shown_frame_scopes.hpp`) takes the frame the Viewer
shows and counts it at the table size the panes draw, without rendering. Opening a Scopes panel
after a RAM Preview is cached renders nothing: the shown frame is counted and the traces appear.
Changing the region, the visible scopes or Ignore Transparent Pixels counts the shown frame again
and renders nothing.

| Route | How |
|---|---|
| Device | A job of the preview display service that is not a frame job (`gpu_preview_display_service_shown_scopes_private.hpp`): one transient upload of the cells, unpacked on the device by the kernel `DisplayUnpackV1` into the signal picture, the CIE XYZ color step only with the Chromaticity table, the count kernel `ScopeTablesV1` (`render::GpuScopeTables`) and one table readback. No scene runs and no picture is read back |
| CPU | A worker counts the stored cells with the CPU reference (`measureScopeStoredFrameCpu`): the same tables, labeled Cpu |

Frames always come first. A measure is taken up only while no frame has native work on the device.
It never delays a frame job: a frame stage that needs the device parks the measure's processing
phase and goes on. A measure the device does not take up, because the device is busy or has no room,
is dropped, and a worker counts the frame with the CPU reference. A measure that was taken up
advances one non-blocking step per turn of the service's owner thread and never waits on a fence.

One measure is in flight per measurer, and a newer shown frame replaces the waiting one. During
playback the scopes follow at the rate the route can count and playback never waits; the frame
shown when playback stops is always counted. Without a device the same structure runs on a worker.

A frame held on the device only (a resident display image with no host cells) has no stored cells
to count: its tables are a Failed record with that reason. The Viewer is painted from host cells on
all three platforms, so the frames it shows have them.

### The Working signal in the frame job

While a Scopes panel is visible and the Working signal is selected, every preview request (the
Viewer's, read-ahead's and RAM Preview's) asks the frame's preparation to count the Working tables
too (`PreviewRequestIdentity::scopes`, `preview_frame_scopes.hpp`); with no panel visible, or with
the Display signal selected, nothing is asked and nothing is counted. The request carries the
session's settings: the tables the panes draw and their size, the Working axis maximum, the
composition's scope region and Ignore Transparent Pixels. A collapsed pane asks for no table, so
the tables asked for follow the open panes.

The frame's own route counts the tables, once, while it prepares the frame:

| Route | How |
|---|---|
| Device | The preview display service's frame job. The worker that prepares the frame's GPU scene also prepares the measure's color programs (`prepareFrameScopeMeasure`). When the frame's display step is ready, the job counts the scene picture it computed: the XYZ color program (with the CIE table) and the fixed embedded kernel `ScopeTablesV1` (`render::GpuScopeTables`), with its one table readback. Only then does the frame's final transfer begin, and the frame is published carrying its tables (`gpu_preview_display_service_scope.cpp`). No scene runs again and no picture is read back |
| CPU | The worker that evaluated the frame counts its float process picture (`measurePreviewFrameScopesCpu`, `composition_preview_cpu_stage.cpp`) before the display picture is made |

A frame the device cannot count (no shared OCIO context for the color programs, a scene outside the
prepared-GPU subset) takes the CPU route, which counts its tables from its process picture. A frame
whose device measure fails after its scene ran is published with a Failed record for its settings,
so it is not prepared again in a loop; the scopes draw the empty graticule for it.

The tables travel with the frame (`PreparedPreviewFrame::scopes`) to the Viewer and into the
preview frame cache. A request for the Display signal gets a Failed record from a frame's
preparation, which says that the Display signal is counted from the shown frame.

### Headless reads

A headless read (MCP and Python, "Reading scopes from MCP and Python") has no Viewer frame. It
measures through `measureScopeFrame` (`scope_frame_measure.hpp`) with the same definition, at the
full table size, with a device route and a CPU reference. A Display read renders its frame, packs it
as a preview display frame and counts the stored cells:

| Route | How |
|---|---|
| Device | A job of the final-render evaluator (`GpuProcessFrameEvaluator::measureScopes`, `gpu_scope_measure.hpp`), when the frame's route is the device and the device can. One program, the scope display step (`GpuOcioTransformKind::ScopeDisplay` with a preview packing), finds each pixel's stored cell with the display pack's statements and writes the unpacked signal, 10:10:10:2 first and a refused frame once more as 8:8:8:8. Then the XYZ step only with the Chromaticity table, the count kernel and one table readback. The frame is packed and unpacked on the device and no picture leaves it |
| CPU reference | The CPU evaluator, the preview frame's CPU packer (`color::produceBloomNeutralDisplayFrame`) and `measureScopeStoredFrameCpu`, the call that counts a shown frame |

The result names the route that counted it and, for a Display read, carries the frame counters and
the layout of the stored cells. A frame with a pixel that is not finite has no display frame, on
either route and in a Viewer alike: its Display read fails with the reason, and its Working signal is
still counted. A Working read counts the scene-linear picture and has no display frame.

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

For the same stored cells the device and the CPU give equal tables, cell for cell, because the
input is whole numbers and the unpack is correctly rounded: the shown-frame measure
(`bloom.runtime.shown_frame_scopes_native`), the unpack kernel
(`bloom.render.gpu_display_unpack_native`) and the count kernel
(`bloom.runtime.scope_measure_gpu_parity`) prove it. The Working signal's Histogram, Waveform and
Vectorscope pass through no OCIO step and are exact too.

The conversion to XYZ is OCIO's CPU and GPU processors, which round differently, and so is the
display transform a headless Display read runs on the device. Through those steps a pixel may land
in the neighboring cell: every pixel is still counted, and along every table axis no pixel is more
than one cell from where the CPU reference puts it (`bloom.runtime.scope_frame_measure_native`,
`bloom.runtime.preview_frame_scopes_native`).

The device's and the CPU's display transforms round differently, so two frames of the same picture
prepared on the two routes agree within one stored code (one code of 1023 for a 10:10:10:2 frame,
one code of 255 for a straight frame), with the same layout and the alpha exact
(`render::displayFramesAgree`, `bloom.render.gpu_display_packing_native`). The scope display step's
picture is, bit for bit, the display program's packed cells unpacked
(`bloom.render.gpu_display_packing_native`).

## The cached display frame

A prepared preview display frame is 4 bytes a pixel, in one of two layouts for the whole frame
(`display_pixel_packing.hpp`).

| Layout | A pixel |
|---|---|
| `Rgb10A2` | One 32-bit word: red, green and blue as 10-bit codes and a 2-bit alpha (3 opaque, 0 transparent) |
| `Straight8888` | Straight 8-bit red, green, blue and alpha |

A frame is `Rgb10A2` when every pixel is opaque or exactly transparent, and `Straight8888` when any
pixel is partly transparent. Both layouts cost the same memory: a frame costs 4 bytes a pixel plus
the 36 bytes of its counters. The layout is the frame's content and is not part of its cache
identity. Export and output packers are not preview frames: they stay 8-bit RGBA with their own
goldens.

The 10-bit code of a value is `floor(clamp(v, 0, 1) * 1023 + 0.5)` in binary32. This is the
scope's code rule, so a stored code is the code a scope counts (`displayCode10`).

### Which routes pack it

Every route that prepares a preview frame packs it this way:

- the CPU route: the OCIO display processor and the built-in reference display;
- the device's neutral display kernel;
- the device's OCIO display program, one program for both layouts, whose dispatch names the layout
  in the status block;
- the resident display image.

### Learning the layout on the device

The device learns a frame's layout only by packing it. A 10:10:10:2 pack of a frame that does not
allow it is refused (`LayoutRefused`) and publishes nothing, and the frame is packed again as
8:8:8:8. An 8:8:8:8 pack whose report says the frame allows 10:10:10:2 is packed again as
10:10:10:2. At most one second pack happens for a frame. The preview display service remembers the
layout of each composition's last frame and asks for that one first (`displayLayoutGuess`,
`gpu_preview_display_service_private.hpp`). The status block of a pack (48 bytes, 12 words,
`gpu_display_pack.hpp`) carries the kernel's error word, whether the frame allows 10:10:10:2 and the
frame counters, so no pixel is read back for them.

### Presentation

An 8-bit output shows the top 8 bits of a 10-bit code (`presentedCode8`, `presentedRgba8`); a 10-bit
output shows the code unchanged. The Viewer's painter draws the 30-bit image without a copy, and the
device presenter follows the same rule. The presenters keep 10 bits when the surface they are given
is deeper than 8 bits; the application does not ask the system for a deeper window surface. The
tests are `bloom.ui.viewer-presentation-depth` and `bloom.ui.gpu_present_image_offscreen`.

### Frame counters

A frame carries the counts that its stored cells cannot (`DisplayFrameCounters`, 36 bytes): under and
over for red, green, blue and luma, and not finite, of the whole frame. They are counted from the
display transform's float result over black, before the Viewer's exposure and gamma and before any
clamp. Luma uses the display's own coefficients, measured through OCIO as the scopes measure them;
when they cannot be resolved the luma pair is marked absent. The counters travel with the frame and
are charged to the preview cache with it. On the device they come from the pack's status block.

They are the Display signal's under and over: the Histogram readout of the panel, the Summary's
clipped counts of a Display read and the MCP and Python results all read them.

## Cached with the frame

A frame's Working tables are kept with the frame in the preview frame cache (`PreviewFrameCache`):
the packed tables (`ScopeTables`, each table packed to the narrowest whole-number width that holds
its largest count), the measured area, the route that counted, the settings and table size they
were counted under and, for a Failed record, the reason. No Display tables are stored, with a frame
or anywhere else.

- A frame's tables never decide whether it is a cache hit. A cached frame without tables of the
  current settings is shown at once; when a visible panel asks for Working tables, the Viewer's
  shown frame is prepared again in the background at the lowest priority to count them
  (`PreviewFrameCache::take`, `containsWithScopes`). RAM Preview coverage never depends on
  tables, and playback never waits for scopes.
- The tables are charged to the cache's host byte budget whatever the frame's pixel storage is
  (`scopeTableBytes`, part of `hostBytes`), and a frame whose tables do not fit is not kept.
  The frame counters are charged with the frame (`previewDisplayCountersByteCost`).
- The frame cached again without tables keeps the ones it carries; a frame with tables of other
  settings replaces them. An edit of the document gives the frame another cache key, so the frame
  and its tables are both new. The tables leave the cache with their frame.
- A RAM Preview fill carries no Display tables, whether or not a Scopes panel is visible, so a
  panel opened after the fill prepares no frame again. With the Working signal and a visible panel,
  the fill counts Working tables with each frame.
- Playback and scrubbing prepare nothing for the scopes. On the Display signal the shown frame is
  counted at display time; on the Working signal a cached frame's tables are read.

## The panel

`ScopesEditor` (`bloom.scopes`) is five collapsible panes, each with only its title and collapse
arrow: no status line, warning, note or other text, no slider, dropdown or button. Its settings are
in the header menus of its editor area (`EditorChromeProvider`, `docs/ux/ui-grammar.md`):

| Menu | Items |
|---|---|
| Signal | Display, Working Space (a radio group); Working Axis Maximum (1, 2, 4, 8, 16; enabled for Working Space); Ignore Transparent Pixels (a check) |
| Region | Draw Scope Region (a check; disabled with no Viewer to draw in); Clear Scope Region |
| Scopes | Waveform: RGB, Luma; Histogram: RGB, Luma, RGB and Luma, then Linear, Logarithmic |

The settings are session state shared by every Scopes panel (`ScopesRegionStore`), so two panels
never ask for different ones. Signal, region and Ignore Transparent Pixels change what is counted.
On the Display signal the panel counts the shown frame again and renders nothing; the Working axis
maximum is not a Display setting and counts nothing. On the Working signal the Viewer's frame is
counted again in the background. The Scopes menu changes only how the tables are drawn: the panes
map the same tables again.

**Pane sizes.** Each pane reports its trace size in device pixels, and the session merges the open
panes of every panel into the tables to count and their size ("Table sizes"). A pane that is
collapsed or hidden asks for no table. A pane resize or collapse counts the shown frame again and
nothing else.

The panel follows a Viewer and is handed the frame the Viewer shows. On the Display signal it hands
that frame to the shown-frame measure, which counts it off the interface thread, and it hands the
panes the tables when they arrive. The same frame under the same settings and table size is asked
once. On the Working signal it hands the panes the tables the frame carries when they were counted
under the session's settings; a frame without them changes nothing. Each pane maps its tables to a
trace image on a task worker (`ScopesTraceRenderer`): the newest request waits behind the one
running, which is never cancelled, and nothing of the mapping runs on the interface thread.

**Never dim.** A pane shows its last trace at full strength until the new trace replaces it in one
swap: the new image, the tables it was mapped from and the modes it was mapped with together. The
panel never dims, fades, blanks, flashes or shows a placeholder while new tables arrive or a trace
is mapped, and playback updates the traces as the measure can follow it. A frame whose tables say
EmptyArea or Failed, and no frame at all, draw the empty graticule, silently. Nothing changes the panel's
layout while frames change: each pane's height depends on its width alone. Hovering a trace shows
the value under the pointer and the count of the cell there, as a tooltip that changes no layout.
A trace of a reduced table reads the reduced cell, so a count is the sum of the cells it covers. The
Histogram's readout at its first and last code adds "Under N" and "Over N" when they are not 0: for
the Working signal these are the tables' under and over cells, and for the Display signal they are
the frame counters of the whole frame (`outsideAxis`, `scopes_view.cpp`).

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
disposition and bound are the same whichever device counts: the preview service's device for the
shown frame's Display measure and for the Working tables of a frame a Viewer, read-ahead or RAM
Preview asked for, the final-render device for a headless read.

- What is read back is at most 3,186,704 unsigned 32-bit words (8 counters, the Histogram's 4,104,
  the Waveform's 2,101,248 at 512 columns of 1,026 words, the Vectorscope's 819,200 counts and luma
  sums and the CIE table's 262,144): 12,746,816 bytes, whatever the size of the picture. This is
  the contract entry's bound (`render::kScopeTablesMaxBytes`). The readback of one measure is the
  bytes of its layout (`ScopeTableLayout::byteCount`): a table counted at the size a pane draws is
  smaller than the full table below, a narrower area has fewer Waveform columns, a measure that asks
  for fewer tables reads back only those, and one without the CIE table runs no XYZ color step.
  A headless read counts the full tables. `ScopeFrameMeasure::tableReadbackBytes` reports the bytes
  a measure read back (0 for the CPU reference).
- The picture is never read back, and no per-node or per-operation full-frame roundtrip exists. The
  color steps and the counting are device work; the measure reports its picture readbacks, which
  are always zero, and the tests assert it.
- It is not an exemption of any pixel operation, feature or route: every operation of the measured
  frame stays Required device work.

Bytes read back by one measure of the full tables, on an area at least 512 wide (the counters are 32
bytes):

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
| The scope display step with a preview packing (a headless Display read only) | 1 | 1 |
| The unpack kernel `DisplayUnpackV1`, on the upload of the cells (a shown frame's Display measure only) | 1 | 1 |
| The XYZ color step (only with the CIE table) | 0 | 1 |
| The count kernel's pixel pass | 1 | 1 |
| The count kernel's column pass (only with the Waveform) | 0 | 1 |
| Table readback | 1 | 1 |

The entry is the decision O1 of the Scopes specification of issue #724.

## Cost and scheduling

No timing is measured or asserted anywhere; the cost is shown by structure and by tests of that
structure.

- The Working tables of a frame are part of its own job: the count runs on the frame's device
  between the frame's display step and its final transfer, with the frame's own byte allowance, one
  native submission at a time, polled by the service's owner thread and never waited on
  (`ScopeMeasureStepper`; there is no `vkWaitForFences` in the scope tables code). It observes the
  frame's cancellation and deadline between its steps and keeps polling a submission until its fence
  is proven retired.
- The Display measure of a shown frame is a job of its own on the same device. It is taken up only
  while no frame has native work and the device holds no processing phase, otherwise it is dropped
  at once and a worker counts the frame. It advances one non-blocking step per owner turn after the
  turn's frame steps, and a frame stage that needs the device parks its phase and goes on, so a
  frame is never delayed. One measure is in flight per measurer and a newer shown frame replaces
  the waiting one.
- The kernel's table slot is one per process; a measure that meets another in flight waits for the
  slot (polled), and a headless read that meets a frame's count is counted by the CPU reference,
  labeled CPU, with the reason. Neither reads the other's tables.
- The count kernel, the unpack step and the color programs are created on first use and released
  when the service is idle; their retained programs are given back first when a frame needs device
  memory.
- On the CPU route the Working count runs on the worker that evaluated the frame and the Display
  count on a worker, never the interface thread.
- Playback never waits for the scopes. Playback of a cached range reads cached Working tables, and
  counts the Display tables of the frames shown at the rate the route can count; the frame shown
  when playback stops is always counted. Playback of frames being rendered counts the Working
  tables of each frame once, as part of preparing it.

What the structure leaves open: the counting pass and the color steps run on the same physical GPU
as the frame's own work and lengthen a Working frame job by their device time.

## Reading scopes from MCP and Python

`scripting/scope_read.hpp` is the one Qt-free read behind MCP's `query` kind `scopes` and Python's
`bloom.scopes`. It validates the request on the calling thread, then runs one task on the caller's
scheduler that resolves the color configuration, chooses the display and view (the configuration's
own defaults, never the Viewer's inspection settings), compiles the plan once and measures each
frame through `measureScopeFrame` ("Headless reads"), at the full table size, with the same CPU/GPU
parity as the frame pipeline. The caller's cancellation is asked before the task is submitted (a
read already cancelled never starts and measures nothing, however soon a worker would have run it)
and polled while the task runs, reaching inside a frame. Tables are described as flat arrays (dense, or the non-zero cells as index and count
pairs), a summary is `summarizeScopes` of the tables, and a picture is `renderScopePicture` or
`renderAllScopesPicture` encoded with `encodeScopePng`, so a picture equals what the panel draws.

**The warm port.** In the desktop application a read of a frame the preview cache holds is answered
from the cache (`warmScopes` of the `McpLiveUiBinding`, `ScopeWarmQuery`). The port answers only
for a frame of the same picture, display and view, and a Viewer state of no adjustment, no region of
interest, no isolation and no channel view. A Display read also needs a cached frame that holds host
cells and carries its counters (`PreviewFrameCache::displayFrameOf`).

- A Display read is answered with the cached frame (`ScopeWarmHit::frame`). The read counts the
  stored cells of that frame with `measureScopeStoredFrameCpu` and renders nothing. The result is
  marked as answered from the cache.
- A Working read is answered with the tables the cached frame's own preparation counted, only when
  they were counted under exactly the read's settings at the full size; a frame without them is
  measured by the read.

**Frame counters and layout.** The result of a Display read carries the frame counters and the
layout of the stored cells, and the frame's counters are the read's under and over. MCP names them
`frameCounters` (`under` and `over`, each a list of red, green, blue and luma, and `notFinite`) and
`storedLayout` (`"10:10:10:2"` or `"8:8:8:8"`). Python names them `frame_counters` (`under`, `over`
and `not_finite`) and `stored_layout`. The luma entry is null (None in Python) when the display's
luma could not be resolved. The Summary's clipped counts of a Display read are the frame
counters; the luma pair is 0 when the frame's luma coefficients were not resolved. The counters
count the whole frame, whatever the region and Ignore Transparent Pixels. A Working read carries
neither.

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

The Working measure of a frame is part of its frame's request: the color outputs, the table buffer,
its staging copy and the host copy are within the frame's byte allowance, and a submission that
could not be proven retired is retired by the service's idle owner as the frame's other stages are.
The color programs are prepared on the worker that prepared the frame's scene and never on the
device owner, the job observes the request's cancellation between its steps, and nothing is
published after a cancel. A headless measure is part of its request on the final-render evaluator
in the same way.

The Display measure of a shown frame reserves its own processing phase, which a frame stage parks
when it needs the device. Its staging buffer, the unpacked picture, the table buffer and the host
copy are within that phase's allowance, and a measure the phase does not admit, or that finds no
room, is dropped and counted by the CPU reference. A newer shown frame replaces a waiting measure.
`clear()` drops the waiting measure, cancels the one in flight where it can be, and discards its
result. A stopping service ends waiting measures as Cancelled, cancels the one in flight and steps
it until nothing of it is unretired, and no result callback is made after the measurer is destroyed.
The measure holds no picture between frames.

## Tests

| Test | What it proves |
|---|---|
| `bloom.runtime.scope_measure` | The CPU reference: known answers for every table, the counters, areas, columns, the axis maximum, the packed form, the summary and the measured area; the fixed layout's exact byte counts; a 10-bit ramp on every one of the 1024 Waveform levels; the Vectorscope's luma sums; 75% and 100% bars inside the table and the 0.60 ring; reduced tables equal to the full tables summed; stored display frames of both layouts counted as their Display signal |
| `bloom.render.gpu_scope_tables` | The kernel on a real device: every count against a host count of the same pictures, full and reduced tables, an area wider than the Waveform's columns, the bytes read back equal to the layout's, and its refusals (no XYZ picture, a budget below the tables) |
| `bloom.runtime.scope_measure_gpu_parity` | CPU and device tables equal for the same float pictures, cell edges and wide areas included |
| `bloom.render.display_packing` | The display pixel rules: every one of the 1024 codes round-trips, the pack boundaries, the layout rule, the Display signal and presentation of both layouts, the frame counters, the reference rows and the layout tag of the buffer descriptor |
| `bloom.render.gpu_display_unpack_native` | `DisplayUnpackV1` on a real device: the unpacked picture equals the CPU rule for both layouts, bit for bit |
| `bloom.render.gpu_display_packing_native` | The device display packs (Bloom Neutral kernel, resident display, OCIO display program) against the CPU packers on a shared frame: every 10-bit code stored exactly, ordinary frames within one stored code with alpha exact, the layout flag, the refusal of a 10:10:10:2 pack that the frame does not allow, equal counters, the 8:8:8:8 words as before, and the scope display step's picture equal to the display program's packed cells unpacked |
| `bloom.runtime.shown_frame_scopes` | The shown-frame measure on a worker: a paused frame is counted exactly, a reduced rate while frames replace each other, `clear`, area and signal, and a measurer destroyed while a measure is held |
| `bloom.runtime.shown_frame_scopes_native` | The shown-frame measure on a real preview device: stored frames and a prepared frame counted equal to the CPU reference cell for cell, frames coming first, regions, and shutdown during a measure |
| `bloom.ui.viewer-presentation-depth` | The presentation rule against Qt for all 1024 codes, and every reader of a cached frame (display view, cache copy, channel pictures, probe, thumbnail) over both layouts |
| `bloom.ui.gpu_present_image_offscreen` | The device presenter shows the same top 8 bits on an 8-bit target and the codes on a 10-bit target |
| `bloom.runtime.preview_frame_scopes_native` | The frame pipeline on a real preview service: a frame whose request asks for Working tables is published carrying them, counted by its own job, equal to the CPU reference, one table readback and no picture readback; a region outside the picture is EmptyArea; a request without tables counts nothing; a request for the Display signal gets a Failed record |
| `bloom.runtime.scope_picture` | The Qt-free picture: the plan's facts for each scope (Waveform codes and ticks, Vectorscope ring, ticks, cross hairs, targets and skin line, CIE grid, labels, loci, triangle, names and printed lines, Parade panes, Histogram rows); no column stripes at measured widths 640, 1000 and 1920; a 10-bit ramp as one continuous trace and a flat Histogram; a column's intensity independent of other content; values outside the axis on the 0 and 1023 lines and in the end bins; a single pixel visible; Histogram heights, outline and fill; CIE presence in full color; trace pixels equal to the mapping over the canvas; the picture of all five; PNG bytes decoded back; the refusals |
| `bloom.ui.scopes-panel` | The panel over real CPU frames: registration, the header menus and the settings they write, no text, slider, dropdown, check box or button in the panel; the Display signal counted from the shown frame on a worker and asked once per frame and settings, with no request to a frame's preparation; a setting change (Ignore Transparent Pixels, region, signal) counting the shown frame again and rendering nothing; the tables the open panes draw at the smallest size not below their device pixels, a collapsed pane asking for none; the Working signal's tables shown and a frame without them changing nothing; never dim: until the new trace is mapped every scope paints the previous one exactly, and every picture painted between two valid traces is one of the two; the layout constant across frames; the empty graticule for an empty area; mapping on a worker; the readout; collapse persistence; a hidden panel asking for no tables |
| `bloom.ui.scopes-drawing` | The panel's trace image is the Qt-free mapping's; additive light traces; null for no tables, a missing table and a cancel |
| `bloom.ui.scopes-picture` | The panel and the picture agree: palette and metrics equal the interface tokens, an offscreen grab and the picture agree on the trace rectangle of each scope, the plan's hairlines are where the grab has graticule pixels, and the hover readouts report the cell under the pointer |
| `bloom.ui.ram-preview` | Includes `testPanelOpenedAfterFillRendersNothing`: a RAM Preview cached with no panel, then a panel opened on the Display signal, prepares no frame again and the traces appear from the shown frame; and `testWorkingTablesNeverGateAFrame`: on the Working signal a cached frame without tables is shown at once and counted in the background, and RAM Preview coverage never depends on tables |
| `bloom.ui.preview-frame-cache-cpu-edge` | Includes `testNoDisplayTablesStored`: no Display tables are stored, a frame's tables never decide a hit, a frame without tables keeps the cached ones and other settings replace them; and `testDisplayFrameBytesAndCounters`: a frame costs its pixel bytes and the counters' bytes |
| `bloom.color.scope-colorimetry` | The XYZ transform, the luma coefficients and the primaries of the built-in configs |
| `bloom.runtime.scope_frame_measure_native` | A frame through the headless device route against the CPU reference: exact for the Working signal, within one cell through the OCIO steps, the Display read packed and unpacked on the device with its frame counters and layout, one table readback of the layout's bytes and no picture readback, the route named; a Histogram-only measure reads 16,448 bytes and runs no color program |
| `bloom.scripting.scope-read` | The headless read: tables equal the CPU reference cell for cell for both signals, counters, luma and primaries, sums, sparse equal to dense, the summary, pictures, a range with progress and a deterministic cancel, the warm port (a Display read answered from a cached frame, Working tables that lack one asked for never served), region, transparency, resolution, display and view, the frame counters and layout of a Display read, and every refusal with its remedy |
| `bloom.mcp.scope-query` | `query` kind `scopes` on the shared core: the tables equal the read's, sparse, summary, image and range answers, and the refusals |
| `bloom.scripting.python-scopes` | `bloom.scopes.read` and `image` from Python: a flat color's tables, sparse equal to dense, summary, range and time, written and replaced files, argument errors |
| `bloom.ui.scopes-mcp-warm` | The MCP read against a real preview and cache: a Display read of a cached frame is counted from the frame and renders nothing, a Working read of a frame whose tables were counted under the read's own settings is answered with them and no measure; every setting, resolution, display and view, region of interest, isolation, look or composition that differs is measured |
| `bloom.scripting.scope-read-native` | The headless read on a real device: route Gpu, Working tables equal the CPU reference, CIE and Display tables within one cell; a disabled provider reads with the CPU reference and says why |
| `bloom.color.scope-colorimetry-external` | The XYZ transform of an external config through the helper process equals the built-in's; spaces with no XYZ interchange are refused with their reason |
| `bloom.ui.scopes-region` | Draw Scope Region and Clear Scope Region from the header menu, the gestures, and a region that is no project state: a project saved with a scope region equal, byte for byte, to one saved without |
| `bloom.ui.viewer_resident_scopes_region_native` | The scope region and its handles are drawn over the device-presented Viewer picture (needs a window system and a device) |

The device tests skip without a device and fail under `--require-device`.
