# Auto Exposure

**Auto Exposure** brightens or darkens the picture to follow what the footage does, and pulls its color
toward neutral, the way a phone camera does. When a light dims the picture is brought back up, and when it
flares the picture is brought back down, a moment late, so the correction visibly hunts. Find it under
**Color Correction** in the Add menu. The algorithm and its limits are in
[Image filters, "Auto Exposure"](../architecture/image-filters.md#auto-exposure).

Connect your picture to **Input**. Alpha is never changed. A frame is measured as a whole: the node takes
the mean color of every pixel, a transparent one counting as black.

## Exposure

- **Exposure** is how much of the correction is applied: 100% brings the measured level all the way to the
  target, 0% leaves the exposure alone.
- **Target** is the level the picture is brought to, in scene-linear light. The default of 0.18 is middle
  gray.
- **Attack** is how quickly the picture is pulled down when the footage gets brighter. **Release** is how
  quickly it is brought back up when the footage gets darker. Each is a time in seconds, the time the
  correction takes to cover about two thirds of the way to the new level. A short attack and a longer
  release is how a camera behaves: it reacts at once to a flare and settles slowly after a dip. At 0 the
  correction follows at once.
- **Minimum gain** and **Maximum gain** limit the correction in stops, so a black frame is lifted only so
  far and a flare is cut only so far.

## White balance

- **White balance** is how much of the color correction is applied. At 100% a picture whose mean color is
  warm loses red and gains blue until its mean is neutral.
- **Balance lag** is the time the color correction takes to follow a change of color. The default is
  slower than the exposure, since a camera adapts its color slowly.
- **Balance limit** is the most the correction may move red or blue against green, in stops.

Set **Exposure** or **White balance** to 0% to use the other alone.

## Look-back

**Look-back** is how many frames the node looks back over, counting the current one: 48 by default, 2 s at
24 frames a second, at most 64. A frame is worked out from these frames alone, so it looks the same
however you reach it: playing, scrubbing, jumping straight to it, stepping backward or exporting. The
node does not remember the last frame it drew. The correction settles smoothly as a change leaves the
window, with no jump. The cost is that a lag longer than about a third of the look-back is cut short,
since the frames before the window are not seen, and that a frame you jump to has
to measure the whole window (playing measures only the new frame). Near the start the window repeats the
first frame.

## Limiting it

Connect a Mask to **Mask** to keep the correction to part of the image, or an image read through **Mask
channel**. Every value except **Look-back** can be keyed or driven through its socket. The frame is always
measured as a whole.

## Limits

- The measurement is the mean of the whole frame. A small bright light moves it little, and a large dark
  area moves it a lot; there is no metering region.
- A cut is followed with the lag like any other change, so the first frames of a new shot are corrected
  from the old shot's level.
- The CPU and the GPU add the same numbers in the same order; their logarithm and exponential differ in the
  last bits, so a frame can differ between them by a tiny amount.
- A frame measures a window of frames, so a slow input makes a jump to a new frame slow. See
  [Benchmarks](../development/benchmarks.md#auto-exposure-566).
