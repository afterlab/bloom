# Releasing Bloom

Status: working

A release is made by creating a version tag. The release workflow
(`.github/workflows/release.yml`) builds every release file from the tagged commit on hosted
runners, checks them, and publishes the GitHub pre-release once the maintainer approves it. This
page is the maintainer's order of work. The design of the update feed and of the updaters is in
[ADR 0041](../decisions/0041-self-update.md); what the installers do is in the
[platform contract](../architecture/platform-support.md).

## What a release holds

| Platform | Files |
| --- | --- |
| macOS (arm64) | `Bloom-<version>-macos-arm64.dmg`, `.zip` (what the updater downloads) and `.pkg` |
| Windows (x86-64) | `Bloom-<version>-windows-x86_64-setup.exe` and `.zip` |
| Linux (x86-64) | `install.sh`, `Bloom-<version>-linux-x86_64.tar.xz`, `.AppImage` and `.AppImage.zsync`, and the `PKGBUILD` of the Arch Linux package |
| All | `SHA256SUMS`, `SHA256SUMS.sig`, the update feed `alpha.json`, `appcast-macos.xml` and `appcast-windows.xml` |

The release notes are the version's section of `CHANGELOG.md`. The same text is the body of the
GitHub release, the notes in the update feed and the notes in the two appcasts.

A second, fixed release named `alpha-channel` holds the files every installed copy reads: the
feed, the appcasts, the install script and the AppImage's control file. Each release replaces
them. Nobody downloads Bloom from it.

## Before the tag

1. `CHANGELOG.md` has a dated section for the version, written for users, and an `Unreleased`
   section above it. `bloom.quality.changelog` checks the form.
2. The default of `BLOOM_PACKAGE_VERSION` in the root `CMakeLists.txt` is the version. A source
   build of the tag then reports the tag's version without a flag.
3. The commit passes the full validation on the project's own Linux, macOS and Windows hardware,
   GPU tests included (`AGENTS.md`, "Platform Validation Before Landing"). Hosted runners have
   no GPU, so nothing the workflow runs stands in for this.
4. That validation is recorded on the commit as the commit status `bloom/hardware-validation`
   with the state `success`:

   ```sh
   gh api repos/afterlab/bloom/statuses/<commit> -f state=success \
       -f context=bloom/hardware-validation -f description="Linux, macOS, Windows: full suite"
   ```

   The workflow refuses a tag whose commit has no such status, or whose newest status of that
   name is not `success`.

## The rehearsal

Before the tag, the workflow is run once on the commit that will be tagged: Actions > Release >
Run workflow, on the branch, with **rehearsal** checked and no tag.

```sh
gh workflow run release.yml --ref main -f rehearsal=true
```

A rehearsal builds that commit as the version the source tree declares and runs every job up to
and including the release candidate: the three builds, the package checks, the clean-system
starts, the real installs and the desktop captures. It is how what only a hosted runner can show
is seen before a tag exists. The last job does not run, so a rehearsal never waits for approval,
never reads the key, and never creates or changes a release, a tag or the files of
`alpha-channel`. It does not need the validation record. Where the two repository variables are
not set yet, it builds with a public key made for the run, whose private half is deleted in the
same step, and with the address of `alpha.json` among the files of `alpha-channel`.

Its artifacts are the ones a release run makes: `release-candidate`, which the end-to-end
update test takes (below), and `desktop-captures`.

## The tag

```sh
git tag v0.1.0-alpha.1 <commit>
git push origin v0.1.0-alpha.1
```

The workflow starts when the tag is created. It also starts by hand (Actions > Release > Run
workflow, with the tag's name), which builds an existing tag again. Apart from a rehearsal it
never runs for a branch, and never for a pull request, and it runs although the tagged commit's
message skips the other workflows. A
tag that is not a version tag (`v<major>.<minor>.<patch>`, optionally `-alpha.<n>`, `-beta.<n>`
or `-rc.<n>`) ends the run at its first job.

## What the workflow does

1. **Tag, validation record and settings.** The tag is the tree's version, the changelog has
   its section, the commit carries the validation status, and the two build settings are set.
2. **Linux, macOS and Windows packages**, each on a hosted runner: the dependency prefix from
   the lock (cached between runs), Bloom, the package, and the package check with its contents,
   resolution, version, icons and run rules, and the `release` rule, which requires the feed
   address and the public keys of the release in the built application and refuses a test
   build. The GPU rule is not run and is reported as not run. Linux is built inside the
   official Ubuntu 22.04 image by digest (`tools/ci/linux-package`). The macOS job also makes
   and inspects the disk image and the installer package, and the Windows job installs,
   upgrades and uninstalls with the installer (`bloom.package.installers` on each). Each job
   also keeps its platform's debug symbols as an artifact (below).
3. **Clean start** of the Linux files on bare Ubuntu 22.04, Ubuntu 24.04, Debian 12 and
   Fedora 44 images (`tools/ci/linux-package/clean-system.sh`, then `start.sh`): the tar, the
   AppImage, and the install script, which installs, runs and renders through the linked
   `bloom-cli`, and uninstalls. The install script form is signed there with a key made for the
   run, because the release's key is not available to that job.
4. **Real installs and desktop captures**, each on a fresh hosted machine with the files the
   build jobs made:
   - macOS: `tools/ci/macos-package/installed.sh` installs the installer package with the
     system's installer (`sudo installer -pkg`), requires `/Applications/Bloom.app` and the
     `/usr/local/bin/bloom-cli` link, runs the package check's contents, version and run rules
     through that link, which renders a frame headless, and attaches the disk image and reads
     its window layout.
   - Windows: the installer runs silently for the current user, as a double click would
     install, and is uninstalled afterwards.
   - Linux: the install script installs on a bare Ubuntu 24.04 image.

   Then `tools/ci/captures` takes pictures of the installed application on each desktop: its
   icon in Finder, the Dock, the opened disk image and the Help menu; in the Start menu, Explorer
   and the taskbar; in an application menu, a launcher and its own window on Linux. The three
   sets are the artifact `desktop-captures`. A picture that cannot be taken fails the run.
5. **Release candidate.** The three platforms' files with `release-notes.md`, `install.sh`,
   `PKGBUILD`, `.SRCINFO` and `SHA256SUMS`, as the artifact `release-candidate`. Nothing is
   signed yet. A rehearsal ends here. Before it, the macOS job has run the end-to-end update
   test on its own zip (below), and the candidate is not assembled when that failed.
6. **Sign and publish**, after the approval (below).

## Debug symbols

A crash report's minidump names code addresses only (issue #736,
[Crash reports](../user-guide/crash-reports.md)). The release builds are configured with
`BLOOM_DEBUG_SYMBOLS=ON` (`cmake/BloomDebugSymbols.cmake`): the same optimization with debug
information added, and on Linux a GNU build id in every program. After the package is made, the
`bloom-debug-symbols` target writes the symbols of each shipped program (the application,
`bloom-cli`, `bloom-mcp`, the workers, the update helper, `bloom-crash-handler` and the Python
extension module) into the build's `symbols` folder, and the job uploads it:

| Artifact | What it holds | A minidump's module is matched by |
| --- | --- | --- |
| `symbols-linux` | `<program>.debug`, split from the program with `objcopy --only-keep-debug` | the GNU build id |
| `symbols-macos` | `<program>.dSYM`, made with `dsymutil` | the Mach-O UUID |
| `symbols-windows` | `<program>.pdb`, written by the linker | the PDB signature and age |

The symbols are never in a package or among the published files: the Linux package is installed
with its programs stripped, a macOS program's debug information stays in the build's object files
until `dsymutil` collects it, and a Windows PDB is not installed. The release builds also pass
the tagged commit as `BLOOM_SOURCE_REVISION`, which every text report names on its `Build:` line.

Workflow artifacts expire after the repository's artifact retention period. Download the three
`symbols-*` artifacts of each published release from its run and keep them with the release's
records for as long as testers may send reports from it.

## The end-to-end update test

The macOS files are tested by the workflow itself, in the job that built them
(`tools/ci/macos-package/update-test.sh`; its result is the job **macOS end-to-end update** and
its output the artifact `update-test-macos`). The platform's updater on macOS only knows a
Bloom that runs with its window: it asks the system's launcher whether Bloom runs, has it ask
Bloom to quit, and has it start the new version. The test therefore needs a desktop it may
use, shows Bloom's window on it, and gives the launcher one variable for the applications it
starts while a part runs. A hosted machine is such a desktop and is thrown away afterwards.
The test refuses to run on macOS without `--desktop`, and nobody gives it that on a personal
computer.

The Linux and Windows files are tested on the project's machines while the run waits for
approval (a rehearsal's candidate is tested the same way, before the tag):

```sh
gh run download <run> -n release-candidate -D candidate
python3 -I tools/release/update_e2e.py --installed <test build's file> \
    --candidate candidate/<the same kind of file> --work <an empty folder>
```

`--installed` is a test build of an older version, packaged the same way: configured with
`-DBLOOM_UPDATE_TEST_BUILD=ON -DBLOOM_PACKAGE_VERSION=<an older label>` and, on macOS and
Windows, `-DBLOOM_UPDATE_PUBLIC_KEYS=<the test key>`, which
`python3 -I tools/release/update_e2e.py --print-test-key` prints. Only a test build reads its
feed address and key from the environment and installs without a question; the package check
refuses to release one.

The test installs the test build, serves a release of the candidate from the same machine,
and requires that the candidate replaces the test build and confirms its start. Then it
installs the test build again, serves a candidate that cannot start, and requires that the
update helper puts the test build back and starts it. It handles the Linux tar and AppImage,
the macOS zip and the Windows installer. Bloom runs under the package check's own identity, so
the settings of the person at the machine are not touched, and on Linux and Windows without a
window. On macOS the test also requires that the platform's updater waited for Bloom to quit
before it replaced the application. The test build trusts the test's key and the candidate
carries the release's, where two releases carry the same key; so the test puts its own key
into the candidate's `Info.plist` and seals the bundle again, and the programs in it stay the
candidate's. An update from one released version to the next, with the release's key on both
sides, is therefore first seen with the second release.
A part that fails prints the requests its release was asked, the last lines of Bloom's own
output and the files of the update folder.

## Approval

The job **Sign and publish** names the environment `release`, whose required reviewer is the
maintainer. It is the only job that waits, and the only one that can read the signing key. After
the approval it:

- signs each release file, `SHA256SUMS` and the feed with `tools/release/sign_release`, and
  refuses a key that is not one of the public keys the builds carry;
- writes the feed and the two appcasts (`tools/release/release.py publish`) and verifies the
  feed with the public key;
- installs from the signed files with the published `install.sh`, which checks the real
  signature and checksum, and uninstalls again;
- creates the GitHub pre-release for the tag with the release notes and the files, and
  refreshes the files of `alpha-channel`.

A release that exists is never replaced: the job stops when the tag already has one.

## What the repository holds for this

- Repository variables `BLOOM_UPDATE_PUBLIC_KEYS` (Base64 of each 32-byte Ed25519 public key,
  separated by commas; a second key is how a key is rotated) and `BLOOM_UPDATE_FEED_URL` (the
  stable HTTPS address every build reads the feed from; it redirects to `alpha.json` among the
  files of `alpha-channel`).
- The environment `release` with its required reviewer and the secret
  `BLOOM_UPDATE_SIGNING_KEY` (the Ed25519 private key as PKCS#8 PEM, without a passphrase).

The variables are repository variables because the build jobs read them, and a job outside an
environment cannot read that environment's variables.

## After the release

- The `PKGBUILD` and `.SRCINFO` of the release are in the candidate. Publishing them to the AUR
  is a maintainer step (`packaging/aur/README.md`).
- `docs/roadmap.md` and `docs/README.md` list the version as released, with the changelog's
  date.

## The tools

| File | What it does |
| --- | --- |
| `tools/release/release.py` | The workflow's steps: `version`, `validation`, `settings`, `candidate`, `publish` |
| `tools/ci/macos-package/installed.sh` | Installs the macOS installer package for real on a hosted machine and checks the result |
| `tools/ci/macos-package/update-test.sh` | Makes the macOS test build in the workflow's build directory and runs the end-to-end update test on the built zip |
| `tools/ci/captures` | The desktop captures of the installed application, one script per platform |
| `tools/release/changelog.py` | Checks the changelog and writes a version's release notes |
| `tools/release/sums.py` | Writes and checks `SHA256SUMS` |
| `tools/release/sign_release` | Signs files, the checksum list and the feed; the only program that reads the key |
| `tools/release/ed25519_route.py` | Picks the signing program: `openssl` (OpenSSL 3), or Bloom's own `bloom-ed25519-tool` on a machine without it |
| `tools/release/feed.py`, `appcast.py` | Write the feed and the appcasts |
| `tools/release/update_e2e.py` | The end-to-end update test |
| `cmake/package/make_install_script.cmake`, `make_pkgbuild.cmake` | Write a release's `install.sh` and `PKGBUILD` |

`bloom.release.tools_tests` and `bloom.release.script_tests` run them on stand-in releases with
RFC 8032's public test keys, and check the rules the workflow's text must keep: it starts only
for a created tag or by hand, every action is pinned to a commit, the validation record is
required before anything is built, only the last job waits for approval and reads the key, and
a rehearsal builds the commit it was started for and publishes nothing.
