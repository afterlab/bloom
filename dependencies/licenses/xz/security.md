# XZ Utils Security Review

Reviewed: 2026-11-12

## Reachability

liblzma runs inside a Bloom process only when a user's Python script uses the `lzma` module (or
`tarfile`, `shutil` or `zipfile` on an LZMA-compressed archive). Its input is then whatever data
that script gives it. Bloom's own readers do not use it.

## Bounded Review

- The GitHub-hosted security advisories for `tukaani-project/xz` were read at review time. Four
  are published; the affected ranges end at 5.8.3 (GHSA-5qpq-xqfv-j9pg, decoder reinitialization),
  5.8.2 (CVE-2026-34743), 5.8.0 (CVE-2025-31115) and 5.6.2 (CVE-2024-47611, Windows only). The
  locked 5.8.4 is after every range.
- CVE-2024-3094 (the 5.6.0 and 5.6.1 backdoor) does not apply to this version, and the build path
  it used is not run here; see the provenance record.
- This is a **bounded review, not a proof of zero vulnerabilities**. No NVD/CVE sweep, scanner or
  fuzzing was performed.

## Disposition

- Accepted for non-release intake as an unqualified component. This record does not qualify it
  for release.

## Security Release Tracking

- Advisory source: <https://github.com/tukaani-project/xz/security/advisories>
- The project's published advisories; its release notes (`NEWS`) name the security fixes of each
  release. See "Security release tracking" in `docs/architecture/dependency-intake.md`.
