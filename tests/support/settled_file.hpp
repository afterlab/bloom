#pragma once
// A fixture file whose identity is settled (bloom::platform::FileIdentity::settled). On a Windows
// volume that keeps no change journal, a file changed less than the settle time ago has an
// unsettled identity: Bloom hashes it instead of trusting its identity, because a rewrite within
// the same clock tick would not show. A test that counts identity-only checks on a file it just
// wrote dates that file's change time back first, as for a file nobody changed lately.
#include <filesystem>

#if defined(_WIN32)
#include <atomic>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace bloom::test_support {
// Windows: sets the file's change time an hour back, a different time on every call so two
// settled versions of one file never share an identity. Other systems need nothing. False when
// the time could not be set.
inline bool settleFileIdentity(const std::filesystem::path& path) {
#if defined(_WIN32)
    static std::atomic<long long> step{0};
    const HANDLE handle =
        CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return false;
    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    const auto ticks =
        static_cast<LONGLONG>((static_cast<unsigned long long>(now.dwHighDateTime) << 32U) |
                              now.dwLowDateTime);
    constexpr LONGLONG kHour = 36'000'000'000LL;   // 100 ns ticks
    constexpr LONGLONG kMillisecond = 10'000LL;    // 100 ns ticks
    FILE_BASIC_INFO info{};                        // zero fields stay as they are
    info.ChangeTime.QuadPart = ticks - kHour + (++step) * kMillisecond;
    const bool set = SetFileInformationByHandle(handle, FileBasicInfo, &info, sizeof info) != 0;
    CloseHandle(handle);
    return set;
#else
    static_cast<void>(path);
    return true;
#endif
}
} // namespace bloom::test_support
