# Test Card

**Test Card** draws a test pattern: bars to check levels and colour, a patch to measure a display, a grid or a
circle to check geometry, a ramp or a zone plate to check banding and sharpness, an alpha gradient to check
compositing. Find it under **Sources** in the Add menu. It has no input and one **Image** output.

Every card is drawn from a formula at the size it is rendered at. There are no stored pictures and no fixed
resolution, so a card is exact at 640 pixels wide and at 16 000, on a proxy and at full size, and costs nothing
to store.

## Controls

- **Card** chooses the pattern. A card shows only the controls that belong to it.
- **Size** is **Composition** (the default) or **Custom**. Composition draws the card at the composition's size,
  so it follows the composition when that changes. Custom shows **Width** and **Height** and draws the card at
  that size, centred in the composition (cut off where it is larger, transparent where it is smaller).
- **Label** shows the card's name and size, on the cards where it does not defeat the card's purpose (see each card).

Geometry is computed from the real pixel size: a circle stays round and a grid cell stays square at any shape.
The four standard bar cards (SD Bars, HD Bars, EBU Bars and HDR Bars) are defined in a 16:9 frame: in a frame of
another shape they keep their layout proportions exactly, as the largest centred 16:9 area, with the rest
padded in black, never stretched. The Colour Chart keeps its 3:2 shape inside the frame in the same way. The
Patch, Grey Ramp and Alpha Ramp are defined as fractions of the frame and fill it, and the geometry and
sharpness cards compute their shapes from the real pixel size.

The colour of a card is a signal, written in the colour space the card's standard names (Rec.709 for the SD, HD
and EBU bars, the colour chart, the ramps and the other cards; Rec.2100 HLG or PQ for the HDR bars). Bloom
converts it into the working space with the normal colour space transform, so the card reaches the Viewer and
every export as any other source does. A level below black or above white (the PLUGE steps below black, HDR
highlights) is kept, not clipped.

## Cards

### SD Bars

Seven 75% bars (white, yellow, cyan, green, magenta, red, blue) over the top two thirds of the frame; a thin
row of reversed bars (blue, black, magenta, black, cyan, black, white) below; and a bottom row of the -I
signal, 100% white, the +Q signal, black, and a PLUGE group of three steps at -4%, 0% and +4%. The -I and +Q
patches use the 8-bit studio-range R'G'B' values 16/70/106 and 72/16/118. The exact proportions of the rows
other than the top two thirds are Bloom's own. No label.

### HD Bars

The SMPTE RP 219-1:2014 HD colour bar pattern in a 16:9 frame, in four rows of 7, 1, 1 and 3 twelfths of the
height: 40% grey strips at the sides with seven 75% bars between; a row with 100% cyan at the left, a 75% white
centre and 100% blue at the right; a row with 100% yellow at the left, black, a 0 to 100% ramp, white and 100% red at
the right; and a bottom row of black, 100% white and a PLUGE group of -2%, 0%, +2% and +4% steps, with 15% grey
at the sides. Widths are in 1920ths of the width as the standard gives them. The pattern is centred, never
stretched, in any frame. No label.

### EBU Bars

Eight equal bars: 100% white, the six colours at 75%, black (the "100/0/75/0" bars of ITU-R BT.471). No label.

### HDR Bars

The ITU-R BT.2111-3 colour bars for HDR, in three variants chosen by **HDR format**: **HLG, narrow range**,
**PQ, narrow range** and **PQ, full range**. The pattern follows the standard's figures and code value tables
(10-bit code values; the 75% BT.709 bars of the HLG pattern and the 58% ones of PQ, the grey stair, the ramp and the
white and black patches). The signal is the code value as a fraction of its range, in the Rec.2100 HLG or PQ
colour space, so the pattern's levels reach the working space in scene or display light as the format defines
them. Values above 1 and below 0 are kept (HDR-safe). No label.

### Patch

One flat colour with an exact value for measuring with a display probe. **Patch value** chooses from 27
values: black, near black (2%, 4%), greys from 10% to 90% in steps of 10%, near white (95%, 98%), white, and the
primaries and secondaries at 75% and at 100%. **Colour space** names the space the value is written in: Rec.709
(the default), Rec.2100 HLG, Rec.2100 PQ or the working space itself (linear). **Window** shows the patch full
frame or as a centred window of half, a quarter or a tenth of the frame's area on black, to keep the display's
own limiting out of the reading. **Step through** advances through the 27 values one per frame.

The card shows what it should produce: its signal in its colour space and the **expected display value**, the
signal taken through the same colour space transform and then through the Viewer's current **Display** and
**View**, including its tone map where the View has one (clamped to the display's 0 to 1 and shown also as 8-bit
codes). This is the value to compare a probe reading with. A Patch has no label and exports through the normal
routes like any image.

This is verification of the path from the composition to the monitor, not display calibration. A probe reading
that differs from the expected value can come from Bloom, the desktop compositor, the GPU's output settings or
the monitor's own profile and settings; the card does not tell which, and does not change any of them.

### Colour Chart

A chart of 24 patches (6 by 4) in a 3:2 area, separated by a dark grid: a row of six neutrals from white to
black in steps of 20%; the primaries and secondaries at 100%; the same at 50%; and a range of six skin tones
from light to dark. The values are Bloom's own definition. It is not a copy of any commercial chart and does
not stand in for one. No label.

### Grey Ramp

A smooth ramp from black to white across the top four fifths, and a PLUGE row in the bottom fifth of nine
segments, black alternating with steps at -4%, -2%, +2% and +4%, for banding, dithering and black level checks. The ramp is a continuous
signal (not stepped), so any visible stepping comes from the pipeline or the display. No label.

### Grid

Evenly spaced lines on a dark ground. **Spacing** and **Line width** are in pixels of the card; the spacing is the
same horizontally and vertically (so cells are square), the lines are anti-aliased, and the two lines through
the centre are drawn in yellow. Label optional.

### Circle

A ring of 90% of the shorter side's diameter and a second, yellow ring at half its radius, over a graticule of
20 square cells across the shorter side (heavier every fifth line), with lines through the centre and a border
just inside the frame's edge, to check aspect and centring: it is round at any frame shape. Label optional.

### Safe Areas

Action safe (93% of the width and height, yellow) and title safe (90%, cyan) outlines, a centre mark, and crop
guides for 16:9, 9:16, 1:1 and 2.39:1 (each the largest such rectangle centred in the frame), each switchable. The 93% and 90% numbers are the figures commonly published for
these areas; check them against your delivery specification. Label optional.

### UV Grid

Square cells, a chosen number across the width (**Cells across**; the cell is the width divided by it), coloured by position (red across, green up) with each
cell's column and row written in it, to check texture orientation and mapping. Label optional.

### Zone Plate

A circular zone plate: a signal whose frequency rises with distance from the centre up to the Nyquist limit at
the corners, to show aliasing and the sharpness of a resample. **Moving** makes the rings drift with time.
Drawn with extra sub-samples so the pattern itself does not alias. Label optional.

### Siemens Star

A star of alternating black and white spokes (**Spokes**) in a circle of 45% of the shorter side, whose spokes
merge toward the centre where the picture can no longer resolve them: a resolution check. Label optional.

### Moving Bar

A white bar that crosses the frame (**Direction**: horizontal or vertical) in **Crossing time** seconds, of a
**Bar width** given as a percent of the frame, to check motion, judder and frame timing. Label optional.

### Countdown

The seconds left, counted down from **Start**, in large digits, the frame within the second beneath it, and a
white flash on the first frame of each second. The counting follows the composition's frame rate. There is no
audio beep: Bloom has no audio route this card can use (see "Not included").

### Alpha Ramp

Six bands of colour, each with an alpha rising from 0 to 1 across the width, over transparency, for checking
premultiplication, blend edges and Merge modes. The colour is straight; the card writes premultiplied pixels
like every image in Bloom. No label.

## Rendering

The card is one procedural kernel evaluated on the CPU reference and on the GPU from the same source, with the
same parity as every other node. A card renders through the same paths as any image: the Viewer, a still,
a sequence, a video export and the headless render. Large sizes are drawn within the memory budget as any
image is, with an explicit refusal when the budget cannot hold them.

## Sources of the standard values

- SMPTE RP 219-1:2014, *High-Definition, Standard-Dynamic-Range Colour Bars*: the HD layout, widths in
  1920ths and heights in twelfths, and the default sub-patterns.
- ITU-R BT.2111-3, *Specification of colour bar test pattern for high dynamic range television systems*: the
  HDR layout, the narrow and full range 10-bit code value tables, and the ramp and stair codes.
- ITU-R BT.471, *Nomenclature and description of colour bar signals*: the 100/0/75/0 EBU bars.
- The -I and +Q studio-range values (16/70/106 and 72/16/118) and the PLUGE steps of plus and minus 4%: the
  SMPTE colour bars as published in public references (the SMPTE ECR 1-1978 pattern).
- ITU-R BT.709 (Rec.709), BT.2100 (HLG and PQ), BT.1886: the colour spaces the signals are written in.
- Safe areas: 93% action and 90% title, as in EBU R 95 and SMPTE RP 218. These numbers were stated from the
  documents' commonly quoted values and have not been re-checked against the documents here.

Everything else (the proportions of the SD rows, the Colour Chart, the geometry cards, the label) is Bloom's
own definition, not a standard.

## Not included

- The Countdown's optional beep. The card is a picture source; an audio signal needs the audio route, which a
  source node does not reach.
