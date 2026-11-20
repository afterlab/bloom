// Linux: every thread of a Bloom program gets an alternate signal stack, so the crash handler's
// signal handler can still run when a thread overflows its own stack (issue #736). This is the
// same thing Crashpad's own handler program does (client/pthread_create_linux.cc): pthread_create
// is wrapped, and each new thread sets up its signal stack before its function starts. The main
// thread gets one when the crash reporter starts.
//
// Compiled into the programs that start the crash reporter (the bloom_crash_program target), not
// into a library, so the wrapper is part of exactly those executables.

#include "client/crashpad_client.h"

#include <cerrno>
#include <dlfcn.h>
#include <new>
#include <pthread.h>

namespace {

using StartRoutine = void* (*)(void*);
using CreateFunction = int (*)(pthread_t*, const pthread_attr_t*, StartRoutine, void*);

struct StartParameters final {
    StartRoutine routine;
    void* argument;
};

void* startWithSignalStack(void* raw) {
    crashpad::CrashpadClient::InitializeSignalStackForThread();
    auto* const parameters = static_cast<StartParameters*>(raw);
    const StartRoutine routine = parameters->routine;
    void* const argument = parameters->argument;
    delete parameters;
    return routine(argument);
}

CreateFunction nextCreate() {
    static const auto next = reinterpret_cast<CreateFunction>(dlsym(RTLD_NEXT, "pthread_create"));
    return next;
}

} // namespace

extern "C" int pthread_create(pthread_t* thread, const pthread_attr_t* attributes,
                              StartRoutine routine, void* argument) {
    const CreateFunction next = nextCreate();
    if (next == nullptr)
        return EAGAIN;
    auto* const parameters = new (std::nothrow) StartParameters{routine, argument};
    if (parameters == nullptr)
        return EAGAIN;
    const int result = next(thread, attributes, startWithSignalStack, parameters);
    if (result != 0)
        delete parameters;
    return result;
}
