#pragma once

// Runs a build tool through the system's command processor for a test fixture.
//
// The tests quote the tool's path and its arguments. The Windows command processor removes the
// first and last quote of such a line before reading it, which leaves a broken path; one more
// pair of quotes around the whole line is what it removes instead.

#include <cstdlib>
#include <string>

namespace bloom::color::test_support {

[[nodiscard]] inline int runToolCommand(const std::string& command) {
#if defined(_WIN32)
    const std::string line = "\"" + command + "\"";
#else
    const std::string& line = command;
#endif
    // NOLINTNEXTLINE(bugprone-command-processor) -- bounded test fixture: pinned build tool path.
    return std::system(line.c_str());
}

} // namespace bloom::color::test_support
