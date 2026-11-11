# sse2neon License And Feature Review

Reviewed: 2026-11-11

## License

- Whole-component SPDX expression: `MIT`. The archive's `LICENSE` is bound beside this file, and
  `sse2neon.h` carries the same notice.
- Reached scope: the one header, `sse2neon.h`. OpenColorIO includes it on Arm in place of the x86
  intrinsic headers, so its inline functions are compiled into `libOpenColorIO.a`. The archive's
  tests, Makefile and Visual Studio project are neither built nor shipped.
- `sourceObligation: none`; `modified: false` with an empty patch set.

## Why It Is Required

OpenColorIO's CPU renderers are written with SSE intrinsics. On Arm, OpenColorIO compiles the same
renderers through this header; without it, it builds scalar renderers that call the C library's
`pow`, `log` and `exp`, which give different values. Bloom's CPU color results are the reference
its GPU color programs reproduce and the pixels every platform has to agree on, so the Arm build
runs the same arithmetic as the x86-64 build. The recipe stops a build in which OpenColorIO did not
use this header or turned its SSE renderers off
(`dependencies/superbuild/projects/opencolorio-require-sse2neon.cmake`).

## Reached Scope And Minimization

- This is a **build-only header source**. The recipe
  (`dependencies/superbuild/projects/sse2neon.cmake`) extracts the archive and neither configures,
  builds nor installs it. OpenColorIO finds the header through `sse2neon_ROOT`, so its own build
  does not clone it from the network. The only role recorded is `license`: the component
  contributes the MIT notice for code compiled into the OpenColorIO library.
- It is built for the Apple silicon profile only. The x86-64 profiles use the processor's own SSE
  instructions and do not build it.
