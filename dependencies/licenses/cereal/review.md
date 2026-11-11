# cereal License And Feature Review

Reviewed: 2026-11-11

## License

- Whole-component SPDX expression: `BSD-3-Clause`. The archive's `LICENSE` is bound beside this
  file.
- Reached scope: MoltenVK includes `cereal/archives/binary.hpp` and the `map`, `string` and
  `vector` type headers, for its pipeline cache. The JSON and XML archives are not included, so the
  third-party code they wrap under `include/cereal/external/` (RapidJSON, RapidXML and a Base64
  routine, each under its own license there) is not reached and is not part of this record.
- `sourceObligation: none`; `modified: false` with an empty patch set.

## Reached Scope And Minimization

- This is a **build-only header source**. The recipe
  (`dependencies/superbuild/projects/moltenvk.cmake`) extracts the archive and neither configures,
  builds nor installs it; cereal's own CMake project, tests, documentation and sandbox are never
  run. The wrapper project puts `include/` on MoltenVK's include path. The only role recorded is
  `license`: the component contributes the BSD-3-Clause notice for headers compiled into
  `libMoltenVK.dylib`.
