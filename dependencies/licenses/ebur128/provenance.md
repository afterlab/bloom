# libebur128 1.2.6 Provenance (Vendored In-Tree Source)

Reviewed: 2026-11-09

## Source

`libebur128` is the EBU R128 / ITU-R BS.1770 loudness measurement library from `jiixyj/libebur128`.
Its C source is compiled directly into Bloom's Qt-free audio library as a private object library
and is not acquired by the dependency superbuild, so it has no lock entry or qualified-prefix
artifact.

- Upstream project: <https://github.com/jiixyj/libebur128>
- Release: v1.2.6 (2021-02-14)
- Pinned commit: `67b33abe1558160ed76ada1322329b0e9e058b02`, the immutable tree used for every
  vendored file below.
- Archive URL: `https://github.com/jiixyj/libebur128/archive/refs/tags/v1.2.6.tar.gz`
  (SHA-256 `baa7fc293a3d4651e244d8022ad03ab797ca3c2ad8442c43199afe8059faa613`).

Acquisition was over HTTPS on 2026-11-09. SHA-256 values were computed over the extracted bytes
before they were copied into the repository. The sources and license texts are unmodified.

| Vendored path | Bytes | SHA-256 |
| --- | ---: | --- |
| `src/media/third_party/ebur128/ebur128.c` | 53176 | `c2fc562f1088cacab4d21250b6e04996ef36c7694ea901e08cc4d64eb542b78d` |
| `src/media/third_party/ebur128/ebur128.h` | 15547 | `a988fa03828bcdd6258e6c52300d709a58727272a9ce6cd3fbf351f090517111` |
| `src/media/third_party/ebur128/queue/sys/queue.h` | 19539 | `8b284031772b1ba2035d9b05b24f2cb9b23e7bd324bcccb5e3fcc57d34aafa48` |
| `src/media/third_party/ebur128/LICENSE` | 1059 | `d6b4754bb67bdd08b97d5d11b2d7434997a371585a78fe77007149df3af8d09c` |
| `src/media/third_party/ebur128/queue/LICENSE` | 1516 | `70cf9b4c1aabf50c12a0bba3d352459d05fb379a047dac5ecc6a9f74e38ed5ef` |
| `dependencies/licenses/ebur128/LICENSE` | 1059 | `d6b4754bb67bdd08b97d5d11b2d7434997a371585a78fe77007149df3af8d09c` |
| `dependencies/licenses/ebur128/LICENSE-queue` | 1516 | `70cf9b4c1aabf50c12a0bba3d352459d05fb379a047dac5ecc6a9f74e38ed5ef` |

No detached signature, Sigstore bundle, or signed release identity was published with the tag.
Re-review is required for any upstream commit change or capability expansion.

## Status

This is checked-in source, not a superbuild component. The dependency-intake contract therefore
requires the license and review records and repository notices, but does not permit fabricating a
lock entry for it.
