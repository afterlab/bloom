#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace bloom::ui {

// Help > Install Command-Line Tool… (macOS, #734): a link named `bloom-cli` in a folder on the
// artist's PATH that names the bloom-cli program inside the application bundle. No widgets and
// nothing but std::filesystem, so the same function runs in a test on every platform that has
// symbolic links.
enum class CommandLineToolLinkOutcome {
    Linked,             // there was no link; it is made now
    AlreadyLinked,      // the link is there and names this program; nothing was changed
    Replaced,           // a link to another place (for example an older copy) now names this one
    NeedsAdministrator, // the folder or the link cannot be written by this user; nothing changed
    Blocked,            // something that is not a link has that name; it is never deleted
};

// The name of the link.
inline constexpr const char* kCommandLineToolLinkName = "bloom-cli";
// The folder the menu item links into.
inline constexpr const char* kCommandLineToolLinkFolder = "/usr/local/bin";

// Links `linkFolder`/bloom-cli to `program`. Creates the folder when it is missing and this user
// may. A link made here replaces an older link atomically, so a running shell never finds the
// name missing.
[[nodiscard]] CommandLineToolLinkOutcome linkCommandLineTool(const std::filesystem::path& program,
                                                             const std::filesystem::path& linkFolder);

// The arguments of /usr/bin/osascript that make the same link in kCommandLineToolLinkFolder with
// the system's administrator prompt. The program's path is the last argument and is read by the
// script as an argument, never pasted into the script text.
[[nodiscard]] std::vector<std::string>
administratorLinkArguments(const std::filesystem::path& program);

} // namespace bloom::ui
