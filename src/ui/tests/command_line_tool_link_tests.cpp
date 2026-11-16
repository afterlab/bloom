// Help > Install Command-Line Tool… (#734): the link function in a temporary folder, the
// arguments of the administrator prompt, and the menu item (present on macOS only).
#include <bloom/runtime/node_definition_registry.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/ui/command_line_tool_link.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/editor_registry.hpp>
#include <bloom/ui/main_window.hpp>
#include <bloom/ui/project_host.hpp>
#include <bloom/ui/task_ui_bridge.hpp>

#include <QAction>
#include <QApplication>
#include <QLabel>
#include <QMenu>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace fs = std::filesystem;
using bloom::ui::CommandLineToolLinkOutcome;
using bloom::ui::linkCommandLineTool;

namespace {

int failures = 0;

void expect(bool condition, const char* what) {
    if (!condition) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    }
}

[[nodiscard]] bool linkNames(const fs::path& link, const fs::path& target) {
    std::error_code error;
    return fs::is_symlink(link, error) && fs::read_symlink(link, error) == target;
}

void write(const fs::path& path, const char* text) {
    std::ofstream(path) << text;
}

void testOutcomes(const fs::path& root) {
    const fs::path program = root / "Bloom.app/Contents/MacOS/bloom-cli";
    const fs::path other = root / "Old.app/Contents/MacOS/bloom-cli";

    // A missing folder is made, and the link is made in it.
    const fs::path folder = root / "usr/local/bin";
    expect(linkCommandLineTool(program, folder) == CommandLineToolLinkOutcome::Linked,
           "a free name in a missing folder is linked");
    expect(fs::is_directory(folder) && linkNames(folder / "bloom-cli", program),
           "the folder exists and the link names the program");

    // Again: nothing changes, and no staging name is left behind.
    expect(linkCommandLineTool(program, folder) == CommandLineToolLinkOutcome::AlreadyLinked,
           "a link that names this program is already linked");
    expect(linkCommandLineTool(root / "Bloom.app/Contents/../Contents/MacOS/bloom-cli", folder) ==
               CommandLineToolLinkOutcome::AlreadyLinked,
           "the same program written another way is already linked");
    std::size_t entries = 0;
    for ([[maybe_unused]] const auto& entry : fs::directory_iterator(folder)) {
        ++entries;
    }
    expect(entries == 1, "the folder holds the link and nothing else");

    // A link to another place is changed.
    expect(linkCommandLineTool(other, folder) == CommandLineToolLinkOutcome::Replaced,
           "a link to another copy is replaced");
    expect(linkNames(folder / "bloom-cli", other), "the replaced link names the new program");
    // A link that points at nothing is another place too.
    fs::remove(folder / "bloom-cli");
    fs::create_symlink(root / "gone/bloom-cli", folder / "bloom-cli");
    expect(linkCommandLineTool(program, folder) == CommandLineToolLinkOutcome::Replaced,
           "a dangling link is replaced");

    // A relative link that names the program counts as the program.
    const fs::path relativeFolder = root / "relative";
    fs::create_directories(relativeFolder);
    fs::create_symlink("../Bloom.app/Contents/MacOS/bloom-cli", relativeFolder / "bloom-cli");
    expect(linkCommandLineTool(program, relativeFolder) ==
               CommandLineToolLinkOutcome::AlreadyLinked,
           "a relative link to the program is already linked");

    // A file that is not a link is never touched; neither is a folder.
    const fs::path blockedFolder = root / "blocked";
    fs::create_directories(blockedFolder);
    write(blockedFolder / "bloom-cli", "mine");
    expect(linkCommandLineTool(program, blockedFolder) == CommandLineToolLinkOutcome::Blocked,
           "a plain file with that name blocks");
    std::ifstream kept(blockedFolder / "bloom-cli");
    std::string text;
    std::getline(kept, text);
    expect(text == "mine" && !fs::is_symlink(blockedFolder / "bloom-cli"),
           "the blocking file is untouched");
    const fs::path folderFolder = root / "blocked-by-folder";
    fs::create_directories(folderFolder / "bloom-cli");
    expect(linkCommandLineTool(program, folderFolder) == CommandLineToolLinkOutcome::Blocked,
           "a folder with that name blocks");
    expect(fs::is_directory(folderFolder / "bloom-cli") &&
               !fs::is_symlink(folderFolder / "bloom-cli"),
           "the blocking folder is untouched");

    // The folder cannot be made: a file is where it would go.
    write(root / "a-file", "x");
    expect(linkCommandLineTool(program, root / "a-file/bin") ==
               CommandLineToolLinkOutcome::NeedsAdministrator,
           "a folder that cannot be made needs the administrator");

    // The folder cannot be written. A user that may write anywhere (root) makes this case
    // meaningless, so it is only checked when the permission really keeps this user out.
    const fs::path locked = root / "locked";
    fs::create_directories(locked);
    fs::permissions(locked, fs::perms::owner_read | fs::perms::owner_exec);
    std::error_code error;
    const bool kept_out = !std::ofstream(locked / "probe").is_open();
    if (kept_out) {
        expect(linkCommandLineTool(program, locked) ==
                   CommandLineToolLinkOutcome::NeedsAdministrator,
               "a folder this user cannot write needs the administrator");
        expect(!fs::exists(locked / "bloom-cli") &&
                   !fs::exists(fs::symlink_status(locked / ".bloom-cli.new", error)),
               "nothing was made in it");
    }
    fs::permissions(locked, fs::perms::owner_all);

    // A link that cannot be changed is the same.
    const fs::path fixed = root / "fixed";
    fs::create_directories(fixed);
    fs::create_symlink(other, fixed / "bloom-cli");
    fs::permissions(fixed, fs::perms::owner_read | fs::perms::owner_exec);
    if (!std::ofstream(fixed / "probe").is_open()) {
        expect(linkCommandLineTool(program, fixed) ==
                   CommandLineToolLinkOutcome::NeedsAdministrator,
               "a link in a folder this user cannot write needs the administrator");
        expect(linkNames(fixed / "bloom-cli", other), "the old link is as it was");
    }
    fs::permissions(fixed, fs::perms::owner_all);
}

void testAdministratorArguments() {
    // A path with every character that is special to AppleScript and to the shell.
    const fs::path program = "/Applications/Bo\"om 'x' $(y) \\ & ;.app/Contents/MacOS/bloom-cli";
    const auto arguments = bloom::ui::administratorLinkArguments(program);
    expect(arguments.size() == 7 && arguments[0] == "-e" && arguments[2] == "-e" &&
               arguments[4] == "-e",
           "the arguments are three script lines and the program");
    expect(arguments.back() == program.string(), "the program is the last argument");
    std::size_t mentions = 0;
    for (std::size_t i = 0; i + 1 < arguments.size(); ++i) {
        if (arguments[i].find("Applications") != std::string::npos) {
            ++mentions;
        }
    }
    expect(mentions == 0, "the program's path is not pasted into the script text");
    expect(arguments[1] == "on run argv" && arguments[5] == "end run",
           "the script is a handler that takes arguments");
    const std::string command =
        "do shell script \"/bin/mkdir -p /usr/local/bin && /bin/ln -sfn \" & quoted form of "
        "(item 1 of argv) & \" /usr/local/bin/bloom-cli\" with administrator privileges";
    expect(arguments[3] == command, "the middle line makes the folder and the link as an administrator");
}

struct WindowBench final {
    bloom::ui::EditorRegistry registry;
    bloom::runtime::TaskScheduler scheduler;
    bloom::ui::ProjectHost projectHost{scheduler};
    bloom::ui::CompositionSession compositionSession;
    bloom::runtime::NodeDefinitionRegistry nodeDefinitions;
    bloom::ui::TaskUiBridge taskUiBridge{scheduler};

    WindowBench()
        : compositionSession(*projectHost.liveDocumentAndStack().first,
                             *projectHost.liveDocumentAndStack().second,
                             projectHost.lowestCompositionId()) {
        nodeDefinitions.freeze();
        const auto add = [this](std::string id, QString name) {
            (void)registry.registerEditor({.id = std::move(id),
                                           .displayName = std::move(name),
                                           .create = [](QWidget* parent) {
                                               return new QLabel(QStringLiteral("editor"), parent);
                                           }});
        };
        add("bloom.viewer", "Compositor");
        add("bloom.nodes", "Nodes");
        add("bloom.timeline", "Timeline");
        add("bloom.assets", "Assets");
        add("bloom.properties", "Properties");
    }
};

void testMenuItem() {
    WindowBench bench;
    bloom::ui::MainWindow window(bench.registry, bench.compositionSession, bench.projectHost);
    auto* action = window.installCommandLineToolAction();
    auto* found = window.findChild<QAction*>(QStringLiteral("installCommandLineToolAction"));
    auto* updates = window.checkForUpdatesAction();
#ifdef Q_OS_MACOS
    expect(action != nullptr && action == found &&
               action->text() == QString::fromUtf8("Install Command-Line Tool…") &&
               action->menuRole() == QAction::NoRole,
           "macOS: Help has Install Command-Line Tool…");
    auto* help = updates != nullptr ? qobject_cast<QMenu*>(updates->parent()) : nullptr;
    const auto actions = help != nullptr ? help->actions() : QList<QAction*>{};
    expect(actions.size() > 2 && actions[0] == updates && actions[1] == action,
           "macOS: the item follows Check for Updates…");
#else
    expect(action == nullptr && found == nullptr, "no Install Command-Line Tool… on this platform");
    expect(updates != nullptr, "the Help menu is there");
#endif
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    const QTemporaryDir home;
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication::setOrganizationName(QStringLiteral("BloomCliLinkTests"));
    QCoreApplication::setApplicationName(QStringLiteral("cli-link-tests"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, home.path());
    QApplication application(argc, argv);

    const QTemporaryDir folder;
    if (!folder.isValid()) {
        std::fprintf(stderr, "FAIL: no temporary folder\n");
        return 1;
    }
    const fs::path root = fs::path(folder.path().toStdString()).lexically_normal();
    testOutcomes(root);
    testAdministratorArguments();
    testMenuItem();
    return failures == 0 ? 0 : 1;
}
