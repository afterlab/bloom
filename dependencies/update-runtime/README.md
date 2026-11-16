# Update runtimes

Bloom checks for new releases with two published programs. On macOS it is Sparkle 2. On Windows
it is WinSparkle. Linux has no updater of its own. `sparkle.json` and `winsparkle.json` are the
only places each one is written: the release, its address, SHA-256 and size, the source it was
built from, its license files as checked in here, and the paths of it that Bloom keeps.
`dependencies/superbuild/projects/update-runtime-fetch.cmake` reads them.

Both are taken as published, by checksum, and are not built here. The superbuild puts each one
into the dependency prefix on its own platform only. The fetch compares the size and SHA-256
with the record before it unpacks anything, and keeps only the listed paths. It does not keep
the archive's signing and appcast tools.

| Item | Release | Platform | License | In the prefix |
| --- | --- | --- | --- | --- |
| Sparkle 2 | 2.10.1 | macOS | MIT, with bsdiff (BSD-2-Clause), sais-lite (MIT), ed25519 (Zlib) and one BSD-2-Clause file | `sparkle/Sparkle.framework`, `sparkle/LICENSE` |
| WinSparkle | 0.9.4 | Windows x64 | MIT, with wxWidgets, expat, OpenSSL and ed25519 inside the DLL | `winsparkle/include/`, `winsparkle/x64/Release/WinSparkle.dll`, `winsparkle/x64/Release/WinSparkle.lib`, `winsparkle/COPYING`, `winsparkle/COPYING.expat` |

## What is in a Bloom package

- macOS: `Sparkle.framework` goes into `Bloom.app/Contents/Frameworks`, with its symbolic links
  and its own code signature files. Its license text goes with the other notices. The framework is
  universal (Intel and Apple silicon).
- Windows: `WinSparkle.dll` goes beside the programs. The import library and the headers are
  not packaged. The license texts of WinSparkle and of the four libraries linked into the DLL go
  with the other notices.
- Linux: nothing.

Bloom links neither library. Each is loaded at run time from inside the package and its classes
or functions are found by name ([ADR 0041](../../docs/decisions/0041-self-update.md), "The
platform updaters"), so a build, a test program and a package without the library all work
without it. The macOS driver is compiled against the framework's headers in the prefix; the
Windows driver declares the few functions it calls as the header of this release declares them.
A package carries the library together with the record `update-runtime.json` of the files it
took, and the package check requires the two to agree.

## Inside WinSparkle

The DLL is built from the release's source archive, which carries its third-party sources. The
versions below are those of that source at the recorded commits. The release does not record
which parts of each library the DLL contains, so the table gives the library, not the parts.

| Part | Version | License | Note |
| --- | --- | --- | --- |
| WinSparkle | 0.9.4 | MIT | |
| wxWidgets | 3.2.10 | wxWindows Library Licence 3.1 (LGPL-2.0-or-later with the wxWindows exception) | Linked statically |
| expat | 2.8.1 | MIT | |
| OpenSSL (libcrypto) | 1.0.2u | OpenSSL and SSLeay licenses | Used for the deprecated DSA signatures only. A release from the 1.0.2 series has no security fixes after 2019 |
| ed25519 | no version; commit `7fa6712ef5d581a6981ec2b08ee623314cd1d1c4` | Zlib | Verifies EdDSA signatures |

## Signatures of the downloads

Neither release has a detached signature. The GitHub release page records a SHA-256 for each
file, and the checksums in the records are the same as the ones computed from the downloaded
files. That ties the files to the release page, not to an independent root of trust.

- Sparkle: the framework carries a code signature (`Versions/B/_CodeSignature/CodeResources`).
  It was not checked on the machine that made the records.
- WinSparkle: `WinSparkle.dll` has no Authenticode signature (the PE file has no certificate
  table). The release tag is signed with PGP. The tag signature was not checked.

## What each updater does (read from the recorded releases)

Where a line is quoted, it is from the page named after it, as read when the record was made.

### Sparkle 2.10.1

- Updates are verified with an EdDSA (Ed25519) signature. Documentation
  (`https://sparkle-project.org/documentation/publishing/`): "In order to prevent corruption and
  man-in-the-middle attacks against your users, you must cryptographically sign your updates." The
  signature is the `sparkle:edSignature` attribute of the appcast's `enclosure`. The public key is
  `SUPublicEDKey` in `Info.plist` (`https://sparkle-project.org/documentation/customization/`):
  "The base64-encoded public EdDSA key."
- The feed address can be set at run time
  (`https://sparkle-project.org/documentation/publishing/`): "The recommended way to change the
  feed URL programmatically is using -[SPUUpdaterDelegate feedURLStringForUpdater:]." The
  `setFeedURL:` call and the `feedURL` property still exist and are deprecated. `SUFeedURL` in
  `Info.plist` stays the default.
- Automatic checks can be turned off. `SUEnableAutomaticChecks` set to NO "disables automatic
  checking for updates by default, also without asking your users for permission". The user's
  choice is changed at run time with `automaticallyChecksForUpdates`. When the key is not set,
  Sparkle asks on the second launch.
- The system profile is not sent unless it is enabled. `SUEnableSystemProfiling` has the
  default NO. In the source (`SUUpdatePermissionPrompt.m`, `SPUUpdaterSettings.m`) the profile is
  offered only when that key is YES, and it is sent only when the stored user choice
  `SUSendProfileInfo` is YES; unset means NO.
- Ad hoc signed bundles. The validator (`Sparkle/SUUpdateValidator.m`, tag 2.10.1) accepts an
  update when the archive's EdDSA signature is valid for the key in the installed app, or when
  the new app's code signature matches the old one. With an EdDSA key present, an ad hoc signed
  app is updated by a valid EdDSA signature. Two rules still apply: an installed app that carries
  any code signature is not replaced by an unsigned one (the check asks only whether a designated
  requirement exists, which an ad hoc signature has), and the key may be rotated but not
  removed. The source's own text: "If no Apple Code Signing certificate is
  available, adhoc signing can be used at minimum." Team identifiers are used only for the
  fallback that checks a Developer ID signature; an ad hoc signature has none.
- Versions are compared as `sparkle:version` of the appcast item against the installed app's
  `CFBundleVersion` (`SUStandardVersionComparator`). The rule: the text is split into runs of
  digits, runs of letters, and separators; digit runs compare as numbers; letter runs compare as
  text; a missing number part counts as 0; a letter part that the other version does not have
  makes that version older (a pre-release is older than the release). So `0.1.0a1` is older
  than `0.1.0a2`, and both are older than `0.1.0`. Reading stops at the first `-`, so
  `0.1.0-alpha.1` compares equal to `0.1.0`. Bloom's build version must therefore not use a
  dash for a pre-release.
- Minimum macOS: 12.0. The release notes say "macOS 12.0 or later is required", and the
  framework's `Info.plist` has `LSMinimumSystemVersion` 12.0. An appcast item can say so with
  `sparkle:minimumSystemVersion`.
- `Info.plist` keys it reads (`SUConstants.m` and the customization page): `SUFeedURL`,
  `SUPublicEDKey`, `SUPublicDSAKey`, `SUPublicDSAKeyFile`, `SUEnableAutomaticChecks`,
  `SUScheduledCheckInterval`, `SUScheduledImpatientCheckInterval`, `SUAutomaticallyUpdate`,
  `SUAllowsAutomaticUpdates`, `SUPromptUserOnFirstLaunch`, `SUEnableSystemProfiling`,
  `SUShowReleaseNotes`, `SUBundleName`, `SUDefaultsDomain`, `SURelaunchHostBundle`,
  `SUVerifyUpdateBeforeExtraction`, `SURequireSignedFeed`, `SUEnableInstallerLauncherService`,
  `SUEnableDownloaderService`, `SUEnableJavaScript` and `SUAllowedURLSchemes`; and the bundle's
  `CFBundleVersion`, `CFBundleShortVersionString`, `CFBundleName`, `CFBundleIdentifier` and
  `LSMinimumSystemVersion`.

### WinSparkle 0.9.4

- EdDSA signatures came with 0.9.0 (`NEWS`: "Added support for EdDSA signatures."), and 0.9.1
  checks the key passed in (`win_sparkle_set_eddsa_public_key()`). 0.9.4 is the newest stable
  release and has both. The key is set with `win_sparkle_set_eddsa_public_key()` or as the
  resource `EdDSAPub` of type `EDDSA`. The signature is the `sparkle:edSignature` attribute of
  the `enclosure`.
- With a key set, an update without a valid signature is refused. In `src/updatedownloader.cpp`
  the downloaded file is checked with `VerifyEdDSASignatureValid`, and an empty signature throws
  `BadSignatureException("Missing EdDSA signature!")`; the file is deleted and the user sees a
  bad signature error. With no key set at all the update is accepted and the log says "Using
  unsigned updates!".
- Calls, all before `win_sparkle_init()` unless noted (`include/winsparkle.h`):
  `win_sparkle_set_appcast_url()` (http and https only; the header says to use https),
  `win_sparkle_set_automatic_check_for_updates()` ("If disabled,
  win_sparkle_check_update_with_ui() must be used explicitly."),
  `win_sparkle_set_app_details()` (company, application, version),
  `win_sparkle_set_app_build_version()` (the build compared with the appcast's `sparkle:version`,
  as `CFBundleVersion` is on macOS; the appcast then also needs `sparkle:shortVersionString`),
  `win_sparkle_set_can_shutdown_callback()` and `win_sparkle_set_shutdown_request_callback()`
  (both are called from a thread that is not the main thread; the second is called only after the
  first returned true), and `win_sparkle_check_update_with_ui()` (a manual check; it ignores
  "Skip this version"; callable after init).
- The appcast reads `sparkle:installerArguments` on the `enclosure` and passes it to the
  downloaded installer (`ShellExecuteEx` parameters). It reads `sparkle:os` on the `enclosure`:
  `windows`, `windows-x64`, `windows-x86` or `windows-arm64`; an enclosure for another OS is
  ignored, and the most specific match wins.
- Versions are compared like Sparkle's, with one difference: `-` is an ordinary character in a
  letter run and does not end the reading (`src/updatechecker.cpp`). `0.1.0a1` is older than
  `0.1.0a2`, and both are older than `0.1.0`. `0.1.0-alpha.1` is older than `0.1.0` (not equal,
  as on macOS).
- Settings are kept in the registry under
  `HKEY_CURRENT_USER\Software\<company>\<application>\WinSparkle` (company and application from
  `win_sparkle_set_app_details()` or the version resource). `win_sparkle_set_registry_path()`
  moves them to another key below `HKCU`. `win_sparkle_set_config_methods()` (since 0.7) replaces
  reading, writing and deleting with the application's own functions, so nothing is written to the
  registry.
