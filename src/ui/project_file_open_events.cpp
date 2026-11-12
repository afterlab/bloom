#include <bloom/ui/project_file_open_events.hpp>

#include <bloom/ui/project_host.hpp>

#include <QCoreApplication>
#include <QEvent>
#include <QFileOpenEvent>

#include <filesystem>

namespace bloom::ui {

ProjectFileOpenEvents::ProjectFileOpenEvents(ProjectHost& host, QObject* parent)
    : QObject(parent), host_(host) {
    if (auto* application = QCoreApplication::instance()) {
        application->installEventFilter(this);
    }
}

bool ProjectFileOpenEvents::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() != QEvent::FileOpen || watched != QCoreApplication::instance()) {
        return QObject::eventFilter(watched, event);
    }
    const QString file = static_cast<QFileOpenEvent*>(event)->file();
    if (file.isEmpty()) {
        // A URL that is not a local file: not a project Bloom can open.
        return QObject::eventFilter(watched, event);
    }
    host_.requestOpenPath(std::filesystem::path(file.toStdU16String()));
    return true;
}

} // namespace bloom::ui
