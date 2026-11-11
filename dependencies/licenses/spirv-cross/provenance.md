# SPIRV-Cross Provenance

Reviewed: 2026-11-11

## Source

- Upstream project: <https://github.com/KhronosGroup/SPIRV-Cross>
- Source ref: exact commit `6c09849fe88c48eaed08413aa022aaa136a3a057`, the revision MoltenVK
  v1.4.2 records in `ExternalRevisions/SPIRV-Cross_repo_revision`
- Archive: `https://codeload.github.com/KhronosGroup/SPIRV-Cross/tar.gz/6c09849fe88c48eaed08413aa022aaa136a3a057`
- SHA-256 of the acquired archive:
  `b81b9956289950570953738e666a031ca32ff64e4fc925eba89f227c42109518`
- Exact archive size: `2007157` bytes

## Acquisition Record

- Acquired 2026-11-11 over HTTPS (curl). GitHub reports the commit's signature as verified.
- Archive inspection: 5176 entries, **0 symbolic links, 0 absolute paths, 0 `..` paths**.
- GitHub publishes no detached signature for this commit archive (`provenancePolicy: not-published`).
- The lock version field is the exact commit: the pin is the one MoltenVK names, not a SPIRV-Cross
  release tag.

## Status

- NOT QUALIFIED. Acquisition provenance only. No production prefix manifest exists. This is a
  source-only provider: the superbuild extracts it and MoltenVK's build compiles it; it installs
  nothing by itself.
