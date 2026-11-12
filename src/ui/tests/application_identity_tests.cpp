// The names and the settings store Bloom starts with, and the package check's own profile
// (application_identity.hpp).
#include <bloom/ui/application_identity.hpp>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <iostream>
#include <string>

namespace {
int failures = 0;
void expect(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);

    qunsetenv(bloom::ui::kPackageCheckProfileVariable);
    auto identity = bloom::ui::installApplicationIdentity();
    expect(identity.organization == QStringLiteral("Kinetik") &&
               identity.application == QStringLiteral("Bloom") &&
               identity.settingsDirectory.isEmpty(),
           "Bloom's own names without the check's variable");
    expect(QCoreApplication::organizationName() == QStringLiteral("Kinetik") &&
               QCoreApplication::applicationName() == QStringLiteral("Bloom"),
           "the names reach QCoreApplication");

    // A relative directory is not a profile: the settings stay where they are.
    qputenv(bloom::ui::kPackageCheckProfileVariable, "relative/profile");
    identity = bloom::ui::installApplicationIdentity();
    expect(identity.organization == QStringLiteral("Kinetik") &&
               identity.settingsDirectory.isEmpty(),
           "a relative profile directory is ignored");

    QTemporaryDir directory;
    expect(directory.isValid(), "temporary directory");
    qputenv(bloom::ui::kPackageCheckProfileVariable, directory.path().toUtf8());
    identity = bloom::ui::installApplicationIdentity();
    expect(identity.organization == QString::fromLatin1(bloom::ui::kPackageCheckOrganization),
           "the check's organization name");
    expect(QCoreApplication::organizationName() != QStringLiteral("Kinetik"),
           "the check never uses Bloom's organization name");
    const QString root = QDir::cleanPath(directory.path());
    {
        QSettings settings;
        settings.setValue(QStringLiteral("check/value"), 7);
        settings.sync();
        expect(settings.format() == QSettings::IniFormat, "the check's settings are a file");
        expect(QDir::cleanPath(settings.fileName()).startsWith(root + QLatin1Char('/')),
               "the settings file is under the check's directory: " +
                   settings.fileName().toStdString());
        expect(QFileInfo::exists(settings.fileName()), "the settings file was written there");
    }
    // The per-user folders Qt derives from the names are not Bloom's own.
    for (const auto location : {QStandardPaths::AppDataLocation, QStandardPaths::CacheLocation}) {
        const QString path = QStandardPaths::writableLocation(location);
        expect(!path.contains(QStringLiteral("Kinetik")),
               "a per-user folder of the check is not Bloom's: " + path.toStdString());
        expect(path.contains(QString::fromLatin1(bloom::ui::kPackageCheckOrganization)),
               "a per-user folder of the check carries the check's name: " + path.toStdString());
    }

    if (failures != 0)
        return 1;
    std::cout << "application identity: ok\n";
    return 0;
}
