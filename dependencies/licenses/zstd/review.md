# Zstandard License And Feature Review

Reviewed: 2026-11-12

## License

- The library is offered under `BSD-3-Clause OR GPL-2.0-only`. Bloom takes it under
  `BSD-3-Clause`; the archive's `LICENSE` is bound beside this file. The GPL text (`COPYING`) is
  not bound because that option is not taken.
- `sourceObligation: none`; `modified: false` with an empty patch set.

## Why It Is Here

Bloom ships its own Python interpreter on macOS (see the `python` component). Python 3.14 has
the `compression.zstd` module, which the Linux system interpreter provides; this component gives
the bundled interpreter the same. Bloom's own code does not call it.

## Reached Scope And Minimization

- Built on the Apple silicon profile only (`dependencies/superbuild/projects/zstd.cmake`), with
  the CMake project under `build/cmake`.
- The static `libzstd.a` and its headers, into the prefix's private `python-support` directory,
  linked into the interpreter's `_zstd` extension module.
- Off: the command line programs, the shared library, tests, contributed code and support for the
  legacy frame formats.
- Recorded role: `license`.
