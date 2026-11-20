# linux-syscall-support Security Review

Reviewed: 2026-11-20

## Reachability

Inline wrappers that issue Linux system calls directly. It parses no data.

## Bounded Review

- Checked the GitHub-hosted security advisories endpoint for
  `getsentry/chromium-linux-syscall-support` at review time: the API returned an empty advisory
  array.
- This is a **bounded review, not a proof of zero vulnerabilities**. No NVD/CVE sweep, scanner, or
  fuzzing was performed. The lock's empty `vulnerabilities` list records only that no advisory was
  identified within this bounded scope.

## Disposition

- Accepted for non-release intake as an unqualified, build-only header source of Crashpad.
