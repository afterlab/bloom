#pragma once

// A single-writer, many-reader seqlock for a trivially copyable value (#423). The writer never
// waits and never allocates; a reader retries while a write is in progress and returns a value
// that one complete write produced. The payload lives in relaxed atomic words, so a reader racing
// a writer reads torn words it then discards, never a data race.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace bloom::runtime {

template <typename T> class ProfilerSeqlock final {
    static_assert(std::is_trivially_copyable_v<T>);

  public:
    // One writer at a time (the caller serializes writers).
    void store(const T& value) noexcept {
        const auto sequence = sequence_.load(std::memory_order_relaxed);
        sequence_.store(sequence + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        const auto* bytes = reinterpret_cast<const unsigned char*>(&value);
        for (std::size_t word = 0; word < kWords; ++word) {
            std::uint64_t chunk = 0;
            std::memcpy(&chunk, bytes + word * sizeof(chunk), bytesIn(word));
            words_[word].store(chunk, std::memory_order_relaxed);
        }
        sequence_.store(sequence + 2, std::memory_order_release);
    }

    // Copies the last complete write into `out`. Any thread; `out` holds the value on return.
    void load(T& out) const noexcept {
        auto* bytes = reinterpret_cast<unsigned char*>(&out);
        for (;;) {
            const auto before = sequence_.load(std::memory_order_acquire);
            if ((before & 1U) != 0)
                continue;
            for (std::size_t word = 0; word < kWords; ++word) {
                const auto chunk = words_[word].load(std::memory_order_relaxed);
                std::memcpy(bytes + word * sizeof(chunk), &chunk, bytesIn(word));
            }
            std::atomic_thread_fence(std::memory_order_acquire);
            if (sequence_.load(std::memory_order_relaxed) == before)
                return;
        }
    }

    // Completed writes so far.
    [[nodiscard]] std::uint64_t writes() const noexcept {
        return sequence_.load(std::memory_order_acquire) / 2;
    }

  private:
    static constexpr std::size_t kWords = (sizeof(T) + sizeof(std::uint64_t) - 1) / 8;
    [[nodiscard]] static constexpr std::size_t bytesIn(const std::size_t word) noexcept {
        const auto offset = word * sizeof(std::uint64_t);
        return sizeof(T) - offset < sizeof(std::uint64_t) ? sizeof(T) - offset
                                                          : sizeof(std::uint64_t);
    }

    std::atomic<std::uint64_t> sequence_{0};
    std::array<std::atomic<std::uint64_t>, kWords> words_{};
};

} // namespace bloom::runtime
