#pragma once

#include <QObject>

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

} // namespace bloom::ui
