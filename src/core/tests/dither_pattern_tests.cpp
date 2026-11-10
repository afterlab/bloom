// #527 Dither's patterns: the Bayer matrices against the bit formula, the void-and-cluster weights
// against the library exp, small void-and-cluster tables against an independent brute-force
// implementation of Ulichney's method, the 64 x 64 table's permutation, its pinned digest (the same
// bit for bit on every platform) and its blue-noise spectrum, and the placements per seed, frame and
// channel.

#include <bloom/core/dither_pattern.hpp>
#include <bloom/core/sha256.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <numbers>
#include <set>
#include <source_location>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace bloom::core::dither;

// The SHA-256 of voidAndCluster(64)'s ranks as little-endian 16-bit words.
constexpr std::string_view kPinnedDigest =
    "aaab7eaf250e7925a5a9f6f3742eab5695e625a05d7de076787dd2a64621aa0d";

class Expectations final {
  public:
    void expect(const bool condition, const std::string_view message,
                const std::source_location location = std::source_location::current()) {
        if (condition)
            return;
        ++failures_;
        std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
    }
    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

bool isPermutation(const std::vector<std::uint16_t>& ranks) {
    std::vector<std::uint16_t> sorted = ranks;
    std::ranges::sort(sorted);
    for (std::size_t i = 0; i < sorted.size(); ++i)
        if (sorted[i] != i)
            return false;
    return true;
}

// The Bayer rank by its bit formula: each bit of x and y, from the lowest, adds 2 (x ^ y) + y times
// 4 to the power of its distance from the highest.
int bayerFormula(const int x, const int y, const int side) {
    int bits = 0;
    while ((1 << bits) < side)
        ++bits;
    int rank = 0;
    for (int b = 0; b < bits; ++b) {
        const int xb = (x >> b) & 1;
        const int yb = (y >> b) & 1;
        rank += (2 * (xb ^ yb) + yb) << (2 * (bits - 1 - b));
    }
    return rank;
}

void testBayer(Expectations& e) {
    for (const int side : {1, 2, 4, 8, 16}) {
        const auto m = bayerMatrix(side);
        e.expect(m.size() == static_cast<std::size_t>(side * side) && isPermutation(m),
                 "a Bayer matrix holds every rank once");
        bool formula = true;
        for (int y = 0; y < side; ++y)
            for (int x = 0; x < side; ++x)
                formula = formula && m[static_cast<std::size_t>(y * side + x)] == bayerFormula(x, y, side);
        e.expect(formula, "the recursive matrix is the bit formula's");
    }
    const std::vector<std::uint16_t> four{0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
    e.expect(bayerMatrix(4) == four, "the 4 x 4 matrix is Bayer's");
    e.expect(bayerMatrix(3).empty() && bayerMatrix(512).empty(), "other sides give nothing");
    e.expect(isBayerSide(2) && isBayerSide(16) && !isBayerSide(1) && !isBayerSide(32),
             "the node offers 2, 4, 8 and 16");
}

std::int64_t libraryWeight(const int dx, const int dy, const int side) {
    const int wx = std::min(dx, side - dx);
    const int wy = std::min(dy, side - dy);
    const double x = static_cast<double>(wx * wx + wy * wy) / (2.0 * kBlueNoiseSigma * kBlueNoiseSigma);
    return x > 40.0 ? 0 : std::llround(kBlueNoiseWeightScale * std::exp(-x));
}

void testWeights(Expectations& e) {
    bool same = true;
    for (const int side : {4, 16, 64})
        for (int dy = 0; dy < side; ++dy)
            for (int dx = 0; dx < side; ++dx)
                same = same && voidAndClusterWeight(dx, dy, side) == libraryWeight(dx, dy, side);
    e.expect(same, "the integer weights are the Gaussian's, rounded, as the library exp gives it");
    e.expect(voidAndClusterWeight(0, 0, 64) == 1073741824, "the centre weighs 2^30");
    e.expect(voidAndClusterWeight(1, 0, 64) == voidAndClusterWeight(63, 0, 64) &&
                 voidAndClusterWeight(3, 5, 64) == voidAndClusterWeight(61, 59, 64),
             "the weights wrap the torus");
    e.expect(voidAndClusterWeight(32, 32, 64) == 0, "far cells weigh nothing");
}

// Ulichney's void-and-cluster written plainly: every energy recomputed from every set cell.
std::vector<std::uint16_t> bruteForce(const int side, const std::uint64_t seed) {
    const auto cells = static_cast<std::size_t>(side * side);
    std::vector<std::int64_t> w(cells);
    for (int dy = 0; dy < side; ++dy)
        for (int dx = 0; dx < side; ++dx)
            w[static_cast<std::size_t>(dy * side + dx)] = libraryWeight(dx, dy, side);
    const auto energy = [&](const std::vector<bool>& set, const std::size_t p) {
        const int px = static_cast<int>(p) % side;
        const int py = static_cast<int>(p) / side;
        std::int64_t sum = 0;
        for (std::size_t q = 0; q < cells; ++q) {
            if (!set[q])
                continue;
            const int dx = ((px - static_cast<int>(q) % side) % side + side) % side;
            const int dy = ((py - static_cast<int>(q) / side) % side + side) % side;
            sum += w[static_cast<std::size_t>(dy * side + dx)];
        }
        return sum;
    };
    const auto pick = [&](const std::vector<bool>& set, const bool wantSet, const bool most) {
        std::size_t best = cells;
        std::int64_t value = 0;
        for (std::size_t p = 0; p < cells; ++p) {
            if (set[p] != wantSet)
                continue;
            const auto en = energy(set, p);
            if (best == cells || (most ? en > value : en < value)) {
                best = p;
                value = en;
            }
        }
        return best;
    };
    std::vector<bool> set(cells, false);
    const std::size_t initial = std::max<std::size_t>(cells / 10, 1);
    std::uint64_t state = seed;
    std::size_t placed = 0;
    while (placed < initial) {
        state += 0x9E3779B97F4A7C15ULL;
        std::uint64_t z = state;
        z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
        z ^= z >> 31U;
        const auto cell = static_cast<std::size_t>(z % cells);
        if (!set[cell]) {
            set[cell] = true;
            ++placed;
        }
    }
    for (std::size_t i = 0; i < cells; ++i) {
        const auto cluster = pick(set, true, true);
        set[cluster] = false;
        const auto hole = pick(set, false, false);
        set[hole] = true;
        if (hole == cluster)
            break;
    }
    std::vector<std::uint16_t> rank(cells);
    auto removing = set;
    for (std::size_t r = initial; r-- > 0;) {
        const auto cluster = pick(removing, true, true);
        rank[cluster] = static_cast<std::uint16_t>(r);
        removing[cluster] = false;
    }
    for (std::size_t r = initial; r < cells; ++r) {
        const auto hole = pick(set, false, false);
        rank[hole] = static_cast<std::uint16_t>(r);
        set[hole] = true;
    }
    return rank;
}

void testVoidAndCluster(Expectations& e) {
    for (const int side : {4, 8, 16}) {
        const auto ranks = voidAndCluster(side);
        e.expect(isPermutation(ranks), "a void-and-cluster table holds every rank once");
        e.expect(ranks == bruteForce(side, 1), "the table is the brute-force method's, side " +
                                                   std::to_string(side));
    }
    e.expect(voidAndCluster(16, 2) != voidAndCluster(16, 1), "another initial pattern gives another table");
    e.expect(voidAndCluster(2).empty() && voidAndCluster(48).empty(), "other sides give nothing");
    const auto table = blueNoiseTable();
    const std::vector<std::uint16_t> ranks(table.begin(), table.end());
    e.expect(ranks.size() == 4096U && isPermutation(ranks), "the node's table is 64 x 64 ranks");
    e.expect(ranks == voidAndCluster(kBlueNoiseSide), "the node's table is voidAndCluster(64)");
    // The same table bit for bit wherever it is built: its little-endian bytes' SHA-256.
    std::vector<std::byte> bytes;
    for (const auto r : ranks) {
        bytes.push_back(static_cast<std::byte>(r & 0xFFU));
        bytes.push_back(static_cast<std::byte>(r >> 8U));
    }
    const auto digest = bloom::core::Sha256Hasher::hash(bytes);
    const auto hex = digest ? std::string(digest->toLowercaseHex().data(), 64) : std::string{};
    std::cout << "blue-noise table SHA-256 " << hex << '\n';
    e.expect(hex == kPinnedDigest, "the blue-noise table is the pinned one");
}

// The share of the thresholds' power below a radius of 8 cycles: white noise puts about
// pi 8^2 / 64^2 (5%) there, blue noise very little.
void testSpectrum(Expectations& e) {
    const auto table = blueNoiseTable();
    constexpr int n = kBlueNoiseSide;
    std::vector<double> t(table.size());
    for (std::size_t i = 0; i < t.size(); ++i)
        t[i] = (static_cast<double>(table[i]) + 0.5) / 4096.0 - 0.5;
    double low = 0.0;
    double total = 0.0;
    for (int v = -n / 2; v < n / 2; ++v)
        for (int u = -n / 2; u < n / 2; ++u) {
            if (u == 0 && v == 0)
                continue;
            std::complex<double> sum{};
            for (int y = 0; y < n; ++y)
                for (int x = 0; x < n; ++x) {
                    const double phase = -2.0 * std::numbers::pi * (u * x + v * y) / n;
                    sum += t[static_cast<std::size_t>(y * n + x)] *
                           std::complex<double>(std::cos(phase), std::sin(phase));
                }
            const double power = std::norm(sum);
            total += power;
            if (u * u + v * v < 64)
                low += power;
        }
    const double share = low / total;
    const double white = std::numbers::pi * 64.0 / 4096.0;
    std::cout << "blue noise: share of power below 8 cycles " << share << " (white noise about " << white
              << ")\n";
    e.expect(share < 0.1 * white, "the table's low frequencies hold under a tenth of white noise's");
}

void testOffsets(Expectations& e) {
    e.expect(offset(1, 0, 0, 64) == offset(1, 0, 0, 64), "a placement repeats");
    std::set<std::uint64_t> seen;
    bool ranged = true;
    for (std::int64_t seed = 0; seed < 8; ++seed)
        for (std::int64_t frame = 0; frame < 8; ++frame)
            for (int channel = 0; channel < 3; ++channel) {
                const auto o = offset(seed, frame, channel, 64);
                ranged = ranged && o.x < 64 && o.y < 64 && o.rank < 4096;
                seen.insert((std::uint64_t{o.x} << 32U) | (std::uint64_t{o.y} << 16U) | o.rank);
                const auto small = offset(seed, frame, channel, 2);
                ranged = ranged && small.x < 2 && small.y < 2 && small.rank < 4;
            }
    e.expect(ranged, "a placement stays inside the pattern");
    e.expect(seen.size() == 192U, "seeds, frames and channels each give their own placement");
    e.expect(offset(1, 0, 0, 3) == Offset{}, "a side that is not a power of two places nothing");
}

} // namespace

int main() {
    Expectations e;
    testBayer(e);
    testWeights(e);
    testVoidAndCluster(e);
    testSpectrum(e);
    testOffsets(e);
    return e.failures() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
