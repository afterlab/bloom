# linux-syscall-support Provenance

Reviewed: 2026-11-20

## Source

- Upstream project: <https://github.com/getsentry/chromium-linux-syscall-support>, Sentry's mirror
  of <https://chromium.googlesource.com/linux-syscall-support>
- Source ref: exact commit `9719c1e1e676814c456b55f5f070eabad6709d31` (2022-10-12), the revision
  the locked Crashpad commit names as its `third_party/lss/lss` submodule
- Archive: `https://codeload.github.com/getsentry/chromium-linux-syscall-support/tar.gz/9719c1e1e676814c456b55f5f070eabad6709d31`
- SHA-256 of the acquired archive:
  `a858367f0de0fd22a22172c562d750c29edb43f586cb6ff94be1941b77656bf3`
- Exact archive size: `41686` bytes

## Acquisition Record

- Acquired 2026-11-20 over HTTPS (curl). GitHub does not report the commit as signed.
- Archive inspection: 22 entries, **0 symbolic links, 0 absolute paths, 0 `..` paths**.
- GitHub publishes no detached signature for this commit archive (`provenancePolicy: not-published`).

## Status

- NOT QUALIFIED. Acquisition provenance only. A header-only source provider of the `crashpad`
  component on Linux; it installs nothing.
