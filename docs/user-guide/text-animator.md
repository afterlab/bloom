# Text Animator

A Text Animator animates a text layer letter by letter, word by word or line by line: letters that
fade, rise, turn or change color in turn, text that types on with a blinking cursor, a word that
fades out. It works on a Text node, and every value it has can be keyed or driven by another
node. The full specification is [Text Animator](../architecture/evaluation-primitives.md#text-animator).

## Adding one

In the Node editor, choose **Text Animator** from the Add menu (Text & Paths) and click the wire
between the Text node and its layer, or connect a Text node to its **Text** input yourself. Its
image goes on to the layer. Add another after it to stack them: each applies after the one before,
so a second animator's color draws over the first's.

A Text Animator reads a Text node, directly or through other Text Animators. Connected to anything
else, it passes the picture through unchanged and shows a warning.

## Selectors

A selector chooses which letters an animator changes, and how much. Each animator has four;
**Selector 1** is on. Each letter gets an amount between 0 % and 100 % (negative with a negative
**Amount**), and the properties below change by that much.

| Row | What it does |
| --- | --- |
| **Units** | What the range counts: Characters, Characters excluding spaces, Words or Lines |
| **Range by** | Start, End and Offset in Percent of the units, or as an Index (0 is the first unit) |
| **Start**, **End** | Where the range begins and ends |
| **Offset** | Moves the whole range; key it to sweep the range across the text |
| **Shape** | Square (fully in or out), Ramp up, Ramp down, Triangle, Round or Smooth |
| **Ease high**, **Ease low** | Flatten the shape toward its top or bottom (100 %) or steepen it (-100 %) |
| **Amount** | Scales the result, from -100 % to 100 % |
| **Randomize order**, **Seed** | Pick the units in a random order, the same for a given seed |
| **Mode** | How Selector 2, 3 and 4 combine with the selectors before them: Add, Subtract or Intersect |

With every selector off, the properties change the whole text.

## Properties

| Row | What it does |
| --- | --- |
| **Opacity** | The selected letters' opacity |
| **Position** | Moves the letters, in pixels |
| **Scale** | Scales each letter about the middle of its baseline |
| **Rotation** | Turns each letter about the middle of its baseline, clockwise for a positive angle |
| **Skew** | Leans each letter's top to the right for a positive angle |
| **Change fill color**, **Fill color** | Moves the letters' color toward this one |
| **Change outline color**, **Outline color** | Moves the outline's color toward this one |
| **Outline width** | Adds to the Text node's outline width, in pixels |
| **Tracking** | Adds space after each letter, in pixels; the line keeps its alignment |

The rows that do nothing are hidden: a selector's rows while it is off, Fill color while Change
fill color is off, Outline color while Change outline color is off, and the cursor rows while
Cursor is off.

## Reveal and the cursor

**Reveal** shows the first part of the text: a Percent of the units, or a Count of them (**Reveal
by**), counted in **Reveal units**. The text is laid out once, so a revealed letter stays exactly where it is in the
full text, in a box, centered or wrapped, and the layer's bounds do not change as the text appears.
**Cursor** draws **Cursor character** after the last revealed letter, blinking **Cursor blinks** times
a second (0 keeps it on).

## Recipes

- **Type-on**: key Reveal from 0 % at the first frame to 100 % at the last, and turn on Cursor.
- **Word fade**: set Units to Words, Shape to Ramp down and Opacity to 0 %, then key Offset from
  -100 % to 100 %. The words fade out one after another; reverse the keys to fade them in.
- **Counter**: no animator needed. Drive the Text node's **Text** from a keyed value through
  **Scalar To String**, and turn on the Text node's **Fixed-width digits** so the digits and the
  decimal point hold still ([Text layers](text-layers.md#fixed-width-digits)).
- **Subtitles typing on**: connect a Subtitles node's **Progress** through a **Map Range** (To max
  100) to Reveal, and its **Text** to the Text node's Text: each cue types on over its time.

Agents and scripts build the same recipes: `help {"name":"concept:text-animator"}` in MCP,
`bloom.help("concept:text-animator")` in Python and `bloom-cli help concept:text-animator`.

## Limits

An animated text draws at most 8,192 letters and 256 different colors and outline widths at once;
past either, the node shows an error naming the limit. Editing a value only redraws the frames it
changes, and a text with no animator draws as it always has.
