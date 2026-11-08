# libebur128 1.2.6 Security Review

Reviewed: 2026-11-09

## Disposition

The library measures samples Bloom has already decoded and mixed in process; it opens no file, parses
no container, and makes no network or system call. Its inputs are a sample rate, a channel count,
and float frames Bloom produced, validated by the Bloom wrapper before the first call (rate and
channel count within the export audio limits; frame counts bounded by the caller's buffer). The
library allocates its own history buffers (a few seconds of audio per channel) and reports
allocation failure through its return codes, which the wrapper turns into an explicit error.

## Reachability And Re-review

Only `bloom/media/audio/loudness.cpp` reaches the library. This review must be renewed if the
library gains a file or stream input path in Bloom, if the pinned commit changes, or if an upstream
security advisory affects v1.2.6.
