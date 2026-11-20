# dav1d 1.5.4 intake review

## Role and boundary

dav1d is Bloom's bundled AV1 decoder. The superbuild compiles VideoLAN's signed BSD-2-Clause
release from its pinned archive, with the one reviewed patch below, on Linux, macOS and Windows (Meson, the adjacent
`dav1d.configure-arguments` vector) into a static, position-independent, library-only archive: no
tools, tests, examples or documentation. x86 assembly is built with the superbuild-private NASM;
ARM uses dav1d's own assembly. FFmpeg's LGPL `libdav1d` wrapper links the archive into the worker's
shared `libavcodec`, and FFmpeg's version script keeps every dav1d symbol private.

## Patch

`dependencies/patches/dav1d/0001-msvc-evaluate-has-include-next-only-under-clang.patch` changes
only `include/compat/msvc/stdatomic.h`, the header dav1d puts on the include path when it builds
with MSVC, whose C library has no `stdatomic.h`. The header chose clang-cl's own `stdatomic.h` with
`#if defined(__clang__) && __has_include_next(<stdatomic.h>)`. MSVC has no `__has_include_next`,
and from compiler version 19.52 its preprocessor rejects the expression even though
`defined(__clang__)` has already made it false (C7780, then fatal C1017), so the Windows build
stopped there. The patched header tests `__has_include_next` only in an `#if` nested inside
`#if defined(__clang__)`, which MSVC skips without evaluating, and selects the same branch as
before through a local macro. No compiled code changes: MSVC still takes the header's own
Interlocked-based atomics, and clang never reads this directory. Upstream fixed the same failure
after 1.5.4 with a fallback definition of `__has_include_next` (commit
`a34e0685b9111b9eb1d7835ea7bea5b425dced73`, released in 1.5.5). The patch is BSD-2-Clause like the
file it changes, is applied in the lock's recorded order and ships with the corresponding source.

## License

dav1d is BSD-2-Clause (`LICENSE`, byte-identical to the archive's `COPYING`); every compiled source
carries the same license. Because the archive is linked into the LGPL `libavcodec`, the lock
records the corresponding-source obligation with the exact source archive digest; the archive plus
the reviewed patch is that corresponding source.

## Qualification posture

AV1 decode is a preview/ingest decode declaration of the FFmpeg worker (`av1`, served by
`libdav1d`) for MP4, Matroska and WebM sources. It runs on the CPU in the worker; the decoded planes
enter Bloom's normal color pipeline.
