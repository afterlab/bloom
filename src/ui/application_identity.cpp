#include <bloom/ui/application_identity.hpp>

#include <bloom/core/version.hpp>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QIcon>
#include <QSettings>

#include <string_view>

namespace bloom::ui {

ApplicationIdentity installApplicationIdentity() {
    ApplicationIdentity identity{QStringLiteral("Afterlab"), QStringLiteral("Bloom"), {}, {}};
    const QString profile = qEnvironmentVariable(kPackageCheckProfileVariable);
    if (!profile.isEmpty()) {
        // Whoever set the variable meant this run not to be Bloom's own. From here on it never
        // is, whatever the value: the names are the check's and the settings a file.
        identity.organization = QString::fromLatin1(kPackageCheckOrganization);
        QSettings::setDefaultFormat(QSettings::IniFormat);
        if (QDir::isAbsolutePath(profile)) {
            identity.settingsDirectory = QDir::cleanPath(profile);
            QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                               identity.settingsDirectory);
        } else {
            // A relative directory would depend on where the program was started; the run is
            // refused instead of guessing, and the caller must not go on.
            identity.refusal = QStringLiteral("%1 is set to '%2', which is not an absolute "
                                              "directory; Bloom does not start")
                                   .arg(QString::fromLatin1(kPackageCheckProfileVariable), profile);
        }
    }
    QCoreApplication::setApplicationName(identity.application);
    const std::string_view version = bloom::core::version();
    QCoreApplication::setApplicationVersion(
        QString::fromUtf8(version.data(), static_cast<qsizetype>(version.size())));
    QCoreApplication::setOrganizationName(identity.organization);
    // The name of Bloom's desktop entry on Linux (apps/bloom/linux), which is how a Wayland
    // desktop finds the icon and name of the window's application.
    QGuiApplication::setDesktopFileName(QStringLiteral("id.afterlab.bloom"));
    return identity;
}

QStringList packagedWindowIconFiles(const QString& programDirectory) {
    QStringList files;
    if (programDirectory.isEmpty()) {
        return files;
    }
    const QDir programs(programDirectory);
    for (const int size : {16, 24, 32, 48, 64, 128, 256, 512}) {
        const QString file = QDir::cleanPath(programs.absoluteFilePath(
            QStringLiteral("../share/icons/hicolor/%1x%1/apps/id.afterlab.bloom.png").arg(size)));
        if (QFileInfo(file).isFile()) {
            files.append(file);
        }
    }
    return files;
}

void installWindowIcon() {
#if defined(Q_OS_LINUX)
    QIcon icon;
    for (const QString& file :
         packagedWindowIconFiles(QCoreApplication::applicationDirPath())) {
        icon.addFile(file);
    }
    if (icon.isNull()) {
        icon = QIcon::fromTheme(QGuiApplication::desktopFileName());
    }
    if (!icon.isNull()) {
        QGuiApplication::setWindowIcon(icon);
    }
#endif
}

} // namespace bloom::ui
