#include <bloom/ui/project_file_open_events.hpp>

#include <bloom/ui/project_host.hpp>

#include <QCoreApplication>
#include <QDir>
#include <QEvent>
#include <QFileOpenEvent>
#include <QRegularExpression>
#include <QTimer>

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

std::optional<QString> projectPathFromArguments(const QStringList& arguments) {
    // An address has a scheme of two or more characters (a Windows drive letter has one).
    static const QRegularExpression address(QStringLiteral("^[A-Za-z][A-Za-z0-9+.-]+://"));
    for (qsizetype index = 1; index < arguments.size(); ++index) {
        const QString& argument = arguments.at(index);
        if (argument.isEmpty() || argument.startsWith(QLatin1Char('-')) ||
            address.match(argument).hasMatch()) {
            continue;
        }
        return QDir::cleanPath(QDir::current().absoluteFilePath(argument));
    }
    return std::nullopt;
}

void openProjectFromArguments(ProjectHost& host, const QStringList& arguments) {
    const auto path = projectPathFromArguments(arguments);
    if (!path.has_value()) {
        return;
    }
    QTimer::singleShot(0, &host, [&host, file = *path] {
        host.requestOpenPath(std::filesystem::path(file.toStdU16String()));
    });
}

} // namespace bloom::ui
