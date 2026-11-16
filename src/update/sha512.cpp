#include <bloom/update/sha512.hpp>

#include <algorithm>
#include <bit>
#include <cstring>

namespace bloom::update {
namespace {

constexpr std::array<std::uint64_t, 80> kRoundConstants{
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL,
    0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
    0xd807aa98a3030242ULL, 0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL,
    0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL,
    0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL, 0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
    0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
    0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
    0xd192e819d6ef5218ULL, 0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL,
    0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL,
    0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL,
    0xca273eceea26619cULL, 0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL,
    0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
    0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL,
    0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL,
};

[[nodiscard]] std::uint64_t bigEndianWord(const std::byte* bytes) noexcept {
    std::uint64_t word = 0;
    for (std::size_t index = 0; index < 8; ++index) {
        word = (word << 8U) | std::to_integer<std::uint64_t>(bytes[index]);
    }
    return word;
}

} // namespace

void Sha512::compress(const std::byte* block) noexcept {
    std::array<std::uint64_t, 80> schedule{};
    for (std::size_t index = 0; index < 16; ++index) {
        schedule[index] = bigEndianWord(block + index * 8);
    }
    for (std::size_t index = 16; index < 80; ++index) {
        const auto w15 = schedule[index - 15];
        const auto w2 = schedule[index - 2];
        const auto small0 = std::rotr(w15, 1) ^ std::rotr(w15, 8) ^ (w15 >> 7U);
        const auto small1 = std::rotr(w2, 19) ^ std::rotr(w2, 61) ^ (w2 >> 6U);
        schedule[index] = schedule[index - 16] + small0 + schedule[index - 7] + small1;
    }
    auto a = state_[0];
    auto b = state_[1];
    auto c = state_[2];
    auto d = state_[3];
    auto e = state_[4];
    auto f = state_[5];
    auto g = state_[6];
    auto h = state_[7];
    for (std::size_t index = 0; index < 80; ++index) {
        const auto big1 = std::rotr(e, 14) ^ std::rotr(e, 18) ^ std::rotr(e, 41);
        const auto choice = (e & f) ^ (~e & g);
        const auto first = h + big1 + choice + kRoundConstants[index] + schedule[index];
        const auto big0 = std::rotr(a, 28) ^ std::rotr(a, 34) ^ std::rotr(a, 39);
        const auto majority = (a & b) ^ (a & c) ^ (b & c);
        const auto second = big0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + first;
        d = c;
        c = b;
        b = a;
        a = first + second;
    }
    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
}

void Sha512::update(std::span<const std::byte> bytes) noexcept {
    if (bytes.empty()) {
        return;
    }
    totalBytes_ += bytes.size();
    if (bufferedCount_ != 0) {
        const auto take = std::min(bytes.size(), buffered_.size() - bufferedCount_);
        std::memcpy(buffered_.data() + bufferedCount_, bytes.data(), take);
        bufferedCount_ += take;
        bytes = bytes.subspan(take);
        if (bufferedCount_ < buffered_.size()) {
            return;
        }
        compress(buffered_.data());
        bufferedCount_ = 0;
    }
    while (bytes.size() >= buffered_.size()) {
        compress(bytes.data());
        bytes = bytes.subspan(buffered_.size());
    }
    if (!bytes.empty()) {
        std::memcpy(buffered_.data(), bytes.data(), bytes.size());
        bufferedCount_ = bytes.size();
    }
}

Sha512::Digest Sha512::finalize() const noexcept {
    Sha512 tail = *this;
    std::array<std::byte, 256> padding{};
    padding[0] = std::byte{0x80};
    // One 0x80 byte, zeros, then the length in bits as 128 bits, ending on a block boundary.
    const auto used = bufferedCount_;
    const auto padded = used < 112 ? 128 - used : 256 - used;
    const std::uint64_t highBits = totalBytes_ >> 61U;
    const std::uint64_t lowBits = totalBytes_ << 3U;
    for (std::size_t index = 0; index < 8; ++index) {
        padding[padded - 16 + index] = std::byte(static_cast<std::uint8_t>(highBits >> (56U - 8U * index)));
        padding[padded - 8 + index] = std::byte(static_cast<std::uint8_t>(lowBits >> (56U - 8U * index)));
    }
    tail.update(std::span<const std::byte>(padding.data(), padded));
    Digest digest{};
    for (std::size_t word = 0; word < 8; ++word) {
        for (std::size_t index = 0; index < 8; ++index) {
            digest[word * 8 + index] =
                static_cast<std::uint8_t>(tail.state_[word] >> (56U - 8U * index));
        }
    }
    return digest;
}

Sha512::Digest Sha512::hash(const std::span<const std::byte> bytes) noexcept {
    Sha512 hasher;
    hasher.update(bytes);
    return hasher.finalize();
}

} // namespace bloom::update
