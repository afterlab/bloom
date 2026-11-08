# Codec Degrade

**Codec Degrade** makes a picture look like heavily compressed video: blocks that lose their detail,
colour that bleeds across them, and blocks that smear while the picture moves and refresh at each
keyframe. It imitates the look; it does not run a real encoder. Find it under **Stylize** in the Add
menu. The algorithm and its limits are in
[Image filters, "Codec Degrade"](../architecture/image-filters.md#codec-degrade).

Connect your picture to **Input**. Alpha is never changed, and a transparent pixel stays transparent.
HDR values keep their range.

## The look of one frame

- **Quality** runs from 0 to 100. Low values throw away most of each block's detail, so you see flat
  tiles with hard edges; 100 keeps nearly all of it.
- **Block size** is **8 px** or **16 px**. Each block is coded on its own, the way JPEG and most
  video codecs do. A proxy shows the same blocks scaled to match, so a half-size proxy draws 4 and 8
  pixel blocks.
- **Chroma bleed** is on by default and imitates 4:2:0 chroma subsampling. Colour is kept at a quarter of the picture's resolution, so
  hard coloured edges bleed into their neighbours as they do in consumer video. Turn it off to keep
  colour at full resolution. A small proxy (below three quarters of full size) always keeps colour at
  full resolution, since the bleed would be less than a pixel.

## Over time

- **GOP length** is how many frames run between keyframes: 1 (all keyframes, so no smearing),
  2, 4, 6, 8, 12, 15, 24 or 30. The default is 12 frames. Frame 0 of the node's clock is a keyframe,
  and so is every multiple of the GOP length.
- A **keyframe** is coded on its own, in full, at the quality you set.
- Between keyframes each frame is coded against the one before it, moved along the picture's motion
  (the optical flow between the two frames, as Optical Flow computes it). The blocks that the
  picture moves through are coded more coarsely, so blocking gets worse while things move, and
  fresh detail only gets in at a keyframe.
- **P-frame persistence** is 50% by default. It is how much of the previous frame's blocks carry
  into the next one between keyframes. At 0% each frame keeps all of its own detail that the
  quantiser allows. At 100% a block keeps what the previous picture had, moved with the motion, and
  nothing new is added until the next keyframe.

A frame always looks the same however you reach it: playing, scrubbing, jumping straight to it,
stepping backward, or exporting. Codec Degrade starts from the last keyframe and re-codes the frames
up to the one you asked for, at most 30 frames back, rather than remembering the last frame it drew.

## Limiting it

Connect a Mask to **Mask** to keep the effect to part of the image, or an image read through
**Mask channel**. **Quality** and **P-frame persistence** can be keyed or driven through their
sockets; **Block size**, **GOP length** and **Chroma bleed** are fixed choices.

Under a retimed layer the frame counted is the layer's own.

## Limits

- The motion is the optical flow between frames. Where the flow is wrong (very fast motion, repeating
  patterns, a cut) the blocks move wrongly, as a real codec's do.
- The flow and the blocks are computed in float, and the CPU and GPU agree exactly on the same input.
  The flow itself can differ by a tiny amount between them, and a block's quantiser can turn that into
  a visible difference on a moving picture (see [Motion blur](../architecture/motion-blur.md)).
- A frame renders the whole chain back to its keyframe, so a long GOP costs more per frame than a
  short one, and 16 px blocks cost more than 8 px. See
  [Benchmarks](../development/benchmarks.md#codec-degrade-563).
