# MoltenVK v1.4.2 License And Feature Review

Reviewed: 2026-11-11

## License

- Whole-component SPDX expression: `Apache-2.0`. Every source file under `Common/`,
  `MoltenVK/MoltenVK/` and `MoltenVKShaderConverter/MoltenVKShaderConverter/` (the compiled graph)
  carries the Apache-2.0 header; the archive's `LICENSE` is bound beside this file.
- The archive ships no `NOTICE` file. `Demos/` is not built.
- Linkage is `shared`: `libMoltenVK.dylib` is the Vulkan driver the loader opens at run time.
  `sourceObligation: none`; `modified: false` with an empty patch set.
- Code compiled into the library from other components carries their own records:
  `spirv-cross` (static, `link`) and `cereal` (headers, `build`). The Vulkan headers come from the
  locked `vulkan-headers` component (`build`).

## How It Is Built

MoltenVK's own CMake build downloads its dependencies while it configures. The recipe
(`dependencies/superbuild/projects/moltenvk.cmake`) does not run that step. A small wrapper project
(`dependencies/superbuild/projects/moltenvk/CMakeLists.txt`) creates the targets MoltenVK's build
files look for from the locked archives and then adds MoltenVK's source tree unchanged; each
upstream dependency recipe returns early when its target already exists. No file of the archive is
edited and nothing is fetched.

MoltenVK's build asks `git` for the revision of its checkout and stops when `git` is missing. An
archive is not a checkout, so the wrapper supplies a stand-in that prints the locked commit. The
library therefore reports `db66022459ffb663aa2b50f6b018bc2e124f5edf` as its revision wherever it is
built.

## Feature Minimization

| Setting | Value | Reason |
| --- | --- | --- |
| `MVK_EXCLUDE_SPIRV_TOOLS` | `ON` | SPIRV-Tools only adds a debug disassembly of shaders to the log. |
| `MVK_EXCLUDE_CEREAL` | `OFF` | cereal serializes the pipeline cache, which Bloom uses. |
| `MVK_USE_METAL_PRIVATE_API` | `OFF` | Upstream default; no private Metal interfaces. |
| `MVK_BUILD_SHADER_CONVERTER_TOOL` | `OFF` | The command-line converter is not shipped. |
| `MOLTEN_VK_WITH_CCACHE` | `OFF` | No compiler cache taken from the host. |
| `MVK_CONFIG_LOG_LEVEL` | `warn` | The default `info` level prints a banner on every instance creation. |
| `MVK_CONFIG_FAST_MATH_ENABLED` | `0` (never) | See below. |

Fast math: Metal compiles shaders with fast math unless told otherwise, which lets the compiler
reorder and contract floating-point arithmetic. Bloom compares GPU results with a CPU reference, so
this build's default is "never" (a compile definition, recorded as the feature decision
`bloom.feature.moltenvk-fast-math`). Bloom also requests the same value when it creates the
Vulkan instance, which takes precedence over the driver's environment variable.

## Installed Files

- `lib/libMoltenVK.1.4.2.dylib` with the links `libMoltenVK.1.dylib` and `libMoltenVK.dylib`. It
  loads only operating-system libraries and frameworks.
- `share/vulkan/icd.d/MoltenVK_icd.json`: MoltenVK's manifest template with the library path
  written relative to the manifest, so the prefix can be moved and packaged.

## Deployment Target

Upstream's project targets macOS 12.0; this build uses the profile's deployment target. The
sources call Metal interfaces that newer systems introduced and select them by a run-time version
check, which the compiler reports as unguarded-availability warnings. Running on a system older
than the build machine's has not been tested here.
