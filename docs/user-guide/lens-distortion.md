# Lens Distortion

**Lens Distortion** removes the bending a real lens puts into a picture, or puts that bending onto a
clean picture. It uses the standard radial and tangential lens model that lens calibration tools
produce. Find it under **Distort** in the Add menu. The model and its limits are in
[Image filters, "Lens Distortion"](../architecture/image-filters.md#lens-distortion).

Connect your picture to **Input**. The node has two outputs:

- **Image** is the warped picture.
- **ST Map** is the same warp as data: its red and green hold, for every pixel, the position the
  picture was read from, as a fraction of the frame (0, 0 at the bottom left, 1, 1 at the top right).
  Use it to apply the same warp to another picture. It is raw data, not a colour. When nothing reads
  it, it is not computed.

## Controls

- **Mode** is **Undistort** or **Redistort**, and the two are inverses of each other. Undistort straightens
  a picture shot through a lens whose coefficients you know; Redistort puts the same lens onto a straight
  picture (CG, graphics) so it sits with the footage. Redistort undoes Undistort to within a fraction of a
  pixel across the frame.
- **Radial k1** and **Radial k2** bend the picture by distance from the centre. Below zero is barrel
  (straight lines bow outward), above zero is pincushion (they bow inward). k1 acts across the frame,
  k2 mostly near the corners.
- **Tangential p1** and **Tangential p2** model a lens that is not centred on the sensor; they tilt and
  shear the picture a little. Most lenses need none.
- **Center** is where the lens's axis meets the frame, in pixels from the frame's centre. Drag it on the
  pad. It is the middle of the frame unless the lens or sensor is off-centre.

Everything starts at zero, which leaves the picture exactly as it is, in both modes. Coefficients and the
centre can be keyed or driven through their sockets; **Mode** is a fixed choice.

Coefficients are in units where half the frame's diagonal is 1, so a value means the same on a proxy as at
full size, and the same for a wide frame as a tall one.

## What it does to the picture

- The warp reads the picture; it never remakes it. Values above one, negative values and partial
  transparency keep their range and their proportions, and the colour is not clipped or changed.
- Where a pixel would read from outside the picture it is transparent. Redistort of a barrel lens pulls
  the corners in from beyond the picture, so they go transparent; Undistort of a barrel lens stays inside.
- The picture is sampled smoothly (bilinear), so a large change softens fine detail a little.

## Limits

- The model is for ordinary lens values. Redistort finds each pixel's source by iteration, so it is not
  defined where the model folds back on itself (a k1 far below -1 with a centred lens), and there the two
  modes are no longer inverses. Stay within the sliders' range for a real lens.
- Redistort costs several times what Undistort does per pixel. See
  [Benchmarks](../development/benchmarks.md#lens-distortion-486).
- The CPU and GPU agree on the same input to the field kernel's rounding (see [Fields](../architecture/fields.md)).
