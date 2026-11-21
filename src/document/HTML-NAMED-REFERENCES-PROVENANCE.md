# HTML named character references provenance

`html_named_references.inc` is generated data derived from the HTML Standard's list of named
character references, which the WebVTT cue text tokenizer consumes (issue #752,
`docs/architecture/subtitles.md`).

- Source: https://html.spec.whatwg.org/entities.json
- SHA-256 of the bytes the table was generated from:
  `d741d877ac77c4194c4ad526b5b4a19aef8dfe411ab840a466891cdbb9f362e6`
- Entries: 2231 (every name the standard defines, with and without the trailing `;` where the
  standard accepts both). The HTML Standard states that this list is static and will not change.
- Date acquired: 2026-11-21
- License: the HTML Standard is "Copyright © WHATWG (Apple, Google, Mozilla, Microsoft)" and is
  licensed under the Creative Commons Attribution 4.0 International License; to the extent portions
  of it are incorporated into source code, such portions in the source code are licensed under the
  BSD 3-Clause License instead. The reference table is incorporated here into source code, so the
  BSD 3-Clause License applies to this generated fragment (copyright holder: WHATWG). Attribution:
  WHATWG HTML Living Standard, https://html.spec.whatwg.org/.

Regenerate (writes the file) or verify (exits non-zero when the checked-in file differs):

    python3 tools/subtitles/generate_html_named_references.py
    python3 tools/subtitles/generate_html_named_references.py --check
