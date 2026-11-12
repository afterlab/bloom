# libffi Security Review

Reviewed: 2026-11-12

## Reachability

libffi runs inside a Bloom process only when a user's Python script uses the `ctypes` module,
which calls native functions that script names with arguments that script gives. That is the
script's own authority; libffi is the mechanism. Bloom's own code does not use it.

## Bounded Review

- The GitHub-hosted security advisories endpoint for `libffi/libffi` returned an empty array at
  review time.
- 3.8.0 is the newest release on the acquisition date.
- This is a **bounded review, not a proof of zero vulnerabilities**. No NVD/CVE sweep, scanner or
  fuzzing was performed. The lock's empty `vulnerabilities` list records only that no advisory
  was identified within this scope.

## Disposition

- Accepted for non-release intake as an unqualified component. This record does not qualify it
  for release.

## Security Release Tracking

- Advisory source: <https://github.com/libffi/libffi/security/advisories>
- The project's published advisories; its release notes name the fixes of each release. See
  "Security release tracking" in `docs/architecture/dependency-intake.md`.
