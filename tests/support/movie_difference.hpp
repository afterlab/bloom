#pragma once

// Where two movie files differ, for a test's failure message: the first differing byte, the path of
// the ISO BMFF / QuickTime boxes that hold it (for example moov/trak/mdia/mdhd) and how many bytes
// differ in all. A test that expects two exports to be the same file says what is not the same.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace bloom::test_support {

namespace movie_difference_detail {

[[nodiscard]] inline std::uint64_t bigEndian(const std::string_view bytes, const std::size_t at,
                                             const std::size_t width) {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < width; ++i)
        value = (value << 8U) | static_cast<unsigned char>(bytes[at + i]);
    return value;
}

[[nodiscard]] inline bool isContainer(const std::string_view type) {
    for (const std::string_view container :
         {"moov", "trak", "mdia", "minf", "stbl", "edts", "dinf", "udta", "tref", "mvex", "moof",
          "traf", "gmhd", "tapt"})
        if (type == container)
            return true;
    return false;
}

// The box path holding byte `offset` among the boxes in [begin, end).
[[nodiscard]] inline std::string boxPath(const std::string_view bytes, std::size_t begin,
                                         const std::size_t end, const std::size_t offset) {
    while (begin + 8 <= end) {
        auto size = bigEndian(bytes, begin, 4);
        std::size_t header = 8;
        if (size == 1 && begin + 16 <= end) {
            size = bigEndian(bytes, begin + 8, 8);
            header = 16;
        } else if (size == 0) {
            size = end - begin;
        }
        if (size < header || size > end - begin)
            return "(unparsed at " + std::to_string(begin) + ")";
        const std::string type(bytes.substr(begin + 4, 4));
        const auto boxEnd = begin + static_cast<std::size_t>(size);
        if (offset < boxEnd) {
            if (isContainer(type) && offset >= begin + header)
                return type + "/" + boxPath(bytes, begin + header, boxEnd, offset);
            return type + " +" + std::to_string(offset - begin);
        }
        begin = boxEnd;
    }
    return "(after the last box)";
}

} // namespace movie_difference_detail

// Empty when the files are the same bytes.
[[nodiscard]] inline std::string movieDifference(const std::string_view a,
                                                 const std::string_view b) {
    if (a == b)
        return {};
    const auto common = std::min(a.size(), b.size());
    std::size_t first = common;
    std::size_t differing = std::max(a.size(), b.size()) - common;
    for (std::size_t i = 0; i < common; ++i)
        if (a[i] != b[i]) {
            first = std::min(first, i);
            ++differing;
        }
    std::string text = "sizes " + std::to_string(a.size()) + " and " + std::to_string(b.size()) +
                       "; first difference at byte " + std::to_string(first);
    if (first < common)
        text += " in " + movie_difference_detail::boxPath(a, 0, a.size(), first);
    return text + "; " + std::to_string(differing) + " bytes differ";
}

} // namespace bloom::test_support
