# CPython Security Review

Reviewed: 2026-11-12

## Reachability

The interpreter runs inside Bloom whenever scripting is used: the Python console, scripts, the
`bloom` package, add-ons of the scripting tier, and the tests. It executes code the user chose
to run, with the user's own rights. Bloom starts it with an isolated configuration (no
environment variables, no user site directory, no `site` import), so nothing outside the bundle
changes which modules it loads. The standard library's parsers and network clients are reachable
by scripts and handle whatever those scripts give them.

## Bounded Review

- The GitHub-hosted security advisories endpoint for `python/cpython` returned an empty array at
  review time. CPython publishes its advisories through the Python Security Response Team and
  CVE records rather than there, and those were not swept.
- 3.14.7 is the newest release of the 3.14 series on the acquisition date and the version the
  Linux validation machine's distribution ships.
- This is a **bounded review, not a proof of zero vulnerabilities**. No NVD/CVE sweep, scanner or
  fuzzing was performed. The lock's empty `vulnerabilities` list records only that no advisory
  was identified within this scope.

## Disposition

- Accepted for non-release intake as an unqualified component. The 3.14 series must be followed
  and the lock moved with it, together with the `openssl` component. This record does not
  qualify the component for release.
