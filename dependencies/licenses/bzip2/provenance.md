# bzip2 1.0.8 Provenance

Reviewed: 2026-11-12

## Source

- Upstream project: <https://sourceware.org/bzip2/>
- Release: 1.0.8, published 2019-07-13
- Archive: `https://sourceware.org/pub/bzip2/bzip2-1.0.8.tar.gz`
- SHA-256 of the acquired archive:
  `ab5a03176ee106d3f0fa90e381da478ddae405918153cca248e682cd0c4a2269`
- Exact archive size: `810029` bytes

## Acquisition Record

- Acquired 2026-11-12 over HTTPS (curl) from the project's download directory.
- Detached signature: `bzip2-1.0.8.tar.gz.sig` from the same directory, 310 bytes, SHA-256
  `c667e2db9a3cd17bde492584263cb7c30d8050538678ece3078ae58230e6e5e1`, checked in beside this
  record byte for byte.
- Verification: `gpg --verify` in an empty keyring holding only the key the project's download
  page links (`https://sourceware.org/pub/bzip2/gpgkey-5C1D1AA44BE649DE760A.gpg`) reported a good
  signature from `Mark Wielaard <mark@klomp.org>`, primary key fingerprint
  `EC3CFE88F6CA0788774F5C1D1AA44BE649DE760A`, made with its signing subkey
  `12768A96795990107A0D2FDFFC57E3CCACD99A78`. No web of trust was evaluated; the key came from
  the same site as the archive.
- Archive inspection: 56 entries, **0 symbolic or hard links, 0 absolute paths, 0 `..` path
  components**.

## Status

- NOT QUALIFIED. Acquisition provenance only. No production prefix manifest exists, so this
  component is not release-qualified.
