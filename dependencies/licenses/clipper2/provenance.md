# Clipper2 1.5.4 Provenance (Vendored In-Tree Source)

Reviewed: 2026-11-09

## Source

Clipper2 is the polygon clipping and offsetting library from `AngusJohnson/Clipper2`. Its C++ source
is compiled directly into Bloom's Qt-free runtime library as a private object library and is not
acquired by the dependency superbuild, so it has no lock entry or qualified-prefix artifact.

- Upstream project: <https://github.com/AngusJohnson/Clipper2>
- Release: 1.5.4 (tag `Clipper2_1.5.4`)
- Pinned commit: `ef88ee97c0e759792e43a2b2d8072def6c9244e8`, the immutable tree used for every
  vendored file below.
- Archive URL: `https://github.com/AngusJohnson/Clipper2/archive/refs/tags/Clipper2_1.5.4.tar.gz`
  (SHA-256 `9d8a35a29d04cd1b7b45f542c0ba48015feece1210036ea9e4efaad3140af4b0`).

Acquisition was over HTTPS on 2026-11-09. SHA-256 values were computed over the extracted bytes
before they were copied into the repository. The sources and license text are unmodified. Only the
core, engine, offset, rectangle clipping and Minkowski headers and the three library sources are
vendored; the export header, the examples, the bindings and the tests are not.

| Vendored path | Bytes | SHA-256 |
| --- | ---: | --- |
| `src/runtime/third_party/clipper2/include/clipper2/clipper.core.h` | 34466 | `bb003258fa7c9ce6f7d1847ae4e0b6ff419528b6f9c58e17622ce3600018d297` |
| `src/runtime/third_party/clipper2/include/clipper2/clipper.engine.h` | 18966 | `c3e8b7cabc80f5ab2592dd4401a8c445bd43422139ff216591aae94de1b5a175` |
| `src/runtime/third_party/clipper2/include/clipper2/clipper.h` | 25563 | `b9674f21cb6add7c5e252220d80af2bdc7e773b526947d2800aa499b716069bf` |
| `src/runtime/third_party/clipper2/include/clipper2/clipper.minkowski.h` | 4129 | `951b3f1d9b804f9fe9f1a142e6133c2efda2ba1c26c47e4c7de14c6d03a0eb39` |
| `src/runtime/third_party/clipper2/include/clipper2/clipper.offset.h` | 4735 | `00d0b670f40d030fad5cd658b71a34b8ff3185673825bbcc9f40e63d9216cbb2` |
| `src/runtime/third_party/clipper2/include/clipper2/clipper.rectclip.h` | 2802 | `279c6ae467cf9cec001c473d569dd7943a241690f5d64a17561efcb1a8e2e38e` |
| `src/runtime/third_party/clipper2/include/clipper2/clipper.version.h` | 126 | `124189ed3b71a705c04e81abf645a57d3d3e525dc1b1d9518e524c000c341bd2` |
| `src/runtime/third_party/clipper2/src/clipper.engine.cpp` | 88067 | `cc24e338e0df3bbf7b9146724e6bdfa001d16128583a04944a8399a7ceff7676` |
| `src/runtime/third_party/clipper2/src/clipper.offset.cpp` | 20980 | `b17738e96e1bd442f1589205fda0a752d228c7d8b4d0779ff50145fc43d4764c` |
| `src/runtime/third_party/clipper2/src/clipper.rectclip.cpp` | 28071 | `f2f86e07013596d00737314584edf21bd6abcbc9b3aefc6efe547928c0590515` |
| `src/runtime/third_party/clipper2/LICENSE` | 1338 | `c9bff75738922193e67fa726fa225535870d2aa1059f91452c411736284ad566` |
| `dependencies/licenses/clipper2/LICENSE` | 1338 | `c9bff75738922193e67fa726fa225535870d2aa1059f91452c411736284ad566` |

No detached signature, Sigstore bundle, or signed release identity was published with the tag.
Re-review is required for any upstream commit change or capability expansion.

## Status

This is checked-in source, not a superbuild component. The dependency-intake contract therefore
requires the license and review records and repository notices, but does not permit fabricating a
lock entry for it.
