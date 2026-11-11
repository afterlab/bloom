# CPython 3.14.7 Provenance

Reviewed: 2026-11-12

## Source

- Upstream project: <https://github.com/python/cpython>, released at <https://www.python.org>
- Release: Python 3.14.7
- Archive: `https://www.python.org/ftp/python/3.14.7/Python-3.14.7.tar.xz`
- SHA-256 of the acquired archive:
  `3b48dac8fb59f62eaa67ac83c1eb12bda1b7a08406dd286e252c11a66be27f81`
- Exact archive size: `24053924` bytes

## Acquisition Record

- Acquired 2026-11-12 over HTTPS (curl) from python.org.
- Sigstore bundle: `Python-3.14.7.tar.xz.sigstore` from the same directory, 7101 bytes, SHA-256
  `6f41efc358b146b5548dbfc0414e9247441f8524fb6ef59f1c4ca946ffc70701`, checked in beside this
  record byte for byte.
- Verification: `sigstore verify identity` (sigstore-python 4.5.1) with certificate identity
  `hugo@python.org` and issuer `https://github.com/login/oauth`, the release manager of the 3.14
  series as python.org publishes it, reported `OK` for the archive.
- Archive inspection: 5557 entries, **0 symbolic or hard links, 0 absolute paths, 0 `..` path
  components**.
- Version choice: 3.14 is the minor version of the system Python that the Linux build of Bloom
  embeds (3.14.7 on the validation machine on this date), so both platforms offer scripts the same
  language and standard library.

## Status

- NOT QUALIFIED. Acquisition provenance only. No production prefix manifest exists, so this
  component is not release-qualified.
