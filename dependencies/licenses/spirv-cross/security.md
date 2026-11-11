# SPIRV-Cross Security Review

Reviewed: 2026-11-11

## Reachability

SPIRV-Cross is compiled into the MoltenVK driver. It parses SPIR-V modules and writes Metal Shading
Language. The modules it sees are the ones Bloom produced with the locked shader tools; no project, media or
network input reaches it directly.

## Bounded Review

- Checked the official GitHub-hosted security advisories endpoint for `KhronosGroup/SPIRV-Cross` at
  review time: the API returned an empty advisory array.
- This is a **bounded review, not a proof of zero vulnerabilities**. No NVD/CVE sweep, scanner, or
  fuzzing was performed. The lock's empty `vulnerabilities` list records only that no advisory was
  identified within this bounded scope.

## Disposition

- Accepted for non-release intake as an unqualified component. This does not qualify it for release
  and does not replace the dependency-intake qualification gates.
