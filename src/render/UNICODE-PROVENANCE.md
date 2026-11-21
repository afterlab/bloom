# Unicode Character Database provenance

`unicode_text_tables.inc` is generated data derived from the Unicode Character Database (UCD) by
`tools/text/generate_unicode_text_tables.py`. The styled text layout (`styled_text.cpp`, issue
#752) reads it for UAX #14 line breaking and to name the scripts it cannot lay out without shaping.

- Source: https://www.unicode.org/Public/16.0.0/ucd/
- Version: 16.0.0
- Date acquired: 2026-11-21
- License: the Unicode License v3 (https://www.unicode.org/license.txt), which permits copying,
  modifying and distributing the data files with the copyright and permission notice.
  Copyright © 1991-2024 Unicode, Inc.

Upstream files used (raw bytes, SHA-256, also recorded at the top of the generated file):

- `LineBreak.txt`  e97e4259d0d20fab150b9c7b4b28abfae5cd78ca97e7f4ac6ed20d685d5f4a7c
- `UnicodeData.txt`  ff58e5823bd095166564a006e47d111130813dcf8bf234ef79fa51a870edb48f
- `Scripts.txt`  9e88f0a677df47311106340be8ede2ecdacd9c1c931831218d2be6d5508e0039

What the generator applies:

- UAX #14 rule LB1: AI, SG, XX and unlisted code points are AL; SA is CM for code points whose
  general category is Mn or Mc, else AL; CJ is NS. AL ranges are left out of the table, which
  answers AL for every code point it does not list.
- The scripts that need shaping or bidi to read correctly are listed by name in the generator;
  only their `Scripts.txt` ranges are kept.

Regenerate with `python3 tools/text/generate_unicode_text_tables.py`; `--check` verifies the
checked-in file.
