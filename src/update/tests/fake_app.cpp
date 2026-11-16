// A stand-in for an installed Bloom in the update helper's test (helper_program_tests.cpp).
//
//   fake_app --confirm <file> [--as-version <version>]
//       What a version does at start: it writes the start confirmation with its version (the
//       file `version` beside it, or the one given) and ends. A version with a file `fails`
//       beside it ends without writing anything, like a program that cannot start.
//   fake_app --hold <file>
//       A running Bloom: it ends when <file> exists (and after a minute in any case, so a test
//       that was stopped leaves nothing running).
#include <bloom/update/apply.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>

int main(const int argc, char* argv[]) {
    const auto own = std::filesystem::path(argv[0]).parent_path();
    if (argc == 3 && std::string_view(argv[1]) == "--hold") {
        std::error_code error;
        for (int waited = 0; waited < 3000 && !std::filesystem::exists(argv[2], error); ++waited) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return 0;
    }
    if ((argc != 3 && argc != 5) || std::string_view(argv[1]) != "--confirm") {
        return 2;
    }
    std::error_code error;
    if (std::filesystem::exists(own / "fails", error)) {
        return 1;
    }
    std::string version;
    if (argc == 5 && std::string_view(argv[3]) == "--as-version") {
        version = argv[4];
    } else {
        std::ifstream file(own / "version");
        std::getline(file, version);
    }
    return bloom::update::writeStartConfirmation(argv[2], version) ? 0 : 1;
}
