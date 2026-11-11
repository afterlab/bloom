# MoltenVK v1.4.2 Security Review

Reviewed: 2026-11-11

## Reachability

MoltenVK is the Vulkan driver on macOS and runs inside every Bloom process that creates a GPU
device. Its inputs are the Vulkan calls Bloom makes and the SPIR-V modules Bloom's own shader
compiler produces; it translates those to Metal Shading Language with SPIRV-Cross and hands them to
the system's Metal compiler. It does not read project or media files. One file-derived input
exists: pipeline cache data that Bloom reads back from its own cache directory is deserialized by
MoltenVK (through cereal) when a pipeline cache is created.

## Bounded Review

- Checked the official GitHub-hosted security advisories endpoint for `KhronosGroup/MoltenVK` at
  review time: the API returned an empty advisory array.
- This is a **bounded review, not a proof of zero vulnerabilities**. No NVD/CVE sweep, dependency
  scanner, or fuzzing was performed. The lock's empty `vulnerabilities` list records only that no
  advisory was identified within this bounded scope; it is not a security assertion.

## Disposition

- Accepted for non-release intake as an unqualified component. This record does not qualify the
  component for release and does not replace the dependency-intake qualification gates or the
  pending prefix manifest.
