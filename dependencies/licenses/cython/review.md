# Cython License And Feature Review

Reviewed: 2026-11-12

## License

- SPDX expression: `Apache-2.0`. The archive's `LICENSE.txt` is bound beside this file, and
  `COPYING.txt`, which states the terms of the older Pyrex code Cython derives from and that
  the output of a Cython compilation is not a derivative work of Cython.
- `sourceObligation: none`; `modified: false` with an empty patch set.

## Why It Is Here

numpy's source distribution holds `.pyx` files that must be compiled to C when numpy is built
(see the `numpy` component). Cython is the compiler. It is a build-host tool: it is recorded
with `executable` linkage and no shipping roles, as NASM is.

## Reached Scope And Minimization

- `dependencies/superbuild/projects/cython.cmake` unpacks the archive into the superbuild's own
  build tree. Nothing is configured, compiled or installed: `cython.py` runs as pure Python
  under the carried interpreter while numpy builds, and Cython's optional compiled accelerators
  are not built.
- Nothing of Cython enters the dependency prefix or a package. The C files it generates for
  numpy contain Cython's utility code; `COPYING.txt` states that such output carries no license
  restriction from Cython.
- No pip, no network: Meson is given the interpreter and the script through a native file.
