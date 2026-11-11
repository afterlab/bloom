#pragma once

// Threads Bloom starts itself. std::thread and std::jthread take the operating system's default
// stack for a secondary thread, which differs widely between platforms (8 MiB on Linux, 512 KiB
// on macOS, the executable's reserve on Windows). Work that fits one platform's default overflows
// another's. Every thread started through this header gets the same stated stack size instead.
//
// WorkerThread mirrors std::thread and StoppableWorkerThread mirrors std::jthread for the members
// Bloom uses; production code starts threads through these two types only.

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <stop_token>
#include <tuple>
#include <type_traits>
#include <utility>

namespace bloom::core {

// The stack size of every thread Bloom starts: the main-thread stack on Linux and macOS, and the
// size Linux already gives a secondary thread by default.
inline constexpr std::size_t kWorkerThreadStackBytes = std::size_t{8} << 20;

// Runs `action` once when the calling thread exits, after its function has returned. It works on
// every thread, including the ones started through this header. The standard library's own
// at-thread-exit calls (std::promise::set_value_at_thread_exit, std::notify_all_at_thread_exit) do
// not: one standard library supports them only on threads std::thread started. Actions run in the
// reverse order of registration; one that throws ends the process. Throws when the registration
// cannot be stored.
void runAtThreadExit(std::function<void()> action);

namespace worker_thread_detail {
struct Entry {
    Entry() = default;
    Entry(const Entry&) = delete;
    Entry& operator=(const Entry&) = delete;
    virtual ~Entry() = default;
    virtual void run() = 0;
};

template <class Callable, class... Arguments> struct BoundEntry final : Entry {
    template <class F, class... A>
    explicit BoundEntry(F&& callable, A&&... arguments)
        : callable_(std::forward<F>(callable)), arguments_(std::forward<A>(arguments)...) {}
    void run() override {
        std::apply(
            [this](Arguments&... arguments) {
                std::invoke(std::move(callable_), std::move(arguments)...);
            },
            arguments_);
    }

  private:
    Callable callable_;
    std::tuple<Arguments...> arguments_;
};

// Starts a thread with kWorkerThreadStackBytes of stack that runs the entry once and destroys it.
// Returns the native handle; throws std::system_error when the thread cannot be created. An
// exception that leaves the entry ends the process, as it does for std::thread.
[[nodiscard]] std::uintptr_t start(std::unique_ptr<Entry> entry);
void join(std::uintptr_t handle);
void detach(std::uintptr_t handle) noexcept;
[[nodiscard]] bool isCurrent(std::uintptr_t handle) noexcept;
[[noreturn]] void terminateJoinable() noexcept;
} // namespace worker_thread_detail

class WorkerThread final {
  public:
    WorkerThread() noexcept = default;

    template <class F, class... Arguments>
        requires(!std::same_as<std::remove_cvref_t<F>, WorkerThread>)
    explicit WorkerThread(F&& callable, Arguments&&... arguments)
        : handle_(worker_thread_detail::start(
              std::make_unique<
                  worker_thread_detail::BoundEntry<std::decay_t<F>, std::decay_t<Arguments>...>>(
                  std::forward<F>(callable), std::forward<Arguments>(arguments)...))),
          joinable_(true) {}

    WorkerThread(const WorkerThread&) = delete;
    WorkerThread& operator=(const WorkerThread&) = delete;
    WorkerThread(WorkerThread&& other) noexcept
        : handle_(std::exchange(other.handle_, 0)),
          joinable_(std::exchange(other.joinable_, false)) {}
    WorkerThread& operator=(WorkerThread&& other) noexcept {
        if (joinable_)
            worker_thread_detail::terminateJoinable();
        handle_ = std::exchange(other.handle_, 0);
        joinable_ = std::exchange(other.joinable_, false);
        return *this;
    }
    ~WorkerThread() {
        if (joinable_)
            worker_thread_detail::terminateJoinable();
    }

    [[nodiscard]] bool joinable() const noexcept { return joinable_; }
    // Whether the calling thread is this thread: what comparing std::thread::get_id() with
    // std::this_thread::get_id() answers.
    [[nodiscard]] bool isCurrent() const noexcept {
        return joinable_ && worker_thread_detail::isCurrent(handle_);
    }
    void join() {
        worker_thread_detail::join(handle_);
        joinable_ = false;
        handle_ = 0;
    }
    void detach() noexcept {
        worker_thread_detail::detach(handle_);
        joinable_ = false;
        handle_ = 0;
    }

  private:
    std::uintptr_t handle_ = 0;
    bool joinable_ = false;
};

// Joins on destruction after requesting a stop, like std::jthread. A callable that accepts a
// std::stop_token as its first argument receives this thread's token.
class StoppableWorkerThread final {
  public:
    StoppableWorkerThread() noexcept : stop_(std::nostopstate) {}

    template <class F, class... Arguments>
        requires(!std::same_as<std::remove_cvref_t<F>, StoppableWorkerThread>)
    explicit StoppableWorkerThread(F&& callable, Arguments&&... arguments)
        : stop_(), thread_(launch(stop_.get_token(), std::forward<F>(callable),
                                  std::forward<Arguments>(arguments)...)) {}

    StoppableWorkerThread(const StoppableWorkerThread&) = delete;
    StoppableWorkerThread& operator=(const StoppableWorkerThread&) = delete;
    StoppableWorkerThread(StoppableWorkerThread&&) noexcept = default;
    StoppableWorkerThread& operator=(StoppableWorkerThread&& other) noexcept {
        if (this != &other) {
            stopAndJoin();
            stop_ = std::move(other.stop_);
            thread_ = std::move(other.thread_);
        }
        return *this;
    }
    ~StoppableWorkerThread() { stopAndJoin(); }

    [[nodiscard]] bool joinable() const noexcept { return thread_.joinable(); }
    [[nodiscard]] bool isCurrent() const noexcept { return thread_.isCurrent(); }
    void join() { thread_.join(); }
    void detach() noexcept { thread_.detach(); }
    bool request_stop() noexcept { return stop_.request_stop(); }
    [[nodiscard]] std::stop_source get_stop_source() noexcept { return stop_; }
    [[nodiscard]] std::stop_token get_stop_token() const noexcept { return stop_.get_token(); }

  private:
    template <class F, class... Arguments>
    static WorkerThread launch(std::stop_token token, F&& callable, Arguments&&... arguments) {
        if constexpr (std::invocable<std::decay_t<F>, std::stop_token,
                                     std::decay_t<Arguments>...>) {
            return WorkerThread(std::forward<F>(callable), std::move(token),
                                std::forward<Arguments>(arguments)...);
        } else {
            return WorkerThread(std::forward<F>(callable), std::forward<Arguments>(arguments)...);
        }
    }
    void stopAndJoin() noexcept {
        if (thread_.joinable()) {
            stop_.request_stop();
            thread_.join();
        }
    }

    std::stop_source stop_;
    WorkerThread thread_;
};

} // namespace bloom::core
