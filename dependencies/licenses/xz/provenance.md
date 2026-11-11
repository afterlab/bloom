# XZ Utils 5.8.4 Provenance

Reviewed: 2026-11-12

## Source

- Upstream project: <https://github.com/tukaani-project/xz>
- Release tag: `v5.8.4`, published 2026-09-09
- Archive: `https://github.com/tukaani-project/xz/releases/download/v5.8.4/xz-5.8.4.tar.gz`
- SHA-256 of the acquired archive:
  `0014c7886930454fe8bd4228665b51af55eeae560ea135c9c4cd33f55b2591d9`
- Exact archive size: `2799797` bytes

## Acquisition Record

- Acquired 2026-11-12 over HTTPS (curl) from the project's release page. This is the release
  tarball the maintainer uploads, not a tag-generated archive.
- Detached signature: `xz-5.8.4.tar.gz.sig` from the same release, 566 bytes, SHA-256
  `486b23126c9b36e34ab98a9b4894b5ca00a0c6105bf25dd1e5660ae5b2e01cf9`, checked in beside this
  record byte for byte.
- Verification: `gpg --verify` in an empty keyring holding only the key published at
  `https://tukaani.org/misc/lasse_collin_pubkey.txt` reported a good signature from
  `Lasse Collin <lasse.collin@tukaani.org>`, primary key fingerprint
  `3690C240CE51B4670D30AD1C38EE757D69184620`. No web of trust was evaluated.
- Archive inspection: 704 entries, **0 symbolic or hard links, 0 absolute paths, 0 `..` path
  components**.
- History that matters for this project: releases 5.6.0 and 5.6.1 carried a backdoor planted
  through the release tarballs (CVE-2024-3094). It was in the autotools build of the release
  archive and in test files. This build does not run the autotools build: it uses the CMake
  project in the archive, builds only `liblzma`, and builds no tests.

## Status

- NOT QUALIFIED. Acquisition provenance only. No production prefix manifest exists, so this
  component is not release-qualified.
