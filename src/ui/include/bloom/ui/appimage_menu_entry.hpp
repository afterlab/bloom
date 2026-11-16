#pragma once

#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/update/install_kind.hpp>

#include <QObject>
#include <QPointer>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class QWidget;

// The menu entry of an AppImage (#734). An AppImage is one file and installs nothing, so on the
// first run Bloom offers to add its entry and icons to the user's own data folder, and keeps the
// entry pointing at the file when the file is moved.
namespace bloom::ui {

class TaskUiBridge;

// What a write put in the data folder, or why nothing was written.
struct AppImageMenuWrite final {
    // Every file that was written.
    std::vector<std::filesystem::path> written;
    // Empty when the write succeeded; otherwise the reason, in a sentence for the artist.
    std::string refusal;

    [[nodiscard]] bool succeeded() const noexcept { return refusal.empty(); }
};

// Writes `<dataHome>/applications/id.afterlab.bloom.desktop` (the package's entry started by
// the full path of `appImage`, marked `X-Bloom-AppImage=true`), every icon of
// `<mountedTree>/share/icons/hicolor` under `<dataHome>/icons/hicolor`, and the file type under
// `<dataHome>/mime/packages`. The files are replaced one at a time and never half written. This
// is blocking file work: it never runs on the interface thread. A path the entry cannot carry
// (a quote, a backquote, `$`, a backslash, `%` or a line break) is refused before anything is
// written.
[[nodiscard]] AppImageMenuWrite writeAppImageMenuEntry(const std::filesystem::path& appImage,
                                                       const std::filesystem::path& mountedTree,
                                                       const std::filesystem::path& dataHome);

// What the data folder holds of Bloom's entry.
enum class AppImageEntryState : std::uint8_t {
    // No entry of Bloom's.
    Absent,
    // An entry this code did not write (the install script's, or the artist's own): left alone.
    Foreign,
    // Written by this code and started by this AppImage file.
    Current,
    // Written by this code for another file: the AppImage was moved or renamed.
    Moved,
};
[[nodiscard]] AppImageEntryState appImageEntryState(const std::filesystem::path& appImage,
                                                    const std::filesystem::path& dataHome);

// Where the offer runs.
struct AppImageMenuEnvironment final {
    // The AppImage file; empty when Bloom does not run from one (the offer does nothing).
    std::filesystem::path appImage;
    // `$APPDIR`, the mounted tree that holds the package's entry, icons and file type.
    std::filesystem::path mountedTree;
    // `$XDG_DATA_HOME` or `~/.local/share`.
    std::filesystem::path dataHome;
    // After a write, on the interface thread: refreshes the desktop's indexes. Must not wait.
    std::function<void(const std::filesystem::path& dataHome)> refreshIndexes;
};

// The environment of the running application.
[[nodiscard]] AppImageMenuEnvironment
applicationAppImageMenuEnvironment(const update::InstallFacts& install);

// Offers "Add to Application Menu" once, and keeps a written entry pointing at the AppImage.
// The file work is a background job of the task scheduler (blocking I/O); this object lives on
// the interface thread and only starts jobs, asks, and takes their results.
class AppImageMenuOffer final : public QObject {
    Q_OBJECT

  public:
    // `owner` is the window the prompt belongs to; `bridge` is borrowed and must outlive this
    // object, and `scheduler` is the one it wraps.
    AppImageMenuOffer(runtime::TaskScheduler& scheduler, TaskUiBridge& bridge,
                      AppImageMenuEnvironment environment, QWidget& owner,
                      QObject* parent = nullptr);
    ~AppImageMenuOffer() override;

    // At start-up, with the window shown. For an AppImage: rewrites a moved entry silently, or
    // asks (without blocking) when there is no entry and the artist was not asked before. Does
    // nothing for any other install.
    void start();

    // A write job is running.
    [[nodiscard]] bool busy() const noexcept { return job_.has_value(); }

  public slots:
    // Takes a finished job's result. Connected to the bridge; a test may call it.
    void poll();

  signals:
    // A write ended. `announced`: the artist asked for it, so a failure was shown.
    void writeFinished(bool succeeded, bool announced);

  private:
    using Handle = runtime::TaskHandle<std::shared_ptr<AppImageMenuWrite>>;

    void ask();
    void write(bool announced);

    runtime::TaskScheduler& scheduler_;
    TaskUiBridge& bridge_;
    AppImageMenuEnvironment environment_;
    QPointer<QWidget> owner_;
    std::optional<Handle> job_;
    bool announced_ = false;
    bool asked_ = false;
};

} // namespace bloom::ui
