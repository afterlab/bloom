#include <bloom/core/dither_pattern.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <utility>

namespace bloom::core::dither {
namespace {

[[nodiscard]] constexpr bool isPowerOfTwo(const int value) noexcept {
    return value > 0 && (value & (value - 1)) == 0;
}

// SplitMix64 (Steele, Lea and Flood, "Fast splittable pseudorandom number generators", 2014).
[[nodiscard]] constexpr std::uint64_t mix(std::uint64_t z) noexcept {
    z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31U);
}

// e^-x for x in [0, 40] from IEEE operations only: e^-(x / 64) by its Taylor series, then squared
// six times. Every operation is a correctly rounded binary64 addition, multiplication or division,
// so the value is the same on every platform (no library exp, no contraction in this target).
[[nodiscard]] double expNegative(const double x) noexcept {
    const double y = x / 64.0;
    double term = 1.0;
    double sum = 1.0;
    for (int k = 1; k <= 24; ++k) {
        term = term * (-y) / static_cast<double>(k);
        sum = sum + term;
    }
    for (int i = 0; i < 6; ++i)
        sum = sum * sum;
    return sum;
}

// The torus offsets with a weight above zero, each once, and their weights.
struct Kernel final {
    std::vector<int> dx;
    std::vector<int> dy;
    std::vector<std::int64_t> weight;
};

[[nodiscard]] Kernel kernelOf(const int side) {
    Kernel kernel;
    for (int dy = 0; dy < side; ++dy) {
        for (int dx = 0; dx < side; ++dx) {
            const auto w = voidAndClusterWeight(dx, dy, side);
            if (w == 0)
                continue;
            kernel.dx.push_back(dx);
            kernel.dy.push_back(dy);
            kernel.weight.push_back(w);
        }
    }
    return kernel;
}

// The energy of every cell: the sum of the weights of the set cells around it, kept as cells are set
// and cleared.
class Field final {
  public:
    explicit Field(const int side)
        : side_(side), mask_(side - 1), kernel_(kernelOf(side)),
          set_(static_cast<std::size_t>(side) * static_cast<std::size_t>(side), 0U),
          energy_(set_.size(), 0) {}

    [[nodiscard]] bool isSet(const std::size_t cell) const noexcept { return set_[cell] != 0U; }
    void toggle(const std::size_t cell, const bool on) {
        set_[cell] = on ? 1U : 0U;
        const int cx = static_cast<int>(cell) & mask_;
        const int cy = static_cast<int>(cell) / side_;
        for (std::size_t k = 0; k < kernel_.weight.size(); ++k) {
            const int x = (cx + kernel_.dx[k]) & mask_;
            const int y = (cy + kernel_.dy[k]) & mask_;
            auto& e = energy_[static_cast<std::size_t>(y * side_ + x)];
            e = on ? e + kernel_.weight[k] : e - kernel_.weight[k];
        }
    }
    // The set cell with the most energy (the tightest cluster); the first in row-major order on a tie.
    [[nodiscard]] std::size_t tightestCluster() const noexcept {
        std::size_t best = 0;
        auto most = std::numeric_limits<std::int64_t>::min();
        for (std::size_t i = 0; i < set_.size(); ++i)
            if (set_[i] != 0U && energy_[i] > most) {
                most = energy_[i];
                best = i;
            }
        return best;
    }
    // The clear cell with the least energy (the largest void); the first in row-major order on a tie.
    [[nodiscard]] std::size_t largestVoid() const noexcept {
        std::size_t best = 0;
        auto least = std::numeric_limits<std::int64_t>::max();
        for (std::size_t i = 0; i < set_.size(); ++i)
            if (set_[i] == 0U && energy_[i] < least) {
                least = energy_[i];
                best = i;
            }
        return best;
    }

  private:
    int side_;
    int mask_;
    Kernel kernel_;
    std::vector<std::uint8_t> set_;
    std::vector<std::int64_t> energy_;
};

} // namespace

std::vector<std::uint16_t> bayerMatrix(const int side) {
    if (!isPowerOfTwo(side) || side > 256)
        return {};
    std::vector<std::uint16_t> m{0};
    for (int k = 1; k < side; k *= 2) {
        const int next = 2 * k;
        std::vector<std::uint16_t> grown(static_cast<std::size_t>(next) * static_cast<std::size_t>(next));
        constexpr std::uint16_t kQuadrant[2][2]{{0, 2}, {3, 1}};
        for (int y = 0; y < next; ++y)
            for (int x = 0; x < next; ++x)
                grown[static_cast<std::size_t>(y * next + x)] = static_cast<std::uint16_t>(
                    4U * m[static_cast<std::size_t>((y % k) * k + (x % k))] +
                    kQuadrant[y / k][x / k]);
        m = std::move(grown);
    }
    return m;
}

std::int64_t voidAndClusterWeight(const int dx, const int dy, const int side) noexcept {
    if (side <= 0 || dx < 0 || dy < 0 || dx >= side || dy >= side)
        return 0;
    const int wx = dx <= side - dx ? dx : side - dx;
    const int wy = dy <= side - dy ? dy : side - dy;
    const auto squared = static_cast<double>(wx * wx + wy * wy);
    const double x = squared / (2.0 * kBlueNoiseSigma * kBlueNoiseSigma);
    if (x > 40.0)
        return 0;
    return std::llround(kBlueNoiseWeightScale * expNegative(x));
}

std::vector<std::uint16_t> voidAndCluster(const int side, const std::uint64_t seed) {
    if (!isPowerOfTwo(side) || side < 4 || side > 128)
        return {};
    const auto cells = static_cast<std::size_t>(side) * static_cast<std::size_t>(side);
    Field field(side);
    // The initial binary pattern: a tenth of the cells, placed by SplitMix64.
    const std::size_t initial = std::max<std::size_t>(cells / kBlueNoiseInitialDivisor, 1U);
    std::uint64_t state = seed;
    std::size_t placed = 0;
    while (placed < initial) {
        state += 0x9E3779B97F4A7C15ULL;
        const auto cell = static_cast<std::size_t>(mix(state) & (cells - 1U));
        if (field.isSet(cell))
            continue;
        field.toggle(cell, true);
        ++placed;
    }
    // Move the tightest cluster to the largest void until they are the same cell.
    for (std::size_t i = 0; i < cells; ++i) {
        const auto cluster = field.tightestCluster();
        field.toggle(cluster, false);
        const auto hole = field.largestVoid();
        field.toggle(hole, true);
        if (hole == cluster)
            break;
    }
    std::vector<std::uint16_t> rank(cells, 0);
    // Phase 1: the initial pattern's cells, the tightest cluster first removed and ranked highest.
    {
        Field removing = field;
        for (std::size_t r = initial; r-- > 0;) {
            const auto cluster = removing.tightestCluster();
            rank[cluster] = static_cast<std::uint16_t>(r);
            removing.toggle(cluster, false);
        }
    }
    // Phases 2 and 3: every other cell, the largest void first. Ulichney's third phase looks for the
    // tightest cluster of clear cells; with the same filter that is the clear cell of least energy, so
    // one rule ranks the rest.
    for (std::size_t r = initial; r < cells; ++r) {
        const auto hole = field.largestVoid();
        rank[hole] = static_cast<std::uint16_t>(r);
        field.toggle(hole, true);
    }
    return rank;
}

std::span<const std::uint16_t> blueNoiseTable() {
    static const std::vector<std::uint16_t> table = voidAndCluster(kBlueNoiseSide);
    return table;
}

Offset offset(const std::int64_t seed, const std::int64_t frame, const int channel,
              const int side) noexcept {
    if (!isPowerOfTwo(side))
        return {};
    std::uint64_t z = static_cast<std::uint64_t>(seed) * 0x9E3779B97F4A7C15ULL +
                      static_cast<std::uint64_t>(frame) * 0xBF58476D1CE4E5B9ULL +
                      static_cast<std::uint64_t>(channel) * 0x94D049BB133111EBULL +
                      0xD1B54A32D192ED03ULL;
    z = mix(mix(z));
    const auto mask = static_cast<std::uint64_t>(side - 1);
    const auto cells = static_cast<std::uint64_t>(side) * static_cast<std::uint64_t>(side);
    return {static_cast<std::uint32_t>(z & mask), static_cast<std::uint32_t>((z >> 16U) & mask),
            static_cast<std::uint32_t>((z >> 32U) & (cells - 1U))};
}

} // namespace bloom::core::dither
