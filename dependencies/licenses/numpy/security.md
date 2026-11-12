# numpy Security Review

Reviewed: 2026-11-12

## Reachability

numpy runs inside a Bloom process only when a user's Python script imports it. Its input is
whatever that script gives it. Loading pickled arrays (`numpy.load` with `allow_pickle=True`)
runs code from the file, as Python's own `pickle` does; that is off by default in numpy and is
the script's decision.

## Bounded Review

- The GitHub-hosted security advisories endpoint for `numpy/numpy` returned an empty array at
  review time.
- The GitHub advisory database lists eight advisories for the `numpy` package; the newest
  affected range ends before 1.22 (CVE-2021-34141) and the others before 1.21 or earlier. The
  locked 2.5.4 is after every range.
- 2.5.4 is the newest release on the acquisition date.
- This is a **bounded review, not a proof of zero vulnerabilities**. No NVD/CVE sweep, scanner or
  fuzzing was performed. The lock's empty `vulnerabilities` list records only that no advisory
  was identified within this scope.

## Disposition

- Accepted for non-release intake as an unqualified component. numpy releases must be followed
  and the lock moved with them (see the security release tracking in
  `docs/architecture/dependency-intake.md`). This record does not qualify it for release.

## Security Release Tracking

- Advisory source: <https://github.com/numpy/numpy/security/advisories>
- The project's published advisories, with the advisory database's entries for the `numpy`
  package. The locked minor series is followed; a patch release that fixes one moves the lock. See
  "Security release tracking" in `docs/architecture/dependency-intake.md`.
