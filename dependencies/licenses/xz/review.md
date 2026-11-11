# XZ Utils License And Feature Review

Reviewed: 2026-11-12

## License

- Reached scope SPDX expression: `0BSD`. Only `liblzma` is built, and the archive's `COPYING`
  states that liblzma is under the BSD Zero Clause License. `COPYING.0BSD` is bound beside this
  file as `LICENSE`, and `COPYING`, the map of which license applies to which part, as `COPYING`.
- Not reached and not part of this record: the command line tools' `getopt_long` fallback
  (LGPL-2.1-or-later), the scripts (GPL-2.0-or-later) and the autotools files
  (GPL-2.0-or-later, GPL-3.0-or-later). None is compiled or installed.
- `sourceObligation: none`; `modified: false` with an empty patch set.

## Why It Is Here

Bloom ships its own Python interpreter on macOS (see the `python` component). On Linux the
system interpreter gives scripts the `lzma` module; this component gives the bundled
interpreter the same. Bloom's own code does not call liblzma.

## Reached Scope And Minimization

- Built on the Apple silicon profile only (`dependencies/superbuild/projects/xz.cmake`), with the
  archive's CMake project.
- The static `liblzma.a` and its headers, into the prefix's private `python-support` directory,
  linked into the interpreter's `_lzma` extension module.
- Off: `xz`, `xzdec`, `lzmadec`, `lzmainfo`, the scripts, documentation, translations, Doxygen and
  tests.
- Recorded role: `license`.
