// A program that starts the crash reporter the way Bloom's programs do and then crashes on
// purpose (issue #736). The crash tests run it as a child process (crash_test.cmake) and check
// what the handler wrote with bloom_crash_check.
//
//   bloom_crash_child <mode> <crash folder>
//
//   null-write     writes through a null pointer on the main thread
//   worker-thread  writes through a null pointer on a second thread
//   abort          calls std::abort()
//   stack-overflow recurses without end on a second thread until its stack runs out
//   clean          exits normally (the prepared text report is removed)
//
// Before crashing it writes log lines to standard error, which the reporter copies into the text
// report, naming the thread that is about to crash so the check can find it in the minidump.
// #756: it also names, in the file BLOOM_TEST_CRASH_THREADS gives, the threads it knows are alive
// at the crash (the main thread, a held thread that waits until the program ends, and the thread
// that crashes), so the check asserts on those and not on a thread the runtime may be ending.

#include <bloom/core/diagnostic_record.hpp>
#include <bloom/core/worker_thread.hpp>
#include <bloom/crash/crash_reporter.hpp>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <string>
#include <string_view>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <pthread.h>
#else
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace {

// The id the minidump gives the thread.
std::uint64_t threadId() {
#if defined(_WIN32)
    return GetCurrentThreadId();
#elif defined(__APPLE__)
    std::uint64_t id = 0;
    pthread_threadid_np(nullptr, &id);
    return id;
#else
    return static_cast<std::uint64_t>(syscall(SYS_gettid));
#endif
}

// Kept out of the optimizer's sight, so the write really happens.
int* volatile nowhere = nullptr;

// Never false at run time, but the compiler cannot know: the recursion has an end on paper.
volatile bool keepGoing = true;

// Each call holds a page the optimizer cannot drop, and uses it after the inner call returns,
// so the recursion is neither folded into a loop nor a tail call.
std::uint64_t exhaustStack(const std::uint64_t depth) {
    volatile unsigned char page[4096];
    page[depth % sizeof(page)] = static_cast<unsigned char>(depth);
    if (!keepGoing)
        return depth;
    return exhaustStack(depth + 1) + page[(depth * 7) % sizeof(page)];
}

// Appends "<label> <id>" to the file BLOOM_TEST_CRASH_THREADS names: a thread alive at the crash.
void recordAlive(const std::string_view label, const std::uint64_t id) {
    const char* file = std::getenv("BLOOM_TEST_CRASH_THREADS");
    if (file == nullptr || *file == '\0')
        return;
    std::ofstream stream(file, std::ios::app);
    stream << label << ' ' << id << std::endl;
}

// A thread that is alive at the crash: it records its id, then waits until the program ends.
class HeldThread final {
  public:
    HeldThread()
        : thread_([this] {
              recordAlive("held thread", threadId());
              started_.set_value();
              release_.get_future().wait();
          }) {
        started_.get_future().wait();
    }
    HeldThread(const HeldThread&) = delete;
    HeldThread& operator=(const HeldThread&) = delete;
    ~HeldThread() {
        release_.set_value();
        thread_.join();
    }

  private:
    std::promise<void> started_;
    std::promise<void> release_;
    bloom::core::WorkerThread thread_;
};

[[noreturn]] void writeThroughNull() {
    std::cerr << "crashing thread " << threadId() << std::endl;
    recordAlive("crashing thread", threadId());
    *nowhere = 42;
    std::abort();
}

} // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
    // No error dialog may wait for a click if the handler is not there.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#endif
    if (argc != 3) {
        std::cerr << "usage: bloom_crash_child "
                     "<null-write|worker-thread|abort|stack-overflow|clean> <folder>\n";
        return 2;
    }
    const std::string_view mode = argv[1];
    bloom::crash::captureStandardError();
    const auto status =
        bloom::crash::startCrashReporter({.program = "bloom",
                                          .executable = std::filesystem::absolute(argv[0]),
                                          .directory = argv[2],
                                          .collect = true});
    if (status.state != bloom::crash::ReporterState::Running) {
        std::cerr << "the crash reporter is unavailable: " << status.detail << '\n';
        return 3;
    }
    bloom::crash::waitForCollection();
    bloom::core::recordGpuDevice("Test GPU (discrete), driver test 1.0, Vulkan 1.3");
    std::cerr << "log line before the crash" << std::endl;
    std::cerr << "main thread " << threadId() << std::endl;
    if (mode == "clean") {
        bloom::crash::finishCrashReporter();
        return 0;
    }
    recordAlive("main thread", threadId());
    const HeldThread held;

    if (mode == "null-write")
        writeThroughNull();
    if (mode == "worker-thread") {
        bloom::core::WorkerThread worker([] { writeThroughNull(); });
        worker.join();
        return 1;
    }
    if (mode == "stack-overflow") {
        bloom::core::WorkerThread worker([] {
            std::cerr << "crashing thread " << threadId() << std::endl;
            recordAlive("crashing thread", threadId());
            std::cerr << "depth " << exhaustStack(0) << std::endl;
        });
        worker.join();
        return 1;
    }
    if (mode == "abort") {
        std::cerr << "crashing thread " << threadId() << std::endl;
        recordAlive("crashing thread", threadId());
        std::abort();
    }
    std::cerr << "unknown mode " << mode << '\n';
    return 2;
}
