// #785: the movie, track and media header times of a finished movie are written as zero (as FFmpeg
// writes them), and no other byte changes. A generated file: ftyp, a 64-bit sized mdat, and a moov
// with a version 0 mvhd, a track whose tkhd is version 1 and whose mdia holds a version 0 mdhd and
// an hdlr, and a udta. A box that runs past the file is refused.

#include "movie_header_times.hpp"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace {

int failures = 0;

void expect(const bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

using Bytes = std::vector<unsigned char>;

void put(Bytes& out, const std::uint64_t value, const std::size_t width) {
    for (std::size_t i = width; i > 0; --i)
        out.push_back(static_cast<unsigned char>((value >> (8U * (i - 1))) & 0xFFU));
}

Bytes box(const char* type, const Bytes& body) {
    Bytes out;
    put(out, 8 + body.size(), 4);
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

Bytes largeBox(const char* type, const Bytes& body) {
    Bytes out;
    put(out, 1, 4);
    out.insert(out.end(), type, type + 4);
    put(out, 16 + body.size(), 8);
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

// A full box: version, flags, the creation and modification times, then `tail` bytes of 0x5A.
Bytes timedBox(const char* type, const unsigned version, const std::uint64_t created,
               const std::uint64_t modified, const std::size_t tail) {
    Bytes body{static_cast<unsigned char>(version), 0, 0, 0};
    const std::size_t width = version == 1 ? 8 : 4;
    put(body, created, width);
    put(body, modified, width);
    body.insert(body.end(), tail, 0x5A);
    return box(type, body);
}

Bytes join(std::initializer_list<Bytes> parts) {
    Bytes out;
    for (const auto& part : parts)
        out.insert(out.end(), part.begin(), part.end());
    return out;
}

Bytes readFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void writeFile(const std::filesystem::path& path, const Bytes& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
}

bool clear(const std::filesystem::path& path) {
    std::FILE* file = std::fopen(path.string().c_str(), "r+b");
    if (file == nullptr)
        return false;
    const bool cleared = bloom::media::container::clearMovieHeaderTimes(
        file, static_cast<std::uint64_t>(std::filesystem::file_size(path)));
    std::fclose(file);
    return cleared;
}

// The movie with its six header times made from `stamp` (all zero when it is zero).
Bytes movie(const std::uint64_t stamp) {
    const auto at = [stamp](const std::uint64_t index) { return stamp == 0 ? 0 : stamp + index; };
    const auto mvhd = timedBox("mvhd", 0, at(0), at(1), 88);
    const auto tkhd = timedBox("tkhd", 1, at(2) << 8U, at(3) << 8U, 72);
    const auto mdhd = timedBox("mdhd", 0, at(4), at(5), 8);
    const auto hdlr = box("hdlr", Bytes(25, 0x33));
    const auto trak = box("trak", join({tkhd, box("mdia", join({mdhd, hdlr}))}));
    const auto udta = box("udta", Bytes(12, 0x44));
    return join({box("ftyp", Bytes{'q', 't', ' ', ' ', 0, 0, 2, 0}),
                 largeBox("mdat", Bytes(40, 0x77)), box("moov", join({mvhd, trak, udta}))});
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: <scratch directory>\n";
        return 2;
    }
    const std::filesystem::path directory(argv[1]);
    std::filesystem::create_directories(directory);
    const auto path = directory / "times.mov";

    // Two writes of the same movie at different times differ only in their header times; cleared,
    // both are the movie with zero times, byte for byte.
    writeFile(path, movie(0xC0FFEE00ULL));
    expect(clear(path), "the header times of a written movie are cleared");
    const auto first = readFile(path);
    writeFile(path, movie(0x0BADF00DULL));
    expect(clear(path), "the header times of a second write are cleared");
    expect(readFile(path) == first, "two writes at different times are the same file once cleared");
    expect(first == movie(0), "every header time is zero and no other byte changed");

    // A movie whose times are zero already is left as it is.
    writeFile(path, movie(0));
    expect(clear(path) && readFile(path) == movie(0), "zero times stay zero");

    // A box that claims more bytes than the file holds is refused.
    auto truncated = movie(7);
    truncated.resize(truncated.size() - 10);
    writeFile(path, truncated);
    expect(!clear(path), "a movie whose boxes run past its end is refused");

    std::filesystem::remove_all(directory);
    if (failures == 0)
        std::cout << "movie header times passed\n";
    return failures == 0 ? 0 : 1;
}
