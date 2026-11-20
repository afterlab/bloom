// bloom-crash-handler: Crashpad's out-of-process crash handler, built from Crashpad's handler
// library (issue #736; docs/architecture/platform-support.md, "Crash Reports").
//
// The application, bloom-cli and bloom-mcp start it beside themselves when they start. It waits
// for its program to crash, then reads the crashed process from outside and writes a minidump of
// every thread, with the program's prepared text report as an attachment, into the crash folder's
// database. It is given no upload address, and Bloom's Crashpad has no network transport, so a
// report never leaves the machine. It exits when its program exits.
//
// This is Crashpad's own handler entry point (handler/main.cc in its source) built as a Bloom
// program, so it is installed, packaged and signed beside the other programs.

#include "handler/handler_main.h"

#if defined(_WIN32)
#include <cstdlib>
#include <string>
#include <vector>
#include <windows.h>

namespace {

// A command-line argument as UTF-8, which is what Crashpad's handler reads on every platform.
std::string utf8(const wchar_t* text) {
    const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1)
        return {};
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), size, nullptr, nullptr);
    result.resize(static_cast<std::size_t>(size - 1));
    return result;
}

} // namespace

// A Windows-subsystem program: no console window opens beside the program it serves.
int APIENTRY wWinMain(HINSTANCE, HINSTANCE, wchar_t*, int) {
    std::vector<std::string> arguments;
    for (int index = 0; index < __argc; ++index)
        arguments.push_back(utf8(__wargv[index]));
    std::vector<char*> pointers;
    for (auto& argument : arguments)
        pointers.push_back(argument.data());
    pointers.push_back(nullptr);
    return crashpad::HandlerMain(static_cast<int>(arguments.size()), pointers.data(), nullptr);
}

#else

int main(int argc, char* argv[]) { return crashpad::HandlerMain(argc, argv, nullptr); }

#endif
