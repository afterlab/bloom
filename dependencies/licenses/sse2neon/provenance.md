# sse2neon Provenance

Reviewed: 2026-11-11

## Source

- Upstream project: <https://github.com/DLTcollab/sse2neon>
- Source ref: exact commit `227cc413fb2d50b2a10073087be96b59d5364aea`, the revision OpenColorIO
  2.5.2 pins in `share/cmake/modules/install/Installsse2neon.cmake`
- Archive: `https://codeload.github.com/DLTcollab/sse2neon/tar.gz/227cc413fb2d50b2a10073087be96b59d5364aea`
- SHA-256 of the acquired archive:
  `3427a495743bb6fd1b5f9f806b80f57d67b1ac7ccf39a5f44aedd487fd7e6da1`
- Exact archive size: `136342` bytes

## Acquisition Record

- Acquired 2026-11-11 over HTTPS (curl). GitHub reports the commit's signature as verified.
- Archive inspection: 30 entries, **0 symbolic links, 0 absolute paths, 0 `..` paths**.
- GitHub publishes no detached signature for this commit archive (`provenancePolicy: not-published`).
- The lock version field is the exact commit: the pin is the one OpenColorIO names, not an
  sse2neon release tag.

## Status

- NOT QUALIFIED. Acquisition provenance only. No production prefix manifest exists. This is a
  build-only header source: the superbuild extracts it and OpenColorIO's build includes it; it
  installs nothing.
