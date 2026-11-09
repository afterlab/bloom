# Motion blur

Motion blur makes moving things smear across the time the shutter stays open. Bloom has one shutter
and several ways of finding the motion, so choose by what you have. Find the nodes under **Blur & Sharpen** in
the Add menu, and Echo under **Time**. The algorithms, their sources and their limits are in
[Motion blur](../architecture/motion-blur.md).

| You have | Use | Why |
|---|---|---|
| Animated layers, motion graphics, CG built in Bloom | **Motion Blur** | exact: renders what moves at each sub-frame |
| One moving Transform 2D | its own **Motion blur** switch | cheapest: one resample a sample |
| A render with a motion vector pass | **Vector Motion Blur** | uses the renderer's motion, including deformation |
| Footage with no motion data | **Optical Flow Motion Blur** | estimates the motion |
| Trails, ghosts, a stepped look | **Echo** | any window of frames |
| A blur in one direction, or round or out from a point | **Directional**, **Spin**, **Zoom Blur** | no motion needed |

## The shutter

Every motion blur shares these, so what you learn on one applies to all:

- **Shutter angle**: drag the dial. 180 degrees is half a frame, the film look; 360 is a whole frame.
- **Shutter opens**: **Centered** (the default) keeps the blur around the object's position on the frame,
  which keeps tracked composites on the track. **Start on frame** and **End on frame** put it after or
  before; **Custom** adds a **Shutter phase** dial.
- **Shutter curve**: **Box** is an ordinary shutter. **Triangle** and **Gaussian** fade the ends.
  **Custom** opens a curve editor: draw how much light passes from opening (left) to closing (right).
- **Quality**: **Draft** 4 samples, **Standard** 16, **High** 64, **Final** as many as the motion needs,
  up to **Maximum samples**. If you see separate copies of a thing rather than a smear, raise it.
- **Jitter samples** swaps banding for fine grain; the grain is the same on every render.
- **Maximum blur** limits how far any pixel may smear.

## Motion Blur

Connect anything to **Input**. The card says how many times it renders its upstream per frame, which is
what it costs: everything still upstream renders once, and only what is animated renders again.

**Footage between frames** decides what footage does between its frames:

- **Automatic** uses motion vectors if the source has them, otherwise holds it.
- **Hold** keeps footage still while the rest moves.
- **Optical flow** estimates the footage's motion.
- **Motion vectors** reads the layer you name in **Motion vector layer**.

## Transform 2D

Turn on **Motion blur** on a Transform 2D, and animate it. The shutter appears on the card. A curved
path stays curved: each sample evaluates the node at its own moment.

## Vector Motion Blur

Connect the picture to **Input** and the vectors to **Vectors**: the source's **channels** output for a
multilayer EXR, or any image.

Set **Vectors from** to your renderer, or to how its vectors are laid out: **Forward and backward** reads
the `forward` and `backward` layers in pixels, and **Previous and next** reads one `Vector` pass with the
motion from the previous frame in red and green and to the next in blue and alpha. If the picture blurs the wrong way or barely at all, your AOV
settings differ from the preset: choose **Custom** and set **Vector scale**, **Vector offset**, **Y points
up**, **Vector layout** and **Vector units**. Pick the layer in **Vector channels** if it is not found. For RSMB's files, set **MaxDisplace** to what was rendered. If an integer file stores 0.5 as still,
use Custom with a scale of twice MaxDisplace and an offset of minus MaxDisplace.

**Reconstruction**: **Gather** is fast and good for most motion. **Layered splat** is slower and handles
overlapping motion better. **Automatic** uses Gather at Draft and Standard and the splat at High and
Final. **Curved motion** bends the path through the previous and next vectors if your vectors have both.

Connect **Depth** so nearer things blur over farther things, and a **Matte** to separate a foreground
when there is no depth.

## Optical Flow

Reads motion from the footage and outputs it as pictures you can inspect or feed to Vector Motion Blur
with the **Bloom** convention: **Forward**, **Backward**, **Confidence** and **Occlusion**.

**Method** defaults to **Automatic**: DIS at Draft, TV-L1 otherwise. **Frame edges** says what is beyond
the frame: **Extend**, **Black**, or **Wrap (360)** for a panorama. Connect a **Guide** to solve the
motion on a cleaner version of the shot, and a **Matte** to solve one layer.

**Detect cuts** is a diagnostic only and off by default. It marks frames that do not follow from the
previous one on the **Cut** output, and it never changes the blur.

## Optical Flow Motion Blur

Connect footage to **Input**. **Amount** is the part of a frame the shutter is open: 0.5 is 180 degrees,
the same as RSMB.

Motion bleeds across an object's edge when one flow covers everything. Connect a **Matte** of the
foreground so it gets its own motion. Then:

- **Background fill** paints in what the foreground uncovers: **Outside-in** (the default) or
  **Temporal**, which borrows from neighbouring frames. Temporal has no published method to follow, so
  it is opt-in; try it when outside-in smears.
- **Matte shrink** pulls the matte in if the background's motion shows on the edge.
- **Alpha boost** recovers an edge that went thin.

It works less well on a frame with a cut or a flash, and on transparent things with two motions at once
(smoke over a moving background). Split those into layers and blur each.

## Echo

**Echoes** copies of the picture **Time between echoes** apart, each fading by **Decay**, combined by the
**Echo operator**: **Add** for light trails, **Maximum** to keep the brightest, **Over** for stacked
copies. **Frame blend** mode averages the window instead. **Posterize time** steps the picture at the
**Posterize rate** first.

## Directional, Spin and Zoom Blur

Select the node and a handle appears in the Viewer:

- **Directional**: drag the arrow's tip from the frame's centre to set the direction and the length.
- **Spin**: drag round the ring. It can turn past a whole revolution. Drag the cross to move the centre.
- **Zoom**: drag the point along its ray: on the ring is no zoom, outside blurs outward, inside inward.
  Drag the cross to move the centre.

The card has the same values on a dial and a point pad. **Escape** ends a drag where it is, like every
Viewer drag, and **Undo** takes it back.
