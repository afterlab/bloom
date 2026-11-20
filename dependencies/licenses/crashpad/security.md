# Crashpad Security Review

Reviewed: 2026-11-20

## Reachability

The client runs inside each Bloom program: it registers the program with the handler, installs the
platform's crash hooks (signal handlers on Linux, exception ports on macOS, an unhandled-exception
filter on Windows) and keeps the annotation and attachment lists. The handler is a separate
process started by the program, with the same user's rights. It reads the crashed process's
memory and writes a minidump into Bloom's per-user crash folder. It reads no network data, and the
patched build has no network transport at all.

## Bounded Review

- Checked the GitHub-hosted security advisories endpoint for `getsentry/crashpad` at review time:
  the API returned an empty advisory array. Upstream Crashpad's security issues are tracked in
  Chromium's issue tracker; none was identified for this revision within this bounded review.
- This is a **bounded review, not a proof of zero vulnerabilities**. No NVD/CVE sweep, scanner, or
  fuzzing was performed. The lock's empty `vulnerabilities` list records only that no advisory was
  identified within this bounded scope.

## Disposition

- Accepted for non-release intake as an unqualified component. This does not qualify it for
  release and does not replace the dependency-intake qualification gates.
