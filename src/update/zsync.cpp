#include <bloom/update/zsync.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstring>
#include <fstream>
#include <string_view>
#include <unordered_map>

namespace bloom::update {
namespace {

// The two digests below are the zsync format's own: MD4 names a block and SHA-1 names the whole
// file in the control file's header. Neither is trusted for anything. What is installed is
// decided by the SHA-256 and the Ed25519 signature of the signed feed alone.

[[nodiscard]] constexpr std::uint32_t rotateLeft(const std::uint32_t value,
                                                 const unsigned count) noexcept {
    return (value << count) | (value >> (32U - count));
}

// MD4 (RFC 1320) of one whole buffer.
[[nodiscard]] std::array<std::uint8_t, 16> md4(const std::span<const std::uint8_t> data) noexcept {
    std::uint32_t a = 0x67452301U;
    std::uint32_t b = 0xefcdab89U;
    std::uint32_t c = 0x98badcfeU;
    std::uint32_t d = 0x10325476U;
    const auto compress = [&](const std::uint8_t* block) {
        std::uint32_t x[16];
        for (std::size_t index = 0; index < 16; ++index) {
            x[index] = static_cast<std::uint32_t>(block[index * 4]) |
                       (static_cast<std::uint32_t>(block[index * 4 + 1]) << 8U) |
                       (static_cast<std::uint32_t>(block[index * 4 + 2]) << 16U) |
                       (static_cast<std::uint32_t>(block[index * 4 + 3]) << 24U);
        }
        std::uint32_t aa = a;
        std::uint32_t bb = b;
        std::uint32_t cc = c;
        std::uint32_t dd = d;
        const auto f = [](std::uint32_t p, std::uint32_t q, std::uint32_t r) {
            return (p & q) | (~p & r);
        };
        const auto g = [](std::uint32_t p, std::uint32_t q, std::uint32_t r) {
            return (p & q) | (p & r) | (q & r);
        };
        const auto h = [](std::uint32_t p, std::uint32_t q, std::uint32_t r) { return p ^ q ^ r; };
        static constexpr unsigned kShift1[4] = {3, 7, 11, 19};
        static constexpr unsigned kShift2[4] = {3, 5, 9, 13};
        static constexpr unsigned kShift3[4] = {3, 9, 11, 15};
        static constexpr std::size_t kOrder2[16] = {0, 4, 8,  12, 1, 5, 9,  13,
                                                    2, 6, 10, 14, 3, 7, 11, 15};
        static constexpr std::size_t kOrder3[16] = {0, 8, 4, 12, 2, 10, 6, 14,
                                                    1, 9, 5, 13, 3, 11, 7, 15};
        for (std::size_t index = 0; index < 16; ++index) {
            const std::uint32_t next = rotateLeft(aa + f(bb, cc, dd) + x[index], kShift1[index % 4]);
            aa = dd;
            dd = cc;
            cc = bb;
            bb = next;
        }
        for (std::size_t index = 0; index < 16; ++index) {
            const std::uint32_t next = rotateLeft(
                aa + g(bb, cc, dd) + x[kOrder2[index]] + 0x5a827999U, kShift2[index % 4]);
            aa = dd;
            dd = cc;
            cc = bb;
            bb = next;
        }
        for (std::size_t index = 0; index < 16; ++index) {
            const std::uint32_t next = rotateLeft(
                aa + h(bb, cc, dd) + x[kOrder3[index]] + 0x6ed9eba1U, kShift3[index % 4]);
            aa = dd;
            dd = cc;
            cc = bb;
            bb = next;
        }
        a += aa;
        b += bb;
        c += cc;
        d += dd;
    };
    std::size_t offset = 0;
    for (; offset + 64 <= data.size(); offset += 64) {
        compress(data.data() + offset);
    }
    std::array<std::uint8_t, 128> tail{};
    const std::size_t rest = data.size() - offset;
    if (rest != 0) {
        std::memcpy(tail.data(), data.data() + offset, rest);
    }
    tail[rest] = 0x80U;
    const std::size_t padded = rest < 56 ? 64 : 128;
    const std::uint64_t bits = static_cast<std::uint64_t>(data.size()) * 8U;
    for (std::size_t index = 0; index < 8; ++index) {
        tail[padded - 8 + index] = static_cast<std::uint8_t>(bits >> (8U * index));
    }
    compress(tail.data());
    if (padded == 128) {
        compress(tail.data() + 64);
    }
    std::array<std::uint8_t, 16> digest{};
    const std::uint32_t words[4] = {a, b, c, d};
    for (std::size_t index = 0; index < 4; ++index) {
        for (std::size_t byte = 0; byte < 4; ++byte) {
            digest[index * 4 + byte] = static_cast<std::uint8_t>(words[index] >> (8U * byte));
        }
    }
    return digest;
}

// SHA-1 (FIPS 180-4), fed in pieces.
class Sha1 final {
  public:
    void update(const std::span<const std::uint8_t> data) noexcept {
        for (const std::uint8_t byte : data) {
            block_[used_++] = byte;
            if (used_ == block_.size()) {
                compress();
                used_ = 0;
            }
        }
        length_ += data.size();
    }

    [[nodiscard]] std::array<std::uint8_t, 20> finish() noexcept {
        const std::uint64_t bits = length_ * 8U;
        block_[used_++] = 0x80U;
        if (used_ > 56) {
            std::fill(block_.begin() + static_cast<std::ptrdiff_t>(used_), block_.end(),
                      std::uint8_t{0});
            compress();
            used_ = 0;
        }
        std::fill(block_.begin() + static_cast<std::ptrdiff_t>(used_), block_.end(),
                  std::uint8_t{0});
        for (std::size_t index = 0; index < 8; ++index) {
            block_[56 + index] = static_cast<std::uint8_t>(bits >> (8U * (7U - index)));
        }
        compress();
        std::array<std::uint8_t, 20> digest{};
        for (std::size_t index = 0; index < 5; ++index) {
            for (std::size_t byte = 0; byte < 4; ++byte) {
                digest[index * 4 + byte] =
                    static_cast<std::uint8_t>(state_[index] >> (8U * (3U - byte)));
            }
        }
        return digest;
    }

  private:
    void compress() noexcept {
        std::uint32_t w[80];
        for (std::size_t index = 0; index < 16; ++index) {
            w[index] = (static_cast<std::uint32_t>(block_[index * 4]) << 24U) |
                       (static_cast<std::uint32_t>(block_[index * 4 + 1]) << 16U) |
                       (static_cast<std::uint32_t>(block_[index * 4 + 2]) << 8U) |
                       static_cast<std::uint32_t>(block_[index * 4 + 3]);
        }
        for (std::size_t index = 16; index < 80; ++index) {
            w[index] = rotateLeft(w[index - 3] ^ w[index - 8] ^ w[index - 14] ^ w[index - 16], 1);
        }
        std::uint32_t a = state_[0];
        std::uint32_t b = state_[1];
        std::uint32_t c = state_[2];
        std::uint32_t d = state_[3];
        std::uint32_t e = state_[4];
        for (std::size_t index = 0; index < 80; ++index) {
            std::uint32_t f = 0;
            std::uint32_t k = 0;
            if (index < 20) {
                f = (b & c) | (~b & d);
                k = 0x5a827999U;
            } else if (index < 40) {
                f = b ^ c ^ d;
                k = 0x6ed9eba1U;
            } else if (index < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8f1bbcdcU;
            } else {
                f = b ^ c ^ d;
                k = 0xca62c1d6U;
            }
            const std::uint32_t next = rotateLeft(a, 5) + f + e + k + w[index];
            e = d;
            d = c;
            c = rotateLeft(b, 30);
            b = a;
            a = next;
        }
        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
    }

    std::array<std::uint32_t, 5> state_{0x67452301U, 0xefcdab89U, 0x98badcfeU, 0x10325476U,
                                        0xc3d2e1f0U};
    std::array<std::uint8_t, 64> block_{};
    std::size_t used_ = 0;
    std::uint64_t length_ = 0;
};

// The format's rolling checksum of one block: `a` is the sum of the bytes and `b` the sum of the
// running sums, both in 16 bits.
struct Weak final {
    std::uint16_t a = 0;
    std::uint16_t b = 0;

    [[nodiscard]] std::uint32_t value() const noexcept {
        return (static_cast<std::uint32_t>(a) << 16U) | b;
    }
};

[[nodiscard]] Weak weakOf(const std::span<const std::uint8_t> block) noexcept {
    Weak sum;
    for (const std::uint8_t byte : block) {
        sum.a = static_cast<std::uint16_t>(sum.a + byte);
        sum.b = static_cast<std::uint16_t>(sum.b + sum.a);
    }
    return sum;
}

[[nodiscard]] std::uint32_t weakMask(const std::uint8_t weakBytes) noexcept {
    return weakBytes >= 4 ? 0xffffffffU : (1U << (8U * weakBytes)) - 1U;
}

constexpr std::uint32_t kMinimumBlockSize = 512;
constexpr std::uint32_t kMaximumBlockSize = 1U << 20U;
constexpr std::uint64_t kMaximumLength = 1ULL << 40U;

[[nodiscard]] std::optional<std::uint64_t> number(const std::string_view text) noexcept {
    std::uint64_t value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] std::string_view asText(const std::span<const std::byte> bytes) noexcept {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

void append(std::vector<std::byte>& out, const std::string_view text) {
    const auto* data = reinterpret_cast<const std::byte*>(text.data());
    out.insert(out.end(), data, data + text.size());
}

[[nodiscard]] std::string hex(const std::span<const std::uint8_t> bytes) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string text;
    text.reserve(bytes.size() * 2);
    for (const std::uint8_t byte : bytes) {
        text.push_back(kDigits[byte >> 4U]);
        text.push_back(kDigits[byte & 0x0fU]);
    }
    return text;
}

// "Mon, 16 Nov 2026 21:27:27 +0000", from seconds since 1970, whatever the system's language.
[[nodiscard]] std::string httpDate(const std::int64_t seconds) {
    static constexpr std::string_view kDays[] = {"Thu", "Fri", "Sat", "Sun", "Mon", "Tue", "Wed"};
    static constexpr std::string_view kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                                   "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    std::int64_t days = seconds / 86400;
    std::int64_t rest = seconds % 86400;
    if (rest < 0) {
        rest += 86400;
        --days;
    }
    const auto weekday = static_cast<std::size_t>(((days % 7) + 7) % 7);
    // The civil date of a day number (the algorithm of Howard Hinnant's `civil_from_days`).
    const std::int64_t z = days + 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const std::int64_t dayOfEra = z - era * 146097;
    const std::int64_t yearOfEra =
        (dayOfEra - dayOfEra / 1460 + dayOfEra / 36524 - dayOfEra / 146096) / 365;
    const std::int64_t dayOfYear = dayOfEra - (365 * yearOfEra + yearOfEra / 4 - yearOfEra / 100);
    const std::int64_t monthIndex = (5 * dayOfYear + 2) / 153;
    const std::int64_t day = dayOfYear - (153 * monthIndex + 2) / 5 + 1;
    const std::int64_t month = monthIndex < 10 ? monthIndex + 3 : monthIndex - 9;
    const std::int64_t year = yearOfEra + era * 400 + (month <= 2 ? 1 : 0);
    const auto two = [](const std::int64_t value) {
        std::string text = std::to_string(value);
        return text.size() < 2 ? "0" + text : text;
    };
    return std::string(kDays[weekday]) + ", " + two(day) + " " +
           std::string(kMonths[static_cast<std::size_t>(month - 1)]) + " " + std::to_string(year) +
           " " + two(rest / 3600) + ":" + two((rest / 60) % 60) + ":" + two(rest % 60) + " +0000";
}

void removeQuietly(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::remove(path, error);
}

[[nodiscard]] std::filesystem::path withSuffix(const std::filesystem::path& path,
                                               const std::string_view suffix) {
    auto result = path;
    result += suffix;
    return result;
}

// A local run shorter than this between two parts that are fetched anyway is fetched as well:
// fewer, larger requests.
constexpr std::uint64_t kMinimumLocalRunBlocks = 8;
// More parts than this is not worth the requests.
constexpr std::size_t kMaximumFetchedRuns = 64;

struct Run final {
    bool local = false;
    std::uint64_t firstBlock = 0;
    std::uint64_t blockCount = 0;
};

[[nodiscard]] std::vector<Run> planRuns(const ZsyncMatch& match) {
    std::vector<Run> runs;
    for (std::uint64_t block = 0; block < match.localOffsets.size(); ++block) {
        const bool local = match.localOffsets[block] >= 0;
        if (runs.empty() || runs.back().local != local) {
            runs.push_back({local, block, 0});
        }
        ++runs.back().blockCount;
    }
    // Short local runs between two fetched ones join them.
    std::vector<Run> merged;
    for (std::size_t index = 0; index < runs.size(); ++index) {
        Run run = runs[index];
        const bool between = index > 0 && index + 1 < runs.size();
        if (run.local && between && run.blockCount < kMinimumLocalRunBlocks) {
            run.local = false;
        }
        if (!merged.empty() && merged.back().local == run.local) {
            merged.back().blockCount += run.blockCount;
        } else {
            merged.push_back(run);
        }
    }
    return merged;
}

} // namespace

std::optional<ZsyncControl> readZsyncControl(const std::span<const std::byte> bytes) {
    if (bytes.size() > kMaximumZsyncControlBytes) {
        return std::nullopt;
    }
    const std::string_view all = asText(bytes);
    const auto headerEnd = all.find("\n\n");
    if (headerEnd == std::string_view::npos) {
        return std::nullopt;
    }
    ZsyncControl control;
    bool hasVersion = false;
    bool hasBlockSize = false;
    bool hasLength = false;
    bool hasLengths = false;
    std::string_view safe;
    std::string_view header = all.substr(0, headerEnd);
    while (!header.empty()) {
        const auto end = header.find('\n');
        const std::string_view line = header.substr(0, end);
        header = end == std::string_view::npos ? std::string_view{} : header.substr(end + 1);
        const auto colon = line.find(": ");
        if (colon == std::string_view::npos) {
            return std::nullopt;
        }
        const std::string_view name = line.substr(0, colon);
        const std::string_view value = line.substr(colon + 2);
        if (name == "zsync") {
            hasVersion = true;
        } else if (name == "Safe") {
            safe = value;
        } else if (name == "Blocksize") {
            const auto size = number(value);
            if (!size.has_value() || *size < kMinimumBlockSize || *size > kMaximumBlockSize ||
                (*size & (*size - 1)) != 0) {
                return std::nullopt;
            }
            control.blockSize = static_cast<std::uint32_t>(*size);
            hasBlockSize = true;
        } else if (name == "Length") {
            const auto length = number(value);
            if (!length.has_value() || *length == 0 || *length > kMaximumLength) {
                return std::nullopt;
            }
            control.length = *length;
            hasLength = true;
        } else if (name == "Hash-Lengths") {
            const auto first = value.find(',');
            const auto second = value.find(',', first == std::string_view::npos ? 0 : first + 1);
            if (first == std::string_view::npos || second == std::string_view::npos) {
                return std::nullopt;
            }
            const auto sequential = number(value.substr(0, first));
            const auto weak = number(value.substr(first + 1, second - first - 1));
            const auto strong = number(value.substr(second + 1));
            if (!sequential.has_value() || !weak.has_value() || !strong.has_value() ||
                *sequential < 1 || *sequential > 2 || *weak < 1 || *weak > 4 || *strong < 3 ||
                *strong > 16) {
                return std::nullopt;
            }
            control.sequentialMatches = static_cast<std::uint8_t>(*sequential);
            control.weakBytes = static_cast<std::uint8_t>(*weak);
            control.strongBytes = static_cast<std::uint8_t>(*strong);
            hasLengths = true;
        } else if (name == "Filename" || name == "MTime" || name == "URL" || name == "SHA-1") {
            // Not used: the feed names the file and its address, and its SHA-256 and signature
            // decide whether the result is accepted.
        } else if (name.starts_with("Z-") || name == "Recompress") {
            // The format's additions for compressed streams. A Bloom release has none.
            return std::nullopt;
        } else if (safe.find(name) == std::string_view::npos) {
            // A line this reader does not know and the writer did not mark as safe to skip.
            return std::nullopt;
        }
    }
    if (!hasVersion || !hasBlockSize || !hasLength || !hasLengths) {
        return std::nullopt;
    }
    const std::uint64_t blocks = (control.length + control.blockSize - 1) / control.blockSize;
    const std::size_t entry = static_cast<std::size_t>(control.weakBytes) + control.strongBytes;
    const std::string_view body = all.substr(headerEnd + 2);
    if (body.size() != blocks * entry) {
        return std::nullopt;
    }
    control.blocks.resize(static_cast<std::size_t>(blocks));
    const auto* data = reinterpret_cast<const std::uint8_t*>(body.data());
    for (std::size_t block = 0; block < control.blocks.size(); ++block) {
        const std::uint8_t* at = data + block * entry;
        std::uint32_t weak = 0;
        for (std::size_t index = 0; index < control.weakBytes; ++index) {
            weak = (weak << 8U) | at[index];
        }
        control.blocks[block].weak = weak;
        std::memcpy(control.blocks[block].strong.data(), at + control.weakBytes,
                    control.strongBytes);
    }
    return control;
}

std::optional<std::vector<std::byte>> makeZsyncControl(const std::filesystem::path& file,
                                                       const ZsyncControlHeader& header,
                                                       const std::function<bool()>& cancelled) {
    std::error_code error;
    const std::uint64_t length = std::filesystem::file_size(file, error);
    std::ifstream stream(file, std::ios::binary);
    if (error || !stream || length == 0 || length > kMaximumLength) {
        return std::nullopt;
    }
    // The reference tool's choices, so that both write the same file.
    const std::uint32_t blockSize = length < 100'000'000ULL ? 2048U : 4096U;
    const double lengthBits = std::log(static_cast<double>(length)) / std::log(2.0);
    const double blockBits = std::log(static_cast<double>(blockSize)) / std::log(2.0);
    const double blockCountBits =
        std::log(static_cast<double>(1 + length / blockSize)) / std::log(2.0);
    int sequential = 1;
    int weakBytes = static_cast<int>(std::ceil((lengthBits + blockBits - 8.6) / 8.0));
    if (weakBytes > 4) {
        sequential = 2;
        weakBytes = 4;
    }
    weakBytes = std::max(2, weakBytes);
    int strongBytes = static_cast<int>(
        std::max(std::ceil((20.0 + lengthBits + blockCountBits) / sequential / 8.0),
                 std::ceil((20.0 + blockCountBits) / 8.0)));
    strongBytes = std::min(16, std::max(4, strongBytes));

    std::vector<std::byte> sums;
    sums.reserve(static_cast<std::size_t>((length / blockSize + 1) *
                                          static_cast<std::uint64_t>(weakBytes + strongBytes)));
    Sha1 whole;
    std::vector<std::uint8_t> block(blockSize);
    std::uint64_t remaining = length;
    while (remaining != 0) {
        if (cancelled && cancelled()) {
            return std::nullopt;
        }
        const auto wanted = static_cast<std::size_t>(std::min<std::uint64_t>(blockSize, remaining));
        stream.read(reinterpret_cast<char*>(block.data()), static_cast<std::streamsize>(wanted));
        if (static_cast<std::size_t>(stream.gcount()) != wanted) {
            return std::nullopt;
        }
        whole.update(std::span(block.data(), wanted));
        // A short last block is filled with zeros for its checksums.
        std::fill(block.begin() + static_cast<std::ptrdiff_t>(wanted), block.end(),
                  std::uint8_t{0});
        const Weak weak = weakOf(block);
        const std::uint8_t stored[4] = {static_cast<std::uint8_t>(weak.a >> 8U),
                                        static_cast<std::uint8_t>(weak.a),
                                        static_cast<std::uint8_t>(weak.b >> 8U),
                                        static_cast<std::uint8_t>(weak.b)};
        for (int index = 4 - weakBytes; index < 4; ++index) {
            sums.push_back(static_cast<std::byte>(stored[index]));
        }
        const auto strong = md4(block);
        for (int index = 0; index < strongBytes; ++index) {
            sums.push_back(static_cast<std::byte>(strong[static_cast<std::size_t>(index)]));
        }
        remaining -= wanted;
    }
    const std::string digest = hex(whole.finish());
    std::vector<std::byte> out;
    append(out, "zsync: 0.6.6\n");
    append(out, "Safe: File-Hash,Strong-Hash-Algorithm\n");
    append(out, "Filename: " + header.fileName + "\n");
    append(out, "MTime: " + httpDate(header.modifiedSeconds) + "\n");
    append(out, "Blocksize: " + std::to_string(blockSize) + "\n");
    append(out, "Length: " + std::to_string(length) + "\n");
    append(out, "Hash-Lengths: " + std::to_string(sequential) + "," + std::to_string(weakBytes) +
                    "," + std::to_string(strongBytes) + "\n");
    append(out, "URL: " + header.fileName + "\n");
    append(out, "SHA-1: " + digest + "\n");
    append(out, "File-Hash: SHA-1:" + digest + "\n");
    append(out, "Strong-Hash-Algorithm: MD4\n");
    append(out, "\n");
    out.insert(out.end(), sums.begin(), sums.end());
    return out;
}

std::optional<ZsyncMatch> matchLocalBlocks(const ZsyncControl& control,
                                           const std::filesystem::path& local,
                                           const std::function<bool()>& cancelled) {
    std::error_code error;
    const std::uint64_t localLength = std::filesystem::file_size(local, error);
    std::ifstream stream(local, std::ios::binary);
    if (error || !stream || control.blockSize == 0) {
        return std::nullopt;
    }
    ZsyncMatch match;
    match.localOffsets.assign(control.blocks.size(), -1);
    const std::size_t blockSize = control.blockSize;
    if (localLength < blockSize || control.blocks.empty()) {
        return match;
    }

    // The new file's blocks by their weak checksum, with a small table in front that says "no"
    // for most positions without a lookup.
    const std::uint32_t mask = weakMask(control.weakBytes);
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> byWeak;
    byWeak.reserve(control.blocks.size());
    std::vector<bool> present(1U << 20U, false);
    for (std::size_t block = 0; block < control.blocks.size(); ++block) {
        byWeak[control.blocks[block].weak].push_back(static_cast<std::uint32_t>(block));
        present[control.blocks[block].weak & 0xfffffU] = true;
    }
    std::size_t missing = control.blocks.size();

    // A window of the local file that moves one byte at a time.
    constexpr std::size_t kChunk = 4U << 20U;
    std::vector<std::uint8_t> buffer(kChunk + blockSize);
    std::uint64_t base = 0;
    std::size_t have = 0;
    const auto fill = [&] {
        stream.read(reinterpret_cast<char*>(buffer.data() + have),
                    static_cast<std::streamsize>(buffer.size() - have));
        have += static_cast<std::size_t>(stream.gcount());
    };
    fill();
    if (have < blockSize) {
        return std::nullopt;
    }
    std::uint64_t position = 0;
    Weak weak = weakOf(std::span(buffer.data(), blockSize));
    while (true) {
        const std::size_t at = static_cast<std::size_t>(position - base);
        const std::uint32_t key = weak.value() & mask;
        if (present[key & 0xfffffU]) {
            if (const auto found = byWeak.find(key); found != byWeak.end()) {
                bool wanted = false;
                for (const std::uint32_t block : found->second) {
                    wanted = wanted || match.localOffsets[block] < 0;
                }
                if (wanted) {
                    const auto strong = md4(std::span(buffer.data() + at, blockSize));
                    for (const std::uint32_t block : found->second) {
                        if (match.localOffsets[block] < 0 &&
                            std::memcmp(strong.data(), control.blocks[block].strong.data(),
                                        control.strongBytes) == 0) {
                            match.localOffsets[block] = static_cast<std::int64_t>(position);
                            --missing;
                        }
                    }
                    if (missing == 0) {
                        break;
                    }
                }
            }
        }
        if (position + blockSize >= localLength) {
            break;
        }
        if (at + blockSize >= have) {
            // The window has reached the end of what is in memory: keep it and read on.
            if (cancelled && cancelled()) {
                return std::nullopt;
            }
            std::memmove(buffer.data(), buffer.data() + at, have - at);
            have -= at;
            base = position;
            fill();
            if (have <= blockSize) {
                break;
            }
            continue;
        }
        const std::uint8_t out = buffer[at];
        const std::uint8_t in = buffer[at + blockSize];
        weak.a = static_cast<std::uint16_t>(weak.a - out + in);
        weak.b = static_cast<std::uint16_t>(weak.b - static_cast<std::uint16_t>(blockSize * out) +
                                            weak.a);
        ++position;
    }
    for (std::size_t block = 0; block < control.blocks.size(); ++block) {
        if (match.localOffsets[block] >= 0) {
            match.reusedBytes += std::min<std::uint64_t>(
                blockSize, control.length - static_cast<std::uint64_t>(block) * blockSize);
        }
    }
    return match;
}

DownloadResult downloadWithDelta(UpdateTransport& transport, const FeedFile& target,
                                 const FeedFile& control, const std::filesystem::path& installedFile,
                                 const std::span<const Ed25519PublicKey> trustedKeys,
                                 const std::filesystem::path& destination,
                                 const std::function<bool()>& cancelled,
                                 const TransportProgress& progress, DeltaStatistics* statistics) {
    DeltaStatistics local;
    DeltaStatistics& stats = statistics != nullptr ? *statistics : local;
    stats = {};
    const auto isCancelled = [&cancelled] { return cancelled && cancelled(); };
    const auto cancelledResult = [] {
        DownloadResult result;
        result.transport.status = TransportStatus::Cancelled;
        result.diagnostic = {UpdateRefusal::Cancelled, "the download was cancelled"};
        return result;
    };
    const auto whole = [&](std::string reason) {
        stats.used = false;
        stats.reusedBytes = 0;
        stats.fetchedBytes = 0;
        stats.fallbackReason = std::move(reason);
        return downloadUpdateFile(transport, target, trustedKeys, destination, cancelled, progress);
    };

    // The control file is a file of the feed like any other: fetched and verified the same way.
    const auto controlPath = withSuffix(destination, ".zsync");
    const auto fetched =
        downloadUpdateFile(transport, control, trustedKeys, controlPath, cancelled, {});
    if (isCancelled()) {
        removeQuietly(controlPath);
        return cancelledResult();
    }
    if (!fetched.succeeded() || control.size > kMaximumZsyncControlBytes) {
        removeQuietly(controlPath);
        return whole("the control file could not be fetched or was refused");
    }
    std::vector<std::byte> controlBytes(static_cast<std::size_t>(control.size));
    {
        std::ifstream stream(controlPath, std::ios::binary);
        stream.read(reinterpret_cast<char*>(controlBytes.data()),
                    static_cast<std::streamsize>(controlBytes.size()));
        if (static_cast<std::size_t>(stream.gcount()) != controlBytes.size()) {
            controlBytes.clear();
        }
    }
    removeQuietly(controlPath);
    const auto parsed = readZsyncControl(controlBytes);
    if (!parsed.has_value() || parsed->length != target.size) {
        return whole("the control file does not describe the release file");
    }
    const auto match = matchLocalBlocks(*parsed, installedFile, cancelled);
    if (isCancelled()) {
        return cancelledResult();
    }
    if (!match.has_value()) {
        return whole("the installed file could not be read");
    }
    const auto runs = planRuns(*match);
    const auto fetchedRuns = static_cast<std::size_t>(
        std::count_if(runs.begin(), runs.end(), [](const Run& run) { return !run.local; }));
    if (match->reusedBytes < target.size / 10 || fetchedRuns > kMaximumFetchedRuns) {
        return whole("too little of the installed file is in the new one");
    }

    std::error_code error;
    std::filesystem::create_directories(destination.parent_path(), error);
    std::filesystem::remove(destination, error);
    const auto partial = withSuffix(destination, ".partial");
    const std::uint64_t blockSize = parsed->blockSize;
    std::uint64_t done = 0;
    std::string problem;
    bool stopped = false;
    {
        std::ofstream out(partial, std::ios::binary | std::ios::trunc);
        std::ifstream in(installedFile, std::ios::binary);
        std::vector<char> block(static_cast<std::size_t>(blockSize));
        if (!out || !in) {
            problem = "the download folder cannot be written";
        }
        for (const Run& run : runs) {
            if (!problem.empty() || stopped) {
                break;
            }
            if (isCancelled()) {
                stopped = true;
                break;
            }
            if (run.local) {
                for (std::uint64_t index = run.firstBlock; index < run.firstBlock + run.blockCount;
                     ++index) {
                    const auto count = static_cast<std::size_t>(
                        std::min<std::uint64_t>(blockSize, target.size - index * blockSize));
                    in.clear();
                    in.seekg(static_cast<std::streamoff>(
                        match->localOffsets[static_cast<std::size_t>(index)]));
                    in.read(block.data(), static_cast<std::streamsize>(count));
                    out.write(block.data(), static_cast<std::streamsize>(count));
                    if (static_cast<std::size_t>(in.gcount()) != count || !out) {
                        problem = "the installed file could not be read";
                        break;
                    }
                    done += count;
                    stats.reusedBytes += count;
                }
                if (progress) {
                    progress(done, target.size);
                }
                continue;
            }
            const std::uint64_t first = run.firstBlock * blockSize;
            const std::uint64_t end =
                std::min<std::uint64_t>((run.firstBlock + run.blockCount) * blockSize, target.size);
            TransportRequest request;
            request.url = target.url;
            request.maximumBytes = end - first;
            request.range = ByteRange{first, end - 1};
            std::uint64_t received = 0;
            const auto answer = transport.get(
                request,
                [&](const std::span<const std::byte> bytes) {
                    out.write(reinterpret_cast<const char*>(bytes.data()),
                              static_cast<std::streamsize>(bytes.size()));
                    received += bytes.size();
                    if (progress) {
                        progress(done + received, target.size);
                    }
                    return static_cast<bool>(out);
                },
                cancelled, {});
            if (answer.status == TransportStatus::Cancelled || isCancelled()) {
                stopped = true;
                break;
            }
            // A server that does not give parts answers with the whole file (or an error).
            if (answer.status != TransportStatus::Ok || !answer.partial ||
                received != end - first) {
                problem = "the server did not send the part that was asked for";
                break;
            }
            done += received;
            stats.fetchedBytes += received;
        }
        out.flush();
        if (problem.empty() && !stopped && !out) {
            problem = "the download could not be written to disk";
        }
    }
    if (stopped) {
        removeQuietly(partial);
        return cancelledResult();
    }
    if (!problem.empty()) {
        removeQuietly(partial);
        return whole(std::move(problem));
    }
    // The assembled file is checked as a downloaded one is. Only this decides.
    const auto verdict = verifyUpdateFile(partial, target, trustedKeys, cancelled);
    if (isCancelled()) {
        removeQuietly(partial);
        return cancelledResult();
    }
    if (!verdict.accepted()) {
        removeQuietly(partial);
        return whole("the assembled file is not the release file");
    }
    std::filesystem::rename(partial, destination, error);
    if (error) {
        removeQuietly(partial);
        return whole("the assembled file could not be put in place");
    }
    stats.used = true;
    DownloadResult result;
    result.transport.status = TransportStatus::Ok;
    result.file = destination;
    return result;
}

} // namespace bloom::update
