#pragma once

// A fixed memory reading for tests that start Bloom's own programs (bloom-mcp, bloom-cli, the
// Python module). A test that admits work against the machine's free memory passes or fails with
// whatever else is running, and such a test cannot hand the program a sampler the way an
// in-process test does. So the program reads one environment variable, once, at its first
// sample:
//
//   BLOOM_TEST_FIXED_MEMORY_READING=<physical bytes>,<available bytes>,<device bytes>
//
// When it is set, the host memory sample is that physical and available memory with no process
// limit, and every GPU device is created as a card of the declared device size
// (render::GpuDeviceCreationOptions::device_memory_limit_bytes). When it is not set, which is
// every run outside the test suite, nothing changes. A value that does not parse is reported once
// on stderr and ignored. Test-only: nothing in the product sets it and no user setting maps to it.

#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string_view>

namespace bloom::core {

inline constexpr const char* kTestFixedMemoryReadingVariable = "BLOOM_TEST_FIXED_MEMORY_READING";

struct TestFixedMemoryReading final {
    std::uint64_t physicalBytes = 0;
    std::uint64_t availableBytes = 0;
    std::uint64_t deviceBytes = 0;
};

// "<physical>,<available>,<device>": three positive decimal byte counts, available no larger
// than physical.
[[nodiscard]] inline std::optional<TestFixedMemoryReading>
parseTestFixedMemoryReading(std::string_view text) noexcept {
    TestFixedMemoryReading reading;
    std::uint64_t* const fields[] = {&reading.physicalBytes, &reading.availableBytes,
                                     &reading.deviceBytes};
    for (std::size_t index = 0; index < 3; ++index) {
        const auto comma = text.find(',');
        const auto field = text.substr(0, comma);
        if ((comma == std::string_view::npos) != (index == 2) || field.empty())
            return std::nullopt;
        const auto* const end = field.data() + field.size();
        const auto parsed = std::from_chars(field.data(), end, *fields[index]);
        if (parsed.ec != std::errc{} || parsed.ptr != end || *fields[index] == 0)
            return std::nullopt;
        text = comma == std::string_view::npos ? std::string_view{} : text.substr(comma + 1);
    }
    if (reading.availableBytes > reading.physicalBytes)
        return std::nullopt;
    return reading;
}

// The process's reading, read from the environment on the first call and never again.
[[nodiscard]] inline const std::optional<TestFixedMemoryReading>&
testFixedMemoryReading() noexcept {
    static const std::optional<TestFixedMemoryReading> reading =
        []() noexcept -> std::optional<TestFixedMemoryReading> {
        const char* const value =
            std::getenv(kTestFixedMemoryReadingVariable); // NOLINT(concurrency-mt-unsafe)
        if (value == nullptr || *value == '\0')
            return std::nullopt;
        auto parsed = parseTestFixedMemoryReading(value);
        if (!parsed)
            std::fprintf(stderr,
                         "%s is set but is not <physical>,<available>,<device> in bytes; ignored\n",
                         kTestFixedMemoryReadingVariable);
        return parsed;
    }();
    return reading;
}

} // namespace bloom::core
