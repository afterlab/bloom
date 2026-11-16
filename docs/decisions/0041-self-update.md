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
(`tools/release/update_e2e.py`) needs. It checks every signature and checksum all the same. The
package check refuses to publish such a build: its `release` rule asks the application what it
was built with (`Bloom --update-settings`) and passes only a build that carries the release's
feed address and public keys and is not a test build.

A feed cannot prove that it is the newest one: someone who controls the network can keep serving
an old signed feed, which leaves a user on an old version and never installs anything. The feed is
fetched over HTTPS, which is the protection against that.

### Ed25519 verification is Bloom's own code

Bloom verifies and never signs, so it needs Ed25519 verification and SHA-512 only. They are
implemented in `src/update` (`ed25519.cpp`, after the public-domain TweetNaCl reference;
`sha512.cpp`), as `src/core` implements SHA-256.

The locked OpenSSL was the alternative. It is built for the carried Python interpreter only:
static libraries in a directory no Bloom target searches on Linux and macOS, and two libraries
inside the interpreter's folder on Windows. Linking it into the application, the command-line
program and the update helper would change the package layout on every platform and put a second
copy of a large library into each program, for one function that reads public values only.
Verification has no secret, so the usual risk of a cryptographic implementation (leaking a key
through timing) does not exist here; what remains is correctness, which the tests pin against
OpenSSL: the three signatures RFC 8032 prints, signatures made by the `openssl` program over the
fixtures, and every way a signature must fail (changed message, changed signature, another key, a
key that is no curve point, and a second half that is not below the group order).

Signing is done by the release tools with the `openssl` program
(`tools/release/sign_release`), in the plain form (the signature covers the file's bytes), which
is also what the platform updaters and the install script verify.

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

Before the installation is replaced, the driver starts the helper in its watch mode:

- macOS: when Sparkle is about to replace the bundle and start Bloom again, the installed bundle
  is copied to `previous/Bloom.app` in the update folder (a clone on the system's own volume
  format, made off the interface thread), and the helper is started with a plan that names it.
- Windows: WinSparkle first asks whether Bloom can close. Bloom answers yes only when nothing is
  unsaved and nothing is running, because the installer needs Bloom gone and a close that stops
  to ask would leave it waiting. WinSparkle then starts the installer, which waits for Bloom to
  close and renames the installed folder to `<folder>.previous`; Bloom starts the helper with a
  plan that names that folder, and closes.

The helper waits for Bloom to exit and then for the new version to confirm its start. The wait
is long (15 minutes), because the installation itself happens inside it. When the confirmation
does not come and the kept version exists, the helper puts it back, starts it and leaves the
notice that version shows. When the watch cannot be started the update goes on without it.

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
