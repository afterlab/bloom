#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

#include <optional>

namespace bloom::ui {

class ProjectHost;

// Opens the project the operating system asks Bloom to open.
//
// macOS tells a running application to open a document with a file-open event: when a project
// is double-clicked in the Finder, dropped on the Dock icon or chosen with Open With, at launch
// (the event follows the start) and while Bloom runs. Qt delivers it to the application object
// as QEvent::FileOpen. This filter, installed on the application, hands the file to
// ProjectHost::requestOpenPath(), so the project opens exactly as it does through File, Open:
// unsaved changes are asked about first, and the outcome is reported the same way.
//
// Other systems never send the event, and the filter does nothing there.
class ProjectFileOpenEvents final : public QObject {
    Q_OBJECT

  public:
    // Installs itself on the application object. `host` must outlive it.
    explicit ProjectFileOpenEvents(ProjectHost& host, QObject* parent = nullptr);

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    ProjectHost& host_;
};

// The project named on the command line: `Bloom <file>`, which is what the Linux and Windows
// file managers run for a double-clicked project. `arguments` is the full list, program name
// first. The project is the first later argument that does not start with `-` and is not an
// address (such as file://); a relative path is made absolute against the working directory.
// The file need not exist: opening it reports a missing file. Returns nothing when there is none.
std::optional<QString> projectPathFromArguments(const QStringList& arguments);

// Opens the project named on the command line, if any, once the event loop runs (so the main
// window is up), through ProjectHost::requestOpenPath() like the file-open event. `host` must
// outlive the event loop.
void openProjectFromArguments(ProjectHost& host, const QStringList& arguments);

} // namespace bloom::ui
