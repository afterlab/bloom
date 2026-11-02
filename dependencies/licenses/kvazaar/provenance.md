# Kvazaar 2.3.2 provenance

- Upstream release: Kvazaar v2.3.2, published 2025-09-17.
- Selection rationale: the latest upstream release at the 2026-10-31 intake; FFmpeg 8.1's
  `libkvazaar` wrapper requires Kvazaar 2.0.0 or newer.
- Archive URL: <https://github.com/ultravideo/kvazaar/releases/download/v2.3.2/kvazaar-2.3.2.tar.xz>
  (the release's distribution tarball, not a generated tag archive)
- Archive retrieval date: 2026-10-31 (UTC date)
- Archive size: 708,352 bytes
- Archive SHA-256: `sha256:35e506a6d2be9ae343f17787eb80fa214492b276e9c88b6e93749050ae380070`
- Upstream signature: none published; the lock pins the archive digest.

The same locked archive is the corresponding-source archive for both static libraries linked into
`libavcodec`: the 8-bit build and the 10-bit build (`KVZ_BIT_DEPTH=10`, issue #408). Bloom does not
modify the source; the 10-bit build only adds compiler definitions, including the force-included
symbol-rename header `dependencies/superbuild/projects/kvazaar10-symbols.h`. The superbuild verifies
the SHA-256 before extraction and builds from the populated archive cache with downloads disabled.
