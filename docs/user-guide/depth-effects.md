# Depth effects: Z Defocus and Depth Fog

**Z Defocus** blurs a picture by how far each pixel is from the camera, like a lens focused at one
distance. **Depth Fog** thickens fog with distance. Both read a *depth pass*: a picture whose values
are distances rather than colours. Find both under **Filters** in the Add menu. The algorithms, their
sources and their limits are in [Image filters, "Depth effects"](../architecture/image-filters.md#depth-effects).

## Connecting a depth pass

Connect the picture to **Input** and the depth pass to **Depth**. Any picture works as depth:

- the depth layer of a multilayer EXR, through the source's **channels** output (or a Pass or
  Shuffle node);
- a PNG, TIFF or JPEG depth or mist pass, at 8 bit, 16 bit or float;
- the output of any node, such as a Grade that remaps a pass.

**Depth channel** picks which channel holds the distance: R, G, B or A of a picture, or a channel by
name from an EXR channel set. The default is the first channel. It is never a mix of channels.

**Depth is** says how the pass encodes distance:

| Choice | Pixel value | Use for |
|---|---|---|
| Z (scene units) | the distance itself | a renderer's float Z pass |
| Inverse depth (1/Z) | 1 / distance | a pass that stores reciprocal depth |
| Normalised (0–1) | 0 at **Near distance**, 1 at **Far distance** | a remapped 8 or 16-bit depth map |
| Mist (0–1) | 0 near to 1 far, by **Mist falloff** | a mist pass |

Turn on **Near is white** for a pass that is bright near the camera. **Empty depth** decides what
pixels with no depth (the sky, an empty background) are: infinitely far, at the far distance, or
left unaffected.

### Warnings on the card

- **The depth is colour-transformed.** A depth pass is data. If the file it comes from is read as
  sRGB or Linear, its values are bent. Click **Read as data** to set that source to Raw in one
  undoable step.
- **The depth's red, green and blue differ.** A depth pass is grey, so this usually means a colour
  picture is connected to Depth by mistake.

An 8-bit depth pass has only 256 distances, so a smooth slope through focus shows as steps
(banding) in the blur and the fog. Use a 16-bit or float pass for anything close to the camera.

## Fog first, then defocus

Distant objects are both fogged and out of focus, and the fog is part of what the lens blurs.
Chain **Depth Fog → Z Defocus**, both reading the same depth pass. Fog after defocus leaves sharp
fog edges on blurred objects.

## Depth Fog

- **Fog color**, or a picture on the **fog** input (a sky, a fog plate) used in its place.
- **Fog amount** scales the whole effect.
- **Falloff** shapes how the fog builds between **Fog start** and **Fog end**: Linear, Quadratic,
  Inverse quadratic, Exponential or Exponential squared with **Density**, or **Curve**, drawn on
  the card's curve graph (left is the start, right is the end).
- **Fog transparent areas** fogs empty pixels too, so fog fills behind a cut-out render.

Start and end are in the depth's own units: 0 to 1 for normalised or mist depth, scene units for Z.
The card shows them as one range slider.

## Z Defocus

Pick a **Mode**:

- **Depth**: the blur grows with distance from **Focus distance**, up to **Maximum blur** in
  pixels. **In-focus range** keeps a band around the focus sharp.
- **Physical lens**: the blur is the lens's circle of confusion, from **Focal length**,
  **Sensor width**, **F-stop** and the focus distance in **Scene units**. Halve the f-stop for
  twice the blur.
- **Size map**: the depth pass is a blur-size map: 0 is sharp and 1 is **Blur size**.
- **Uniform**: one **Blur size** everywhere, with no depth needed; a lens blur rather than a
  Gaussian one.

**Blur** limits the blur to near and far, far only, or near only.

The **Bokeh shape** is the shape of out-of-focus highlights: a **Disk**, a **Polygon** with
**Blades**, **Blade rotation** (a dial on the card) and **Blade curvature**, or an **Image** on the
`bokeh` input as the kernel. **Highlight boost** brightens pixels over **Highlight threshold** so
bright points bloom into visible discs. **Chromatic bokeh** gives the discs coloured fringes.
**Quality** sets how many samples each pixel reads (Draft is fastest, Maximum smoothest).

Foreground edges stay clean: a sharp object in front keeps its edge, and a blurred object in front
spreads over the sharp background without dragging the background's colour onto it. **Edge
expand** grows the near side of depth edges by a few pixels. This helps with a depth pass that is
not antialiased, where the depth edge and the colour edge do not quite line up.

### Picking the focus in the Viewer

1. Set **Focus from** to *Depth at the focus point*, or just use the picker, which sets it for you.
2. Click **Pick focus in Viewer** on the card, then click the subject in the Viewer.

The node reads the mean depth of the 5 × 5 pixels around the point, so one noisy or edge pixel
does not throw the focus. While the node is selected, the Viewer marks the point and the area it
samples. Escape cancels a pick, and the whole pick is one undo step.

**Rack focus**: Shift-click keeps the picker armed. Move the playhead, then Shift-click the next
subject. When **Focus point** is keyframed, each click sets its value at the playhead. You can also
keyframe **Focus distance** directly.

## Cost

Depth Fog costs about what a Luma Key into Image Mix costs. Z Defocus reads up to a few hundred
samples a pixel, so it costs several Gaussian blurs of the same size. The measured numbers and
their budgets are in [Benchmarks, "Depth effects"](../development/benchmarks.md#depth-effects-483).
Lower **Quality** or **Maximum blur** first when a comp is slow.
