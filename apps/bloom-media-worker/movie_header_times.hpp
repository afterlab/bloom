#pragma once

// A movie's header times (#785). The movie, track and media headers of a QuickTime or ISO BMFF file
// (mvhd, tkhd, mdhd) each carry a creation and a modification time. FFmpeg writes both as zero; the
// macOS system writer (AVAssetWriter) writes the moment it finished, so two exports of the same
// frames and sound differed in those bytes alone. Bloom writes zero on every platform, so the same
// export is the same file wherever and whenever it is made.

#include <cstdint>
#include <cstdio>

namespace bloom::media::container {

// Sets every creation and modification time in the mvhd, tkhd and mdhd boxes of the finished
// file `file` (`size` bytes, opened for update) to zero. Changes no other byte. False when the
// file cannot be read or written, or its boxes do not parse.
[[nodiscard]] bool clearMovieHeaderTimes(std::FILE* file, std::uint64_t size);

} // namespace bloom::media::container
