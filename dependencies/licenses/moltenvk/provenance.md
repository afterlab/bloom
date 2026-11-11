# MoltenVK v1.4.2 Provenance

Reviewed: 2026-11-11

## Source

- Upstream project: <https://github.com/KhronosGroup/MoltenVK>
- Release tag: `v1.4.2` (a lightweight tag)
- Tag commit: `db66022459ffb663aa2b50f6b018bc2e124f5edf`
- Archive: `https://codeload.github.com/KhronosGroup/MoltenVK/tar.gz/refs/tags/v1.4.2`
- SHA-256 of the acquired archive:
  `6864db532f1dbbdb621a8d0ec13f24edae318fd9269dd3dd0cdff791334bb1cb`
- Exact archive size: `1260733` bytes

## Acquisition Record

- Acquired 2026-11-11 over HTTPS (curl), twice, from the release-tag archive URL above and from the
  equivalent `archive/refs/tags` URL; both downloads have the digest above.
- The tag was resolved with `git ls-remote` on the same day: `refs/tags/v1.4.2` names the commit
  above directly. GitHub reports that commit's signature as verified.
- Archive inspection: 328 entries, **0 absolute paths, 0 `..` path components, 10 symbolic links**.
  Every link target is relative and stays inside the archive's top directory. Seven of them point
  into `External/` or `Package/`, directories the archive does not contain (upstream's own fetch
  script creates them), so they dangle; the build does not follow them. GitHub's tag-generated
  archive contains no submodule content.
- `ExternalRevisions/` records the revisions MoltenVK is built against. The three this build uses:
  SPIRV-Cross `6c09849fe88c48eaed08413aa022aaa136a3a057` and cereal
  `a56bad8bbb770ee266e930c95d37fff2a5be7fea` (locked as `spirv-cross` and `cereal`), and
  Vulkan-Headers `e3b1eec08173d6b825cd3ac88c885a63b621504a`, which is the commit of the already
  locked `vulkan-headers` component. The wrapper project refuses to configure when a locked
  revision differs from the recorded one.
- GitHub publishes no detached signature for this tag archive (`provenancePolicy: not-published`).

## Status

- NOT QUALIFIED. Acquisition provenance only. No production prefix manifest exists, so this
  component is not release-qualified.
