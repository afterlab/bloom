#include <bloom/core/test_fixed_memory_reading.hpp>

#include <cstdint>
#include <iostream>
#include <string_view>

namespace {

int failures = 0;

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

} // namespace

int main() {
    using bloom::core::parseTestFixedMemoryReading;
    const auto reading = parseTestFixedMemoryReading("17179869184,8589934592,2147483648");
    expect(reading.has_value() && reading->physicalBytes == (std::uint64_t{16} << 30U) &&
               reading->availableBytes == (std::uint64_t{8} << 30U) &&
               reading->deviceBytes == (std::uint64_t{2} << 30U),
           "three byte counts are the physical, available and device figures");
    for (const std::string_view bad :
         {"", "1", "1,1", "1,1,1,1", "1,,1", ",1,1", "1,1,", "0,0,1", "4,2,0", "2,4,1", "a,1,1",
          "4,2,1 ", " 4,2,1", "4,2,1x", "-4,2,1", "99999999999999999999999,1,1"})
        expect(!parseTestFixedMemoryReading(bad).has_value(), bad);
    // This test is registered with FIXED_MEMORY, so the process itself has the reading the
    // registration hands out, and it is the one the build declares.
    const auto& own = bloom::core::testFixedMemoryReading();
    const auto declared = parseTestFixedMemoryReading(BLOOM_TEST_FIXED_MEMORY_READING_VALUE);
    expect(own.has_value() && declared.has_value() &&
               own->physicalBytes == declared->physicalBytes &&
               own->availableBytes == declared->availableBytes &&
               own->deviceBytes == declared->deviceBytes,
           "a FIXED_MEMORY test's process reads the declared reading from its environment");
    return failures == 0 ? 0 : 1;
}
