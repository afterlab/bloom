#pragma once

// Per-case times from a normal run of a test that has its own main. The test prints one line for
// each named part that took a quarter of a second or more, when the next part starts (and for the
// last one at exit):
//
//   CASE-TIME <milliseconds> <name>
//
// With BLOOM_TEST_CASE_TIMES set in the environment it prints every part. The times are for
// finding what a slow test spends its time on (`ctest -V -R <test>` shows them); nothing asserts
// on them.
//
//   BLOOM_TIMED_CASE(testBlurLowering(expectations));   // names the case after the call
//   bloom::test_support::caseMark("2 HDR and alpha");   // a part of one long function

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace bloom::test_support {

namespace detail {
struct CaseClock final {
    bool every = std::getenv("BLOOM_TEST_CASE_TIMES") != nullptr;
    std::string name;
    std::chrono::steady_clock::time_point start;
    void finish() {
        if (name.empty())
            return;
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);
        if (every || elapsed >= std::chrono::milliseconds{250})
            std::cout << "CASE-TIME " << elapsed.count() << ' ' << name << std::endl;
        name.clear();
    }
    CaseClock() = default;
    CaseClock(const CaseClock&) = delete;
    CaseClock& operator=(const CaseClock&) = delete;
    ~CaseClock() { finish(); }
};
inline CaseClock& caseClock() {
    static CaseClock clock;
    return clock;
}
} // namespace detail

// Ends the part that was running and starts `name`.
inline void caseMark(const std::string_view name) {
    auto& clock = detail::caseClock();
    clock.finish();
    clock.name = std::string(name);
    clock.start = std::chrono::steady_clock::now();
}

} // namespace bloom::test_support

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage): the macro names the case after the call's text.
#define BLOOM_TIMED_CASE(call)                                                                     \
    do {                                                                                           \
        ::bloom::test_support::caseMark(#call);                                                    \
        call;                                                                                      \
    } while (false)
