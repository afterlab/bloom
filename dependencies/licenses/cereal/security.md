# cereal Security Review

Reviewed: 2026-11-11

## Reachability

cereal is a header-only serialization library. MoltenVK uses its binary archive to write and read
pipeline cache data. Reading is the exposed direction: the bytes Bloom passes when it creates a
pipeline cache come from a file in Bloom's own cache directory, and cereal's binary input archive
deserializes them inside the driver. That file is written by the same code on the same machine and
has the trust of the user's own cache directory.

## Bounded Review

- Checked the official GitHub-hosted security advisories endpoint for `USCiLab/cereal` at review
  time: the API returned an empty advisory array.
- This is a **bounded review, not a proof of zero vulnerabilities**. No NVD/CVE sweep, scanner, or
  fuzzing was performed. The lock's empty `vulnerabilities` list records only that no advisory was
  identified within this bounded scope.

## Disposition

- Accepted for non-release intake as an unqualified, build-only component. This does not qualify it
  for release and does not replace the dependency-intake qualification gates.
