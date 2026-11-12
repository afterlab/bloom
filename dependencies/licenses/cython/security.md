# Cython Security Review

Reviewed: 2026-11-12

## Reachability

Cython runs on the build machine only, while numpy is built, on numpy's own source files from
the locked numpy archive. It is not in the dependency prefix and not in any package, so it never
runs on a user's computer and never sees a user's data.

## Bounded Review

- The GitHub-hosted security advisories endpoint for `cython/cython` returned an empty array at
  review time.
- 3.3.1 is the newest release on the acquisition date and satisfies numpy 2.5.4's requirement
  (Cython 3.1.0 or newer).
- What a compromised compiler could do is put code into numpy's extension modules. The archive
  is pinned by digest; it has no published signature (see the provenance record).
- This is a **bounded review, not a proof of zero vulnerabilities**. No NVD/CVE sweep, scanner or
  fuzzing was performed.

## Disposition

- Accepted for non-release intake as an unqualified build tool. This record does not qualify it
  for release.

## Security Release Tracking

- Advisory source: <https://github.com/cython/cython/security/advisories>
- The project's published advisories. Cython is a build tool and is not carried by a package; it
  is followed because the C code it generates is. See "Security release tracking" in
  `docs/architecture/dependency-intake.md`.
