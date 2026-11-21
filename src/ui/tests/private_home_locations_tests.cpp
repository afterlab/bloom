// #749 (macOS): a test never reaches the user's real per-user folders. bloom_add_test gives every
// test a home of its own in the build tree, in HOME and CFFIXED_USER_HOME
// (cmake/BloomTesting.cmake). This check runs as a test, so it has that home, and requires that
// every per-user location a test can reach through Bloom or Qt lies in it and not in the real
// home's own folders (the real home is the one the user database names): Bloom's cache, data and
// GPU pipeline cache folders, Qt's writable standard locations, and the preferences file a native
// QSettings writes. The last is written once, so the check proves where CoreFoundation really puts
// it rather than where Qt says it is.
//
// Registered on macOS only (src/ui/tests/CMakeLists.txt says why).

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>
#include <QString>
#include <bloom/platform/cache_directory.hpp>
#include <bloom/platform/user_data_directory.hpp>

#include <pwd.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

namespace {

int failures = 0;

void expect(const bool condition, const std::string_view what) {
    if (!condition) {
        std::cerr << "FAIL: " << what << '\n';
        ++failures;
    }
}

[[nodiscard]] QString clean(const QString& path) { return QDir::cleanPath(path); }

[[nodiscard]] bool under(const QString& path, const QString& root) {
    const auto cleaned = clean(path);
    const auto base = clean(root);
    return cleaned == base || cleaned.startsWith(base + QLatin1Char('/'));
}

[[nodiscard]] QString fromPath(const std::filesystem::path& path) {
    return QString::fromStdString(path.generic_string());
}

// The preferences files below `folder` whose name holds `domain`.
[[nodiscard]] QStringList preferenceFiles(const QString& folder, const QString& domain) {
    QStringList files;
    QDirIterator it(folder + QStringLiteral("/Library/Preferences"), QDir::Files,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const auto file = it.next();
        if (QFileInfo(file).fileName().contains(domain, Qt::CaseInsensitive))
            files.push_back(file);
    }
    return files;
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("Afterlab"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("afterlab.invalid"));
    QCoreApplication::setApplicationName(QStringLiteral("Bloom"));

    const auto* account = getpwuid(getuid());
    expect(account != nullptr && account->pw_dir != nullptr, "the user database names a home");
    if (account == nullptr || account->pw_dir == nullptr)
        return 1;
    const QString real = clean(QString::fromLocal8Bit(account->pw_dir));
    const char* homeVariable = std::getenv("HOME");               // NOLINT(concurrency-mt-unsafe)
    const char* fixedVariable = std::getenv("CFFIXED_USER_HOME"); // NOLINT(concurrency-mt-unsafe)
    const QString home = homeVariable ? clean(QString::fromLocal8Bit(homeVariable)) : QString{};
    const QString fixed = fixedVariable ? clean(QString::fromLocal8Bit(fixedVariable)) : QString{};
    expect(!home.isEmpty() && home == fixed,
           "HOME and CFFIXED_USER_HOME name the test's own home: HOME=" + home.toStdString() +
               ", CFFIXED_USER_HOME=" + fixed.toStdString());
    expect(home.endsWith(QStringLiteral("/test-home/bloom.quality.private_home_locations")),
           "the home is this test's own folder in the build tree: " + home.toStdString());
    // The build tree, and so the test's home, may itself lie in the real home (a checkout in the
    // user's folders): what must never be reached is the real home's own per-user folders.
    const QString realLibrary = real + QStringLiteral("/Library");
    expect(home != real && !under(home, realLibrary),
           "the test's home is not the real home or one of its per-user folders: " +
               home.toStdString());
    if (failures != 0)
        return 1;

    const auto check = [&](const std::string& what, const QString& path) {
        if (path.isEmpty()) {
            expect(false, what + " resolves");
            return;
        }
        expect(!under(path, realLibrary) && path != real,
               what + " is not in the real home's folders: " + path.toStdString());
        expect(under(path, home), what + " is in the test's home: " + path.toStdString());
    };

    // Bloom's own per-user folders (src/platform). The cache follows XDG_CACHE_HOME when a run
    // sets one; it is then checked to be outside the real home only.
    const bool cacheOverridden = std::getenv("XDG_CACHE_HOME") != nullptr; // NOLINT
    if (const auto cache = bloom::platform::userCacheDirectory()) {
        if (cacheOverridden)
            expect(!under(fromPath(*cache), realLibrary),
                   "Bloom's cache folder is not in the real home's folders");
        else
            check("Bloom's cache folder", fromPath(*cache));
    } else {
        expect(false, "Bloom's cache folder resolves");
    }
    if (!cacheOverridden)
        check("Bloom's GPU pipeline cache", fromPath(bloom::platform::gpuPipelineCacheDirectory()));
    if (const auto data = bloom::platform::userDataDirectory())
        check("Bloom's data folder", fromPath(*data));
    else
        expect(false, "Bloom's data folder resolves");

    // Qt's writable standard locations, which the application and some tests use.
    check("QDir::homePath()", QDir::homePath());
    for (const auto& [location, name] :
         {std::pair{QStandardPaths::HomeLocation, "HomeLocation"},
          std::pair{QStandardPaths::AppDataLocation, "AppDataLocation"},
          std::pair{QStandardPaths::AppLocalDataLocation, "AppLocalDataLocation"},
          std::pair{QStandardPaths::AppConfigLocation, "AppConfigLocation"},
          std::pair{QStandardPaths::CacheLocation, "CacheLocation"},
          std::pair{QStandardPaths::GenericCacheLocation, "GenericCacheLocation"},
          std::pair{QStandardPaths::GenericDataLocation, "GenericDataLocation"},
          std::pair{QStandardPaths::GenericConfigLocation, "GenericConfigLocation"},
          std::pair{QStandardPaths::ConfigLocation, "ConfigLocation"},
          std::pair{QStandardPaths::DocumentsLocation, "DocumentsLocation"}})
        check(std::string("QStandardPaths ") + name, QStandardPaths::writableLocation(location));

    // A native QSettings writes through CoreFoundation's preferences, which find the home on their
    // own: write one value and look for the file it made.
    const QString domain = QStringLiteral("private-home-check");
    {
        QSettings settings(QSettings::NativeFormat, QSettings::UserScope,
                           QStringLiteral("bloom-test"), domain);
        settings.setValue(QStringLiteral("written"), 1);
        settings.sync();
        expect(settings.status() == QSettings::NoError, "the native preferences were written");
    }
    const auto inHome = preferenceFiles(home, domain);
    const auto inReal = preferenceFiles(real, domain);
    expect(!inHome.isEmpty(), "the native preferences file is in the test's home");
    expect(inReal.isEmpty(), "no native preferences file appeared in the real home: " +
                                 inReal.join(QStringLiteral(", ")).toStdString());
    if (!inReal.isEmpty()) {
        // Take the value back out of the user's preferences.
        QSettings settings(QSettings::NativeFormat, QSettings::UserScope,
                           QStringLiteral("bloom-test"), domain);
        settings.clear();
        settings.sync();
    }

    if (failures != 0)
        return 1;
    std::cout << "every per-user location is in the test's own home\n";
    return 0;
}
