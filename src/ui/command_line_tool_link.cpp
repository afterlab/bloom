#include <bloom/ui/command_line_tool_link.hpp>

#include <system_error>

namespace bloom::ui {

namespace {

// Where a link points, as an absolute normalized path, so that `a/../b` and `b` compare equal and
// a relative link is read from the folder it is in.
std::filesystem::path resolvedTarget(const std::filesystem::path& link,
                                     const std::filesystem::path& target) {
    return (target.is_absolute() ? target : link.parent_path() / target).lexically_normal();
}

} // namespace

CommandLineToolLinkOutcome linkCommandLineTool(const std::filesystem::path& program,
                                               const std::filesystem::path& linkFolder) {
    namespace fs = std::filesystem;
    const fs::path link = linkFolder / kCommandLineToolLinkName;
    const fs::path wanted = program.lexically_normal();

    // What is there now. A name that cannot be looked at because the folder is missing is the
    // same as a free name; any other failure is a folder this user cannot use.
    std::error_code error;
    const fs::file_status status = fs::symlink_status(link, error);
    bool replacing = false;
    if (!error && status.type() != fs::file_type::not_found) {
        if (status.type() != fs::file_type::symlink) {
            return CommandLineToolLinkOutcome::Blocked;
        }
        const fs::path target = fs::read_symlink(link, error);
        if (error) {
            return CommandLineToolLinkOutcome::NeedsAdministrator;
        }
        if (resolvedTarget(link, target) == wanted) {
            return CommandLineToolLinkOutcome::AlreadyLinked;
        }
        replacing = true;
    }

    fs::create_directories(linkFolder, error);
    if (error) {
        return CommandLineToolLinkOutcome::NeedsAdministrator;
    }
    // The new link is made under another name and renamed over the old one: rename replaces a
    // link in one step, so there is no moment without the name.
    const fs::path staged = linkFolder / (std::string(".") + kCommandLineToolLinkName + ".new");
    fs::remove(staged, error);
    fs::create_symlink(program, staged, error);
    if (error) {
        return CommandLineToolLinkOutcome::NeedsAdministrator;
    }
    fs::rename(staged, link, error);
    if (error) {
        std::error_code ignored;
        fs::remove(staged, ignored);
        return CommandLineToolLinkOutcome::NeedsAdministrator;
    }
    return replacing ? CommandLineToolLinkOutcome::Replaced : CommandLineToolLinkOutcome::Linked;
}

std::vector<std::string> administratorLinkArguments(const std::filesystem::path& program) {
    const std::string folder = kCommandLineToolLinkFolder;
    const std::string name = kCommandLineToolLinkName;
    return {
        "-e", "on run argv",
        "-e",
        "do shell script \"/bin/mkdir -p " + folder + " && /bin/ln -sfn \" & quoted form of "
        "(item 1 of argv) & \" " + folder + "/" + name + "\" with administrator privileges",
        "-e", "end run",
        program.string(),
    };
}

} // namespace bloom::ui
