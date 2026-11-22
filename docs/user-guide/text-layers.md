# Text layers

A text layer's **Position** is a point on its text block that follows the text's **Alignment**, so a
block can be placed predictably whatever its lines say or how long they are:

| Alignment | Position is the block's |
| --- | --- |
| Left | left edge |
| Center | centre |
| Right | right edge |

With Left alignment, a line that grows longer therefore grows to the right of Position instead of
spreading to both sides and running off the left of the frame. With Right alignment it grows to the
left, and with Center alignment evenly on both sides. Every line of the block shares the same edge or
centre, so lines of different lengths line up on it. Vertically, Position is still the middle of the
block, and **Anchor** moves the pivot away from Position by a pixel offset as it does for any layer.

The block is the widest line for text without a box, and the box for text with a **Box** (see
[the layer and graph model](../architecture/layer-graph-model.md#text-and-mute-lowering)). The same
layout is used by the Viewer, by still and sequence export and by video export, on the CPU path and
on the GPU path.

A new text layer starts at the centre of the composition, so a Left-aligned new layer starts at the
centre and reads to the right; drag it or set **Position** to move its left edge where you want it.

## Anchor mode and older projects

The Text node's **Anchor Mode** choice has four settings. **Follow alignment** is the behaviour above
and is what every new text layer uses. **Left**, **Center** and **Right** are the settings projects
saved before Follow alignment existed carry: they keep Position at the centre of the text block, so an
old project looks exactly as it did when you reopen it, with no jump. Nothing is rewritten when you
open a project; switch a text layer to Follow alignment when you want its Position to follow the
alignment. Changing it moves the text, because Position then names a different point of the block.

## Fixed-width digits

**Fixed-width digits** (off by default) draws every digit 0 to 9 at the width of the font's widest
digit, centered in it. A counter's digits then hold still as they change, and its decimal point
stays in place: `1.00` and `8.00` line up. Fonts whose digits already share one width (DejaVu
Sans, the default) draw the same either way. For per-letter animation, see
[Text Animator](text-animator.md).

## Markup, accent colors, outline and background

A Text node draws plain text until you turn one of these on, so a project saved before them draws
exactly as it did. In an older project, select the Text node and click **Show Styling** in Properties to add the
rows; the picture does not change until you edit one.

**Markup** reads inline tags in the text, the same cue-text markup [Subtitles](subtitles.md) reads,
parsed by the same code:

| Tag | Draws |
| --- | --- |
| `<b>word</b>` | bold |
| `<i>word</i>` | italic |
| `<u>word</u>` | underlined |
| `<c.accent1>word</c>` .. `<c.accent4>word</c>` | in **Accent 1** .. **Accent 4** |
| `<c.#ff8800>word</c>` | in a literal color (`#RGB`, `#RRGGBB` or `#RRGGBBAA`, sRGB) |

The four accent colors are ordinary animatable color parameters, so a word's color keys through its
accent: key **Accent 1** and every `<c.accent1>` span follows it. A tag Bloom cannot read is drawn as
written, and the node shows a warning naming it. With Markup off, `<b>` is just text. When the font
has no bold or italic face, bold draws the regular face with the font-missing warning and italic
draws a synthesized oblique, as Subtitles does.

**Outline width** (pixels, animatable) draws an outline of **Outline color** outside the glyphs, so
the letters keep their full weight. **Outline join** is Round, Miter or Bevel: how the outline turns
at the corners of letters such as V and W.

**Background** draws a box behind the text: **Per line** gives each line its own box, **Block** one
box around every line (or the text's Box, when it has one). **Background color**, **Background
padding** (in em, so it grows with the size) and **Background corner radius** (pixels) shape it.

Rows that draw nothing are hidden in Properties, on the node card and in the timeline: the
background's color, padding and corner radius while Background is Off, and the outline's color and
join while Outline width is 0. They come back as soon as you turn Background on or set, key or
connect an Outline width.

The paint order is box, outline, fill. The layer's bounds, the Layer Bounds node and Composition >
Align include the outline and the box, so an outlined caption aligns by what is drawn. Text Outline
still reads the glyph outlines.

Other outlines:

- **Alpha Contour** after the layer traces the drawn image instead of the letters. Set it to Under
  with Offset at half the Width for an outline outside the letters that also follows effects.
- **Stroke Text** on a text layer's node menu in the node editor (`bloom.layer.stroke-outline` in
  MCP, Python and bloom-cli) adds a stroked path layer above the text that follows it: Text Outline,
  Path Stroke and a Shape Source, ready for Trim Path and dashes. **Stroke Masks** does the same for
  a layer's path masks.

Driving the text: connect a String to the Text node's text input. A counter is an animated value
through **Scalar To String** (its Decimals and Pad Width keep the digits steady); a type-on is
**Substring** with an animated Length.
