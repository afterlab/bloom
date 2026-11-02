// bloom.platform.private-temporary-directory: each call makes a new, distinct directory under the
// system temporary directory, owner-only on Linux and macOS, and a prefix with a path separator is
// refused.
#include <bloom/platform/private_temporary_directory.hpp>

#include <filesystem>
#include <iostream>
#include <string>

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

namespace {

namespace fs = std::filesystem;

int failures = 0;
void expect(const bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAILED: " << message << '\n';
    }
}

} // namespace

int main() {
    using bloom::platform::createPrivateTemporaryDirectory;
    const auto first = createPrivateTemporaryDirectory("bloom-private-test-");
    const auto second = createPrivateTemporaryDirectory("bloom-private-test-");
    expect(first && second, "two directories are created");
    if (first && second) {
        expect(*first != *second, "each call makes a new directory");
        for (const auto& folder : {*first, *second}) {
            expect(fs::is_directory(fs::symlink_status(folder)), "a real directory, not a link");
            expect(fs::equivalent(folder.parent_path(), fs::temp_directory_path()),
                   "the directory is under the system temporary directory");
            expect(folder.filename().string().starts_with("bloom-private-test-") &&
                       folder.filename().string().size() >
                           std::string("bloom-private-test-").size(),
                   "the name is the prefix and a unique suffix");
#if !defined(_WIN32)
            struct stat status{};
            expect(::stat(folder.c_str(), &status) == 0 && (status.st_mode & 0777U) == 0700U,
                   "the directory is mode 0700");
#endif
        }
    }
    expect(!createPrivateTemporaryDirectory("nested/prefix-"),
           "a separator in the prefix is refused");
    std::error_code ignored;
    if (first)
        fs::remove_all(*first, ignored);
    if (second)
        fs::remove_all(*second, ignored);
    if (failures == 0)
        std::cout << "private temporary directory: distinct owner-only directories passed\n";
    return failures == 0 ? 0 : 1;
}
