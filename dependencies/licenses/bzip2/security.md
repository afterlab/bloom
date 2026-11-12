# bzip2 Security Review

Reviewed: 2026-11-12

## Reachability

libbz2 runs inside a Bloom process only when a user's Python script uses the `bz2` module (or
`tarfile`, `shutil` or `zipfile` on a bzip2-compressed archive). Its input is then whatever data
that script gives it. Bloom's own readers do not use it.

## Bounded Review

- The project's `CHANGES` file was read at review time. 1.0.7 fixed CVE-2019-12900 (the selector
  count in the decompressor) and CVE-2016-3189 (`bzip2recover`); 1.0.8 relaxed the first fix to
  accept valid files again. 1.0.8 is the newest release, and `bzip2recover` is not built here.
- bzip2 has had no release since 2019. That is recorded as a maintenance risk: a fix would have
  to be taken as a patch.
- This is a **bounded review, not a proof of zero vulnerabilities**. No NVD/CVE sweep, scanner or
  fuzzing was performed.

## Disposition

- Accepted for non-release intake as an unqualified component. This record does not qualify it
  for release.

## Security Release Tracking

- Advisory source: <https://sourceware.org/bzip2/>
- The project page, whose release announcements name the security fixes (1.0.8 fixed
  CVE-2019-12900). Releases are rare; the page is read at every review. See "Security release
  tracking" in `docs/architecture/dependency-intake.md`.
