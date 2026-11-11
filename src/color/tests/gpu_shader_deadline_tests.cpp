// The compile limit of the GPU shader compiler (#623): what it counts, what it does not, and its
// wall-clock ceiling. The waits are injected, so nothing here depends on how busy the machine is
// or on how long anything takes.

#include "gpu_shader_compiler_process.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>

namespace {

using bloom::color::gpu_shader_detail::ScheduledDeadline;
using namespace std::chrono_literals;

int failures = 0;
void check(const bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

struct Waits final {
    std::optional<std::chrono::nanoseconds> thread = 0ns;
    std::optional<std::chrono::nanoseconds> process = 0ns;
    std::int64_t asked = 0;
};

ScheduledDeadline::Sources sourcesOf(Waits& waits) {
    return {[&waits] { return waits.thread; },
            [&waits](const std::int64_t processId) {
                waits.asked = processId;
                return waits.process;
            }};
}

} // namespace

int main() {
    // Nothing waited: the limit is the limit.
    {
        Waits waits;
        ScheduledDeadline deadline(10s, 120s, sourcesOf(waits));
        deadline.refresh();
        check(deadline.current() == deadline.start() + 10s && deadline.waited() == 0ns,
              "with no waiting the limit ends one limit after the start");
        check(!deadline.expired(), "a limit that has not been reached is not expired");
    }
    // The thread's own waiting counts from the start of the compile, not from the thread's birth.
    {
        Waits waits;
        waits.thread = 7s;
        ScheduledDeadline deadline(10s, 120s, sourcesOf(waits));
        deadline.refresh();
        check(deadline.current() == deadline.start() + 10s,
              "waiting before the compile began is not added back");
        waits.thread = 9s;
        deadline.refresh();
        check(deadline.current() == deadline.start() + 12s && deadline.waited() == 2s,
              "the thread's waiting during the compile is added back");
    }
    // A tool's waiting is added back while it runs and kept after it ends; the next tool adds
    // its own.
    {
        Waits waits;
        ScheduledDeadline deadline(10s, 120s, sourcesOf(waits));
        deadline.processStarted(4242);
        waits.process = 3s;
        deadline.refresh();
        check(waits.asked == 4242 && deadline.current() == deadline.start() + 13s,
              "a running tool's waiting is added back");
        deadline.processFinished();
        waits.process = 0ns;
        deadline.refresh();
        check(deadline.current() == deadline.start() + 13s, "a finished tool's waiting is kept");
        deadline.processStarted(4243);
        waits.process = 1500ms;
        deadline.refresh();
        check(waits.asked == 4243 && deadline.current() == deadline.start() + 14500ms &&
                  deadline.waited() == 4500ms,
              "the next tool adds its own waiting");
        deadline.processFinished();
        waits.process = 60s;
        deadline.refresh();
        check(deadline.current() == deadline.start() + 14500ms,
              "a tool that has ended is not read again");
    }
    // The ceiling holds whatever is added back.
    {
        Waits waits;
        ScheduledDeadline deadline(10s, 120s, sourcesOf(waits));
        deadline.processStarted(7);
        waits.process = 3600s;
        waits.thread = 3600s;
        deadline.refresh();
        check(deadline.current() == deadline.start() + 120s,
              "no compile outlives the wall-clock ceiling");
    }
    // A tool that is blocked or asleep reports no waiting: the limit does not move, and a limit
    // already reached is expired. (A zero limit is rejected before any deadline exists; one
    // millisecond is the smallest a request can ask for.)
    {
        Waits waits;
        ScheduledDeadline deadline(1ms, 120s, sourcesOf(waits));
        deadline.processStarted(9);
        while (ScheduledDeadline::Clock::now() < deadline.current()) {
        }
        check(deadline.expired() && deadline.current() == deadline.start() + 1ms,
              "a hung tool ends at the limit");
    }
    // Without scheduler statistics the limit is plain wall-clock time.
    {
        Waits waits;
        waits.thread.reset();
        waits.process.reset();
        ScheduledDeadline deadline(10s, 120s, sourcesOf(waits));
        deadline.processStarted(11);
        deadline.refresh();
        check(deadline.current() == deadline.start() + 10s && deadline.waited() == 0ns,
              "where the system reports no waiting the limit is wall-clock time");
        ScheduledDeadline bare(10s, 120s, ScheduledDeadline::Sources{});
        bare.processStarted(12);
        bare.refresh();
        check(bare.current() == bare.start() + 10s, "no sources at all is wall-clock time too");
    }
    // The real sources: reading them never moves the limit backwards or past the ceiling.
    {
        ScheduledDeadline deadline(10s, 120s);
        deadline.refresh();
        check(deadline.current() >= deadline.start() + 10s &&
                  deadline.current() <= deadline.start() + 120s,
              "the system's own figures keep the limit between the limit and the ceiling");
    }
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
