# Clipper2 1.5.4 Security Review

Reviewed: 2026-11-09

## Disposition

The library operates on coordinate arrays Bloom has already produced in process; it opens no file,
parses no format, and makes no network or system call. Its inputs are polygons whose vertex counts are
bounded by Bloom's path value limits (at most 262144 anchors in one value) before flattening, and
whose coordinates are finite and bounded to a safe magnitude by the Bloom wrapper before conversion to
the library's integer coordinates. The library reports failure through its result codes and, for
oversized coordinates, by returning empty output; the wrapper turns both into an explicit evaluation
failure.

## Reachability And Re-review

Only the path operations in `bloom_runtime` reach the library. This review must be renewed if the
library gains a file or stream input path in Bloom, if the pinned commit changes, or if an upstream
security advisory is published for the release.
