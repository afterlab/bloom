# Zstandard Security Review

Reviewed: 2026-11-12

## Reachability

The library runs inside a Bloom process only when a user's Python script uses
`compression.zstd` (or `tarfile`, `shutil` or `zipfile` on a Zstandard-compressed archive). Its
input is then whatever data that script gives it. Bloom's own readers do not use it.

## Bounded Review

- The GitHub-hosted security advisories endpoint for `facebook/zstd` returned an empty array at
  review time.
- This is a **bounded review, not a proof of zero vulnerabilities**. No NVD/CVE sweep, scanner or
  fuzzing was performed.

## Disposition

- Accepted for non-release intake as an unqualified component. This record does not qualify it
  for release.

## Security Release Tracking

- Advisory source: <https://github.com/facebook/zstd/security/advisories>
- The project's published advisories; its release notes name the security fixes of each release.
  See "Security release tracking" in `docs/architecture/dependency-intake.md`.
