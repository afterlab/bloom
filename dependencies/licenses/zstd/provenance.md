# Zstandard 1.5.7 Provenance

Reviewed: 2026-11-12

## Source

- Upstream project: <https://github.com/facebook/zstd>
- Release tag: `v1.5.7`
- Archive: `https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz`
- SHA-256 of the acquired archive:
  `eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3`
- Exact archive size: `2434947` bytes

## Acquisition Record

- Acquired 2026-11-12 over HTTPS (curl) from the project's release page.
- The digest equals the one in the release's own `zstd-1.5.7.tar.gz.sha256`.
- Detached signature: `zstd-1.5.7.tar.gz.sig` from the same release, 858 bytes, SHA-256
  `24425933fb954f4608ae9383bc37ad8398e50364c1ec30bbdb5adbfe88209fb1`, checked in beside this
  record byte for byte.
- Verification: `gpg --verify` in an empty keyring reported a good signature from
  `Zstandard Release Signing Key <signing@zstd.net>`, primary key fingerprint
  `4EF4AC63455FC9F4545D9B7DEF8FE99528B52FFD`. The key was fetched by that fingerprint from
  `keyserver.ubuntu.com`, a public key server and not a channel of the project; the fingerprint
  is the one the project's releases have been signed with. No web of trust was evaluated.
- Archive inspection: 743 entries, **2 links, 0 absolute paths, 0 `..` path components**. Both
  link targets stay inside the archive.

## Status

- NOT QUALIFIED. Acquisition provenance only. No production prefix manifest exists, so this
  component is not release-qualified.
