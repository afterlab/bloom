# Interpret Footage

Interpret Footage tells Bloom how to read a piece of media: at what rate it plays, where its
timecode starts, what its alpha channel means, whether it is interlaced, how wide its pixels are,
how often it loops, and what color it is in. It works like After Effects' Interpret Footage
dialog. The interpretation belongs to the asset, so every layer that uses the asset follows it,
and it is saved with the project.

## Opening it

- Right-click an image, image sequence or video in **Assets** and choose **Interpret Footage…**.
- Or select footage and click the Interpret Footage button at the end of the selection header's
  facts line.

With several items selected, the dialog shows the item you clicked and its title names how many
items it applies to. **OK** changes only the settings you changed, on every selected item, and a
setting an item does not have is skipped for that item (a still has no frame rate; only a sequence
has missing frames). One **Undo** reverses the whole change.

To give other footage the same interpretation, right-click the footage you set up and choose
**Copy Interpretation**, then select the other items and choose **Paste Interpretation**. Paste
applies every setting.

## The settings

**Frame Rate.** A video plays at **its file's rate** or you can **Conform to** another rate. An
image sequence either **matches the composition** (one image per composition frame, how Bloom has
always played sequences) or you can **Assume this frame rate**. Type a whole number (`24`), a
decimal (`12.5`), a fraction (`30000/1001`), or one of the NTSC rates by its usual name: `23.976`,
`29.97`, `59.94`, `47.952` and `119.88` mean exactly 24000/1001, 30000/1001 and so on. A conformed
video's sound plays along with the picture, faster or slower. A layer that ended where its footage
ended keeps showing the whole footage at the new length.

**Start Timecode.** Use the source's own start timecode (a video's container timecode) or type an
override as `HH:MM:SS:FF` (`;` before the frames for drop-frame at 29.97 and 59.94). Timecode only
labels frames; no frame moves.

**Alpha.**

- **Guess from the file** reads the alpha the way the file says (most PNGs straight, EXRs
  premultiplied). Each image layer's own **Premultiply** switch in Properties still applies.
- **Ignore** uses the RGB and treats the picture as fully opaque.
- **Straight (Unmatted)** treats the alpha as straight.
- **Premultiplied (Matted with color)** treats the color as already multiplied by the alpha against
  a **Matte Color** (black for most renders); Bloom removes the matte.
- **Invert alpha** flips the alpha channel first.

An explicit choice applies to every layer that uses the footage.

**Fields.** Use the file's field order or choose **Progressive**, **Upper Field First** or **Lower
Field First**. **Deinterlace** keeps the first field and fills the other field's lines from the
lines around them.

**Pixel Aspect Ratio.** Use the file's ratio (anamorphic video is already shown at its intended
width) or choose square or a common SD or anamorphic ratio. The layer is drawn wider or narrower;
a child layer parented to it is not stretched.

**Loop.** Play the footage this many times end to end (up to 1000). The footage becomes that much
longer.

**Missing Frames** (image sequences). A frame missing from the sequence can **hold the previous
frame**, as before, or **show nothing**.

**Input Color.** **Auto** reads the file's color tags (for untagged video, the assumption Bloom
shows in the note); the space Auto chose is named in the menu. Choose **Preserve RGB** to bring the
values in unchanged, **Interpret as Linear** for linear data, or any input space of the project's
color configuration. Camera log spaces (S-Log3, LogC3, LogC4, V-Log, Log3G10, REDLogFilm, C-Log2,
C-Log3 and Apple Log) are always offered, even when the configuration does not list them. A
source node's own input color in Properties still overrides the asset's.

## From an agent or a script

`bloom.asset.set-interpretation` sets the same members (see [Using Bloom from an agent](mcp.md)),
and `query` with `kind=assets` returns each footage asset's interpretation.

## Platforms

The dialog and every setting behave the same on Linux, macOS and Windows. Bloom's GPU path for the
alpha and deinterlace settings is verified on Linux; macOS and Windows use the same CPU reference
where the GPU path is not available.
