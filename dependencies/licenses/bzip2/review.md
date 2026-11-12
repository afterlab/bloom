# bzip2 License And Feature Review

Reviewed: 2026-11-12

## License

- SPDX expression: `bzip2-1.0.6`, the BSD-style license of bzip2 and libbzip2. The archive's
  `LICENSE` is bound beside this file.
- `sourceObligation: none`; `modified: false` with an empty patch set. The recipe adds a build
  file of Bloom's own beside the sources and changes no source file.

## Why It Is Here

Bloom carries its own Python interpreter in every package (see the `python` component). This
component gives the interpreter's `bz2` module one libbz2 on every platform instead of the
operating system's. Bloom's own code does not call it.

## Reached Scope And Minimization

- Linux and macOS: `dependencies/superbuild/projects/bzip2.cmake` compiles the library's seven
  source files (`blocksort.c`, `huffman.c`, `crctable.c`, `randtable.c`, `compress.c`,
  `decompress.c`, `bzlib.c`) into a static library with
  `dependencies/superbuild/projects/bzip2/CMakeLists.txt` and installs it with `bzlib.h` into the
  prefix's private `python-support` directory. It is linked into the interpreter's `_bz2`
  extension module.
- Windows: CPython's own project file compiles the same seven files into `_bz2.pyd`.
- Not built: the `bzip2` and `bzip2recover` programs, the scripts, the shared library.
- Recorded role: `license`.
