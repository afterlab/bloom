# OpenSSL 3.5.9 Provenance

Reviewed: 2026-11-12

## Source

- Upstream project: <https://github.com/openssl/openssl>
- Release: `openssl-3.5.9`, published 2026-09-29, of the 3.5 long-term-support series
- Archive: `https://github.com/openssl/openssl/releases/download/openssl-3.5.9/openssl-3.5.9.tar.gz`
- SHA-256 of the acquired archive:
  `603f5602e2eef00d77fbd429d34dcd5822bb301757a1bc9cdb24c670f1eb859a`
- Exact archive size: `53279637` bytes

## Acquisition Record

- Acquired 2026-11-12 over HTTPS (curl) from the project's release page.
- The digest equals the one in the release's own `openssl-3.5.9.tar.gz.sha256`.
- Detached signature: `openssl-3.5.9.tar.gz.asc` from the same release, 931 bytes, SHA-256
  `bdccf2057a7c4ae377b40e832cb8c632ae8b532d4fb64cc7b843066eee748f89`, checked in beside this
  record byte for byte.
- Verification: `gpg --verify` in an empty keyring holding only the keys of
  `https://openssl-library.org/source/pubkeys.asc` reported a good signature from
  `OpenSSL <openssl@openssl.org>`, primary key fingerprint
  `B146647E45A7B33947AB226B2A2C87D161692D40` (signing subkey
  `C46ED3F2CBEFDA1FDAADA44264ED7B1DCCE71CB2`). The key was taken from the project's own site; no
  web of trust was evaluated.
- Archive inspection: 6078 entries, **0 symbolic or hard links, 0 absolute paths, 0 `..` path
  components**.

## Status

- NOT QUALIFIED. Acquisition provenance only. No production prefix manifest exists, so this
  component is not release-qualified.
