# The Scopes panel

The Scopes panel measures the picture a Viewer shows and draws it as five video scopes: Waveform,
Parade, Vectorscope, Histogram and CIE chromaticity. Use it to judge exposure, clipping, color
balance, saturation and gamut by numbers instead of by eye.

Open it by choosing **Scopes** in any panel's editor picker. It is an ordinary panel: put it
anywhere, or open several. It follows the Viewer you used last.

The scopes only read. They never change the picture, the project, the export or the undo history.
Every number in a scope is a whole count of pixels of the frame, and the graphics processor and the
processor fallback give the same counts for the same frame.

## How the scopes keep up

The Display scopes are measured from the frame you see. When a frame is shown, its scopes appear at
once, with no new render: also after a RAM Preview was cached, when you open a Scopes panel on a
cached frame, and after you change a setting such as the region, the visible scopes or Ignore
Transparent Pixels. Nothing is stored with the frame for them.

While you play, the scopes follow as fast as they can count and the picture is never held back. The
frame that is shown when playback stops is always counted.

The Working scopes are measured as each frame is made, while a Scopes panel is visible, and kept
with the frame. A cached frame without them is still shown at once, and its scopes are counted in
the background. With no Scopes panel visible, nothing is measured and nothing extra is kept.

A scope is counted at the size of its pane, so a small pane counts a small table. A collapsed
scope is not counted.

The panel never dims, fades or blanks: each scope keeps its last picture at full strength until the
next one is ready and then replaces it at once. The panel has no status line or warnings; a scope
with nothing to show (no frame yet, or a region outside the picture) is an empty graticule.

Collapse the scopes you do not need with the arrow on their title; that changes only what is shown.

### Display scopes are 10-bit

The Viewer keeps each frame it shows at 10 bits a color channel when every pixel is opaque or fully
transparent, so the Display scopes show the 10-bit values your display transform produced. A frame
with a partly transparent pixel is kept at 8 bits a channel, and its scopes show those values. Both
cost the same memory. On an 8-bit screen the Viewer shows the top 8 bits of each 10-bit value.

### Under and over

The display encoding can leave values below 0 or above 1. A Display scope draws them on its bottom
and top lines. The number of such pixels in the whole frame is shown as **Under** and **Over** when
you point at the first or last code of the Histogram. These counts cover the whole frame, whatever
the scope region and Ignore Transparent Pixels.

## The header menus

The panel's settings are in its header menus. They are shared by every Scopes panel in the window.

- **Signal** chooses what is measured.
  - **Display** (default) is the picture through the composition's display, view and look: the
    values your screen is sent, at 10 bits for an opaque frame. The Viewer's exposure and gamma
    are viewing aids and are not part of it. Its axis is 10-bit code values, 0 to 1023.
  - **Working Space** is the scene-linear picture before the display transform. Its axis runs from 0 to
    the **Working Axis Maximum** you choose (1, 2, 4, 8 or 16), and its labels are linear values.
  - **Ignore Transparent Pixels** leaves out every pixel whose alpha is 0. It is off by default, so
    an empty part of the frame counts as black.
- **Region**: **Draw Scope Region** arms a rectangle drag in the Viewer, to measure only part of the
  frame; **Clear Scope Region** removes it. See [The region](#the-region).
- **Scopes**: the Waveform's **RGB** or **Luma**, and the Histogram's **RGB**, **Luma** or **RGB
  and Luma** and its **Linear** or **Logarithmic** height scale. These change only the drawing.

Changing the signal, the axis maximum, the region or Ignore Transparent Pixels measures the frame
you see again. The Display scopes render nothing for it. The Working scopes of other cached frames
are counted again as the frames are reached.

## The scopes

### Waveform

Brightness against horizontal position. Each column of the scope is a column of the frame; the
height of a trace is the code value. Red, green and blue are drawn over each other in their own
colors, so where they agree the trace turns white. Choose **Luma** in the Scopes menu to see one
gray trace instead. Pixels above the top or below the bottom of the axis are drawn on the top or
bottom line.

### Parade

The same measure as the Waveform with red, green and blue side by side, each a third of the width.
Use it to compare the channels: a color cast shows as one channel sitting higher or lower.

### Vectorscope

Hue and saturation without brightness. The center is neutral gray; distance from the center is
saturation and the angle is hue. The ring marks full saturation of 100% color bars, the six targets
R, M, B, C, G and Y mark where 75% color bars fall, and the faint line is the skin tone line. A
trace takes the color of the pixels it counts and whitens where they are dense.

### Histogram

How many pixels have each code value, one row per channel, on one shared scale. The logarithmic
scale makes small counts visible beside large ones. Pixels below 0 or above the top of the axis
are counted in the first or last bin.

### CIE chromaticity

The colors of the frame on the CIE 1931 xy diagram. The curve is the edge of visible color, the
triangle is the gamut of the measured color space, and the small ring is its white point. The top
right corner prints the gamut's name and the xy of its red, green, blue and white.

Point at any scope to read the value under the pointer in that scope's units, and the count of
pixels there.

### Comparing with other scopes

To compare Bloom's scopes with another application's, measure the same picture through the same
output encoding. The Display signal is the display transform's own output, so the encoding matters:
with the default sRGB display, a mid-tone sits about 3% of the range lower than with a Rec.709
(gamma 2.4) display, because the two encodings use different curves. Choose the display that
matches the other application's output, or compare with the Working signal.

## The region

Choose **Region > Draw Scope Region**, then drag a rectangle in the Viewer. The scopes then
measure only that part of the frame, and the Waveform's width becomes the region's width.

- While Draw Scope Region is on, drag inside the rectangle to move it. Drag a handle to resize it
  at any time. Escape cancels a drag.
- **Clear Scope Region** measures the whole frame again.
- The rectangle is drawn in every Viewer that shows the composition, labeled **Scopes**, while a
  Scopes panel is visible. It is separate from the Viewer's region of interest.
- The region belongs to the session and to one composition. It is not saved in the project, is not
  an undo step, and does not change rendering or export.

## RAM Preview

A RAM Preview does not measure the Display scopes: they are counted from the frame you see. Open a
Scopes panel after a RAM Preview is cached and the scopes of the frame on show appear at once, and
they play along with the cached range as it plays. A fill with no Scopes panel visible costs nothing
extra. With the Working signal and a Scopes panel visible, the fill also counts each frame's Working
scopes.

## Reading scopes from MCP and Python

The same measure is available to agents and scripts, with no panel open and in headless sessions.

- MCP: `query` with `kind: "scopes"` returns the count tables, a summary (per channel minimum,
  maximum, mean, percentiles and clipped counts, and the Vectorscope's mean saturation and hue) or
  a PNG picture of one scope or all five. See [Using Bloom from an agent (MCP)](mcp.md).
- Python: `bloom.scopes.read` and `bloom.scopes.image`. See [Scripting Bloom with Python](python.md).

A read names its own composition, frame, signal, display and view. It uses the composition's own
defaults, not the exposure, gamma, channel view or isolation of an open Viewer. A read counts the
full tables.

A Display read counts the frame as the Viewer would keep it: at 10 bits for an opaque frame, at 8
bits for a frame with a partly transparent pixel. The result carries the frame's under and over
counts and the layout it was counted in. MCP names them `frameCounters` (`under` and `over`, each a
list for red, green, blue and luma, and `notFinite`) and `storedLayout` (`"10:10:10:2"` or
`"8:8:8:8"`); Python names them `frame_counters` (`under`, `over` and `not_finite`) and
`stored_layout`. The luma entry is null (None in Python) when the display's luma could not be
resolved. The Summary's
clipped counts of a Display frame are these counts, for the whole frame. A frame with a pixel that is
not finite has no Display scopes, and the read says so; its Working scopes are still counted.

In the desktop application, a Display read of a frame the RAM Preview already holds is counted from
that frame and renders nothing. A read that differs from the cached frame in picture, display, view,
Viewer adjustment, region of interest, isolation or channel view is measured as usual.
