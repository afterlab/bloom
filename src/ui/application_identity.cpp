#include <bloom/ui/application_identity.hpp>

#include <QCoreApplication>
#include <QDir>
#include <QGuiApplication>
#include <QSettings>

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
    QCoreApplication::setApplicationVersion(QStringLiteral("0.1.0"));
    QCoreApplication::setOrganizationName(identity.organization);
    // The name of Bloom's desktop entry on Linux (apps/bloom/linux), which is how a Wayland
    // desktop finds the icon and name of the window's application.
    QGuiApplication::setDesktopFileName(QStringLiteral("id.afterlab.bloom"));
    return identity;
}

} // namespace bloom::ui
