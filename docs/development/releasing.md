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

## The tag

```sh
git tag v0.1.0-alpha.1 <commit>
git push origin v0.1.0-alpha.1
```

The workflow starts when the tag is created. It also starts by hand (Actions > Release > Run
workflow, with the tag's name), which builds an existing tag again. It never runs for a branch or
a pull request, and it runs although the tagged commit's message skips the other workflows. A
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
   upgrades and uninstalls with the installer (`bloom.package.installers` on each).
3. **Clean start** of the Linux files on bare Ubuntu 22.04, Ubuntu 24.04, Debian 12 and
   Fedora 44 images (`tools/ci/linux-package/clean-system.sh`, then `start.sh`): the tar, the
   AppImage, and the install script, which installs, runs and renders through the linked
   `bloom-cli`, and uninstalls. The install script form is signed there with a key made for the
   run, because the release's key is not available to that job.
4. **Release candidate.** The three platforms' files with `release-notes.md`, `install.sh`,
   `PKGBUILD`, `.SRCINFO` and `SHA256SUMS`, as the artifact `release-candidate`. Nothing is
   signed yet.
5. **Sign and publish**, after the approval (below).

## The end-to-end update test

While the run waits for approval, the candidate is tested on the three machines:

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
the macOS zip and the Windows installer. Bloom runs without a window and under the package
check's own identity, so the settings of the person at the machine are not touched.

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
| `tools/release/changelog.py` | Checks the changelog and writes a version's release notes |
| `tools/release/sums.py` | Writes and checks `SHA256SUMS` |
| `tools/release/sign_release` | Signs files, the checksum list and the feed; the only program that reads the key |
| `tools/release/feed.py`, `appcast.py` | Write the feed and the appcasts |
| `tools/release/update_e2e.py` | The end-to-end update test |
| `cmake/package/make_install_script.cmake`, `make_pkgbuild.cmake` | Write a release's `install.sh` and `PKGBUILD` |

`bloom.release.tools_tests` and `bloom.release.script_tests` run them on stand-in releases with
RFC 8032's public test keys, and check the rules the workflow's text must keep: it starts only
for a created tag or by hand, every action is pinned to a commit, the validation record is
required before anything is built, and only the last job waits for approval and reads the key.
