# The Scopes panel

The Scopes panel measures the picture a Viewer shows and draws it as five video scopes: Waveform,
Parade, Vectorscope, Histogram and CIE chromaticity. Use it to judge exposure, clipping, color
balance, saturation and gamut by numbers instead of by eye.

Open it by choosing **Scopes** in any panel's editor picker. It is an ordinary panel: put it
anywhere, or open several. It follows the Viewer you used last.

The scopes only read. They never change the picture, the project, the export or the undo history.
A hidden panel measures nothing, and a collapsed section's scope is not measured, so collapse what
you do not need.

Every number in a scope is a whole count of pixels of the frame. The scopes follow the Viewer's own
picture: they count the picture the Viewer just produced, in full precision, and never render the
frame a second time. The graphics processor and the processor fallback give the same counts.

## The header

- **Signal** chooses what is measured.
  - **Display** (default) is the picture after the Viewer's display and view: the values your
    screen is sent. Its axis is 10-bit code values, 0 to 1023.
  - **Working space** is the scene-linear picture before the display transform. Its axis runs from
    0 to an **Axis maximum** you choose (1, 2, 4, 8 or 16), and its labels are linear values.
- **Region** arms a rectangle drag in the Viewer, to measure only part of the frame. **Clear
  Region** removes it. See [The region](#the-region).
- **Ignore Transparent Pixels** leaves out every pixel whose alpha is 0. It is off by default, so
  an empty part of the frame counts as black.
- **Brightness** makes faint traces brighter or dimmer. It changes only the drawing, never the
  counts. A cell that holds even one pixel is always visible.

## The status line

Under the header, always visible, the status line says exactly what the scopes on show were
measured from:

- the composition and the frame (with its time in seconds);
- the display and view, or the working space;
- the resolution measured: Full, Half or Quarter, as the Viewer is set;
- the measured rectangle, when a region or the Viewer's region of interest applies;
- **GPU** or **CPU**: which one counted the frame. When the graphics processor could not count a
  frame, the processor does, and the line says why;
- the number of pixels counted;
- **Live detail**, while the scopes on show are the slightly coarser live measure;
- a note in parentheses at the end when the frame was counted from its cached picture (see
  [Cached frames](#cached-frames)).

While the picture is changing, as in playback or while you scrub, the scopes show a slightly
coarser live measure of each frame they reach and update at that live detail; a frame that stays is
measured again at full detail, and the status line says "Live detail" until then. When a newer
frame arrives before a frame could be counted at full precision, that frame is counted from its
cached picture instead, and the scopes settle to full detail once the frame stays still and its
full-precision picture is still held.

A second line in the warning color lists every Viewer setting that makes the measured picture
differ from what an export writes: exposure or gamma that is not neutral, a channel view,
isolation, or the look turned off.

**Behind** means the scopes on show belong to another frame, or to an earlier setting, than the
one the Viewer shows now, because the new measure has not finished. The line names the frame that
was measured, the traces are dimmed, and each scope carries the word. Scopes of another frame are
never shown without it.

## The scopes

### Waveform

Brightness against horizontal position. Each column of the scope is a column of the frame; the
height of a trace is the code value. Red, green and blue are drawn over each other in their own
colors, so where they agree the trace turns white. Choose **Luma** in the section's header to see
one gray trace instead. A small mark on the top or bottom edge of a column means pixels in that
column are above the top or below the bottom of the axis.

### Parade

The same measure as the Waveform with red, green and blue side by side, each a third of the width.
Use it to compare the channels: a color cast shows as one channel sitting higher or lower.

### Vectorscope

Hue and saturation without brightness. The center is neutral gray; distance from the center is
saturation and the angle is hue. The six boxes R, Mg, B, Cy, G and Yl mark where 75% color bars
fall, and the dashed line is the skin tone line. With the Working space signal the header says
**Linear values**, because the positions are then computed from linear light.

### Histogram

How many pixels have each code value, one row per channel, on one shared scale. The section's
header chooses **RGB**, **Luma** or **RGB and Luma**, and a **Linear** or **Logarithmic** height
scale; the logarithmic scale makes small counts visible beside large ones. A mark at the left or
right end of a row means that channel has pixels below 0 or above the top of the axis; point at it
to read how many.

### CIE Chromaticity

The colors of the frame on the CIE 1931 xy diagram. The curve is the edge of visible color, the
triangle is the gamut of the measured color space, and the small ring is its white point. The top
right corner prints the space's name and the xy of its red, green, blue and white.

Point at any scope to read the value under the pointer in that scope's units, and the count of
pixels there.

## The region

Click **Region**, then drag a rectangle in the Viewer. The scopes then measure only that part of
the frame, and the status line gives the rectangle and the pixels counted. The Waveform's width
becomes the region's width.

- While **Region** is on, drag inside the rectangle to move it. Drag a handle to resize it at any
  time. Escape cancels a drag.
- **Clear Region** measures the whole frame again.
- The rectangle is drawn in every Viewer that shows the composition, labeled **Scopes**, while a
  Scopes panel is visible. It is separate from the Viewer's region of interest.
- The region belongs to the session and to one composition. It is not saved in the project, is not
  an undo step, and does not change rendering or export.

## RAM Preview

While a RAM Preview fills and a Scopes panel is open, each frame is measured as it is made, at full
detail and for both signals, and its scopes are kept with the cached frame. Every frame in the range
then has full scopes, and the scopes play along with the cached range without measuring again. A
fill with no Scopes panel open costs nothing extra.

## Cached frames

A frame that was cached while Scopes was closed, or shown from an older cache, has no full-precision
picture left to count: only the 8-bit picture the Viewer paints. For such a frame:

- **Display** and **CIE chromaticity** are counted from that cached picture. They are slightly
  coarser, because the picture has 8 bits per channel, and values the display encoding pushed below
  0 or above the top of the axis are not counted as under or over. The status line ends with
  "(counted from the cached picture)", and a reason when there is one.
- **Working space** is not available. The panel says "The Working signal is not available for a
  cached frame. Switch to the Display signal, or show the frame again to measure it."

To get full scopes for such a frame, show it again so that it is rendered rather than taken from
the cache (a frame the Viewer has just rendered is measured at full precision), or run the RAM
Preview with Scopes open.

## Reading scopes from MCP and Python

The same measure is available to agents and scripts, with no panel open and in headless sessions.

- MCP: `query` with `kind: "scopes"` returns the count tables, a summary (per channel minimum,
  maximum, mean, percentiles and clipped counts, and the Vectorscope's mean saturation and hue) or
  a PNG picture of one scope or all five. See [Using Bloom from an agent (MCP)](mcp.md).
- Python: `bloom.scopes.read` and `bloom.scopes.image`. See [Scripting Bloom with Python](python.md).

A read names its own composition, frame, signal, display and view. It uses the composition's own
defaults, not the exposure, gamma, channel view or isolation of an open Viewer. A read is answered
from the application's RAM preview only with full-precision scopes; scopes counted from a cached
8-bit picture are never given to a read, which measures the frame instead.
