# Qt 6.11.3 License And Feature Review

Reviewed: 2026-11-12

## License

- Qt's libraries are offered under commercial terms, LGPL-3.0-only, GPL-2.0-only and
  GPL-3.0-only. Bloom uses them under **LGPL-3.0-only**
  ([ADR 0014](../../../docs/decisions/0014-apache-license-and-qt-distribution.md)): the license
  texts are `texts/LGPL-3.0-only.txt` and `texts/GPL-3.0-only.txt` (which the LGPL supplements),
  copied from the source archive's `qtbase/LICENSES` directory.
- Bloom links Qt dynamically and carries the Qt Project's official binaries unmodified
  (`modified: false`, no patches). The libraries and plugins are separate files in every
  package, so a recipient can replace them with another build of the same Qt version; Bloom adds
  no technical measure against that.
- `sourceObligation: corresponding-source`. The corresponding source is the locked Qt source
  archive; `provenance.md` records that every source file the binaries were built from equals
  that archive's. Every package names the archive and its SHA-256.
- Some Qt modules are published under the GPL or commercial terms only. None is used:
  `dependencies/qt-modules.txt` lists every Qt library and plugin Bloom links or carries, and
  the `bloom.quality.qt_modules` test fails when the build names a part outside that list or
  when the locked Qt's own bill of materials does not offer a listed part under LGPL-3.0-only.
- Qt's build tools (`moc`, `rcc`, `uic` and others) are offered under GPL-3.0-only with the Qt
  GPL exception 1.0, which permits using them to build software under any license. They run
  during the build and no package carries them.

## Notices

- `NOTICE.txt` lists every Qt library and plugin the packages carry and every third-party
  component built into them, with version, license and copyright text, per operating system.
  `tools/quality/generate_qt_notices` writes it from the bills of materials inside the locked
  binaries and the module list, and copies the text of every license used into `texts/` from the
  source archive. Nothing in it is written by hand. Where a third-party component offers a
  choice of licenses, the notice records the first that is neither the GPL nor commercial.
- The ICU 73.2 libraries carried on Linux are under the Unicode license of that release
  (`texts/ICU.txt`, the `LICENSE` file of the ICU `release-73-2` tag).

## Why It Is Here

Qt is the desktop application's toolkit: windows, widgets, input, SVG icons and HTTP downloads
of fonts. One exact version on every platform makes a user interface result on one platform
mean the same on the others and makes the packages carry a known Qt (issue #679).

## Reached Scope And Minimization

- Libraries linked: Core, Gui, Widgets, Network, Svg, and DBus on Linux. Test is linked by the
  test programs only.
- Plugins carried: the platform plugins (xcb, Wayland and offscreen on Linux; Cocoa and
  offscreen on macOS; Windows and offscreen on Windows), on Linux the xcb GL integrations, the
  Wayland XDG shell, decoration and EGL buffer plugins, the compose and IBus input contexts and
  the desktop portal theme, the platform style plugin on macOS and Windows, the platform TLS
  backend (OpenSSL on Linux, Secure Transport on macOS, Schannel on Windows), and the SVG image
  format and icon engine.
- Everything else in the archives (SQL drivers, print support, other image formats, other
  platform plugins, the development tools) is used by no Bloom program and carried by no
  package.
- Qt's own build, not Bloom's, decided the binaries' options; `config_qtbase.summary` and
  `config_qtsvg.summary` inside the archives record them.
