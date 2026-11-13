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
| `bloom.color.scope-colorimetry` | The XYZ transform, the luma coefficients and the primaries of the built-in configs |
| `bloom.runtime.scope_frame_measure_native` | A frame through the production device route against the CPU reference: exact for the Working signal, within one cell through the OCIO steps, one table readback and no picture readback, the route named |

The device tests skip without a device and fail under `--require-device`.
