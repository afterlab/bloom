# ADR 0041: Self-Update

Status: accepted

Date: 2026-11-17

Issues: #734

## Context

Bloom v0.1.0-alpha.1 is the first build testers install. Every later alpha has to reach them
without a manual reinstall, on three platforms and through several kinds of install: a macOS
bundle, a Windows installer or zip, a Linux AppImage, the install script, an unpacked tar, and a
package manager's package.

An updater replaces the program a user trusts with bytes from the network, so the design starts
from what must never happen: a file that the project did not release is installed, an installation
is left half replaced, or a version that cannot start replaces one that can.

## Decision

### One signed feed per channel

Each release publishes one small file for its channel (`alpha.json` for the alpha channel), the
**update feed** ([schema](../../schemas/update/feed-1.schema.json)). It gives the newest version of
the channel, the release date, the release notes (the version's section of `CHANGELOG.md`) and, for
each platform, the release files with their address, size, SHA-256 and Ed25519 signature.

The feed is a DSSE envelope (the Dead Simple Signing Envelope): the payload type, the payload in
Base64 and one or more Ed25519 signatures over both. The signed bytes are exactly the bytes that
are read, so a reader parses nothing before it has checked a signature. A release build carries the
public keys it accepts (one today, two while a key is replaced) and the feed's address as build
settings: `BLOOM_UPDATE_PUBLIC_KEYS` and `BLOOM_UPDATE_FEED_URL`. The source tree carries neither;
a build without them says that updates are not configured and contacts nothing.

The channel is the first pre-release word of a version (`alpha` for `0.1.0-alpha.1`, `stable` for
a version without one). A build reads its own channel's feed and refuses a feed of another.

### What is refused

`src/update` (Qt-free, the target `bloom_update`) reads the feed and checks files. Its order is
fixed:

1. The feed: size limit, envelope shape, a signature by a carried key, then the content (form,
   channel, version, HTTPS addresses). Each refusal has a typed reason (`UpdateRefusal`).
2. Only a version newer than the running one is offered, by Semantic Versioning precedence. An
   older signed feed served again changes nothing.
3. A downloaded file is accepted only when its size, its SHA-256 and its Ed25519 signature are the
   ones the verified feed gives. The file is read once and all three are checked; a file that
   fails is never handed to anything that installs it.

There is no switch, variable or setting that weakens a check in a release build. A test build
(`BLOOM_UPDATE_TEST_BUILD`) may be pointed at a feed on the same machine with a test key through
two environment variables, and a third (`BLOOM_UPDATE_TEST_INSTALL_NOW`) makes it install the
update it is offered without a question, which is what the end-to-end update test
(`tools/release/update_e2e.py`) needs. It checks every signature and checksum all the same, and
everything after the answer is the course a release takes: on macOS Sparkle asks Bloom to quit,
replaces the bundle and starts the new version through the system's launcher. The
package check refuses to publish such a build: its `release` rule asks the application what it
was built with (`Bloom --update-settings`) and passes only a build that carries the release's
feed address and public keys and is not a test build.

A feed cannot prove that it is the newest one: someone who controls the network can keep serving
an old signed feed, which leaves a user on an old version and never installs anything. The feed is
fetched over HTTPS, which is the protection against that.

### Signatures and checksums are OpenSSL's

Bloom verifies and never signs. The Ed25519 check and the SHA-256 of an update are made by the
locked OpenSSL (`libcrypto` 3.5.9): `EVP_DigestVerify` with a raw Ed25519 public key
(`src/update/ed25519.cpp`) and `EVP_sha256` (`src/update/sha256.cpp`). `src/update` holds no
cryptography of its own. A check of the update path is security code, and an established
library that the release tools also sign with is preferred over a second implementation.

The library is the one the dependency prefix already builds for the carried Python interpreter,
so the lock and the packages gain no component:

- Linux and macOS: the static `libcrypto` of the prefix is linked into every program that
  checks an update. Its names are kept out of the program's exported names, because the
  interpreter's own modules hold the same library.
- Windows: `libcrypto-3-x64.dll` is the file the package already carries beside the
  interpreter's modules. A program imports it late and loads it by its full name from there
  before the first check (`src/update/crypto_library.cpp`), so no second copy is carried, a
  program that never checks an update needs no library file, and nothing is ever taken from the
  search path. A program that belongs to a package looks nowhere else; a program run from a
  build folder takes the same file from the dependency prefix. When the file is missing every
  check is refused with the reason "verification unavailable", and nothing is installed.

Ed25519 in its plain form is defined over the whole message at once, and OpenSSL verifies it in
one call. A downloaded file is therefore first read in pieces for its size and SHA-256, and only
a file that passes both is given to the signature check, as a read-only mapping of the file
(`src/update/file_verification.cpp`), never as a second copy in memory.

The tests pin the behavior: the three signatures RFC 8032 prints, signatures made by the
`openssl` program over the fixtures, and every way a signature must fail (changed message,
changed signature, another key, a key that is no curve point, and a second half that is not
below the group order); a tampered file, a wrong signature and a wrong checksum are each
refused, and so are a tampered feed and a feed signed by an unknown key.

Signing is done by the release tools with the `openssl` program (OpenSSL 3;
`tools/release/sign_release`), in the plain form (the signature covers the file's bytes), which
is also what the platform updaters and the install script verify. The release workflow signs
that way. A machine whose `openssl` is another program (macOS) has no route to it, so the
release tools' tests and the end-to-end update test sign their stand-in releases there with
`bloom-ed25519-tool`, a build tool made from the same `libcrypto`
(`src/update/ed25519_tool_main.cpp`, chosen by `tools/release/ed25519_route.py`). No package
carries it. Where a machine has both programs a test requires that they give the same public
key and the same signature.

The AppImage delta update reads the zsync control file format, whose block checksums are MD4
and whose file checksum is SHA-1. Those are that format's own, are implemented in
`src/update/zsync.cpp`, and decide nothing about trust: the assembled file passes the same
size, SHA-256 and signature checks as any download before it is used.

### How each install is updated

`bloom::update::detectInstall` decides the kind of install from what is around the program:

| Install | How it is recognized | How it is updated |
|---|---|---|
| macOS bundle | the program is inside `Bloom.app` with the package's records | Sparkle 2 reads the macOS appcast named in the verified feed |
| Windows installer | the installer's marker beside the program | WinSparkle reads the Windows appcast named in the verified feed and runs the next installer silently |
| Windows zip | no marker | Bloom says where the next version is and changes nothing |
| Linux AppImage | the AppImage runtime's variables, and the program is inside that mount | Bloom downloads the next AppImage (as a delta when it can) and the update helper replaces the file |
| Install script, unpacked tar | the script's marker or none, in a folder the user can replace | Bloom downloads the tar and the update helper swaps the folder |
| System-wide install | the same, in a folder the user cannot replace | Bloom says that the installer must be run again |
| Package manager | a marker that names the package manager | Bloom says that a newer version exists and leaves it to the package manager |
| Build folder | no package records | never updated |

The marker is the file `install-kind` in the package's record directory, written by whatever
installed the package. A marker Bloom does not understand is treated as a package manager's.

The platform updaters read an appcast, not Bloom's feed. A release publishes
`appcast-macos.xml` and `appcast-windows.xml`; their addresses are inside the signed feed and
Bloom hands them to the two libraries at run time, after its own check. The libraries verify the
archive they download with the same public key. The appcast and the bundle carry the version in a
compact form (`0.1.0a1`), because the libraries' own comparison ranks `0.1.0-alpha.1` above `0.1.0`.

### The update helper

On Linux, Bloom itself downloads the file as a background job (progress, cancel, never on the
interface thread), verifies it, and prepares the new folder or file beside the installed one. A
small separate program, `bloom-update-helper`, then does the replacement after Bloom has exited.
It uses the standard library and operating system calls only, runs from a copy outside the
installation, and writes a journal so that an interrupted run is finished or undone by the next
one:

1. wait for Bloom to exit;
2. put the prepared version in the installed version's place and keep the installed one as
   `previous`;
3. start the new version and wait for it to confirm that it started;
4. if it does not confirm, put the previous version back, start it, and tell the user.

Step 2 is one exchange of two names where the system has one (Linux and macOS), so there is no
moment without an installed version; elsewhere it is two renames on one volume, and a run that
was stopped between them is finished by the next one. Nothing is copied over an installation,
so an installation is never half replaced. One previous version is kept until the next update
succeeds. On macOS and Windows, where the platform's updater replaces the installation, the
helper runs in a watch mode: it keeps the previous version and restores it when the new one does
not confirm its start.

A package tar is unpacked with the system's `tar` and `xz` programs, which the install script
needs as well.

### The platform updaters

On macOS and Windows Bloom still checks and verifies its own feed first. Only then does it give
the platform's updater the address of the appcast the feed names
(`bloom::ui::PlatformUpdateDriver`, one driver per platform). The updater never checks by
itself: its own automatic check, its own permission question and its system profile are off, so
the automatic check stays Bloom's, at most once a day, behind Bloom's one switch.

Neither library is linked. The macOS driver loads `Sparkle.framework` from the bundle's
`Frameworks` folder and finds its classes by name; the Windows driver loads `WinSparkle.dll` by
its full path from beside the program and finds its functions by name. A build folder, a test
program and an unpacked Windows zip therefore never need the library, and a copy that lacks it
simply has no driver: the update window then says where the next version is. The driver exists
only in a build with an update key, for a copy the platform's updater can replace (`Bloom.app`,
or a copy the Windows installer installed).

The macOS driver also exists only when Bloom runs on the system's own window platform.
Sparkle's installer asks the system's launcher whether Bloom runs, has it ask Bloom to quit,
and replaces the bundle once the launcher says Bloom is gone. A Bloom started without a window
(a script's or a test's run on another Qt platform) is not an application the launcher knows:
Sparkle takes it for closed and replaces the bundle at once, under the running program and
without the kept copy below. Sparkle accepts an update whose archive has the Ed25519 signature
of the key the installed bundle carries; the bundle's ad hoc code signature differs between
any two builds, which Sparkle notes in the system's log and does not refuse, because one of
the two checks is enough for it.

Before the installation is replaced, the driver starts the helper in its watch mode:

- macOS: when Sparkle is about to replace the bundle and start Bloom again, the installed bundle
  is copied to `previous/Bloom.app` in the update folder (a clone on the system's own volume
  format, made off the interface thread), and the helper is started with a plan that names it.
  That restart is the only way an update is installed. Sparkle would otherwise ask once more
  when the update is unpacked ("ready to install"), and while that question is open its
  installer already holds the update and installs it the moment Bloom is gone, without the
  copy and without the watch; an artist who quit Bloom then would get an update nothing can
  put back. So Bloom stands between Sparkle and its window and answers that one question with
  "install" at once: the artist's "Install Update" downloads, unpacks, keeps the copy, starts
  the watch and asks Bloom to quit, and unsaved work is Bloom's own question at that quit.
- Windows: WinSparkle first asks whether Bloom can close. Bloom answers yes only when nothing is
  unsaved and nothing is running, because the installer needs Bloom gone and a close that stops
  to ask would leave it waiting. WinSparkle then starts the installer, which waits for Bloom to
  close and renames the installed folder to `<folder>.previous`; Bloom starts the helper with a
  plan that names that folder, and closes. The installer shows its progress window from Bloom's
  close until it starts the new version, and never installs without the kept folder: when the
  folder stays in use it installs nothing, and an install that installs nothing, fails or stops
  puts the installed version back, leaves it a notice and starts it, which the helper's watch
  sees as the installed version starting again. A Bloom opened by hand while the installer runs
  shows the installer's window and ends at once (#788).

The helper waits for Bloom to exit and then for the new version to confirm its start. It waits
for the exit as long as Bloom runs, because the platform's updater installs whenever Bloom is
gone: a quit that stops at unsaved work and is finished an hour later is still followed by the
installation, and still watched. The wait for the confirmation is long (15 minutes), because
the installation itself happens inside it. When the confirmation
does not come and the kept version exists, the helper puts it back, starts it and leaves the
notice that version shows. When the watch cannot be started the update goes on without it.

What stays outside the watch on macOS: a Bloom that ends by itself (a crash) in the moment
between Sparkle's unpacking and the kept copy, and a watch that the system ended while it
waited (a logout or a restart). Sparkle may still install in both, and the new version then
starts without a helper beside it.

### The AppImage's delta

An AppImage is one large file of which little changes between two alphas, so a release publishes
a control file beside it (`<name>.AppImage.zsync`, the zsync 0.6.6 format that AppImage tools
read): the file's length and, for each block, a short rolling checksum and a short MD4 checksum.
Bloom reads the control file, finds the blocks the installed AppImage already holds, and asks
the server for the other byte ranges only.

The control file is listed in the signed feed like every other release file and is checked the
same way before it is read. Its block checksums are short and prove nothing; they only decide
which bytes are copied and which are fetched. The file put together from both is then checked
exactly as a whole download is (size, SHA-256, Ed25519 signature), so a wrong block can only
cause a refusal. Anything that goes wrong on the way (no control file, a server without range
requests, too little in common, a damaged range) ends in the ordinary whole download; a delta
never makes an update fail that a whole download would complete.

`bloom-zsync-make`, a build tool from the same source as the reader, writes the control file
when the AppImage is made; a test compares its output with the reference tool's, byte for byte.
The AppImage's runtime also carries the update information text (`.upd_info`) that says where
the control file is published; it is the build setting `BLOOM_UPDATE_APPIMAGE_INFORMATION` and
empty in a developer build.

### The check

Help > Check for Updates checks on demand. An automatic check is on by default and runs at most
once a day (`automaticCheckDue`, with the caller's clock); Preferences turns it off. The request
is a plain GET of the feed's address: no query, cookie or identifier is sent.

## Consequences

- `src/update` is a new declared module with the edges `update -> core, platform`; `src/ui` and
  the applications link it.
- One Ed25519 key signs the feed and every release file for all three platforms. Replacing it
  takes two releases: one signed by both keys that carries both public keys, then one by the new
  key alone.
- Operating system code signing is separate from this and is not part of it. The update signature
  protects the path from the release to an installed Bloom; it does not change how a system
  treats a first download.
- A release that cannot be verified is refused with its reason, and the installed version keeps
  running.
