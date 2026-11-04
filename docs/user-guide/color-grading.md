# Grade, Levels, Curves and Hue/Saturation

Grade, Levels, Curves and Hue/Saturation adjust an image's colour. They work on render passes as
well as on colour: none of them converts colour, so a depth, mist, normal or position pass is
adjusted exactly as it was rendered. All four are in the **Color** section of the Add menu and
the Tab search. Each takes an image on **Input** and returns the adjusted image on **Image**.

Every control can be animated, and every change can be undone and is saved with the project.
Grade, Levels and Hue/Saturation controls can also be linked from another node. A node left at its
defaults changes nothing.

## Grade

Grade is a primary colour correction with lift, gamma, gain and offset colour wheels, as used in
colour-grading suites. Edit it right on its node card, or in Properties, which shows the same
controls.

### Wheels

| Wheel | Changes |
| --- | --- |
| **Lift** | The shadows: raises or lowers black while white stays put. |
| **Gamma** | The midtones: above zero brightens them, below darkens. Black and white stay put. |
| **Gain** | The highlights: scales the image, 1 is unchanged. Black stays put. |
| **Offset** | Everything: adds the same amount to every value. |

- **Drag the puck** in a wheel to push that range toward a colour: toward red adds red, toward the
  opposite side adds cyan. Hold **Shift** to move it slowly. The puck stops at the rim.
- **Drag the slider** under a wheel to change its master: how much the wheel lifts, bends, scales
  or offsets all three channels.
- **Double-click** a wheel, or press its **Reset** button, to set it back to neutral.
- **Alt-click** a wheel, or press the arrow next to Reset, to show its numbers: the master and the
  red, green and blue values. Type exact values there, key them, or link them from another node.

Each drag is one step to undo. The wheels go round like a vectorscope: red at the top left, then
yellow, green, cyan, blue and magenta. Moving the puck keeps the brightness of the three values the
same and only changes their balance.

### Primaries

The bar under the wheels has **Temperature** (positive is warmer), **Tint** (positive is more
magenta), **Contrast** and its **Pivot** (the value contrast turns around; 0.18 is middle grey),
**Saturation** (1 is unchanged, 0 is grey) and **Exposure** in stops. Drag a field or type a value.
The arrow next to **Primaries** shows their rows, to key or link them.

### How it works

Grade works on the image's linear values, with no limit on bright or negative values, and never
changes alpha. In order, it applies exposure and white balance, then lift and gain, offset, gamma,
contrast and finally saturation. Gamma and contrast leave values at or below zero as they are.
[Image filters](../architecture/image-filters.md#grade) has the exact formulas.

## Channels

Levels and Curves each have five sets of controls:

| Channel | Adjusts |
| --- | --- |
| Master | Red, green and blue together, before their own channels. |
| Red, Green, Blue | That channel only, after the master. |
| Alpha | The alpha channel only. |

They adjust the colour as if it were not premultiplied, then premultiply it again by the adjusted
alpha. Alpha always stays between 0 and 1. Raising alpha can reveal pixels that were transparent
inside the image's area.

**Clamp** limits the red, green and blue results to 0 to 1. It is off by default, so values above
1 (HDR) and below 0 pass through with the adjustment applied.

## Levels

Levels draws the input's brightness histogram with handles you drag, on its node card and in
Properties. Choose a channel with the tabs (**RGB**, **R**, **G**, **B**, **A**), then:

- Drag the **black** and **white** handles under the histogram to set Input Black and Input White.
- Drag the **grey** handle between them for Gamma: left brightens the middle, right darkens it.
- Drag the two handles under the output bar for Output Black and Output White.
- **Double-click** a handle to reset it. **Escape** or the right mouse button cancels a drag.
- **Alt-click** the graph, or press the arrow next to the tabs, to show that channel's numbers.

Each drag is one step to undo. The graph grows to show a black or white point outside 0 to 1. The
histogram is only worked out while the graph is on screen.

Each channel has **Input Black**, **Input White**, **Gamma**, **Output Black** and **Output White**.
Input Black and Input White pick the range that maps to Output Black and Output White. Gamma bends
the values in between: above 1 brightens the middle, below 1 darkens it. Any value is allowed for
the black and white points (up to a million either way), including values far above 1 and
negative values, and Input White can be below Input Black to invert the channel. Gamma runs from
0.01 to 100. Values outside the input range keep going in the same direction rather than stopping.

### Normalize a depth pass

1. Import the EXR with the pass and set its **Input color space** to **Raw (no conversion)**.
2. Add **Levels** after it. Set the master **Input Black** to the nearest depth and **Input White**
   to the farthest depth you want to see.
3. Turn on **Clamp** to keep the result between 0 and 1.

The **Normalized** display of an [Image Preview node](image-preview.md) shows a pass's lowest to
highest value, which helps you find the near and far values.

## Curves

Curves has a curve for each channel and four hue curves, edited in one graph on the node card and,
the same, in Properties. The tabs pick **RGB** (the master), **R**, **G**, **B**, **A** or **Hue**;
on the **Hue** tab the menu beside **Preset** picks the hue curve by name.

- **Click** the graph to add a point there, and keep the button down to drag it into place.
- **Drag** a point to move it. It moves with the pointer from where you pressed it, so it never
  jumps. Hold **Ctrl** (**Cmd** on macOS) to move it a tenth as far; you can press or release Ctrl
  during the drag. **Escape** or the right mouse button cancels the drag.
- **Drag a point off the graph** to remove it, or right-click it and choose **Delete Point**. The
  **Delete** key removes the selected point.
- **Double-click** the graph to reset the curve.
- **Right-click** the graph for **Delete Point**, **Reset Curve** and a key for the curve at the
  playhead.
- The two numbers under the graph are the selected point's input and output. Drag or type them to
  set it exactly. Hue values are in degrees, gains are multipliers (×).

A curve can have any number of points, up to 256. Each drag, added point, removed point, reset or
preset is one step to undo. The diamond next to the tabs adds a key for the whole curve at the
playhead, so a curve can change over time; between two keys with the same number of points each
point moves in a straight line, otherwise the curve changes at the next key. A curve's keys select,
move and delete in the timeline like any other key.

### Presets

**Preset** sets the curve you're editing to a starting shape, as one step to undo:

| Preset | Shape |
| --- | --- |
| **Linear** | The straight diagonal, which changes nothing. |
| **Medium Contrast** | A gentle S-curve: darker shadows, brighter highlights. |
| **Strong Contrast** | A steeper S-curve. |
| **Low Contrast** | An inverse S-curve: lighter shadows, darker highlights. |
| **Lighten** | Raises the midtones; black and white stay. |
| **Darken** | Lowers the midtones; black and white stay. |
| **Fade** | Lifts black to 0.1 and brings white down to 0.92. |
| **Crush Blacks** | Takes everything below 0.12 to black. |
| **Invert** | Swaps black and white. |

Past 1 a preset continues in a straight line like any curve, so HDR values are still graded. A hue
curve has a single preset, **Reset**, which empties it.

### Channel curves

The input runs along the bottom and the output up the side. A new curve is the straight diagonal
from 0, 0 to 1, 1, which changes nothing. Raise the low part and lower the high part for less
contrast, or the reverse for an S-curve. The box marks 0 to 1; the graph always shows some range
above 1 and grows to hold any point you place there, for HDR. A channel always keeps at least two
points.

The curve passes through every point and never overshoots between them. Past the first and last
points it continues in a straight line in the direction it was heading, so HDR values above 1 are
still graded. Points with the same input value count once.

Behind the curve the graph draws a histogram of the node's input brightness, as the Viewer would
show it. Turn it off with the **Histogram** check box beside **Preset**; it is only worked out while
the graph is on screen and never changes the image.

### Hue curves

| Curve | Changes |
| --- | --- |
| **Hue vs Hue** | Turns the colours of each hue toward another hue, up to half way round. |
| **Hue vs Sat** | Makes each hue more (above 1) or less (below 1) colourful. |
| **Hue vs Luma** | Brightens (above 1) or darkens (below 1) each hue. |
| **Luma vs Sat** | Makes dark, middle or bright parts more or less colourful. |

Each hue curve names its two axes and draws a colour band along each: the hue strip (or black to
white for luma) along the bottom, and up the side the turned hue for Hue vs Hue, grey to colour for
saturation or black to white for luma. The hue curves run along the colour strip drawn behind them,
which wraps: the right edge is the
same red as the left edge, so a point near one edge also shapes the curve near the other. A new hue
curve is empty and changes nothing; the dashed line marks the value that leaves colours unchanged.
Greys are never changed by the hue curves, and alpha never is.

The hue curves use the same colour wheel and brightness weights as Hue/Saturation, after the
channel curves and before **Clamp**. Like the rest of the node they don't convert colour, so HDR
values and render passes keep their range.

### Older projects

A project saved before Curves had a graph stored five points per channel. It opens with those
points as the new curves, exactly. If a point was animated, it opens at its first key; if it was
linked from another node, it opens at its starting value. Hue curves start empty.

## Hue/Saturation

**Hue** turns every colour around the colour wheel, in degrees. **Saturation** scales how colourful
the image is: 1 is unchanged, 0 is grey and values above 1 (up to 1000) are more colourful. Greys
stay grey, and each colour keeps its brightness. Hue/Saturation uses the hue-rotate and saturate
filters of the web Filter Effects standard. It doesn't change alpha, and it works on HDR and
negative values without limiting them.

## Masks

These nodes don't have a mask input yet. To limit an adjustment to part of the image, mix the
adjusted and original images with an **Image Mix** node and its **Mask** input.
