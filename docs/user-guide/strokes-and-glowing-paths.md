# Strokes and glowing paths

Bloom draws a stroke as a shape: **Path Stroke** turns any path into the outline the stroke covers,
and a **Shape** node fills it. Because the result is an ordinary image, any effect can follow it,
and **Glow** is what makes it shine. Everything here is under **Text & Paths**, **Stylize** and
**Light & Shadow** in the Add menu. The full definitions are in
[Path processing](../architecture/path-processing.md) and
[Image filters, "Alpha Contour"](../architecture/image-filters.md#alpha-contour).

## Stroking a path

Connect a path to Path Stroke's **Path**, and its **Path** output to a Shape node's **Path**.

- **Width** is the stroke's width in pixels; **Cap** and **Join** shape its ends and corners.
- **Start**, **End** and **Offset** keep part of the path, in percent of its length. Key **End** from
  0 to 100 to write the stroke on.
- **Taper Start** and **Taper End** narrow the stroke to a point over that share of it.
- **Dash Length**, **Dash Gap** and **Dash Offset** cut it into dashes; animate **Dash Offset** to make
  them run.
- **Wiggle**, **Wiggle Frequency**, **Wiggle Phase** and **Seed** roughen it; animate the phase to make
  it boil.
- **Core Width** is a second, thinner stroke on the **Core** output, for a hot centre drawn over the
  body.

### Brush stamps

Set **Spacing** above 0 and the stroke becomes a row of stamps, one every **Spacing** pixels.
**Brush stamp** picks the shape: a round **Dot**, a **Square**, a **Diamond** or a **Dash**.
**Stamp direction** says whether each stamp turns with the path: **Automatic** turns dashes only,
**Follow path** turns every stamp, **Fixed** keeps them square to the frame. **Stamp Rotation** turns
every stamp further, with the dial or by typing degrees. Tapers scale the stamps too.

## Paths you already drew

- **Mask Path** reads the masks of a layer, or the path of a Mask node. Choose the layer or Mask node in
  **Masks of**; **Mask** 0 takes every mask, 1 the first, 2 the second. A keyed mask moves the path
  with it, so a stroke can follow a rotoscoped shape.
- **Text Outline** reads the outlines of a text's letters, exactly where the text draws them. Choose
  the text in **Text to outline**. Change the text, its size or its spacing and the outline follows.

Put either into Path Stroke to outline masks or letters, write them on, or dash them. The quickest
way: select a text layer (or a layer with masks) and choose **Text & Paths > Stroke the Selected
Text** (or **Stroke the Selected Layer's Masks**). Bloom adds the Text Outline or Mask Path, a Path
Stroke and a Shape on a layer of its own just above, and parents that layer to the one you selected,
so the stroke sits on the letters or masks however the layer is moved, scaled or turned, and follows
it when it is animated. Undo removes it in one step. Select the Path Stroke to set its width, trims,
dashes and the rest; the Shape sets its colour.

## Outlining an image: Alpha Contour

**Alpha Contour** (under **Stylize**) draws a band along the edge of any image's alpha: a keyed
element, a matte, a rendered pass. **Width** is the band's width, **Softness** blurs its edges, and
**Offset** moves it out from the edge (positive) or into the element (negative). **Color** may be
brighter than white for a glow to pick up. **Composite** puts the band over the image, under it, or
alone. The band follows the edge exactly at any render scale. Because it is drawn from the pixels and
not from a path, the path-only controls (trims, dashes, tapers, stamps, wiggle) are not available
here: use Mask Path or Text Outline when you need them.

## Ready-made glowing paths

**Light & Shadow > Glowing paths** adds a finished look in one step:

- **Saber**: a white-hot core in a wide coloured glow.
- **Beam**: a thin straight beam of coloured light.
- **Vegas**: dashes chasing round a closed outline.
- **Write-on**: a stroke that draws itself on over two seconds.
- **Stroke**: a plain stroke with a soft glow.

Each adds a **Node Group**: connect its **Image 1** output to a layer. On the node and in Properties
its controls read as what they drive: **Path Stroke Width**, **Glow Size**, **Glow Intensity** and
the colour (**Glow Tint** for Saber, **Shape Fill Color** for the others). To change the path
or the speed, open the group (**Open** beside **Group** in Properties) and edit the Path Stroke inside
it, its path or the rate a Write-on or Vegas moves at. A preset is sized for the composition you add
it to. Undo removes it in one step, and it saves and reopens like any other group.

## Cost

A stroke is computed once per frame on the CPU and drawn by the Shape node's fill, so its cost grows
with the number of anchors, dashes and stamps. Glow's cost is its blur. Alpha Contour's cost is the
distance transform over the image plus the band's reach; keying its width, softness or offset makes it
reach its widest, so leave them constant when you can. See
[Benchmarks](../development/benchmarks.md) for `alpha-contour-2k` and `text-outline-2k`.
