# mini_chromium License And Feature Review

Reviewed: 2026-11-20

## License

- Whole-component SPDX expression: `BSD-3-Clause`. The archive's `LICENSE` (The Chromium Authors)
  is bound beside this file.
- `sourceObligation: none`; `modified: false` with an empty patch set.

## Why It Is Required

Crashpad is written against Chromium's "base" library; mini_chromium is the small part of it that
Crashpad needs (logging, file paths, strings, scoped handles and the build configuration macros).

## Reached Scope And Minimization

- Compiled by Crashpad's build (the recipe `dependencies/superbuild/projects/crashpad.cmake` copies
  it into Crashpad's `third_party/mini_chromium/mini_chromium` directory) into
  `libmini_chromium.a`, which Bloom's programs and handler link. Its headers are installed under
  `include/crashpad/mini_chromium`.
- Its tests and its GN build files are not used.
