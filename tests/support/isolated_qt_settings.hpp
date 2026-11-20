#pragma once

// Qt tests: QSettings() in this process reads and writes an INI file in the process's own folder
// (#749). Without it the default settings live in one per-user place (the user's configuration
// folder, Qt's test-mode folder, the macOS preferences or the Windows registry) under the test's
// organization and application names, which the same test of another build tree running at the
// same time also uses. Call it in main before anything constructs a QSettings:
//
//   QApplication application(argc, argv);
//   bloom::test_support::isolateQtSettings();

#include "unique_temp_dir.hpp"

#include <QSettings>
#include <QString>

namespace bloom::test_support {

inline void isolateQtSettings() {
    const auto folder = processTempDirectory() / "qt-settings";
    const auto path = QString::fromStdU16String(folder.generic_u16string());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, path + QStringLiteral("/user"));
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope,
                       path + QStringLiteral("/system"));
}

} // namespace bloom::test_support
