# ADR 0038: SVG Artwork Module

Status: accepted

Date: 2026-11-11

Issues: #642 (SVG artwork builder and Vector Artwork expansion), #343

## Context

The SVG artwork builder (`buildSvgArtwork`, `SvgArtworkWriter`) and the Vector Artwork layer's
compile-time expansion (`expandVectorArtwork`) lived in `src/document`. They read a parsed
`bloom/media/svg_scene.hpp` scene and the installed font catalogue
(`bloom/platform/font_catalog.hpp`) and describe the result as document records. `src/document` may
depend only on `src/core`, so `bloom.quality.architecture_boundaries` failed on those includes.

No existing module can host the code. `src/commands` consumes it for the editable import
(`AddSvgArtwork`), and `src/runtime` consumes it for the snapshot compiler and the vector artwork
scene cache. Neither may depend on the other, so placing it in either would force the other to
depend on it or to duplicate it. Both routes must build exactly the same artwork, so duplication is
not an option.

## Decision

A new declared module, `src/svgartwork`, built as the target `bloom_svg_artwork`, with public
headers under `src/svgartwork/include/bloom/svgartwork/`, in the way ADR 0034 added `src/mcp`.

It owns `svg_artwork.hpp`, `svg_artwork.cpp` and `svg_artwork_expansion.cpp`, and the unit test
`bloom.svgartwork.svg-artwork`. The code moved without a behavior change. The symbols keep their
`bloom::document` namespace, because they are document record builders and renaming them would
touch every caller for no boundary benefit; the module boundary is the directory, target and
include root.

Declared edges, in `repository_checks.cpp`:

- `svgartwork -> core, document, media, platform`
- `commands -> svgartwork`
- `runtime -> svgartwork`

The graph stays acyclic: `core`, `document`, `media` and `platform` do not depend on `svgartwork`;
only `commands` and `runtime` consume it. This declares a module. It is not an opt-out, and no other
rule or allowlist changed.

## Consequences

- `src/document` no longer includes `bloom/media` or `bloom/platform`, and no longer links the SVG
  reader or the font catalogue.
- The editable import and the Vector Artwork layer still share one builder, so the one-layer
  artwork remains the editable import by construction.
- A later builder that needs media or platform and is consumed by more than one module can use the
  same pattern; this ADR is not a general exemption.
