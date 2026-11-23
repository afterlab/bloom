#include "movie_header_times.hpp"

#include <array>
#include <cstring>
#include <optional>
#include <string_view>

namespace bloom::media::container {
namespace {

struct Box final {
    std::uint64_t offset = 0;
    std::uint64_t header = 8;
    std::uint64_t size = 0;
    std::array<char, 4> type{};
    [[nodiscard]] bool is(const std::string_view name) const {
        return std::string_view(type.data(), type.size()) == name;
    }
    [[nodiscard]] std::uint64_t body() const { return offset + header; }
    [[nodiscard]] std::uint64_t end() const { return offset + size; }
};

[[nodiscard]] std::uint64_t bigEndian(const unsigned char* bytes, const std::size_t width) {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < width; ++i)
        value = (value << 8U) | bytes[i];
    return value;
}

[[nodiscard]] bool seekTo(std::FILE* file, const std::uint64_t at) {
    if (at > static_cast<std::uint64_t>(INT64_MAX))
        return false;
#if defined(_WIN32)
    return _fseeki64(file, static_cast<__int64>(at), SEEK_SET) == 0;
#else
    return fseeko(file, static_cast<off_t>(at), SEEK_SET) == 0;
#endif
}

[[nodiscard]] bool readAt(std::FILE* file, const std::uint64_t at, unsigned char* into,
                          const std::size_t count) {
    return seekTo(file, at) && std::fread(into, 1, count, file) == count;
}

// The box at `at`, which must end by `limit`.
[[nodiscard]] std::optional<Box> readBox(std::FILE* file, const std::uint64_t at,
                                         const std::uint64_t limit) {
    if (at + 8 > limit)
        return std::nullopt;
    std::array<unsigned char, 16> head{};
    if (!readAt(file, at, head.data(), 8))
        return std::nullopt;
    Box box;
    box.offset = at;
    std::memcpy(box.type.data(), head.data() + 4, 4);
    std::uint64_t size = bigEndian(head.data(), 4);
    if (size == 1) {
        if (at + 16 > limit || !readAt(file, at + 8, head.data() + 8, 8))
            return std::nullopt;
        size = bigEndian(head.data() + 8, 8);
        box.header = 16;
    } else if (size == 0) {
        size = limit - at;
    }
    if (size < box.header || size > limit - at)
        return std::nullopt;
    box.size = size;
    return box;
}

// Zeroes the two times after the version and flags of a full box: 32-bit for version 0, 64-bit
// for version 1.
[[nodiscard]] bool clearTimes(std::FILE* file, const Box& box) {
    unsigned char version = 0;
    if (box.body() + 4 > box.end() || !readAt(file, box.body(), &version, 1))
        return false;
    const std::size_t width = version == 1 ? 8 : 4;
    if (version > 1 || box.body() + 4 + 2 * width > box.end())
        return false;
    const std::array<unsigned char, 16> zeros{};
    return seekTo(file, box.body() + 4) &&
           std::fwrite(zeros.data(), 1, 2 * width, file) == 2 * width;
}

[[nodiscard]] bool walk(std::FILE* file, std::uint64_t at, const std::uint64_t limit) {
    while (at < limit) {
        const auto box = readBox(file, at, limit);
        if (!box)
            return false;
        if (box->is("mvhd") || box->is("tkhd") || box->is("mdhd")) {
            if (!clearTimes(file, *box))
                return false;
        } else if (box->is("moov") || box->is("trak") || box->is("mdia")) {
            if (!walk(file, box->body(), box->end()))
                return false;
        }
        at = box->end();
    }
    return true;
}

} // namespace

bool clearMovieHeaderTimes(std::FILE* file, const std::uint64_t size) {
    return file != nullptr && walk(file, 0, size) && std::fflush(file) == 0;
}

} // namespace bloom::media::container
