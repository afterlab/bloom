#pragma once

#include <filesystem>

namespace bloom::update::detail {

#if defined(_WIN32)
// Where a program in `programDirectory` finds libcrypto: the carried interpreter's module folder
// (`python\DLLs`) of the package it belongs to; for a program with no such folder beside it,
// the folder the build was configured with. Empty when there is neither.
[[nodiscard]] std::filesystem::path
cryptoLibraryFile(const std::filesystem::path& programDirectory,
                  const std::filesystem::path& buildDirectory);
#endif

} // namespace bloom::update::detail
