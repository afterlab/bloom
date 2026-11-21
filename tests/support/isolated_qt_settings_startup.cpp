// #749: every Qt test program keeps its default QSettings in an INI file in its own process folder
// (isolated_qt_settings.hpp), from the moment its application object exists. bloom_add_test adds
// this file to every registered test program that links Qt (cmake/BloomTesting.cmake), so no test
// writes settings into a per-user place: the configuration folder, the Windows registry, or the
// macOS preferences, which the preferences daemon keeps in the user's real home whatever HOME and
// CFFIXED_USER_HOME say. A program that chose its own settings format before its application
// object (an INI default with its own path) keeps it.

#include "isolated_qt_settings.hpp"

#include <QCoreApplication>

namespace {

void isolateTestSettings() {
    if (QSettings::defaultFormat() == QSettings::NativeFormat)
        bloom::test_support::isolateQtSettings();
}

} // namespace

Q_COREAPP_STARTUP_FUNCTION(isolateTestSettings)
