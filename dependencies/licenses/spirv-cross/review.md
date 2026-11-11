# SPIRV-Cross License And Feature Review

Reviewed: 2026-11-11

## License

- Whole-component SPDX expression: `(Apache-2.0 OR MIT) AND Apache-2.0 AND MIT AND CC-BY-4.0 AND
  LicenseRef-KhronosFreeUse`. The archive's `LICENSE` and every text under `LICENSES/` are bound
  beside this file.
- Reached compiled graph (the four libraries MoltenVK links): `spirv_cross*.cpp`, `spirv_cfg.cpp`,
  `spirv_parser.cpp`, `spirv_glsl.cpp`, `spirv_msl.cpp`, `spirv_reflect.cpp` and their headers are
  `Apache-2.0 OR MIT`; `spirv.hpp` is `LicenseRef-KhronosFreeUse` and `GLSL.std.450.h` is `MIT`,
  both Khronos headers. The shader corpora (`shaders*/`, `reference/`, `tests-other/`, Apache-2.0)
  and the documentation (CC-BY-4.0) are neither compiled nor shipped. The per-file mapping is the
  upstream `.reuse/dep5`.
- Linkage is `static`: the objects become part of `libMoltenVK.dylib`. `sourceObligation: none`;
  `modified: true` with the one patch below. The only role recorded is `license`.

## Patches

`dependencies/patches/spirv-cross/0001-msl-keep-the-sign-of-a-zero-product.patch` changes only
`spirv_msl.cpp`, in the helper functions the Metal backend writes for a multiply that SPIR-V
decorates `NoContraction`. The helper computed `fma(l, r, 0)` so the Metal compiler could not fuse
the multiply with a neighboring add, and adding positive zero turns a product of negative zero into
positive zero. The patched helpers add negative zero, which leaves every value as it is, so a
no-contraction multiply gives the result Vulkan specifies. Bloom compares GPU results with a CPU
reference bit for bit, and its sample conversion shader reached the difference with a negative-zero
half-float sample. The patch is `Apache-2.0 OR MIT` like the file it changes, and the superbuild
applies it to the extracted locked archive before MoltenVK is built.

## Feature Minimization

The options are the ones MoltenVK's own build passes, set by the wrapper project
(`dependencies/superbuild/projects/moltenvk/CMakeLists.txt`):

| CMake option | Value | Reason |
| --- | --- | --- |
| `SPIRV_CROSS_ENABLE_MSL` | `ON` | The Metal Shading Language backend MoltenVK exists to use. |
| `SPIRV_CROSS_ENABLE_GLSL` | `ON` | The MSL backend derives from the GLSL backend. |
| `SPIRV_CROSS_ENABLE_REFLECT` | `ON` | MoltenVK links the reflection library. |
| `SPIRV_CROSS_ENABLE_HLSL` | `OFF` | Not used. |
| `SPIRV_CROSS_ENABLE_CPP` | `OFF` | Not used. |
| `SPIRV_CROSS_ENABLE_C_API` | `OFF` | Not used. |
| `SPIRV_CROSS_ENABLE_UTIL` | `OFF` | Not used. |
| `SPIRV_CROSS_ENABLE_TESTS` | `OFF` | No test tree or shader corpus is reached. |
| `SPIRV_CROSS_CLI` | `OFF` | No command-line tool is built. |
| `SPIRV_CROSS_SHARED` | `OFF` | Static only. |
| `SPIRV_CROSS_STATIC` | `ON` | Static only. |
| `SPIRV_CROSS_SKIP_INSTALL` | `ON` | Nothing of this component is installed. |
| `SPIRV_CROSS_NAMESPACE_OVERRIDE` | `MVK_spirv_cross` | MoltenVK's private namespace, so the symbols cannot collide with another copy. |
