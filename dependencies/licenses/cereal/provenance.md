# cereal Provenance

Reviewed: 2026-11-11

## Source

- Upstream project: <https://github.com/USCiLab/cereal>
- Source ref: exact commit `a56bad8bbb770ee266e930c95d37fff2a5be7fea`, the revision MoltenVK
  v1.4.2 records in `ExternalRevisions/cereal_repo_revision`
- Archive: `https://codeload.github.com/USCiLab/cereal/tar.gz/a56bad8bbb770ee266e930c95d37fff2a5be7fea`
- SHA-256 of the acquired archive:
  `12e10fb541270632f6683286ce8c7e8c321fe4addb53fa91ed498b95c1758f93`
- Exact archive size: `386830` bytes

## Acquisition Record

- Acquired 2026-11-11 over HTTPS (curl). GitHub reports the commit's signature as verified.
- Archive inspection: 232 entries, **0 symbolic links, 0 absolute paths, 0 `..` paths**.
- GitHub publishes no detached signature for this commit archive (`provenancePolicy: not-published`).
- The lock version field is the exact commit: the pin is the one MoltenVK names, not a cereal
  release tag.

## Status

- NOT QUALIFIED. Acquisition provenance only. No production prefix manifest exists. This is a
  build-only header source: the superbuild extracts it and MoltenVK's build includes it; it
  installs nothing.
