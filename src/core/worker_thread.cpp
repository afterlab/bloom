// See bloom/core/worker_thread.hpp.
#include <bloom/core/worker_thread.hpp>

#include <cstring>
#include <exception>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <process.h>
#include <windows.h>
#else
#include <pthread.h>
#endif

namespace bloom::core {
namespace {
struct ThreadExitActions final {
    ThreadExitActions() = default;
    ThreadExitActions(const ThreadExitActions&) = delete;
    ThreadExitActions& operator=(const ThreadExitActions&) = delete;
    ~ThreadExitActions() {
        for (auto action = actions.rbegin(); action != actions.rend(); ++action)
            (*action)();
    }
    std::vector<std::function<void()>> actions;
};
} // namespace

void runAtThreadExit(std::function<void()> action) {
    thread_local ThreadExitActions exitActions;
    exitActions.actions.push_back(std::move(action));
}
} // namespace bloom::core

namespace bloom::core::worker_thread_detail {
namespace {
// noexcept: an exception that leaves a thread's entry ends the process, as it does for std::thread.
void runEntry(void* raw) noexcept {
    const std::unique_ptr<Entry> entry(static_cast<Entry*>(raw));
    entry->run();
}

#if defined(_WIN32)
unsigned __stdcall nativeEntry(void* raw) {
    runEntry(raw);
    return 0;
}
#else
void* nativeEntry(void* raw) {
    runEntry(raw);
    return nullptr;
}

static_assert(sizeof(pthread_t) <= sizeof(std::uintptr_t));
std::uintptr_t toHandle(const pthread_t thread) noexcept {
    std::uintptr_t handle = 0;
    std::memcpy(&handle, &thread, sizeof(thread));
    return handle;
}
pthread_t fromHandle(const std::uintptr_t handle) noexcept {
    pthread_t thread{};
    std::memcpy(&thread, &handle, sizeof(thread));
    return thread;
}
#endif
} // namespace

std::uintptr_t start(std::unique_ptr<Entry> entry) {
#if defined(_WIN32)
    // STACK_SIZE_PARAM_IS_A_RESERVATION: the size is address space, committed as the stack grows.
    const auto handle =
        _beginthreadex(nullptr, static_cast<unsigned>(kWorkerThreadStackBytes), &nativeEntry,
                       entry.get(), STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (handle == 0)
        throw std::system_error(errno, std::generic_category(), "worker thread");
    static_cast<void>(entry.release());
    return handle;
#else
    pthread_attr_t attributes;
    if (const int error = pthread_attr_init(&attributes); error != 0)
        throw std::system_error(error, std::generic_category(), "worker thread attributes");
    int error = pthread_attr_setstacksize(&attributes, kWorkerThreadStackBytes);
    pthread_t thread{};
    if (error == 0)
        error = pthread_create(&thread, &attributes, &nativeEntry, entry.get());
    pthread_attr_destroy(&attributes);
    if (error != 0)
        throw std::system_error(error, std::generic_category(), "worker thread");
    static_cast<void>(entry.release());
    return toHandle(thread);
#endif
}

void join(const std::uintptr_t handle) {
#if defined(_WIN32)
    const auto native = reinterpret_cast<HANDLE>(handle);
    if (WaitForSingleObject(native, INFINITE) != WAIT_OBJECT_0)
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                "worker thread join");
    CloseHandle(native);
#else
    if (const int error = pthread_join(fromHandle(handle), nullptr); error != 0)
        throw std::system_error(error, std::generic_category(), "worker thread join");
#endif
}

void detach(const std::uintptr_t handle) noexcept {
#if defined(_WIN32)
    CloseHandle(reinterpret_cast<HANDLE>(handle));
#else
    static_cast<void>(pthread_detach(fromHandle(handle)));
#endif
}

bool isCurrent(const std::uintptr_t handle) noexcept {
#if defined(_WIN32)
    return GetThreadId(reinterpret_cast<HANDLE>(handle)) == GetCurrentThreadId();
#else
    return pthread_equal(pthread_self(), fromHandle(handle)) != 0;
#endif
}

void terminateJoinable() noexcept { std::terminate(); }

} // namespace bloom::core::worker_thread_detail
