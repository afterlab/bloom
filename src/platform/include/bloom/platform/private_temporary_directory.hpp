#pragma once

#include <filesystem>
#include <optional>
#include <string_view>

namespace bloom::platform {

// Creates a new directory under the system temporary directory that only the current user can use:
// an unpredictable name made exclusively (an existing path, including a planted symbolic link, is
// never reused), mode 0700 on Linux and macOS (mkdtemp). On Windows the new directory inherits the
// per-user temporary directory's access control. `prefix` starts the name and must not contain a
// path separator. Returns nullopt when no directory could be created. Blocking I/O: call it off
// the UI thread. The caller owns and removes the directory.
[[nodiscard]] std::optional<std::filesystem::path>
createPrivateTemporaryDirectory(std::string_view prefix);

} // namespace bloom::platform
