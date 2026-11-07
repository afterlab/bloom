# Camera Shake

**Camera Shake** moves a picture the way a hand, a blast or a worn projector gate would: a seeded
shake applied as a transform, with the motion blur that movement would have made and edges that never
show. Find it under **Transform** in the Add menu, next to Transform 2D. The arithmetic is in
[Evaluation primitives, "Camera Shake"](../architecture/evaluation-primitives.md#camera-shake).

Connect your picture to **Input**. The same seed and frame always give the same shake: a cached
frame, an export and a second machine agree, and the CPU and the GPU draw the same movement.

## Style

- **Handheld** is smooth, wandering movement, like a camera on a shoulder. **Frequency** is how often
  it picks a new direction, in times a second. **Octaves** adds finer, faster tremor on top of the
  main sway (up to four, each three times as fast), and **Roughness** is how much of it each finer
  octave adds: 0 is just the sway, 1 gives them all the same weight.
- **Impact** is a hit every interval: a sharp push in a random direction that rings out and settles
  before the next, like an explosion or a heavy footstep. **Frequency** is hits a second.
- **Gate weave** is the small unsteady registration of film in a projector gate: a new random nudge on
  every frame, with a little slow drift. Keep the amounts small, a pixel or two.

## Amounts

**Amplitude X** and **Amplitude Y** are the largest the picture moves sideways and up and down, in
full-resolution pixels, so a preview looks like the full frame. Set one to 0 for a shake along one
axis. **Roll amount** is the largest turn, in degrees; its dial turns clockwise, like Transform 2D's rotation.
All three can be keyed or driven.

**Smoothing** rounds the movement off by averaging it over a short window: 0 is untouched, 1
averages over a whole interval, which takes the sharp edge off an impact and the frame-to-frame
nervousness off a gate weave.

## Seed

**Seed** chooses which shake. Every new Camera Shake gets a seed of its own, so two nodes added with
their defaults never move together. **Randomise seed** on the card gives the node a new seed in one
click; you can also type one, to match a shake you liked on another node. The seed (0 to 999999) is saved in the
project. A copy of a Camera Shake keeps its seed and so shakes the same; use **Randomise seed** on
the copy to make it different. Two projects can start with the same seed on their first node.

## Loop

With **Loop** on, the movement repeats exactly every **Loop length** seconds: the frame one loop length
after another is the same frame, and the join is invisible. The length is rounded so a whole number of
intervals fits. Works for all three styles.

## Edges

With **Auto overscan** on, the picture is scaled up from its centre by just enough that the frame's
edges never show at the largest shake the amounts allow, however it is rolled. It is computed from the
amplitudes and the roll, so when you key an amount the scale follows, and it is exact for the frame
size, not a guess. It also keeps a preview at a fraction of the resolution clean to the edge. With it off,
the picture keeps its own size and the moved edge shows as transparency.

## Motion blur

Turn on **Motion blur** and the picture is resampled along the shake across the shutter, exactly as
Transform 2D's [motion blur](motion-blur.md#transform-2d): the same shutter angle, offset, curve,
quality and jitter, shown on the card while it is on. It is off by default, because it costs one
resample per sample.

## Using the shake elsewhere

Besides the picture, the node has two outputs: **Offset**, the movement it applied in pixels, and
**Roll**, the turn in degrees. Connect them to any parameter that takes a vector or a number: shake a
camera-locked title with the same movement, nudge a light, or drive a second Transform 2D to give a
layer on top the same wobble. They are the exact values the picture was drawn with at that frame.

## Cost

The shake itself is a few numbers a frame. The picture costs one Transform 2D resample, and several
with motion blur. See [Benchmarks](../development/benchmarks.md).
