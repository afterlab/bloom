# Interpret Footage

Interpret Footage tells Bloom how to read a piece of media: at what rate it plays, where its
timecode starts, what its alpha channel means, whether it is interlaced, how wide its pixels are,
how often it loops, and what color it is in. It works like the Interpret Footage
dialog of a layer-based compositor. The interpretation belongs to the asset, so every layer that
uses the asset follows it, and it is saved with the project.

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
frame**, as before, or **show nothing**; **Skip them** plays the frames the sequence has back to
back, so a sequence of frames 1, 2 and 4 is three frames long.

**Input Color.** **Auto** reads the file's color tags (for untagged video, the assumption Bloom
shows in the note); the space Auto chose is named in the menu. Choose **Preserve RGB** to bring the
values in unchanged, **Interpret as Linear** for linear data, or any input space of the project's
color configuration. Camera log spaces (S-Log3, LogC3, LogC4, V-Log, Log3G10, REDLogFilm, C-Log2,
C-Log3 and Apple Log) are always offered, even when the configuration does not list them. A
source node's own input color in Properties still overrides the asset's.

## Import options and preferences

The **Import Media** dialog sets how footage is first read, under **Files of type**:

- **Import numbered images as a sequence** (on by default). Off, each numbered file you chose is
  imported as its own still. Numbered images of one name with no two consecutive frame numbers
  are always imported as stills (see [Assets](assets.md#import-options)).
- **Skip missing frames** gives a new sequence **Missing Frames: Skip them**.
- **Only frames** *first* **to** *last* reads only those numbered frames of each sequence; the rest
  are not read at all. The range stays with the sequence: **Reload Footage** and **Relink** read the
  same frames again. A range holding one frame imports that frame as a still. The size and alpha
  come from the frames the range keeps, even when the file you chose is outside it.
- **Create a composition** makes a composition from the imported footage, as **New Composition from
  Selection** does, and opens it.

The **Import** page of Preferences sets what new footage starts with: the **Sequence frame rate** a
new sequence assumes (24 fps unless you change it; **Match composition** plays one image per
composition frame) and the **Still duration** a new still lasts (the composition's length unless
you choose a fixed length; a layer made from the still then ends there, and a composition made from
it is that long). These are your preferences, not part of a project: Bloom writes them into each
new asset when it imports, so footage keeps its interpretation wherever the project is opened, and
sequences already in a project keep playing as they did. Reload Footage and Relink keep a still a
still even when its file is numbered.

The Assets thumbnails show the interpretation: **Ignore** and **Invert alpha** (and a premultiplied
matte color) the way a layer renders them, and the pixel aspect ratio as the thumbnail's shape.

## From an agent or a script

`bloom.asset.set-interpretation` sets the same members (see [Using Bloom from an agent](mcp.md)),
including `missingFrames` `skip` and a still's `stillDuration` (`compositionLength: true` restores
the composition's length), and `query` with `kind=assets` returns each footage asset's
interpretation. `bloom.asset.import` takes the import options as `asSequence`, `firstFrame` with
`lastFrame` and `skipMissingFrames`, and the preferences as `sequenceFrameRate` (omit it to match
the composition) and `stillDuration`; an agent then makes a composition with
`bloom.composition.add-from-assets` from the asset IDs the import outputs.

## Platforms

The dialog and every setting behave the same on Linux, macOS and Windows. Bloom's GPU path for the
alpha and deinterlace settings is verified on Linux; macOS and Windows use the same CPU reference
where the GPU path is not available.
