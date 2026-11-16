// bloom-cli-launcher: the bloom-cli.exe in the `cli` folder of a Windows installation.
//
// The installer's "Add bloom-cli to PATH" option puts the `cli` folder, and only that folder,
// into the user's PATH. The real bloom-cli.exe sits in the installation's root, beside the
// libraries it loads, and none of those libraries may enter the user's PATH: other programs
// would find them first. This program is the one file in `cli`. It starts ..\bloom-cli.exe,
// so the real program still runs from its own folder, and it hands on:
//
//   - the arguments, exactly as typed (the tail of its own command line, not re-quoted),
//   - the standard handles, the environment and the current directory,
//   - the exit code of the real program, which it waits for.
//
// Ctrl+C and Ctrl+Break reach both programs, because they share the console. This program does
// not end on them, so that the real program decides what they mean.
//
// The program uses the Win32 API and the standard library only, and a static C++ runtime
// (CMakeLists.txt): no runtime library sits beside it.
#include <windows.h>

#include <cstdio>
#include <string>

namespace {

// Keeps this program alive when Ctrl+C or Ctrl+Break arrives; the real program gets the event
// too. Any other event (the console closing) is left to the system.
BOOL WINAPI keepRunning(const DWORD event) {
    return event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT ? TRUE : FALSE;
}

// The full path of this program.
[[nodiscard]] std::wstring ownPath() {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length =
            GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0) {
            return {};
        }
        if (length < path.size()) {
            path.resize(length);
            return path;
        }
        path.resize(path.size() * 2);
    }
}

// What follows the program name in the command line, with the blanks that separate it. The
// name ends at the closing quote when it starts with one and at the first blank otherwise, as
// Windows splits it.
[[nodiscard]] std::wstring argumentsOf(const wchar_t* commandLine) {
    const wchar_t* cursor = commandLine;
    if (*cursor == L'"') {
        ++cursor;
        while (*cursor != L'\0' && *cursor != L'"') {
            ++cursor;
        }
        if (*cursor == L'"') {
            ++cursor;
        }
    } else {
        while (*cursor != L'\0' && *cursor != L' ' && *cursor != L'\t') {
            ++cursor;
        }
    }
    return cursor;
}

// The folder part of a path, without the closing separator; empty when there is none.
[[nodiscard]] std::wstring folderOf(const std::wstring& path) {
    const auto separator = path.find_last_of(L"\\/");
    return separator == std::wstring::npos ? std::wstring{} : path.substr(0, separator);
}

} // namespace

int main() {
    const std::wstring own = ownPath();
    if (own.empty()) {
        std::fputs("bloom-cli: cannot find its own location\n", stderr);
        return 1;
    }
    // <root>\cli\bloom-cli.exe -> <root>\bloom-cli.exe
    const std::wstring root = folderOf(folderOf(own));
    if (root.empty()) {
        std::fputs("bloom-cli: cannot find the installation folder\n", stderr);
        return 1;
    }
    const std::wstring target = root + L"\\bloom-cli.exe";

    SetConsoleCtrlHandler(keepRunning, TRUE);

    std::wstring commandLine = L"\"" + target + L"\"" + argumentsOf(GetCommandLineW());

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    // The standard handles are named outright when this program has all three, so a redirected
    // input or output reaches the real program and not only the console.
    const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    const HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    const HANDLE error = GetStdHandle(STD_ERROR_HANDLE);
    const auto usable = [](const HANDLE handle) {
        return handle != nullptr && handle != INVALID_HANDLE_VALUE;
    };
    if (usable(input) && usable(output) && usable(error)) {
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = input;
        startup.hStdOutput = output;
        startup.hStdError = error;
    }
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(target.c_str(), commandLine.data(), nullptr, nullptr, TRUE, 0, nullptr,
                        nullptr, &startup, &process)) {
        std::fprintf(stderr, "bloom-cli: cannot start %ls (error %lu)\n", target.c_str(),
                     static_cast<unsigned long>(GetLastError()));
        return 1;
    }
    CloseHandle(process.hThread);
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exitCode = 1;
    if (!GetExitCodeProcess(process.hProcess, &exitCode)) {
        exitCode = 1;
    }
    CloseHandle(process.hProcess);
    return static_cast<int>(exitCode);
}
