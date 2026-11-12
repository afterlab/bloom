#include <bloom/ui/application_identity.hpp>

#include <QCoreApplication>
#include <QDir>
#include <QSettings>

namespace bloom::ui {

ApplicationIdentity installApplicationIdentity() {
    ApplicationIdentity identity{QStringLiteral("Kinetik"), QStringLiteral("Bloom"), {}};
    const QString profile = qEnvironmentVariable(kPackageCheckProfileVariable);
    if (!profile.isEmpty() && QDir::isAbsolutePath(profile)) {
        identity.organization = QString::fromLatin1(kPackageCheckOrganization);
        identity.settingsDirectory = QDir::cleanPath(profile);
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, identity.settingsDirectory);
    }
    QCoreApplication::setApplicationName(identity.application);
    QCoreApplication::setApplicationVersion(QStringLiteral("0.1.0"));
    QCoreApplication::setOrganizationName(identity.organization);
    return identity;
}

} // namespace bloom::ui
