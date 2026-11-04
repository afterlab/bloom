# Levels, Curves and Hue/Saturation

Levels, Curves and Hue/Saturation adjust an image channel by channel. They work on render passes
as well as on colour: none of them converts colour, so a depth, mist, normal or position pass is
adjusted exactly as it was rendered. All three are in the **Color** section of the Add menu and
the Tab search. Each takes an image on **Input** and returns the adjusted image on **Image**.

Every control can be animated or linked from another node, and every change can be undone and is
saved with the project. A node left at its defaults changes nothing.

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

Each channel has five points, **Point 1** to **Point 5**. Each point is an input value (X) and the
output value (Y) it maps to. They start on a straight line at 0, 0.25, 0.5, 0.75 and 1, which
changes nothing. Move a point's Y to bend the curve there: raise the low points and lower the high
points for less contrast, or the reverse for an S-curve. You can also move the X values; the points
don't need to be in order.

The curve passes through every point and never overshoots between them. Past the first and last
points the curve continues in a straight line in the direction it was heading, so HDR values above
1 are still graded. Points with the same input value count once.

Bloom has no graphical curve editor yet: set the points as numbers in Properties or on the node
card.

## Hue/Saturation

**Hue** turns every colour around the colour wheel, in degrees. **Saturation** scales how colourful
the image is: 1 is unchanged, 0 is grey and values above 1 (up to 1000) are more colourful. Greys
stay grey, and each colour keeps its brightness. Hue/Saturation uses the hue-rotate and saturate
filters of the web Filter Effects standard. It doesn't change alpha, and it works on HDR and
negative values without limiting them.

## Masks

These nodes don't have a mask input yet. To limit an adjustment to part of the image, mix the
adjusted and original images with an **Image Mix** node and its **Mask** input.
