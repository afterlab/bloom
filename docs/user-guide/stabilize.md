# Stabilize

**Stabilize** follows one feature through a shot and uses what it found. It can hold the picture still
against a shake (**Stabilize**) or move something along with the feature (**Match Move**). Find it under
**Transform** in the Add menu. The method, limits and stored data are in
[Point tracking and Stabilize](../architecture/point-tracking.md).

## Stabilize a shot

1. Connect your picture to **Input**.
2. Go to the frame to start from and drag the **Track Point** on the pad onto a feature that stays in the
   picture and has detail around it: a corner, a mark, an edge with texture.
3. Press **Track**. A bar shows the frames done; **Cancel** stops it and changes nothing. The picture is
   followed from this frame to the end of the composition.
4. When it finishes the card says how many frames were tracked. The **Tracked Position** is keyed with where
   the feature was found, and with **Mode** on **Stabilize** the picture is held still. Frames before the
   one you started on are left as they were.

## Match Move

Set **Mode** to **Match Move** and connect the picture you want to carry (a graphic, a plate of a
screen) to **Input**. It moves with the track instead of against it.

## Controls

- **Mode**: Stabilize or Match Move.
- **Pattern** and **Search**: how much of the feature is compared, and how far it may move between two
  frames. Use a larger pattern for a soft feature and a wider search for a fast move; both cost more.
- **Track Point** and **Tracked Position**: pixels from the frame's centre. Both can be keyed or driven.
  **Tracked Position** is written by the track; correct it by hand by editing its X and Y on a frame.
- **Filter** and **Edges**: how the moved picture is sampled and what shows where the picture no longer
  reaches (transparent by default).

## What ends a track

The track stops, and keeps what it found, where the feature is lost: it changes too much, leaves the
picture, or has no detail. The card says which and at what frame. Pick a better feature or start later.
A track made on a keyed **Track Point** is refused: set it to a fixed value first.

## Limits

- One feature, so the picture moves but does not turn or scale. Following a surface in perspective,
  and applying a track to **Corner Pin**, are not part of this version.
- The picture is read at the composition's resolution (a proxy of at most 1920 pixels on its longest
  side for larger frames) and on the processor, whatever the preview uses.
