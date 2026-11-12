# Fonts

Status: current (issue 489). Fonts are not assets. A text node names a font by an ID, and every
machine finds the font itself: an installed system font, a family from Google Fonts, or the built-in
face. No font file is bundled into a project, saved, or copied by Collect Files.

## The font ID

A text node's `font` parameter is a String holding the encoded font ID
(`src/document/font_id.hpp`):

```
source=google;family=Roboto;style=Bold;weight=700;italic=false;axes=wdth:100;version=v51
```

| Member | Meaning |
| --- | --- |
| `source` | `system`, `google` or `builtin`. |
| `family` | The family name. |
| `style` | The style name the artist sees ("Bold Italic", or a named instance). |
| `weight`, `italic` | The requested weight (1 to 1000) and slope. Matching falls back to these when a family spells a style differently. |
| `axes` | Other variable-axis values, `tag:value` pairs sorted by tag, for example `wdth:110`. Weight and italic are stated by their own members. |
| `ps` | System fonts only: the PostScript name, the most exact system match. |
| `version` | Google fonts only: the family version the project was saved with (for example `v51`). |

The characters `% ; = | :` and control characters inside a value are written as `%XX`. Decoding
ignores members it does not know, so a newer build's extra members do not stop an older build from
opening the project. Encoding then decoding returns the same ID. The built-in face is
`source=builtin;family=DejaVu Sans;style=Book;weight=400;italic=false`.

The document version is unchanged (1.43). The parameter was already a String and the schema did not
move; the writer simply no longer emits Font assets, and the open path converts older projects (see
Migration).

## Resolution

`FontResolver` (`src/runtime/font_resolver.hpp`) turns an ID into a font file in this order:

1. An installed system font: the PostScript name first, then family and style, then the same family
   with equal weight and slope. A different weight is never taken as the requested one.
2. The per-user Google Fonts cache.
3. An on-demand download from Google Fonts into that cache.
4. The fallback table.

An installed variable font is drawn at the coordinates the ID records. The named instance a platform
lists (on Linux the instance number in the high 16 bits of the face index) and the ID's `axes` values
are applied by writing a static instance of the font (`variable_font_instance.cpp`): TrueType outlines
with `gvar` deltas, `avar`, composite offsets, advance widths from `HVAR` (else the `gvar` phantom
points) and glyph bounds follow the coordinates, an axis value outside the font's range is clamped,
and a tag the font does not have is ignored. The file digest of the instance is the font's identity in
plan and cache keys. A variable font that stores compact (CFF2) outlines is drawn at its default
instance; hinting instructions are dropped.

A `system` ID whose font is not installed but whose family exists on Google Fonts is fetched from
Google Fonts, with a status-bar notice. When the cached or downloaded family version differs from the
one the project recorded, the latest is used and a "font version changed" notice appears. The ID keeps
the version it was saved with until the artist saves again with a font chosen.

Callers choose how to wait (`FontFetch`):

| Mode | Used by | Behavior |
| --- | --- | --- |
| `CachedOnly` | Tools that must not touch the network | Cache and system fonts only. |
| `Background` | The Viewer and preview | Draws the fallback immediately, starts the download, and refreshes the preview when the font arrives. |
| `Wait` | Headless render and every export | Waits for the download on the compile worker, with cancellation, before producing a frame. |

Downloads never run on the UI thread. One worker owns the queue, deduplicates by face, reports
progress, honors cancellation, writes the cache atomically (a `.part` file renamed into place with a
`.meta` file carrying the family version), and backs off for 30 seconds after a failure. A project
opens offline and says which fonts are missing: each missing font produces a
`bloom.runtime.compile.font-missing` warning naming both the font and the face used instead.

The compiled-plan cache key includes the resolver's arrival epoch, so a plan built with a fallback is
rebuilt after the font arrives. Picker preview subsets use a separate counter that no plan sees.

## Network

Only Google Fonts' keyless CSS2 endpoint is used. No API key exists or is stored. The stylesheet is
requested without a browser user agent, which returns TrueType files; a single weight value returns a
static instance; axes in a request are sorted with lowercase tags first. Font files are accepted only
from `https://fonts.gstatic.com/`. The family version is read from the file URL.

The transport is a seam (`FontTransport`). The desktop application installs a Qt Network
implementation. The command-line tools, the MCP server and Python install a headless one
(`makeHeadlessFontTransport`, `src/runtime/headless_font_transport_curl.cpp`) at start-up: a blocking,
cancellable HTTPS GET with a size cap and a stall timeout, run on the same font worker and writing the
same per-user cache. It speaks through the system's own HTTP client library, loaded when first needed,
so no network library is linked and none is bundled. Where that library is absent, and on Windows and
macOS (where this transport has not been written or run), the headless programs report "is not
installed and cannot be downloaded here" for a font that is not already installed or cached, as
before. Offline still ends in the fallback face with the usual diagnostic. Tests use a fake transport.

## Fallback table

The fallback is chosen by category. The table (sans, serif and monospace lists) is documented once, in
`src/runtime/font_fallback.cpp`, and is not repeated here.

Anything else takes the sans list. The last resort is the built-in face. The category comes from the
baked Google Fonts index when the family is listed there, and otherwise from the name.

## The baked index

`src/runtime/google_catalogue_data.cpp` holds a versioned index of Google Fonts families: name,
category, family version, styles and variable axes. It is generated by
`tools/fonts/generate_google_catalogue.py` from a sparse checkout of the public font repository plus
keyless stylesheet queries, and it records its date and source revision. Browsing needs no network.
A family missing from the index is still fetched by name and is shown in the picker as "not in this
version's catalogue".

No font binaries are committed.

## Missing fonts

A missing font keeps its recorded ID. It is shown in two places and both are display only:

- The font name in Properties, on the node card and in the picker is drawn in the error color.
- With the Viewer option "Highlight missing fonts" on (Guides menu, default on), text with a missing
  font is overlaid in red. Every text layer is marked, however its source is wired to the Layer Output
  (straight in, or through effects, merges and other nodes); a layer that only parents or stacks over
  that text is not. The overlay is never drawn into RAM Preview frames, exports or headless renders.

Notices (a download started or finished, a failure, a version change, a system font taken from Google
Fonts) are transient: the status bar and the log. The red name is the only persistent indication.

## Picker

The font field opens a searchable list with Recent, Installed and Google Fonts sections and category
filters (sans, serif, display, handwriting, monospace). Each row is drawn in its own typeface. A
Google row uses a name-only subset (`text=`) fetched only for rows in view and cached. Moving the
current row with the pointer or the arrow keys previews the font live in the Viewer; Escape reverts;
choosing commits exactly one undo step. A style selector beside the field lists weights, italics and
named instances, and a Google family with variable axes shows a field for each axis other than weight.

## Migration

Opening a project that holds Font assets converts them (`src/project/document_font_migration.cpp`):
an embedded face becomes a `builtin` ID, a system face becomes a `system` ID with weight and slope
read from its style name, and a text node's old reference (an asset ID, a number 0 to 3, or a
`family|style|digest` reference) becomes the matching ID. Anything unrecognized becomes the built-in
face. The Font assets and their data blocks are then removed. A project saved afterward carries IDs
only.

## Platform notes

Installed fonts are listed with fontconfig on Linux, Core Text on macOS, and DirectWrite's system
font collection on Windows, which holds the system fonts and the ones a user installed for
themselves, without downloadable fonts. On Windows DirectWrite names each face's file, weight,
width and slant, and the shared reader names the face from the file's name table (TrueType,
OpenType and collections), so a face has the same family, style and PostScript name on every
platform. The Windows enumeration runs in `bloom.platform.font-catalog` on Windows.
