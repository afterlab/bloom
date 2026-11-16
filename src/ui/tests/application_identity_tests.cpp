// The names and the settings store Bloom starts with, and the package check's own profile
// (application_identity.hpp).
#include <bloom/ui/application_identity.hpp>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
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
    expect(identity.organization == QStringLiteral("Afterlab") &&
               identity.application == QStringLiteral("Bloom") &&
               identity.settingsDirectory.isEmpty(),
           "Bloom's own names without the check's variable");
    expect(QCoreApplication::organizationName() == QStringLiteral("Afterlab") &&
               QCoreApplication::applicationName() == QStringLiteral("Bloom"),
           "the names reach QCoreApplication");
    // The desktop entry a Linux package installs is named after this.
    expect(QGuiApplication::desktopFileName() == QStringLiteral("id.afterlab.bloom"),
           "the window's application is named after Bloom's desktop entry");

    expect(identity.refusal.isEmpty(), "nothing is refused without the check's variable");

    // The window icon of a Linux package: the icon files beside the program's folder, smallest
    // first, and none for a program that belongs to no package.
    {
        QTemporaryDir package;
        expect(package.isValid(), "temporary package directory");
        const QDir root(package.path());
        expect(root.mkpath(QStringLiteral("bin")), "the package's program folder");
        expect(bloom::ui::packagedWindowIconFiles(root.filePath(QStringLiteral("bin"))).isEmpty(),
               "a program outside a package has no icon files");
        expect(bloom::ui::packagedWindowIconFiles(QString{}).isEmpty(),
               "no program folder, no icon files");
        for (const int size : {512, 16, 48}) {
            const QString folder =
                QStringLiteral("share/icons/hicolor/%1x%1/apps").arg(size);
            expect(root.mkpath(folder), "an icon folder of the package");
            QFile file(root.filePath(folder + QStringLiteral("/id.afterlab.bloom.png")));
            expect(file.open(QIODevice::WriteOnly) && file.write("png") == 3, "an icon file");
        }
        // A folder of that name is not an icon file.
        expect(root.mkpath(QStringLiteral(
                   "share/icons/hicolor/64x64/apps/id.afterlab.bloom.png")),
               "a folder in place of an icon");
        const QStringList files =
            bloom::ui::packagedWindowIconFiles(root.filePath(QStringLiteral("bin")));
        expect(files.size() == 3, "the package's three icon files are found");
        expect(files.size() == 3 && files.at(0).contains(QStringLiteral("/16x16/")) &&
                   files.at(1).contains(QStringLiteral("/48x48/")) &&
                   files.at(2).contains(QStringLiteral("/512x512/")),
               "smallest first");
    }

    // A relative directory is not a profile, and it is not a reason to run as Bloom either:
    // the names are the check's from the moment the variable is set, and the run is refused.
    qputenv(bloom::ui::kPackageCheckProfileVariable, "relative/profile");
    identity = bloom::ui::installApplicationIdentity();
    expect(identity.organization == QString::fromLatin1(bloom::ui::kPackageCheckOrganization) &&
               identity.settingsDirectory.isEmpty() && !identity.refusal.isEmpty(),
           "a relative profile directory is refused");
    expect(QCoreApplication::organizationName() != QStringLiteral("Afterlab") &&
               QSettings::defaultFormat() == QSettings::IniFormat,
           "a refused run is not Bloom's own: another organization and no system store");

    QTemporaryDir directory;
    expect(directory.isValid(), "temporary directory");
    qputenv(bloom::ui::kPackageCheckProfileVariable, directory.path().toUtf8());
    identity = bloom::ui::installApplicationIdentity();
    expect(identity.organization == QString::fromLatin1(bloom::ui::kPackageCheckOrganization),
           "the check's organization name");
    expect(QCoreApplication::organizationName() != QStringLiteral("Afterlab"),
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
        expect(!path.contains(QStringLiteral("Afterlab")),
               "a per-user folder of the check is not Bloom's: " + path.toStdString());
        expect(path.contains(QString::fromLatin1(bloom::ui::kPackageCheckOrganization)),
               "a per-user folder of the check carries the check's name: " + path.toStdString());
    }

    if (failures != 0)
        return 1;
    std::cout << "application identity: ok\n";
    return 0;
}
