# sse2neon Security Review

Reviewed: 2026-11-11

## Reachability

sse2neon is one header of inline functions that map SSE intrinsics to Arm NEON instructions. It
parses nothing, allocates nothing on its own account and reads no file or network data; it is
reached only as arithmetic inside OpenColorIO's CPU renderers, on values OpenColorIO already holds.

## Bounded Review

- Checked the official GitHub-hosted security advisories endpoint for `DLTcollab/sse2neon` at
  review time: the API returned an empty advisory array.
- This is a **bounded review, not a proof of zero vulnerabilities**. No NVD/CVE sweep, scanner, or
  fuzzing was performed. The lock's empty `vulnerabilities` list records only that no advisory was
  identified within this bounded scope.

## Disposition

- Accepted for non-release intake as an unqualified, build-only component. This does not qualify it
  for release and does not replace the dependency-intake qualification gates.
