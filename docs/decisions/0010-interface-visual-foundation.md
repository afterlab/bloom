# ADR 0010: Interface Visual Foundation

Status: accepted; amended: the primary interface typeface is Inter Variable, not DejaVu Sans (see the note at the end)

Date: 2026-08-25

## Context

Bloom's dense editor shell needs consistent icons and typography before individual panels grow
their own visual conventions. The application is native C++/Qt software, so using an icon or font
family must not introduce a JavaScript runtime or unrelated web build pipeline.

## Decision

- Adopt Phosphor Icons as Bloom's default interface icon family.
- Use curated, pinned raw SVG assets in the main application rather than framework-specific icon
  packages or the complete catalog.
- Reserve a separately compiled Qt resource pack for a future workflow that genuinely needs the
  complete catalog.
- Adopt DejaVu Sans as Bloom's primary interface typeface.
- Adopt Geist Mono as Bloom's monospaced interface typeface.
- Vendor pinned native font assets and their upstream license texts.
- Centralize icon and typography access behind semantic UI roles in `src/ui`.
- Keep accessible names, labels, scale behavior, and safe platform fallbacks independent of the
  chosen glyph or font asset.

The implementation and usage contract is defined in
[`../ux/visual-language.md`](../ux/visual-language.md).

## Consequences

- Bloom's source and runtime remain C++/Qt-only for interface assets.
- The main binary and ordinary rebuilds do not absorb thousands of unused icon files.
- Third-party license texts and upstream provenance become required distribution artifacts.
- Qt SVG and application-font loading become UI foundation capabilities when the first real icon
  and font assets are integrated.
- Visual regressions need checks at dense control sizes, high-DPI scale factors, disabled states,
  and on Linux, macOS, and Windows.
- Platform-managed application chrome may use the operating-system font while Bloom-controlled
  surfaces use the selected families.

## Amendment note

Added 2026-11-15; the decision text above stands. The code no longer follows the DejaVu Sans line:
the interface typeface is Inter Variable, with Geist Mono for values and code, bundled as one
variable face per family on every platform. The icon family (Phosphor) and the monospaced family
are unchanged. [`ux/visual-language.md`](../ux/visual-language.md) is the current contract.
