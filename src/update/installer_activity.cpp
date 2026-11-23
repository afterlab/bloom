#include <bloom/update/installer_activity.hpp>

#include <algorithm>
#include <array>
#include <fstream>
#include <system_error>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace bloom::update {

bool takeStartedByInstallerArgument(int& argc, char** argv) {
    bool found = false;
    int kept = argc > 0 ? 1 : 0;
    for (int index = 1; index < argc; ++index) {
        if (std::string_view(argv[index]) == kStartedByInstallerArgument) {
            found = true;
            continue;
        }
        argv[kept++] = argv[index];
    }
    if (found) {
        argv[kept] = nullptr;
        argc = kept;
    }
    return found;
}

#if defined(_WIN32)

namespace {

[[nodiscard]] std::wstring wide(const std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int size =
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(static_cast<std::size_t>(std::max(size, 0)), L'\0');
    if (size > 0) {
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(),
                            size);
    }
    return result;
}

BOOL CALLBACK findInstallerWindow(HWND window, LPARAM found) {
    if (IsWindowVisible(window) == FALSE) {
        return TRUE;
    }
    std::array<wchar_t, 128> title{};
    const int length = GetWindowTextW(window, title.data(), static_cast<int>(title.size()));
    const std::wstring_view text(title.data(), static_cast<std::size_t>(std::max(length, 0)));
    if (text == kUpdatingWindowTitle || text == kSetupWindowTitle) {
        *reinterpret_cast<HWND*>(found) = window;
        return FALSE;
    }
    return TRUE;
}

} // namespace

bool installerRunning(const std::string_view mutexName) {
    const auto name = wide(mutexName);
    HANDLE mutex = OpenMutexW(SYNCHRONIZE, FALSE, name.c_str());
    if (mutex != nullptr) {
        CloseHandle(mutex);
        return true;
    }
    // A mutex this user may not open still exists.
    return GetLastError() == ERROR_ACCESS_DENIED;
}

bool showInstallerWindow() {
    HWND found = nullptr;
    EnumWindows(&findInstallerWindow, reinterpret_cast<LPARAM>(&found));
    if (found == nullptr) {
        return false;
    }
    if (IsIconic(found) != FALSE) {
        ShowWindow(found, SW_RESTORE);
    }
    // The artist just started this program, so it may bring another window to the front.
    SetForegroundWindow(found);
    FLASHWINFO flash{};
    flash.cbSize = sizeof(flash);
    flash.hwnd = found;
    flash.dwFlags = FLASHW_ALL;
    flash.uCount = 3;
    FlashWindowEx(&flash);
    return true;
}

#else

bool installerRunning(const std::string_view /*mutexName*/) { return false; }

bool showInstallerWindow() { return false; }

#endif

std::optional<InstallFailure> takeInstallFailure(const std::filesystem::path& installRoot) {
    const auto notice = installRoot / std::string(kInstallFailureNoticeName);
    std::error_code error;
    if (!std::filesystem::is_regular_file(notice, error)) {
        return std::nullopt;
    }
    InstallFailure failure;
    {
        std::ifstream stream(notice, std::ios::binary);
        for (std::string line; std::getline(stream, line);) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (line.starts_with("version=")) {
                failure.version = line.substr(8);
            } else if (line.starts_with("reason=")) {
                failure.reason = line.substr(7);
            }
        }
    }
    // Read once: the notice is told one time. (The stream is closed first: an open file cannot be
    // removed on Windows.)
    std::filesystem::remove(notice, error);
    return failure;
}

} // namespace bloom::update
