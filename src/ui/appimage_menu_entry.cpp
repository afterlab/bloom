#include <bloom/ui/appimage_menu_entry.hpp>

#include <bloom/ui/kit/dialog_prompts.hpp>
#include <bloom/ui/task_ui_bridge.hpp>

#include <QDebug>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QWidget>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <sstream>
#include <system_error>
#include <utility>

namespace bloom::ui {

namespace {

constexpr auto kAnsweredKey = "appimage/menu-offer-answered";
constexpr std::string_view kApplication = "id.afterlab.bloom";
constexpr std::string_view kMarkerLine = "X-Bloom-AppImage=true";

[[nodiscard]] std::filesystem::path entryPath(const std::filesystem::path& dataHome) {
    return dataHome / "applications" / (std::string(kApplication) + ".desktop");
}

[[nodiscard]] std::optional<std::string> readText(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::nullopt;
    }
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

// The characters a desktop entry's Exec line cannot carry as they are.
[[nodiscard]] bool carriesInEntry(const std::string& text) {
    return text.find_first_of("\"`$\\%\n\r") == std::string::npos;
}

// Writes `bytes` beside `path` and renames it into place, so a reader never sees half a file.
[[nodiscard]] bool replaceFile(const std::filesystem::path& path, const std::string& bytes) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) {
        return false;
    }
    auto temporary = path;
    temporary += ".bloom-new";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        file.flush();
        if (!file) {
            file.close();
            std::filesystem::remove(temporary, error);
            return false;
        }
    }
    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::filesystem::remove(temporary, error);
        return false;
    }
    return true;
}

// The package's entry started by the AppImage's full path. Empty when the entry has no Exec line.
[[nodiscard]] std::string entryText(const std::string& packaged, const std::string& appImage) {
    std::istringstream lines(packaged);
    std::string output;
    std::string line;
    bool replaced = false;
    while (std::getline(lines, line)) {
        if (line.starts_with("TryExec=") || line.starts_with(kMarkerLine)) {
            continue;
        }
        if (!replaced && (line == "Exec=Bloom" || line.starts_with("Exec=Bloom "))) {
            output += "TryExec=" + appImage + "\n";
            output += "Exec=\"" + appImage + "\"" + line.substr(10) + "\n";
            output += std::string(kMarkerLine) + "\n";
            replaced = true;
            continue;
        }
        output += line + "\n";
    }
    return replaced ? output : std::string{};
}

[[nodiscard]] AppImageMenuWrite refused(std::string reason) {
    AppImageMenuWrite result;
    result.refusal = std::move(reason);
    return result;
}

[[nodiscard]] runtime::TaskRequest request() {
    return {"Add Bloom to the application menu",
            {.kind = runtime::TaskOwnerKind::Application, .id = runtime::TaskOwnerId::fromRaw(734)},
            runtime::TaskPriority::Foreground,
            runtime::TaskExecutor::BlockingIo};
}

} // namespace

AppImageMenuWrite writeAppImageMenuEntry(const std::filesystem::path& appImage,
                                         const std::filesystem::path& mountedTree,
                                         const std::filesystem::path& dataHome) {
    const auto appImageText = appImage.string();
    if (!appImage.is_absolute() || appImageText.empty()) {
        return refused("The AppImage must be named by its full path.");
    }
    if (!carriesInEntry(appImageText)) {
        return refused("The AppImage's path has a character a menu entry cannot hold: a quote, "
                       "a backquote, $, a backslash, % or a line break. Rename the file or "
                       "its folder.");
    }
    if (dataHome.empty()) {
        return refused("Your data folder could not be found.");
    }
    const auto share = mountedTree / "share";
    const auto packaged =
        readText(share / "applications" / (std::string(kApplication) + ".desktop"));
    if (!packaged.has_value()) {
        return refused("This AppImage has no menu entry to add.");
    }
    const auto entry = entryText(*packaged, appImageText);
    if (entry.empty()) {
        return refused("This AppImage's menu entry cannot be started by its file.");
    }
    const auto mime = readText(share / "mime" / "packages" / (std::string(kApplication) + ".xml"));
    if (!mime.has_value()) {
        return refused("This AppImage has no file type to add.");
    }

    AppImageMenuWrite result;
    std::error_code error;
    // The icons first, then the file type, then the entry: an entry never points at an icon that
    // is not there yet.
    std::vector<std::filesystem::path> sizes;
    for (std::filesystem::directory_iterator it(share / "icons" / "hicolor", error), end;
         !error && it != end; it.increment(error)) {
        sizes.push_back(it->path().filename());
    }
    std::sort(sizes.begin(), sizes.end());
    for (const auto& size : sizes) {
        const auto icon = readText(share / "icons" / "hicolor" / size / "apps" /
                                   (std::string(kApplication) + ".png"));
        if (!icon.has_value()) {
            continue;
        }
        const auto destination =
            dataHome / "icons" / "hicolor" / size / "apps" / (std::string(kApplication) + ".png");
        if (!replaceFile(destination, *icon)) {
            result.refusal = "An icon could not be written to your data folder.";
            return result;
        }
        result.written.push_back(destination);
    }
    const auto mimeDestination =
        dataHome / "mime" / "packages" / (std::string(kApplication) + ".xml");
    if (!replaceFile(mimeDestination, *mime)) {
        result.refusal = "The file type could not be written to your data folder.";
        return result;
    }
    result.written.push_back(mimeDestination);
    const auto entryDestination = entryPath(dataHome);
    if (!replaceFile(entryDestination, entry)) {
        result.refusal = "The menu entry could not be written to your data folder.";
        return result;
    }
    result.written.push_back(entryDestination);
    return result;
}

AppImageEntryState appImageEntryState(const std::filesystem::path& appImage,
                                      const std::filesystem::path& dataHome) {
    const auto text = readText(entryPath(dataHome));
    if (!text.has_value()) {
        return AppImageEntryState::Absent;
    }
    std::istringstream lines(*text);
    std::string line;
    bool marked = false;
    std::string exec;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line == kMarkerLine) {
            marked = true;
        } else if (exec.empty() && line.starts_with("Exec=\"")) {
            const auto close = line.find('"', 6);
            if (close != std::string::npos) {
                exec = line.substr(6, close - 6);
            }
        }
    }
    if (!marked) {
        return AppImageEntryState::Foreign;
    }
    return exec == appImage.string() ? AppImageEntryState::Current : AppImageEntryState::Moved;
}

AppImageMenuEnvironment applicationAppImageMenuEnvironment(const update::InstallFacts& install) {
    AppImageMenuEnvironment environment;
    if (install.kind != update::InstallKind::AppImage) {
        return environment;
    }
    const auto variables = update::processEnvironment();
    environment.appImage = install.root;
    if (const auto mount = variables("APPDIR"); mount.has_value()) {
        environment.mountedTree = *mount;
    }
    if (const auto data = variables("XDG_DATA_HOME");
        data.has_value() && !data->empty() && std::filesystem::path(*data).is_absolute()) {
        environment.dataHome = *data;
    } else if (const auto home = variables("HOME"); home.has_value() && !home->empty()) {
        environment.dataHome = std::filesystem::path(*home) / ".local" / "share";
    }
#if defined(__linux__)
    // The desktop's own indexes, started and not waited for; a desktop without these tools reads
    // the files directly.
    environment.refreshIndexes = [](const std::filesystem::path& dataHome) {
        const auto start = [](const QString& program, const std::filesystem::path& folder) {
            const auto found = QStandardPaths::findExecutable(program);
            if (!found.isEmpty()) {
                (void)QProcess::startDetached(found, {QString::fromStdString(folder.string())});
            }
        };
        start(QStringLiteral("update-desktop-database"), dataHome / "applications");
        start(QStringLiteral("update-mime-database"), dataHome / "mime");
    };
#endif
    return environment;
}

AppImageMenuOffer::AppImageMenuOffer(runtime::TaskScheduler& scheduler, TaskUiBridge& bridge,
                                     AppImageMenuEnvironment environment, QWidget& owner,
                                     QObject* parent)
    : QObject(parent), scheduler_(scheduler), bridge_(bridge), environment_(std::move(environment)),
      owner_(&owner) {
    connect(&bridge_, &TaskUiBridge::resultsAvailable, this, &AppImageMenuOffer::poll);
    connect(&bridge_, &TaskUiBridge::snapshotsPolled, this, &AppImageMenuOffer::poll);
}

AppImageMenuOffer::~AppImageMenuOffer() = default;

void AppImageMenuOffer::start() {
    if (environment_.appImage.empty() || job_.has_value()) {
        return;
    }
    switch (appImageEntryState(environment_.appImage, environment_.dataHome)) {
    case AppImageEntryState::Moved:
        // Nobody is asked: the entry the artist added keeps working.
        write(false);
        return;
    case AppImageEntryState::Absent:
        ask();
        return;
    case AppImageEntryState::Current:
    case AppImageEntryState::Foreign:
        return;
    }
}

void AppImageMenuOffer::ask() {
    const QSettings settings;
    if (asked_ || owner_.isNull() || settings.contains(QLatin1String(kAnsweredKey))) {
        return;
    }
    asked_ = true;
    kit::KConfirmSpec spec;
    spec.title = tr("Application Menu");
    spec.objectName = QStringLiteral("appImageMenuOffer");
    spec.heading = tr("Add Bloom to the Application Menu?");
    spec.detail = tr("Bloom is one file, so it is not in your application menu yet. Adding it "
                     "puts a menu entry and an icon in your home folder. Nothing else on this "
                     "computer is changed.");
    spec.acceptText = tr("Add to Application Menu");
    spec.rejectText = tr("Not Now");
    kit::confirm(*owner_, spec,
                 [self = QPointer<AppImageMenuOffer>(this)](const kit::KDialog::Role role) {
                     // Either answer is the answer: the offer is made once.
                     QSettings().setValue(QLatin1String(kAnsweredKey), true);
                     if (self != nullptr && role == kit::KDialog::Role::Accept) {
                         self->write(true);
                     }
                 });
}

void AppImageMenuOffer::write(const bool announced) {
    if (job_.has_value()) {
        return;
    }
    auto submission = scheduler_.submit<std::shared_ptr<AppImageMenuWrite>>(
        request(), [environment = environment_](runtime::TaskContext&) {
            return runtime::TaskResult<std::shared_ptr<AppImageMenuWrite>>::succeeded(
                std::make_shared<AppImageMenuWrite>(writeAppImageMenuEntry(
                    environment.appImage, environment.mountedTree, environment.dataHome)));
        });
    if (!submission.accepted()) {
        return;
    }
    announced_ = announced;
    job_.emplace(std::move(submission.handle));
    bridge_.wake();
}

void AppImageMenuOffer::poll() {
    if (!job_.has_value()) {
        return;
    }
    auto result = job_->tryTakeResult();
    if (!result.has_value()) {
        return;
    }
    job_.reset();
    const bool announced = announced_;
    AppImageMenuWrite finished;
    finished.refusal = "The menu entry could not be written.";
    if (result->state() == runtime::TaskState::Succeeded && result->value().has_value() &&
        *result->value() != nullptr) {
        finished = std::move(**result->value());
    }
    if (finished.succeeded()) {
        if (environment_.refreshIndexes) {
            environment_.refreshIndexes(environment_.dataHome);
        }
    } else if (announced && !owner_.isNull()) {
        kit::inform(*owner_, kit::KPromptTone::Error, tr("Application Menu"),
                    tr("Bloom could not be added to the application menu."),
                    QString::fromStdString(finished.refusal), QStringLiteral("appImageMenuFailed"));
    } else {
        qWarning() << "The application menu entry was not refreshed:"
                   << QString::fromStdString(finished.refusal);
    }
    emit writeFinished(finished.succeeded(), announced);
}

} // namespace bloom::ui
