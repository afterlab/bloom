#pragma once

#include <bloom/ui/kit/tokens.hpp>

#include <QLoggingCategory>
#include <QByteArray>
#include <QFont>
#include <QString>
#include <QStringList>

namespace bloom::ui::kit {

// Diagnostics channel for bundled-font loading. A missing or invalid bundled face is reported
// here and then survived, never crashed on.
Q_DECLARE_LOGGING_CATEGORY(kitFontsLog)

struct BundledFontStatus {
    // Whether each bundled family actually registered. False means the interface is running on the
    // platform's own sans-serif or monospace family -- degraded, reported, and still usable.
    bool interfaceRegistered = false;
    bool monospaceRegistered = false;

    // The distinct family names Qt registered, in load order. Qt reports a face's own family
    // name and, where declared, its typographic family; duplicates across faces are collapsed.
    QStringList registeredFamilies;

    // One human-readable line per face that failed to load, empty when everything registered.
    QStringList diagnostics;
};

// Registers the bundled TTFs with Qt's application font database. Idempotent: the first call does
// the work, every later call returns the same result without re-registering.
const BundledFontStatus& registerBundledFonts();

// The current registration result, registering on first use.
const BundledFontStatus& bundledFontStatus();

// The family list a type role should ask for: the bundled variable family (Inter Variable, or Geist
// Mono for Value) when it registered, then the platform's own family as the reported fallback.
// The weight is a point on the face's wght axis (kit::font()), never a separate family.
[[nodiscard]] QStringList fontFamiliesForRole(TypeRole role);

// True when `family` is one of the bundled chrome families and it registered.
[[nodiscard]] bool isBundledFontFamily(const QString& family);

// The font picker draws each family in its own typeface. These two are the only way a family the
// artist chose reaches a QFont: a family by name at a pixel size, and a name-only subset file
// (downloaded for a picker row) registered with Qt so the row can use it. registerPickerFont returns
// the family Qt registered, or empty when the data was not a usable font.
[[nodiscard]] QFont pickerFont(const QString& family, int pixelSize);
[[nodiscard]] QString registerPickerFont(const QByteArray& data);

} // namespace bloom::ui::kit
