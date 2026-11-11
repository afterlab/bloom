// bloom.core.worker_thread: every thread Bloom starts has the stated stack size on every platform,
// and the two thread types behave like std::thread and std::jthread for the members Bloom uses.
#include <bloom/core/worker_thread.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <future>
#include <iostream>
#include <memory>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>

namespace {
int failures = 0;
void expect(const bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAILED: " << message << '\n';
    }
}

// Touches `bytes` of stack in frames of a few kilobytes. The volatile writes and the use of the
// frame after the call keep the compiler from folding the recursion away.
std::uint64_t useStack(const std::size_t bytes) {
    volatile unsigned char frame[4096];
    for (std::size_t index = 0; index < sizeof(frame); index += 512)
        frame[index] = static_cast<unsigned char>(index / 512 + 1);
    const std::uint64_t below = bytes > sizeof(frame) ? useStack(bytes - sizeof(frame)) : 0;
    return below + frame[512];
}
} // namespace

int main() {
    using bloom::core::kWorkerThreadStackBytes;
    using bloom::core::StoppableWorkerThread;
    using bloom::core::WorkerThread;

    expect(kWorkerThreadStackBytes == (std::size_t{8} << 20), "the stated stack size is 8 MiB");

    {
        // Three quarters of the stated size: more than any platform's default for a secondary
        // thread except Linux's, so a thread started without the size would end the process here.
        std::atomic<std::uint64_t> sum{0};
        WorkerThread deep([&sum](const std::size_t bytes) { sum.store(useStack(bytes)); },
                          kWorkerThreadStackBytes / 4 * 3);
        expect(deep.joinable(), "a started thread is joinable");
        expect(!deep.isCurrent(), "the starting thread is not the started thread");
        deep.join();
        expect(!deep.joinable(), "a joined thread is not joinable");
        expect(sum.load() != 0, "a thread uses three quarters of the stated stack");
    }
    {
        WorkerThread empty;
        expect(!empty.joinable() && !empty.isCurrent(),
               "a default-constructed thread is not joinable and is no thread");
        std::atomic<bool> sawItself{false};
        std::atomic<const WorkerThread*> self{nullptr};
        WorkerThread asks([&sawItself, &self] {
            const WorkerThread* thread = nullptr;
            while ((thread = self.load()) == nullptr)
                std::this_thread::yield();
            sawItself.store(thread->isCurrent());
        });
        self.store(&asks);
        asks.join();
        expect(sawItself.load(), "a thread recognizes itself");
        std::atomic<bool> ran{false};
        auto owned = std::make_unique<int>(7);
        WorkerThread first([&ran, value = std::move(owned)] { ran.store(*value == 7); });
        WorkerThread second(std::move(first));
        expect(!first.joinable() && second.joinable(), // NOLINT(bugprone-use-after-move)
               "moving a thread moves its joinable state");
        empty = std::move(second);
        empty.join();
        expect(ran.load(), "a move-only callable runs once on the thread");
    }
    {
        std::atomic<bool> ran{false};
        WorkerThread detached([&ran] { ran.store(true); });
        detached.detach();
        expect(!detached.joinable(), "a detached thread is not joinable");
        while (!ran.load())
            std::this_thread::yield();
    }
    {
        std::atomic<bool> sawStop{false};
        {
            const StoppableWorkerThread waiter([&sawStop](const std::stop_token& stop) {
                while (!stop.stop_requested())
                    std::this_thread::yield();
                sawStop.store(true);
            });
        }
        expect(sawStop.load(), "destruction requests a stop and joins");
    }
    {
        std::atomic<int> value{0};
        StoppableWorkerThread plain([&value](const int amount) { value.store(amount); }, 3);
        plain.join();
        expect(value.load() == 3, "a callable without a stop token gets its arguments only");
        StoppableWorkerThread none;
        expect(!none.joinable() && !none.request_stop(),
               "a default-constructed stoppable thread has no thread and no stop state");
        std::atomic<bool> stopped{false};
        none = StoppableWorkerThread([&stopped](const std::stop_token& stop) {
            while (!stop.stop_requested())
                std::this_thread::yield();
            stopped.store(true);
        });
        expect(none.request_stop(), "a stop request reaches the running thread");
        none.join();
        expect(stopped.load(), "the thread ends after the stop request");
    }
    {
        // Thread-exit actions run after the thread's function, last registered first, on a
        // thread started here and on one the standard library started.
        for (const bool standard : {false, true}) {
            std::promise<int> first;
            std::promise<int> second;
            std::atomic<int> order{0};
            std::atomic<bool> bodyDone{false};
            const auto body = [&] {
                bloom::core::runAtThreadExit(
                    [&] { first.set_value(bodyDone.load() ? order.fetch_add(1) : -1); });
                bloom::core::runAtThreadExit(
                    [&] { second.set_value(bodyDone.load() ? order.fetch_add(1) : -1); });
                bodyDone.store(true);
            };
            if (standard) {
                std::thread thread(body);
                thread.join();
            } else {
                WorkerThread thread(body);
                thread.join();
            }
            expect(second.get_future().get() == 0 && first.get_future().get() == 1,
                   standard ? "thread-exit actions run in reverse order on a standard thread"
                            : "thread-exit actions run in reverse order on a worker thread");
        }
    }
    return failures == 0 ? 0 : 1;
}
