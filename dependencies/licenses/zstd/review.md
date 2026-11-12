# Zstandard License And Feature Review

Reviewed: 2026-11-12

## License

- The library is offered under `BSD-3-Clause OR GPL-2.0-only`. Bloom takes it under
  `BSD-3-Clause`; the archive's `LICENSE` is bound beside this file. The GPL text (`COPYING`) is
  not bound because that option is not taken.
- `sourceObligation: none`; `modified: false` with an empty patch set.

## Why It Is Here

Bloom carries its own Python interpreter in every package (see the `python` component). Python
3.14 has the `compression.zstd` module; this component is the one Zstandard behind it on every
platform. Bloom's own code does not call it.

## Reached Scope And Minimization

- Linux and macOS: `dependencies/superbuild/projects/zstd.cmake` builds the static `libzstd.a`
  with the CMake project under `build/cmake` and installs it with its headers into the prefix's
  private `python-support` directory. It is linked into the interpreter's `_zstd` extension
  module.
- Windows: CPython's own project file compiles the library's sources from the same archive into
  `_zstd.pyd`; the recipe only unpacks the archive there.
- Off: the command line programs, the shared library, tests, contributed code and support for the
  legacy frame formats.
- Recorded role: `license`.
