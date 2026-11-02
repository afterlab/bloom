# Kvazaar 2.3.2 intake review

## Role and boundary

Kvazaar is Bloom's bundled HEVC software encoder. The superbuild compiles the unmodified
BSD-3-Clause release from its pinned archive on Linux, macOS and Windows (CMake, the adjacent
`kvazaar.configure-arguments` vector) into static, position-independent, library-only archives:
no command-line tool, no tests, no Crypto++ integration. FFmpeg's LGPL wrappers link them into the
worker's shared `libavcodec`; FFmpeg's version script keeps every Kvazaar symbol private.
Kvazaar's x86 kernels are compiler intrinsics with run-time CPU dispatch, so no assembler is
involved.

Kvazaar fixes its sample depth at compile time, so the same archive is built twice (owner
decision, issue #408): the 8-bit build behind FFmpeg's `libkvazaar` (HEVC Main) and a 10-bit build
(`KVZ_BIT_DEPTH=10`) behind `libkvazaar10` (HEVC Main 10, FFmpeg patch 0002). Both builds use the
same configure vector. The 10-bit build force-includes `kvazaar10-symbols.h`, which renames every
external symbol to a `kvz10_` prefix so the two archives link into one `libavcodec` without
resolving against each other; `kvazaar10-install.cmake` refuses an archive that still defines an
unprefixed symbol and installs it as `libkvazaar10`, `include/kvazaar10/` and `kvazaar10.pc`. The
renaming is a build definition, not a source change: the corresponding source is the same
archive.

## License

Kvazaar is BSD-3-Clause since 2.0 (`LICENSE`, byte-identical to the archive's copy). The compiled
library also contains Colin Plumb's public-domain MD5 (`src/extras/libmd5.c`). On MSVC only it
adds the upstream ISC-licensed thread wrapper (`src/threadwrapper/`, Tampere University) and the
BSD-2-Clause NetBSD `getopt` (`src/extras/getopt.c`); their notices are reproduced in
`THIRD_PARTY_NOTICES.md`. The "Greatest" test library (ISC) is not built. Because the archive is
linked into the LGPL `libavcodec`, the lock records the corresponding-source obligation with the
exact source archive digest.

## Patent posture

The owner's decision (2026-10-31) accepts bundling a source-built HEVC encoder; codec patent
questions outside ProRes are covered by Bloom's existing deployment warning. No patent license is
claimed.

## Qualification posture

Software HEVC is the FFmpeg worker's `hevc` encode declarations, Main served by `libkvazaar` and
Main 10 by `libkvazaar10`, and is the silent fallback when NVENC or VA-API hardware encode is
unavailable or refused. It is a development
qualification exercised by the encode/reopen round trip.
