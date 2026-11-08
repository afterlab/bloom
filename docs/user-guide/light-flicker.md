# Light Flicker

**Light Flicker** makes the brightness of an unsteady light: a bulb that wavers, dips and drops out, a
sputtering headlight. It draws no picture. It gives a number you connect to something else, usually the
**Stops** of an **Exposure** node, so you can place and time the flicker in post and use the same number to
cut a sound effect or the score to it. Find it under **Values** in the Add menu. How it works is in
[Value node library, "Light Flicker"](../architecture/value-node-library.md#light-flicker).

## Outputs

- **Value** is the brightness as a gain: 1 is a steady light, 0.5 is half as bright, 0 is dark.
- **Stops** is the same brightness in stops (the base-2 logarithm of the gain): 0 is steady and -1 is half as
  bright. Connect it to the Stops of an Exposure node. A gain of 0 reads as -10 stops rather than minus
  infinity.

## Controls

- **Rate** is how many times a second the flicker picks a new level (12 by default).
- **Depth** is how far the light dips at its dimmest flicker: 0 is steady, 1 lets a flicker reach black
  (0.35 by default).
- **Dropout Rate** is the average number of dropouts a second; 0 turns dropouts off (0.5 by default).
- **Dropout Length** is the longest a dropout lasts, in seconds; each dropout lasts between half and all of it
  (0.12 s by default).
- **Dropout Level** is how bright the light falls to during a dropout, as a share of what the flicker left:
  0 is dark, 1 is no dropout (0.1 by default).
- **Softness** runs from hard steps and instant dropouts (0) to smooth waves and dropouts that fade in and out
  (1) (0.5 by default).
- **Seed** chooses which flicker. Every new Light Flicker gets a seed of its own, so two nodes never flicker
  together; a copy keeps its seed and flickers the same.

Every control can be keyed or driven through its socket.

## What to know

- The flicker depends only on the seed and the time, so any frame is the same in the viewer, when you scrub and
  in an export, and nothing is remembered from earlier frames.
- It is a value, not a picture: it costs next to nothing by itself. See
  [Benchmarks](../development/benchmarks.md#light-flicker-567).
