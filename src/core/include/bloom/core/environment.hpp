#pragma once

// Setting and removing a variable in this process's environment, spelled once for every platform:
// POSIX has setenv and unsetenv, the C runtime on Windows has _putenv_s (an empty value removes).
// Not thread-safe against concurrent getenv, as the underlying calls are not; used by start-up code
// and by tests.

#include <cstdlib>

namespace bloom::core {

inline bool setEnvironmentVariable(const char* name, const char* value) noexcept {
#if defined(_WIN32)
    return _putenv_s(name, value) == 0;
#else
    return ::setenv(name, value, 1) == 0;
#endif
}

inline bool unsetEnvironmentVariable(const char* name) noexcept {
#if defined(_WIN32)
    return _putenv_s(name, "") == 0;
#else
    return ::unsetenv(name) == 0;
#endif
}

} // namespace bloom::core
