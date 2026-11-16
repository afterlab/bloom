// bloom-update-helper (ADR 0041): replaces an installed Bloom with a prepared version after
// Bloom has exited, keeps the previous version, starts the new one, and puts the previous one
// back when the new one does not confirm its start.
//
//   bloom-update-helper --plan <file>     carry out (or finish) the plan Bloom wrote
//   bloom-update-helper --version
//
// The program is started from a copy outside the installation, because the installation is what
// it moves. It uses the standard library and operating system calls only.
#include <bloom/update/apply.hpp>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#if defined(_WIN32)
#include <windows.h>

#include <shellapi.h>
#endif

#ifndef BLOOM_UPDATE_HELPER_VERSION
#error "BLOOM_UPDATE_HELPER_VERSION must name the build's version"
#endif

namespace {

[[nodiscard]] std::string_view outcomeName(const bloom::update::ApplyOutcome outcome) noexcept {
    using bloom::update::ApplyOutcome;
    switch (outcome) {
    case ApplyOutcome::Updated:
        return "updated";
    case ApplyOutcome::Restored:
        return "restored";
    case ApplyOutcome::Unchanged:
        return "unchanged";
    case ApplyOutcome::StillRunning:
        return "still-running";
    case ApplyOutcome::AlreadyDone:
        return "already-done";
    case ApplyOutcome::Failed:
        return "failed";
    }
    return "failed";
}

// The plan's path as the system gave it. On Windows the arguments of `main` are in the user's
// code page, which cannot hold every folder name, so the path is read from the wide command line.
[[nodiscard]] std::filesystem::path planArgument([[maybe_unused]] const char* argument) {
#if defined(_WIN32)
    std::filesystem::path result;
    int count = 0;
    if (wchar_t** wide = CommandLineToArgvW(GetCommandLineW(), &count); wide != nullptr) {
        if (count == 3) {
            result = wide[2];
        }
        LocalFree(wide);
    }
    return result;
#else
    return std::filesystem::path(argument);
#endif
}

[[nodiscard]] int exitCode(const bloom::update::ApplyOutcome outcome) noexcept {
    using bloom::update::ApplyOutcome;
    switch (outcome) {
    case ApplyOutcome::Updated:
    case ApplyOutcome::Unchanged:
    case ApplyOutcome::AlreadyDone:
        return 0;
    case ApplyOutcome::Restored:
        return 3;
    case ApplyOutcome::StillRunning:
        return 4;
    case ApplyOutcome::Failed:
        return 5;
    }
    return 5;
}

} // namespace

int main(const int argc, char* argv[]) {
    if (argc == 2 && std::strcmp(argv[1], "--version") == 0) {
        std::printf("bloom-update-helper %s\n", BLOOM_UPDATE_HELPER_VERSION);
        return 0;
    }
    if (argc != 3 || std::strcmp(argv[1], "--plan") != 0) {
        std::fputs("usage: bloom-update-helper --plan <file> | --version\n", stderr);
        return 2;
    }
    const std::filesystem::path planPath = planArgument(argv[2]);
    auto journal = planPath;
    journal += ".journal";
    auto log = planPath;
    log += ".log";
    const auto plan = bloom::update::readApplyPlan(planPath);
    if (!plan.has_value()) {
        std::ofstream(log, std::ios::app) << "failed: the plan cannot be read\n";
        return 2;
    }
    const auto host = bloom::update::makeSystemApplyHost();
    const auto result = bloom::update::applyUpdate(*plan, journal, *host);
    std::ofstream(log, std::ios::app) << outcomeName(result.outcome) << ": " << result.detail
                                      << '\n';
    return exitCode(result.outcome);
}
