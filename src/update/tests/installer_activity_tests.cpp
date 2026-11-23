// What Bloom knows of its Windows installer while it starts (#788): the argument the installer
// starts it with, the installer's mutex, and the notice of an update that could not be installed.
// Registered on every platform: the argument and the notice are read the same everywhere, and
// outside Windows no installer is ever found running.
#include "unique_temp_dir.hpp"
#include "update_test_support.hpp"

#include <bloom/update/installer_activity.hpp>

#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

using namespace bloom::update;
using namespace bloom::update::testing;

struct Arguments final {
    std::vector<std::string> texts;
    std::vector<char*> pointers;
    int count = 0;
    explicit Arguments(std::vector<std::string> given) : texts(std::move(given)) {
        for (auto& text : texts) {
            pointers.push_back(text.data());
        }
        pointers.push_back(nullptr);
        count = static_cast<int>(texts.size());
    }
    [[nodiscard]] std::vector<std::string> left() const {
        std::vector<std::string> result;
        for (int index = 0; index < count; ++index) {
            result.emplace_back(pointers[static_cast<std::size_t>(index)]);
        }
        return result;
    }
};

} // namespace

int main() {
    Expectations checks;
    const std::string flag(kStartedByInstallerArgument);

    {
        Arguments arguments({"Bloom.exe", flag, "C:/projects/shot.bloom"});
        checks.expect(takeStartedByInstallerArgument(arguments.count, arguments.pointers.data()),
                      "argument: the installer's argument is found");
        checks.expect(arguments.left() ==
                              std::vector<std::string>{"Bloom.exe", "C:/projects/shot.bloom"} &&
                          arguments.pointers[2] == nullptr,
                      "argument: it is removed and the others stay in order");
    }
    {
        Arguments arguments({"Bloom.exe", "C:/projects/shot.bloom"});
        checks.expect(!takeStartedByInstallerArgument(arguments.count, arguments.pointers.data()),
                      "argument: a start by hand has none");
        checks.expect(arguments.left() ==
                          std::vector<std::string>{"Bloom.exe", "C:/projects/shot.bloom"},
                      "argument: a start by hand keeps its arguments");
    }
    {
        Arguments arguments({"Bloom.exe", flag, flag});
        checks.expect(takeStartedByInstallerArgument(arguments.count, arguments.pointers.data()) &&
                          arguments.left() == std::vector<std::string>{"Bloom.exe"},
                      "argument: every copy of it is removed");
    }

    // A mutex of a name of the test's own: never the one a real installer holds.
    const std::string name = "BloomInstallerActivityTest-" + std::to_string(std::random_device{}());
    checks.expect(!installerRunning(name), "mutex: no installer runs under an unused name");
#if defined(_WIN32)
    {
        const std::wstring wide(name.begin(), name.end());
        HANDLE held = CreateMutexW(nullptr, FALSE, wide.c_str());
        checks.expect(held != nullptr, "mutex: the test holds its mutex");
        checks.expect(installerRunning(name), "mutex: a held mutex is an installer that runs");
        CloseHandle(held);
        checks.expect(!installerRunning(name), "mutex: a released mutex is none");
    }
#endif

    const bloom::test_support::UniqueTempDir temporary("bloom-installer-activity");
    const auto root = temporary.path() / "Programs" / "Bloom";
    std::filesystem::create_directories(root);
    checks.expect(!takeInstallFailure(root).has_value(), "notice: none without the file");
    const auto notice = root / std::string(kInstallFailureNoticeName);
    std::ofstream(notice, std::ios::binary)
        << "version=0.1.0-alpha.5\r\nreason=Something kept Bloom's folder in use.\r\n";
    const auto failure = takeInstallFailure(root);
    checks.expect(failure.has_value() && failure->version == "0.1.0-alpha.5" &&
                      failure->reason == "Something kept Bloom's folder in use.",
                  "notice: the version and the reason are read, without line ends");
    checks.expect(!std::filesystem::exists(notice) && !takeInstallFailure(root).has_value(),
                  "notice: it is told once");
    return checks.result();
}
