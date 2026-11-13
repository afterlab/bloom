#pragma once

// One wait helper for tests. A test that waits for asynchronous work names the condition it waits
// for; when the condition does not hold before the limit, the helper prints a failure naming it and
// counts it, so a wait can never time out unnoticed. The result is [[nodiscard]]: a test either
// asserts it or returns early on it.
//
//   if (!bloom::test_support::awaitCondition("import finishes", [&] { return !assets.busy(); }))
//       return 1;
//   ...
//   return failures + bloom::test_support::awaitFailureCount() == 0 ? 0 : 1;
//
// The limit is a ceiling for a broken run, never part of what the test proves: a passing run
// returns as soon as the condition holds. Qt tests use await_qt.hpp, which pumps the event loop.

#include <chrono>
#include <iostream>
#include <string_view>
#include <thread>
#include <utility>

namespace bloom::test_support {

inline constexpr std::chrono::milliseconds kAwaitLimit{30'000};

namespace detail {
inline int& awaitFailures() noexcept {
    static int count = 0;
    return count;
}
} // namespace detail

// Waits that timed out so far in this process. Add it to the test's failure count.
[[nodiscard]] inline int awaitFailureCount() noexcept { return detail::awaitFailures(); }

// Polls `predicate`, calling `pump` between polls, until it holds or `limit` passes.
template <typename Predicate, typename Pump>
[[nodiscard]] bool awaitConditionWith(const std::string_view name, Predicate&& predicate,
                                      Pump&& pump,
                                      const std::chrono::milliseconds limit = kAwaitLimit) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    for (;;) {
        if (predicate())
            return true;
        if (std::chrono::steady_clock::now() >= deadline)
            break;
        pump();
    }
    ++detail::awaitFailures();
    std::cerr << "FAIL: timed out after " << limit.count() << " ms awaiting: " << name << '\n';
    return false;
}

// Polls `predicate` from a thread with no event loop, yielding one millisecond between polls.
template <typename Predicate>
[[nodiscard]] bool awaitCondition(const std::string_view name, Predicate&& predicate,
                                  const std::chrono::milliseconds limit = kAwaitLimit) {
    return awaitConditionWith(
        name, std::forward<Predicate>(predicate),
        [] { std::this_thread::sleep_for(std::chrono::milliseconds{1}); }, limit);
}

} // namespace bloom::test_support
