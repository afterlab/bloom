# Rolling Shutter

**Rolling Shutter** adds the lean and wobble of a camera sensor that reads its picture one row at a time. A
phone camera does this: the top of the frame is recorded a little before the bottom, so a fast pan leans the
picture and a shake makes it wobble like jelly. Find it under **Distort** in the Add menu. The model and its
limits are in [Image filters, "Rolling Shutter"](../architecture/image-filters.md#rolling-shutter).

Connect your picture to **Input**; **Image** is the result. The node does something only when it is given
motion to follow, by one or both of these:

- **A Camera Shake.** Put the Camera Shake before the Rolling Shutter and connect the shake's **Offset** to the
  Rolling Shutter's **Camera Offset** and its **Roll** to **Camera Roll**. The Rolling Shutter then follows the
  shake's own motion exactly across the time the sensor reads, so a harder shake wobbles more.
- **Optical flow.** Connect the **Forward** output of an **Optical Flow** node (made from the same footage) to
  **Motion**, to skew whatever moves in the footage by how fast it moves.

## Controls

- **Readout** is how long the sensor takes to read the frame, as a share of the frame's time. 0 is a global
  shutter and leaves the picture alone; 0.5 is a typical phone; 1 reads the frame over the whole frame
  time. It cannot be keyed, because it sets the moments the shake is read at.
- **Direction** is the order the sensor reads: Top to Bottom, Bottom to Top, Left to Right or Right to Left
  (a sensor turned on its side). A fast pan to the right leans a top-to-bottom picture one way and a
  bottom-to-top picture the other.
- **Edges** is what shows past the edge of the picture when a row is moved: Extend repeats the edge pixels
  (the default); Transparent leaves the gap clear. Camera Shake's automatic overscan already hides the gap.
- **Amount** scales all the motion. 1 is the motion as it is; more exaggerates it and 0 turns the effect off.
  It can be keyed or driven through its socket.

## What it does to the picture

- Every row is moved by how far the camera moved since the frame's time, at that row's own moment. The middle
  of the frame stays where the shake put it; the first row read moves one way, the last the other.
- The picture is sampled smoothly (bilinear). Values above one, negative values and partial transparency keep
  their range; only the position they are read from changes.
- The same frame looks the same in the viewer, when you scrub and in an export: the shake depends only on its
  seed and the time, and nothing is carried from earlier frames.

## Limits

- Only a Camera Shake's Offset and Roll can drive the camera sockets (the node needs to read the shake at other
  moments). Anything else on those sockets is refused with a message; a typed value moves nothing.
- The Camera Shake's automatic overscan scale is not part of the skew, and the roll is followed to first order.
  Both are well under a pixel for ordinary shakes.
- It costs one read of the picture per pixel (two with a flow image). See
  [Benchmarks](../development/benchmarks.md#rolling-shutter-568).
- The CPU and GPU agree on the same input to the field kernel's rounding (see [Fields](../architecture/fields.md)).
