# SQLite Security Review

Reviewed: 2026-11-12

## Reachability

SQLite runs inside a Bloom process only when a user's Python script uses the `sqlite3` or
`dbm.sqlite3` module. Its input is then the SQL that script runs and the database files that
script opens. Bloom's own project and cache files do not use it.

## Bounded Review

- The project's own vulnerability table (`https://www.sqlite.org/cves.html`) was read at review
  time. The newest entries with a fixing release are CVE-2026-11822 and CVE-2026-11824 (fixed in
  3.53.2), CVE-2025-70873 (the zipfile extension, 3.52.0), CVE-2025-7709 (FTS5, 3.50.3) and
  CVE-2025-6965 (3.50.2). The locked 3.54.1 is after every one. The table marks CVE-2026-119586
  as a matter of the shell's `.recover` command, which is not built here, and six 2026 entries
  as not reproducible.
- Loading extensions from a script is off unless the script enables it; that is the `sqlite3`
  module's own default and is not changed.
- This is a **bounded review, not a proof of zero vulnerabilities**. No NVD/CVE sweep, scanner or
  fuzzing was performed.

## Disposition

- Accepted for non-release intake as an unqualified component. SQLite releases must be followed
  and the lock moved with them (see the security release tracking in
  `docs/architecture/dependency-intake.md`). This record does not qualify it for release.

## Security Release Tracking

- Advisory source: <https://www.sqlite.org/cves.html>
- The project's own table of vulnerabilities, with the release that fixes each. See "Security
  release tracking" in `docs/architecture/dependency-intake.md`.
