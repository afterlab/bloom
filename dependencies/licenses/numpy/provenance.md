# numpy 2.5.4 Provenance

Reviewed: 2026-11-12

## Source

- Upstream project: <https://github.com/numpy/numpy>
- Release: 2.5.4, the source distribution published on the Python Package Index
- Archive: `https://files.pythonhosted.org/packages/source/n/numpy/numpy-2.5.4.tar.gz`
- SHA-256 of the acquired archive:
  `9a94cf751c9ad8ebaa835bcd3d40dacf8534ad086b88c38029b65123c7999d2a`
- Exact archive size: `20866315` bytes

## Acquisition Record

- Acquired 2026-11-12 over HTTPS (curl). The index's own record of the file gives the same
  SHA-256 and size.
- Published attestation: the index's provenance record for the file
  (`https://pypi.org/integrity/numpy/2.5.4/numpy-2.5.4.tar.gz/provenance`), 10029 bytes, SHA-256
  `3c29a523370c295d2dccf5d36da867301543dccc0f1e06e76a153aa8823ede6f`, checked in beside this
  record byte for byte as `numpy-2.5.4.tar.gz.provenance.json`. It holds one Sigstore-signed
  publish attestation.
- What was verified, with OpenSSL and no Sigstore client:
  - The attestation's statement names `numpy-2.5.4.tar.gz` with the SHA-256 above.
  - The envelope's signature is a valid ECDSA signature over the statement (DSSE encoding) by
    the key in the attestation's certificate.
  - The certificate was issued by `sigstore-intermediate` of `sigstore.dev`, valid 2026-10-10
    20:01:21 to 20:11:21 UTC, to the identity
    `https://github.com/numpy/numpy-release/.github/workflows/wheels.yml@refs/heads/maintenance/2.5.x`
    with the GitHub Actions token issuer, source repository `numpy/numpy-release`, commit
    `a1cc2f4a9705f4171d480263fde93fa01ba816d6`. That is the publisher the index records for the
    project (repository `numpy/numpy-release`, workflow `wheels.yml`, environment `pypi`).
  - The certificate chains to the Sigstore root: `openssl verify` accepts it, at the time the
    log recorded the entry, against the root and intermediate certificates the Sigstore
    certificate authority serves (`https://fulcio.sigstore.dev/api/v2/trustBundle`; SHA-256 of
    the two PEM certificates as served: root
    `dcf166eebe7cbd9760947a88213d94e656349c647d439569dc76a275f05b7159`, intermediate
    `ac5a60ca80f473f8b7742111f9eb49bf7dbd8f03f41a5b39d58b62b6f0de4d0f`).
  - The entry is in the transparency log. The logged entry (log index 3192472614, recorded
    2026-10-10 20:01:22 UTC, inside the certificate's ten minutes) holds this envelope's
    signature, certificate and the SHA-256 of its statement. Its inclusion proof (20 hashes,
    RFC 6962) gives the root hash of the checkpoint for tree size 3070568399; the checkpoint and
    the signed entry timestamp are valid ECDSA signatures by the log's key
    (`https://rekor.sigstore.dev/api/v1/log/publicKey`; SHA-256 of the PEM file
    `dce5ef715502ec9f3cdfd11f8cc384b31a6141023d3e7595e9908a81cb6241bd`), whose digest is the
    entry's log id.
- Limit of this verification: the authority's certificates and the log's key were fetched over
  HTTPS from the services themselves, not through Sigstore's signed trust-root repository, so
  it rests on those two HTTPS connections. No Sigstore client was used.
- Archive inspection: 8527 entries, **0 symbolic or hard links, 0 absolute paths, 0 `..` path
  components**.
- The source distribution carries code from other projects (see the license review) and a copy
  of the Meson build system, which the recipe runs.

## Status

- NOT QUALIFIED. Acquisition provenance only, with the attestation verified within the limit
  stated above. No production prefix manifest exists, so this component is not
  release-qualified.
