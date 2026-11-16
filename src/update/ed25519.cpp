#include <bloom/update/ed25519.hpp>

#include <algorithm>

// The arithmetic follows the public-domain TweetNaCl reference (verification half only): field
// elements of GF(2^255 - 19) are sixteen 16-bit limbs held in 64-bit integers, and points are in
// extended coordinates. C++20 defines shifts of negative values as arithmetic, which the carry
// steps rely on.
namespace bloom::update {
namespace {

using Field = std::array<std::int64_t, 16>;
using Point = std::array<Field, 4>;

constexpr Field kZero{};
constexpr Field kOne{1};
// d = -121665 / 121666, 2d, the base point, and the square root of -1.
constexpr Field kD{0x78a3, 0x1359, 0x4dca, 0x75eb, 0xd8ab, 0x4141, 0x0a4d, 0x0070,
                   0xe898, 0x7779, 0x4079, 0x8cc7, 0xfe73, 0x2b6f, 0x6cee, 0x5203};
constexpr Field kD2{0xf159, 0x26b2, 0x9b94, 0xebd6, 0xb156, 0x8283, 0x149a, 0x00e0,
                    0xd130, 0xeef3, 0x80f2, 0x198e, 0xfce7, 0x56df, 0xd9dc, 0x2406};
constexpr Field kBaseX{0xd51a, 0x8f25, 0x2d60, 0xc956, 0xa7b2, 0x9525, 0xc760, 0x692c,
                       0xdc5c, 0xfdd6, 0xe231, 0xc0a4, 0x53fe, 0xcd6e, 0x36d3, 0x2169};
constexpr Field kBaseY{0x6658, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666,
                       0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666};
constexpr Field kSqrtMinusOne{0xa0b0, 0x4a0e, 0x1b27, 0xc4ee, 0xe478, 0xad2f, 0x1806, 0x2f43,
                              0xd7a7, 0x3dfb, 0x0099, 0x2b4d, 0xdf0b, 0x4fc1, 0x2480, 0x2b83};
// The order of the base point, least significant byte first.
constexpr std::array<std::int64_t, 32> kOrder{
    0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10};

void carry(Field& value) noexcept {
    for (std::size_t index = 0; index < 16; ++index) {
        value[index] += std::int64_t{1} << 16;
        const std::int64_t out = value[index] >> 16;
        if (index < 15) {
            value[index + 1] += out - 1;
        } else {
            value[0] += 38 * (out - 1);
        }
        value[index] -= out << 16;
    }
}

// Swaps the two when `swap` is set.
void select(Field& first, Field& second, const bool swap) noexcept {
    if (swap) {
        std::swap(first, second);
    }
}

[[nodiscard]] std::array<std::uint8_t, 32> packField(const Field& value) noexcept {
    Field reduced = value;
    carry(reduced);
    carry(reduced);
    carry(reduced);
    for (int round = 0; round < 2; ++round) {
        Field minus{};
        minus[0] = reduced[0] - 0xffed;
        for (std::size_t index = 1; index < 15; ++index) {
            minus[index] = reduced[index] - 0xffff - ((minus[index - 1] >> 16) & 1);
            minus[index - 1] &= 0xffff;
        }
        minus[15] = reduced[15] - 0x7fff - ((minus[14] >> 16) & 1);
        const bool borrowed = ((minus[15] >> 16) & 1) != 0;
        minus[14] &= 0xffff;
        select(reduced, minus, !borrowed);
    }
    std::array<std::uint8_t, 32> bytes{};
    for (std::size_t index = 0; index < 16; ++index) {
        bytes[2 * index] = static_cast<std::uint8_t>(reduced[index] & 0xff);
        bytes[2 * index + 1] = static_cast<std::uint8_t>((reduced[index] >> 8) & 0xff);
    }
    return bytes;
}

[[nodiscard]] bool differ(const Field& first, const Field& second) noexcept {
    return packField(first) != packField(second);
}

[[nodiscard]] std::uint8_t parity(const Field& value) noexcept {
    return static_cast<std::uint8_t>(packField(value)[0] & 1U);
}

[[nodiscard]] Field unpackField(const std::span<const std::uint8_t, 32> bytes) noexcept {
    Field value{};
    for (std::size_t index = 0; index < 16; ++index) {
        value[index] = static_cast<std::int64_t>(bytes[2 * index]) +
                       (static_cast<std::int64_t>(bytes[2 * index + 1]) << 8);
    }
    value[15] &= 0x7fff;
    return value;
}

[[nodiscard]] Field add(const Field& first, const Field& second) noexcept {
    Field sum{};
    for (std::size_t index = 0; index < 16; ++index) {
        sum[index] = first[index] + second[index];
    }
    return sum;
}

[[nodiscard]] Field subtract(const Field& first, const Field& second) noexcept {
    Field difference{};
    for (std::size_t index = 0; index < 16; ++index) {
        difference[index] = first[index] - second[index];
    }
    return difference;
}

[[nodiscard]] Field multiply(const Field& first, const Field& second) noexcept {
    std::array<std::int64_t, 31> product{};
    for (std::size_t i = 0; i < 16; ++i) {
        for (std::size_t j = 0; j < 16; ++j) {
            product[i + j] += first[i] * second[j];
        }
    }
    for (std::size_t index = 0; index < 15; ++index) {
        product[index] += 38 * product[index + 16];
    }
    Field result{};
    std::copy_n(product.begin(), 16, result.begin());
    carry(result);
    carry(result);
    return result;
}

[[nodiscard]] Field square(const Field& value) noexcept { return multiply(value, value); }

[[nodiscard]] Field invert(const Field& value) noexcept {
    Field result = value;
    for (int bit = 253; bit >= 0; --bit) {
        result = square(result);
        if (bit != 2 && bit != 4) {
            result = multiply(result, value);
        }
    }
    return result;
}

// value ^ ((p - 5) / 8)
[[nodiscard]] Field power2523(const Field& value) noexcept {
    Field result = value;
    for (int bit = 250; bit >= 0; --bit) {
        result = square(result);
        if (bit != 1) {
            result = multiply(result, value);
        }
    }
    return result;
}

void addPoint(Point& p, const Point& q) noexcept {
    const Field a = multiply(subtract(p[1], p[0]), subtract(q[1], q[0]));
    const Field b = multiply(add(p[0], p[1]), add(q[0], q[1]));
    const Field c = multiply(multiply(p[3], q[3]), kD2);
    Field d = multiply(p[2], q[2]);
    d = add(d, d);
    const Field e = subtract(b, a);
    const Field f = subtract(d, c);
    const Field g = add(d, c);
    const Field h = add(b, a);
    p[0] = multiply(e, f);
    p[1] = multiply(h, g);
    p[2] = multiply(g, f);
    p[3] = multiply(e, h);
}

void swapPoints(Point& p, Point& q, const bool swap) noexcept {
    for (std::size_t index = 0; index < 4; ++index) {
        select(p[index], q[index], swap);
    }
}

[[nodiscard]] std::array<std::uint8_t, 32> packPoint(const Point& p) noexcept {
    const Field inverse = invert(p[2]);
    const Field x = multiply(p[0], inverse);
    const Field y = multiply(p[1], inverse);
    auto bytes = packField(y);
    bytes[31] = static_cast<std::uint8_t>(bytes[31] ^ (parity(x) << 7U));
    return bytes;
}

// scalar * q, the scalar as 32 bytes, least significant first. `q` is used up.
[[nodiscard]] Point scalarMultiply(Point q, const std::span<const std::uint8_t, 32> scalar) noexcept {
    Point p{kZero, kOne, kOne, kZero};
    for (int bit = 255; bit >= 0; --bit) {
        const auto index = static_cast<std::size_t>(bit);
        const bool set = ((scalar[index / 8] >> (index & 7U)) & 1U) != 0;
        swapPoints(p, q, set);
        addPoint(q, p);
        const Point doubled = p;
        addPoint(p, doubled);
        swapPoints(p, q, set);
    }
    return p;
}

[[nodiscard]] Point scalarBase(const std::span<const std::uint8_t, 32> scalar) noexcept {
    const Point base{kBaseX, kBaseY, kOne, multiply(kBaseX, kBaseY)};
    return scalarMultiply(base, scalar);
}

// A 64-byte value modulo the order of the base point, as 32 bytes.
[[nodiscard]] std::array<std::uint8_t, 32>
reduceModuloOrder(const std::span<const std::uint8_t, 64> bytes) noexcept {
    std::array<std::int64_t, 64> x{};
    for (std::size_t index = 0; index < 64; ++index) {
        x[index] = bytes[index];
    }
    for (std::size_t i = 63; i >= 32; --i) {
        std::int64_t carried = 0;
        std::size_t j = i - 32;
        for (; j < i - 12; ++j) {
            x[j] += carried - 16 * x[i] * kOrder[j - (i - 32)];
            carried = (x[j] + 128) >> 8;
            x[j] -= carried << 8;
        }
        x[j] += carried;
        x[i] = 0;
    }
    std::int64_t carried = 0;
    for (std::size_t j = 0; j < 32; ++j) {
        x[j] += carried - (x[31] >> 4) * kOrder[j];
        carried = x[j] >> 8;
        x[j] &= 255;
    }
    for (std::size_t j = 0; j < 32; ++j) {
        x[j] -= carried * kOrder[j];
    }
    std::array<std::uint8_t, 32> reduced{};
    for (std::size_t index = 0; index < 32; ++index) {
        x[index + 1] += x[index] >> 8;
        reduced[index] = static_cast<std::uint8_t>(x[index] & 255);
    }
    return reduced;
}

// The point a public key names, negated; nothing when the bytes name no point of the curve.
[[nodiscard]] bool unpackNegated(Point& r, const Ed25519PublicKey& key) noexcept {
    r[2] = kOne;
    r[1] = unpackField(std::span<const std::uint8_t, 32>(key));
    Field numerator = square(r[1]);
    Field denominator = multiply(numerator, kD);
    numerator = subtract(numerator, r[2]);
    denominator = add(r[2], denominator);

    const Field denominator2 = square(denominator);
    const Field denominator4 = square(denominator2);
    const Field denominator6 = multiply(denominator4, denominator2);
    Field t = multiply(denominator6, numerator);
    t = multiply(t, denominator);
    t = power2523(t);
    t = multiply(t, numerator);
    t = multiply(t, denominator);
    t = multiply(t, denominator);
    r[0] = multiply(t, denominator);

    Field check = multiply(square(r[0]), denominator);
    if (differ(check, numerator)) {
        r[0] = multiply(r[0], kSqrtMinusOne);
    }
    check = multiply(square(r[0]), denominator);
    if (differ(check, numerator)) {
        return false;
    }
    if (parity(r[0]) == (key[31] >> 7U)) {
        r[0] = subtract(kZero, r[0]);
    }
    r[3] = multiply(r[0], r[1]);
    return true;
}

// RFC 8032 section 5.1.7: the second half of a signature is a number below the order. A larger
// one would be a second encoding of an accepted signature.
[[nodiscard]] bool belowOrder(const std::span<const std::uint8_t, 32> scalar) noexcept {
    for (std::size_t index = 32; index-- > 0;) {
        const auto limit = static_cast<std::uint8_t>(kOrder[index]);
        if (scalar[index] != limit) {
            return scalar[index] < limit;
        }
    }
    return false;
}

} // namespace

Ed25519Verifier::Ed25519Verifier(const Ed25519PublicKey& publicKey,
                                 const Ed25519Signature& signature) noexcept
    : publicKey_(publicKey), signature_(signature) {
    // The hash of R, the public key and the message, in that order.
    hash_.update(std::as_bytes(std::span(signature_).first(32)));
    hash_.update(std::as_bytes(std::span(publicKey_)));
}

void Ed25519Verifier::update(const std::span<const std::byte> message) noexcept {
    hash_.update(message);
}

bool Ed25519Verifier::finish() const noexcept {
    const auto scalar = std::span<const std::uint8_t, 64>(signature_).last<32>();
    if (!belowOrder(scalar)) {
        return false;
    }
    Point negatedKey{};
    if (!unpackNegated(negatedKey, publicKey_)) {
        return false;
    }
    const auto digest = hash_.finalize();
    const auto challenge = reduceModuloOrder(std::span<const std::uint8_t, 64>(digest));
    // S * B - h * A must be R.
    Point result = scalarMultiply(negatedKey, std::span<const std::uint8_t, 32>(challenge));
    addPoint(result, scalarBase(scalar));
    const auto encoded = packPoint(result);
    return std::equal(encoded.begin(), encoded.end(), signature_.begin());
}

bool verifyEd25519(const Ed25519PublicKey& publicKey, const std::span<const std::byte> message,
                   const Ed25519Signature& signature) noexcept {
    Ed25519Verifier verifier(publicKey, signature);
    verifier.update(message);
    return verifier.finish();
}

} // namespace bloom::update
