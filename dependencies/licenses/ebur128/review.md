# libebur128 1.2.6 License And Feature Review

Reviewed: 2026-11-09

## License

`libebur128` is released under the MIT License (Copyright (c) 2011 Jan Kokemuller). Its bundled
`queue.h` is the BSD three-clause list header of the University of California. Both are compatible
with Bloom's Apache-2.0 distribution; the complete, unmodified texts are retained beside the sources
and in this record (`LICENSE`, `LICENSE-queue`), and the repository notices list the component.

## Build Boundary

The sources are unmodified. `ebur128.c` is compiled as C by one object-library target
(`bloom_media_ebur128`) with third-party warnings disabled; its optional speex resampler path is not
enabled, so true-peak measurement uses the library's own interpolator. Bloom's public audio headers
contain no `ebur128` types, macros, or pointers: `bloom/media/audio/loudness.hpp` wraps the state
behind a Bloom-owned class. No superbuild recipe, `FetchContent`, or lock entry is used.

## Feature Use

Bloom uses integrated loudness (`EBUR128_MODE_I`) and sample-peak/true-peak measurement
(`EBUR128_MODE_TRUE_PEAK`) over float interleaved frames, per ITU-R BS.1770-4 and EBU R128. Loudness
range, histogram and block-history modes are not used.
