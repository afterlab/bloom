#pragma once

// A read an operation factory makes when it is created (a media probe, an evaluation, a subtitle
// file), run as one task on a private scheduler so it gets a real runtime cancellation token, and
// waited for on the calling client thread (the script, MCP or CLI thread; never the UI thread).

#include <bloom/runtime/task_scheduler.hpp>

#include <chrono>
#include <memory>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>

namespace bloom::scripting::detail {

// What a cancellable read returned: the value, or that it was cancelled or could not start.
template <typename Value> struct ReadOutcome final {
    std::shared_ptr<Value> value;
    bool cancelled = false;
};

// Runs `work` as one task on a private one-worker scheduler, so the read gets a real runtime
// cancellation token, and waits for it on the calling client thread. `stop` cancels the task. The
// scheduler is retired only once quiescent, so no worker outlives what it borrowed.
template <typename Value, typename Work>
ReadOutcome<Value> runCancellable(const std::stop_token& stop, std::string name,
                                  const runtime::TaskExecutor executor, Work work) {
    if (stop.stop_requested())
        return {.value = nullptr, .cancelled = true};
    auto config = runtime::TaskSchedulerConfig::defaults();
    config.cpuWorkerCount = 1;
    config.blockingIoWorkerCount = 1;
    runtime::TaskScheduler scheduler(config);
    auto submission = scheduler.submit<std::shared_ptr<Value>>(
        runtime::TaskRequest(std::move(name),
                             {.kind = runtime::TaskOwnerKind::Application,
                              .id = runtime::TaskOwnerId::fromRaw(1)},
                             runtime::TaskPriority::Foreground, executor),
        [&work](runtime::TaskContext& context) {
            using Result = runtime::TaskResult<std::shared_ptr<Value>>;
            if (context.isCancellationRequested())
                return Result::cancelled();
            auto value = std::make_shared<Value>(work(context.cancellation()));
            if (context.isCancellationRequested())
                return Result::cancelled();
            return Result::succeeded(std::move(value));
        });
    ReadOutcome<Value> outcome;
    if (submission.accepted()) {
        const auto handle = submission.handle;
        const std::stop_callback onStop(stop, [handle] { handle.cancel(); });
        for (;;) {
            if (auto result = handle.tryTakeResult()) {
                if (result->state() == runtime::TaskState::Succeeded && result->value())
                    outcome.value = *result->value();
                outcome.cancelled = result->state() == runtime::TaskState::Cancelled;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    scheduler.beginShutdown();
    while (!scheduler.isQuiescent())
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (stop.stop_requested())
        outcome.cancelled = true;
    return outcome;
}

} // namespace bloom::scripting::detail
