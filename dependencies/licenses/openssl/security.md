# OpenSSL Security Review

Reviewed: 2026-11-12

## Reachability

OpenSSL runs inside a Bloom process only when a user's Python script imports `ssl` or uses an
OpenSSL-backed `hashlib` algorithm. Then it handles whatever that script connects to: network
peers, certificates and keys chosen by the script. Bloom's own features open no TLS connection
through it.

## Bounded Review

- The project's vulnerability list for the 3.5 series
  (<https://openssl-library.org/news/vulnerabilities-3.5/>) was read at review time. The newest
  fixed release it names is 3.5.9, the locked one; no entry names a later 3.5 release.
- The GitHub-hosted security advisories endpoint for `openssl/openssl` returned an empty array.
- This is a **bounded review, not a proof of zero vulnerabilities**. No NVD/CVE sweep, scanner or
  fuzzing was performed. The lock's empty `vulnerabilities` list records only that no advisory
  was identified against 3.5.9 within this scope.

## Disposition

- Accepted for non-release intake as an unqualified component. A TLS library ages quickly: the
  3.5 series must be followed and the lock moved when a fix is published. This record does not
  qualify the component for release.

## Security Release Tracking

- Advisory source: <https://openssl-library.org/news/vulnerabilities-3.5/>
- The project's vulnerability list for the 3.5 series, the locked long-term-support series. A
  release it names as a fix moves the lock. See "Security release tracking" in
  `docs/architecture/dependency-intake.md`.
