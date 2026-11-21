# Cross-Platform Support Contract

Status: working

Implementation status: the portable staged-artifact interface has tested Linux, macOS and Windows
providers with checked writer close, no-follow exact-stage reopen, bounded verifier reads, explicit
acceptance or rejection, and publication blocked until acceptance. Any other system fails
explicitly as unsupported. Save and open use them with cancellation and progress. What was measured on each platform is in the macOS and Windows
sections below.

Updated: 2026-11-20

## Objective

Linux, macOS, and Windows are first-class Bloom platforms. Cross-platform means the same project can
be authored, opened, evaluated, and rendered with equivalent user-visible semantics on each target;
it does not mean forcing every platform to use the same low-level implementation.

A feature is not complete when it works on only one target. A platform-specific optimization is
acceptable only behind a portable contract with an equivalent implementation or an explicit,
tested fallback on the other supported platforms.

## Product Parity Rules

- Project and render semantics are platform-independent.
- Core authoring workflows, commands, keyboard actions, panel types, and diagnostics exist on all
  three platforms.
- Platform conventions may change labels, modifier keys, menu placement, file dialogs, and other
  presentation details without changing capability.
- No project may require an operating-system-only API merely to open or preserve its editable data.
- An optional capability that is genuinely unavailable must be discovered at runtime, shown before
  use, saved without data loss, and reported clearly when evaluation depends on it.
- A feature backed by a native optimization retains a portable path. If no honest equivalent or
  fallback exists, the feature remains experimental rather than entering the supported product.

“Works on my platform” is not acceptance. Feature reviews and milestone acceptance identify Linux,
macOS, and Windows behavior explicitly.

## Portability Layers

Bloom uses three implementation layers:

```text
Product and domain code
        |
        v
Portable Bloom service interfaces
        |
        +------ Qt/platform adapter
        +------ render backend adapter
        +------ media/VFX library adapter
        |
        v
Operating-system and device APIs
```

Domain code depends on Bloom interfaces, the C++ standard library, and deliberately adopted
cross-platform libraries. It does not call Win32, Cocoa/AppKit, Metal, Direct3D, X11, Wayland, or
other native APIs directly.

Qt 6 owns the common desktop shell and provides the default adapters for windows, menus, input,
clipboard, drag and drop, accessibility, display scaling, and standard dialogs. File dialogs are
always Qt's own widget dialog, styled by the kit, never a native or desktop-portal chooser, so
they look and behave the same on every platform and stay child windows of Bloom's window
([ui-grammar.md](../ux/ui-grammar.md), Dialogs). Narrow native
adapters are allowed where Qt cannot provide the required quality, but they stay below a portable
interface and do not leak native handles or types into document, command, project, runtime, or
render semantics.

Conditional compilation is localized to platform and backend implementation files. Scattered
`#ifdef` branches in product logic are a design warning.

## Files, Paths, And Project Portability

- Canonical project data stores stable asset IDs and portable locators, not raw native handles.
- Serialized paths use a defined representation and never depend on native separator syntax.
- Filesystem access preserves Unicode names and does not assume case sensitivity, case folding,
  drive letters, executable bits, symlink behavior, or a fixed maximum path length.
- Project-relative media remains relative when a project moves between supported systems.
- Missing or inaccessible media produces a relinkable asset state rather than corrupting project
  structure.
- Atomic save uses a platform adapter with tested crash and replacement behavior on every target.
- Cache, configuration, logs, temporary files, and user data use platform-appropriate locations and
  are never embedded into portable project truth.

Projects and fixtures saved on one platform are round-tripped and evaluated on the other platforms
in automated tests.

## Media, Color, And Interchange Dependencies

Open standards and established cross-platform implementations are preferred for media, color, and
interchange. Adopting a standard does not bypass the parity contract:

- the selected library and required feature set must build and be tested on every supported target
- library-specific objects stay behind Bloom-owned interfaces
- unavailable optional codecs, transforms, or plugins report capability diagnostics rather than
  changing interpretation silently
- unsupported data is preserved where the project format promises round-trip safety
- interchange tests use shared fixtures across platforms

Platform media frameworks may be optional accelerators, importers, or exporters. They cannot be the
only implementation of a core project or evaluation semantic unless equivalent support exists on
the other platforms.

## GPU And Display Portability

Feature code targets Bloom's render interfaces rather than a native graphics API. The chosen GPU
strategy may use one portable backend or multiple per-platform backends, but it must provide:

- the same declared node and parameter semantics
- explicit capability discovery and limits
- equivalent color intent and alpha behavior
- consistent cache and invalidation rules
- device-loss handling without loss of project truth
- a CPU reference path for correctness and diagnostics where defined by the render architecture

GPU results may use documented numerical tolerances where hardware arithmetic differs. A tolerance
is not permission for visible, temporal, color-management, or alpha-compositing divergence.
Presentation interop is separate from canonical render resources so changing a window-system or
swapchain integration does not redefine an image.

## Input, Layout, And Artist Workflows

- Every supported command has an action-level identity independent of its displayed shortcut.
- Default shortcuts follow platform conventions where appropriate and remain remappable.
- Mouse, pen/tablet, high-resolution wheel, keyboard, text input, and input-method events enter
  through normalized UI adapters.
- Logical layout is independent of device pixels; mixed-DPI and display-scale changes are tested.
- Panel replacement, workspace restore, focus, drag and drop, and direct manipulation have the same
  user model on all targets.
- Accessibility names, keyboard focus, and non-color status cues are preserved across platform
  adapters.

A native convention may improve familiarity, but platform presentation must not fork Bloom into
three different workflow models.

## Concurrency And Process Behavior

Bloom follows the non-blocking contract in `task-system.md` on every platform. Code must not rely on
one operating system's scheduler, thread priority semantics, filesystem notification behavior, or
process lifecycle behavior for correctness.

Background work uses Bloom task and process services. Platform adapters normalize cancellation,
exit status, path quoting, environment handling, and diagnostics. The UI thread never waits for an
external process or a platform-specific GPU operation.

## Loopback Sockets And Owner-only Secrets

`src/platform` owns the only socket code in Bloom (ADR 0034, `bloom/platform/loopback_listener.hpp`)
and the owner-only secret files (`bloom/platform/owner_only_secret.hpp`), split by backend like the
staged-artifact provider.

| Capability | Linux | macOS | Windows |
| --- | --- | --- | --- |
| Loopback listener | POSIX, `SO_REUSEADDR`, `SOCK_CLOEXEC`, `MSG_NOSIGNAL` | POSIX, `SO_REUSEADDR`, `FD_CLOEXEC`, `SO_NOSIGPIPE` | Winsock 2, `SO_EXCLUSIVEADDRUSE`, non-inheritable sockets |
| Port in use | `EADDRINUSE` | `EADDRINUSE` | `WSAEADDRINUSE`, or `WSAEACCES` under another process's exclusive bind |
| Secret file | 0600 in a 0700 folder under the XDG data home | 0600 in a 0700 folder under Application Support | protected DACL for the current user under `%LOCALAPPDATA%` |
| CSPRNG | `getrandom` | `getentropy` | `BCryptGenRandom` (system-preferred RNG) |

A listener binds the literal IPv4 loopback address only; the API has no address parameter, so a
wildcard or routable bind cannot be expressed. Binding is one synchronous system call, so a taken
port is a typed `PortInUse` naming the port, and the boundary never tries another port. `wake()`
and `close()` unblock a blocked `accept()`, `read()` or `write()` from any thread, and every
blocking call takes a timeout and a stop token. Secret writes replace the file atomically (an
owner-only temporary, flushed, then renamed), a file other users can access is reported as exposed
and never returned, and no message carries the secret. Binding only the loopback address keeps the
Windows Defender Firewall and the macOS application firewall from asking the artist to allow
incoming connections. `bloom.platform.loopback-listener` and `bloom.platform.owner-only-secret`
run on all three platforms.

## Crash Reports

A crash of the application, `bloom-cli` or `bloom-mcp` leaves a minidump and a short text report
in the per-user crash folder (issue #736; the user guide is
[Crash reports](../user-guide/crash-reports.md)). Bloom uses Crashpad, a locked dependency built
from source on all three platforms (`dependencies/superbuild/projects/crashpad.cmake`): the
CMake-built fork that sentry-native maintains, because upstream Crashpad builds only with GN and
depot_tools, which no other Bloom dependency needs. `src/crash` holds Bloom's side and keeps Qt
out; `apps/bloom-crash-handler` is Crashpad's handler entry point built as a Bloom program.

**The handler.** Each program starts the crash reporter (`bloom::crash::startCrashReporter`)
first thing in `main`, before Qt, the interpreter or the GPU exist. It starts
`bloom-crash-handler` from beside the program as a separate process; when the program crashes,
the handler reads the crashed process from outside and writes the minidump, so nothing in the
crashed process has to work. The minidump holds every live thread's stack, the loaded modules
and the memory around each stack. A thread that is exiting at the moment of the crash (a pool
worker retiring, say) is still listed but has already given back its stack, so the minidump holds
none for it (on macOS nothing is mapped at its stack pointer any more). The summary records any
thread but the crashed one without a stack as exited, and the text report names those threads on
an `Exited at the crash (no stack):` line, so no part of a report depends on a thread that was
ending (#756). A program whose handler cannot start (no crash folder, no handler
beside it, a build without Crashpad) runs on and records nothing; it never fails to start
because of crash reporting.

| | Linux | macOS | Windows |
| --- | --- | --- | --- |
| Crash caught by | handlers for the crash signals (SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT and the others Crashpad installs) | the task's Mach exception port, so a signal (an abort) arrives as `EXC_CRASH` | the unhandled-exception filter, and a SIGABRT handler that raises `STATUS_FATAL_APP_EXIT` |
| Stack overflow | every thread gets an alternate signal stack: Crashpad's for the main thread, and a `pthread_create` wrapper linked into each program for the rest (`thread_signal_stack_linux.cpp`) | the exception is handled out of process and needs none of the crashed thread's stack | the exception filter only hands the exception to the handler process, which writes the minidump |
| Handler reads the process by | `ptrace`, permitted by the crashed process | the task port | `ReadProcessMemory` |
| Module identity in the minidump | GNU build id | Mach-O UUID | PDB signature and age |

A process a program starts can inherit its handler on macOS (a child keeps its parent's
exception ports): such a crash is recognized by its process id and kept, marked as a helper's,
but never offered as "Bloom quit unexpectedly".

**Nothing leaves the machine.** The handler gets no upload address, uploads are off in the
database's settings, and Bloom's Crashpad is built with a patch that removes its HTTP transport on
every platform (`dependencies/patches/crashpad/`), so even a given address could not be used.
Linux therefore needs no libcurl.

**The text report.** Every running program keeps its text report ready in
`crashes/sessions/<program>-<process>-<id>.txt`, held open and locked (`flock` on Linux and
macOS; on Windows a share mode without delete and a lock on one byte past the content, because a
Windows lock would otherwise keep the handler from reading the file) so that another program's collection can tell a
running program's file from a crashed one's. It holds the program, version, build (with the
source revision a release build is given, `BLOOM_SOURCE_REVISION`), platform, OS version, process
id and start time, the GPU and driver line the Vulkan device bootstrap records once a device
exists (`bloom::core::recordGpuDevice`), and the last 100 log lines: everything written to
standard error, and the Qt messages in the application. The file is rewritten as the program
runs, on ordinary threads. It is handed to the handler as a Crashpad attachment, so the handler
copies it when the program crashes; nothing is written, allocated or formatted in a signal
handler.

**Collection and pruning.** The next start of any Bloom program collects on a worker thread
(never the UI thread): each finished report in Crashpad's database becomes a folder
`crashes/<date>_<time>_<program>_<id>/` with `minidump.dmp` and `report.txt` (the prepared report
with the crash's time, exception and minidump name added), and leaves the database. Prepared
reports of programs that are no longer running are removed. Then the folder is pruned to the 20
newest crashes, none older than 30 days. A lock file keeps two programs from collecting at once.

**The next-start notice.** The application waits for its own collection off the UI thread, then
offers the newest crash not yet offered (`src/ui/crash_notice.cpp`): "Bloom quit unexpectedly",
with Report on GitHub (a new-issue address with the text report only, shortened from the oldest
log line to fit 8,000 characters, and the minidump shown in its folder to attach by hand), Copy
Report, Open Crash Folder and Dismiss. It is never modal. Showing it records the offer in the
crash's folder, so no later start offers that crash again. It is not shown, and nothing is
recorded, while the session has no window (`QT_QPA_PLATFORM` offscreen or minimal), during the
package check's run, or while an agent drives the session over MCP (#737); the crash then waits for
the next interactive start.

| Crash folder | Linux | macOS | Windows |
| --- | --- | --- | --- |
| `crashes` in | `$XDG_DATA_HOME/Afterlab/Bloom`, else `~/.local/share/Afterlab/Bloom` | `~/Library/Application Support/Afterlab/Bloom` | `%LOCALAPPDATA%\Afterlab\Bloom` |
| `bloom-crash-handler` in a package | `bin` | `Bloom.app/Contents/MacOS` | the package folder |

A test never writes into the user's crash folder: `cmake/BloomTesting.cmake` gives every test
`BLOOM_TEST_CRASH_DIRECTORY` in the build tree, and the reporter takes an absolute value of it as
the crash folder. Tests: `bloom.crash.reports` (minidump streams, the text report, offering once,
pruning to 20 and 30 days, the GitHub address, held files), `bloom.crash.child-*` (a child process
writes through a null pointer on the main thread or on a worker thread, aborts, overflows a worker
thread's stack, or exits cleanly; the check then collects the folder as the next start does and
requires one minidump with the right exception and the stacks of the threads the program names as
alive at the crash in `BLOOM_TEST_CRASH_THREADS` (its main thread, a held thread waiting for the
program to end, and the crashing thread), every other thread with its stack or recorded as exited,
the text report's lines,
that the next start offers it and the start after that does not), `bloom.crash.cli-headless`
(`bloom-cli` crashing inside a script, which names its main and a held thread the same way) and `bloom.ui.crash-notice` (the notice is offered once,
never modal, and not shown when it may not be). They run on Linux, macOS and Windows.

**Symbols.** The release workflow builds with `BLOOM_DEBUG_SYMBOLS` (`cmake/BloomDebugSymbols.cmake`):
debug information without changing optimization, and the `bloom-debug-symbols` target, which
writes each shipped program's split debug file (Linux), dSYM (macOS) or PDB (Windows) into the
build's `symbols` folder. The workflow keeps that folder as an artifact of the run; packages do
not carry it, and a Linux package is installed stripped
([Releasing Bloom](../development/releasing.md#debug-symbols)).

## Build, Test, And Release Matrix

CMake describes targets and feature discovery. Supported builds use the native production-quality
toolchain for each platform while keeping warning policy and tests semantically aligned.

Continuous integration must cover Linux, macOS, and Windows before a feature becomes supported. The
matrix grows in proportion to implementation risk and includes:

- configure, compile, unit tests, and headless integration tests
- project-format and cross-platform fixture compatibility
- deterministic CPU reference renders and tolerance-based GPU comparisons where applicable
- UI smoke tests for panel lifecycle and asynchronous result delivery
- installation, launch, resource discovery, and clean uninstall/upgrade checks
- representative path, locale, scale-factor, and missing-capability cases

Platform-specific tests supplement the shared suite; they do not replace it. A platform may be
temporarily red during development, but a release cannot call a feature complete while its required
workflow is absent or knowingly broken on another first-class target.

A release is built by the release workflow from a version tag, on hosted runners for the three
platforms, and published after the maintainer's approval
([Releasing Bloom](../development/releasing.md)). The workflow refuses a tag whose commit does
not carry the record of a passing full validation on the project's own Linux, macOS and Windows
hardware, because a hosted runner has no GPU: there the package check's GPU rule is not run,
and nothing the workflow runs counts as GPU validation.

## Qt

Bloom uses one exact Qt version on Linux, macOS and Windows: the dependency lock's `qt`
component, taken as the Qt Project's official open-source binaries and linked dynamically under
LGPL-3.0-only ([Dependency intake](dependency-intake.md),
[ADR 0014](../decisions/0014-apache-license-and-qt-distribution.md)). A `qualified` build uses
no other Qt. `dependencies/qt-modules.txt` is the list of what Bloom uses, and a check holds the
build to it:

| | Linux | macOS | Windows |
| --- | --- | --- | --- |
| Libraries | Core, Gui, Widgets, Network, Svg, DBus; OpenGL, the xcb platform library and the Wayland client, which the plugins load | Core, Gui, Widgets, Network, Svg, DBus (which Qt Gui loads) | Core, Gui, Widgets, Network, Svg |
| Platform plugins | xcb, Wayland, offscreen | Cocoa, offscreen | Windows, offscreen |
| Window system plugins | xcb GLX and EGL integration; Wayland XDG shell, window decoration and EGL buffers; compose and IBus input contexts; desktop portal theme | | |
| Style plugin | | macOS style | Windows style |
| TLS backend | OpenSSL | Secure Transport | Schannel |
| Image plugins | SVG image format and icon engine | SVG image format and icon engine | SVG image format and icon engine |

Test programs also link Qt Test, which no package carries. Every other Qt module and plugin is
left out. The Linux binaries bring the ICU libraries they link.

## Scripting Interpreter

Every package carries the same CPython, built from one locked source archive, with the same
modules and the same bundled packages ([Dependency intake](dependency-intake.md)). A script needs
no Python on the computer, and Bloom never uses one that is installed there.

- **Modules.** The standard library, with `ssl`, `hashlib`, `lzma`, `compression.zstd`, `zlib`,
  `bz2`, `sqlite3`, `ctypes`, `pyexpat` and `decimal` made of locked libraries. Absent on every
  platform: `tkinter` (and `idlelib`, `turtle`), `curses`, `readline`, `dbm.gnu`, `dbm.ndbm` and
  `ensurepip`; `dbm` works through `dbm.sqlite3`. Modules that CPython itself limits to one
  system (`winreg`, `msvcrt`, `termios`, `fcntl`, `pwd` and the like) exist where CPython has
  them. The committed list is `tests/fixtures/scripting/python_module_set.txt`, and
  `bloom.scripting.python-module-set` compares it with the interpreter on every platform.
- **Bundled packages.** numpy 2.5.4, and nothing else. It is built without a BLAS or LAPACK
  library and without its kernels for particular instruction sets, so linear algebra and large
  arrays are slower than in an accelerated numpy.
- **How the set grows.** A package is added as a lock component like any other dependency: its
  source archive with digest, license, provenance, review and security records with an advisory
  source, a superbuild recipe that builds it from source for every profile with the carried
  interpreter (no wheel, no pip), a line in the module table, and a section of the same-result
  fixture that exercises it. It is added on all three platforms or on none.
- **Packages a user installs are not supported.** The interpreter runs isolated: it reads no
  environment variable, no user site directory and no `site` configuration, and it has no pip.
  A script can add a directory of pure Python modules to `sys.path` itself.
- **The same result.** `bloom.scripting.python-same-result` runs one script on every platform
  and requires the digests committed in `tests/fixtures/scripting/python_same_result.txt`:
  integer and rational arithmetic, floating-point array arithmetic including complex products
  and quotients, square roots, sorting, reductions, linear algebra (`@`, `dot`, `solve`, `inv`,
  `det`, `cholesky`, `qr`, `einsum`), hashing, compression, SQLite and `decimal`. Measured equal
  on x86-64 Linux, arm64 macOS and x86-64 Windows (2026-11-12).
- **What can differ in the last bit.** The functions each system's math library computes, other
  than the square root: the transcendental functions (`sin`, `exp`, `log`, `pow` with a
  fractional exponent and the like) and `hypot`, which is also the absolute value of a complex
  number, in `math`, `cmath` and numpy. Linux uses glibc's, macOS Apple's and Windows the
  Universal C Runtime's; making them equal would mean replacing the math library. Measured:
  `numpy.abs` of a complex array and `numpy.hypot` differ between Linux and macOS.

## Packages

A package is everything Bloom needs besides the operating system: the application, `bloom-cli`
and `bloom-mcp`, the media and color workers, the shader tools, the locked Qt, the carried
interpreter with numpy and Bloom's Python package, and the license texts, notices and
dependency lock of what it was built from. The three operating systems get the same parts from
the same code; only where each part sits differs, and that is written once, in
`cmake/BloomPackageLayout.cmake`:

| Part | Linux | macOS, inside `Bloom.app/Contents` | Windows |
| --- | --- | --- | --- |
| Application, `bloom-cli`, `bloom-mcp`, crash handler, shader tools | `bin` | `MacOS` | the package folder |
| Carried libraries | `lib` | `Frameworks` | the package folder |
| Qt libraries and plugins | `lib/qt/lib`, `lib/qt/plugins` | `Frameworks`, `PlugIns` | the package folder, `qt/plugins` |
| Color worker | `libexec/bloom` | `MacOS` | the package folder |
| Media workers | `libexec/bloom/media` | `libexec/bloom/media` | `media` |
| Interpreter home | `share/bloom/python` | `Resources/python` | `python` |
| Bloom's Python package | `share/bloom/bloom-python` | `Resources/bloom-python` | `bloom-python` |
| Licenses, notices, dependency lock | `share/bloom` | `Resources` | the package folder |

Only a qualified build with the desktop application and the locked interpreter can be
packaged; a developer build links the host's Qt and Python, which a package cannot carry.

- On Linux and Windows `cmake --install <build> --prefix <folder>` writes the whole package:
  the programs through their own install rules, then Qt, the interpreter and the records
  through the shared scripts in `cmake/package` (`stage_qt.cmake`, `stage_python.cmake`,
  `stage_records.cmake`, and on Linux `stage_linux_runtime.cmake`). The `bloom-package` target
  installs into the build tree and checks the result; `bloom-package-archives` then makes the
  published files, two on Linux and, on Windows, a zip and the installer. A Windows package also gets the Visual
  C++ runtime (`stage_msvc_runtime.cmake`). Only the shipped programs have install rules for
  the shader tools (`bloom_package_gpu_shader_tools(<target> INSTALL)`); a test program stages
  them beside itself in the build tree only, so `bloom-package` builds everything the install
  needs from an empty build folder without building the tests (#710).
- On macOS the build fills `Bloom.app` in the build tree with the same scripts every time it
  builds, so that the bundle can be signed once it is complete; `bloom-deploy-macos` signs and
  checks it.

The `<version>` in every package file's name is `BLOOM_PACKAGE_VERSION`: the label of the release
this source tree builds (`0.1.0-alpha.1` today, written once in the root `CMakeLists.txt`) unless
the configure names another. A pre-release carries its own label, so its files are never mistaken
for the release's.
The application, `bloom-cli` and `bloom-mcp` report that same version; each prints it when run
with `--version`.

Qt is copied exactly as the Qt Project published it: nothing is patched, stripped or signed
again, and only the libraries and plugins `dependencies/qt-modules.txt` lists are taken. The
license directory holds, for every component the lock builds for the platform, exactly the
files the lock names, and the license files of the components whose sources are checked in.

The carried interpreter never writes into the package: the compiled form of the modules it
imports goes to Bloom's per-user cache (`python-bytecode`). A package can be read-only, and a
signed macOS bundle stops being valid when a file appears in it.

### Package Check

`cmake/package/check_package.cmake` is the one check for all three platforms. It fails when

- the package lacks a part listed above (among them `bloom-crash-handler`, without which a
  crash is not recorded), or a license file is missing or is not the file the lock records;
- a program or library in the package loads a library that is neither inside the package,
  where the loader would look for it, nor a declared part of the operating system, or carries
  a library search path that leads outside the package;
- a run of the packaged programs loads anything from elsewhere. The package's own `bloom-cli`
  runs a script in the carried interpreter that uses every carried module and then starts the
  packaged application without a window; both report every image they loaded.

What counts as the operating system is declared per platform: on Linux the libraries named in
`cmake/package/linux-base-libraries.txt` (and what those load themselves), on macOS `/usr/lib`
and `/System`, on Windows the system directory, without the Visual C++ runtime, which is not
part of Windows. On macOS the check also fails on a file built for a newer system than the
bundle declares. On Windows a program may load one library late and by its full name: the
libcrypto of the update checks, which must be among the carried interpreter's modules
(`python\DLLs`), so the package holds one copy of it. Any other late load is held to the same
rules as an ordinary one.

The run never reads or changes the settings of the person whose computer it is. The check
gives the application a settings file, data and cache directories of its own, and the
application then runs under an organization name of its own as well
(`src/ui/include/bloom/ui/application_identity.hpp`), so the per-user folders Qt names after
the organization are the check's; the check removes them afterwards. The application gives up
its own names as soon as the check's variable is set, and when the value is not an absolute
directory it refuses to start rather than run as Bloom. On Linux the run is offered no GPU
driver, so that what it loads does not depend on the machine's.

The macOS bundle and the Windows package also carry the platform's updater when the dependency
prefix has it: `Sparkle.framework` in `Frameworks`, `WinSparkle.dll` beside the programs
([ADR 0041](../decisions/0041-self-update.md), `dependencies/update-runtime/README.md`).
`cmake/package/stage_update_runtime.cmake` copies the library, its license texts and the record
`update-runtime.json`, and the check fails when a package has the library without the record,
the record without the library, or a license text that is not the one the record names. Bloom
loads the library at run time and links nothing of it, so a package without it starts and runs
the same and only has no platform updater.

The `bloom.package.check` test runs the check on the package of the build and then on a copy
broken in each way that matters: the interpreter removed, a license file removed or changed, the
crash handler removed, a program made to load a library from outside, a program given a search path outside the
package, and on Linux one of the carried distribution libraries removed.

A rule for releases, `release`, is never part of the default rules. It asks the packaged
application what it was built with (`Bloom --update-settings`) and fails unless that is the
release's feed address and exactly the release's public keys, and the build is not a test build
(`BLOOM_UPDATE_TEST_BUILD`), which takes its feed and key from the environment. On macOS and
Windows it also requires the platform's updater in the package, and on macOS the release's first
key in the bundle's `Info.plist`. The release workflow runs it on each platform's package
([Releasing Bloom](../development/releasing.md)); `bloom.package.check` requires that it refuses
the package of an ordinary build.

A separate rule, `gpu`, asks more of the package than being self-contained: that its
application renders on the GPU. The package's own `bloom-cli` runs
`tests/fixtures/scripting/package_gpu_preview.py`, which starts the packaged application
without a window with its MCP server on a loopback port, adds a composition with one Solid
layer, shows it in the Viewer and reads which route produced the displayed frame
(`query kind=render-route`, the route the status bar shows). The rule fails unless that frame's
scene was evaluated on the GPU, and names the fallback reasons the preview service counted. It
is never part of the default rules, because it needs a computer with a GPU and leaves the
machine's driver visible; `bloom.package.check` includes it in every build with the Vulkan
backend, on all three platforms, and on Linux also requires the rule to refuse the same run
when no GPU driver is offered. The run uses the check's own settings, data and cache
directories, as the other run does.

### Linux Package

The Linux package is one tree published in two forms: `Bloom-<version>-linux-x86_64.tar.xz`,
which unpacks to a folder that runs from wherever it is put, and
`Bloom-<version>-linux-x86_64.AppImage`, the same tree as one file. There is no other form.

**The oldest system.** The package is built on Ubuntu 22.04 against its C library, glibc 2.35,
and its own C++ library, so it runs on that system and on newer ones and carries no C++
runtime of its own. The build environment is the official `ubuntu:22.04` image by digest with
the locked Clang 22 (`tools/ci/linux-package`: `provision.sh`, `Dockerfile`, `build.sh`), and
the dependency lock's Linux profile records that floor. The check enforces it: with
`FLOOR=GLIBC=2.35;GLIBCXX=3.4.30;CXXABI=1.3.13` it fails on any file in the package that needs
a newer version of a C or C++ library symbol.

**What the system must have.** Everything a package loads from outside itself is named in
`cmake/package/linux-base-libraries.txt`: the C and C++ libraries; OpenGL, EGL, GBM and DRM
(which load the GPU's own driver); Xlib and the X protocol library; Wayland; fontconfig,
FreeType and xkbcommon; D-Bus; the audio servers' client libraries; GLib; zlib, zstd and
Brotli; the Kerberos sign-on library; and VA-API. A desktop installation has them. On a
minimal system they are these packages:

| System | Packages |
| --- | --- |
| Debian, Ubuntu | `libgl1 libegl1 libopengl0 libgbm1 libdrm2 libx11-6 libx11-xcb1 libxcb1 libxcb-glx0 libxcb-randr0 libxcb-render0 libxcb-shape0 libxcb-shm0 libxcb-sync1 libxcb-xfixes0 libxcb-xkb1 libxcb-xinput0 libwayland-client0 libwayland-cursor0 libwayland-egl1 libfontconfig1 libfreetype6 libxkbcommon0 libxkbcommon-x11-0 libdbus-1-3 libasound2 libpulse0 libglib2.0-0 zlib1g libzstd1 libbrotli1 libgssapi-krb5-2 libva2 libva-drm2` (on Ubuntu 24.04 `libasound2t64` and `libglib2.0-0t64`) |
| Fedora | `libglvnd-glx libglvnd-egl libglvnd-opengl mesa-libgbm libdrm libX11 libX11-xcb libxcb libwayland-client libwayland-cursor libwayland-egl fontconfig freetype libxkbcommon libxkbcommon-x11 dbus-libs alsa-lib pulseaudio-libs glib2 zlib-ng-compat libzstd libbrotli krb5-libs libva` |

No Python, Qt or compiler is needed: the interpreter, numpy and Qt are in the package.

**What it carries from a distribution.** Six small X11 helper libraries that Qt's X11 platform
plugin loads and a desktop does not always have are carried beside Qt, each taken from a
pinned Ubuntu 22.04 package by checksum with its copyright file
(`dependencies/linux-runtime`). They are listed in the Open Source Licenses window.

**The AppImage.** The tree as a zstd-compressed SquashFS image behind the AppImage project's
type 2 runtime, which is pinned by checksum and recorded with the licenses of what is linked
into it (`dependencies/appimage-runtime`); those license files are inside the AppImage. It is
made without an AppImage tool (`cmake/package/make_archives.cmake`). Running it needs FUSE
(`fusermount3` or `fusermount`); `--appimage-extract` unpacks it where there is none, and the
tar is the form for such a system. The AppImage starts the application; `bloom-cli` and
`bloom-mcp` are in the tar and in the unpacked AppImage, under `bin`.

A release publishes `<name>.AppImage.zsync` beside the AppImage: the control file from which an
installed AppImage downloads only the blocks it lacks
([ADR 0041](../decisions/0041-self-update.md)). `bloom-zsync-make`, a build tool that is in no
package, writes it, and a release build writes the place the control file is published into
the runtime's update information (`BLOOM_UPDATE_APPIMAGE_INFORMATION`).

**The AppImage's menu entry.** An AppImage is one file and installs nothing, so on the first run
Bloom offers to add itself to the application menu: "Add Bloom to the Application Menu?", with
**Add to Application Menu** and **Not Now**. The answer is kept (`appimage/menu-offer-answered`),
so the offer is made once, and only for an AppImage that has no entry of Bloom's yet.
**Add to Application Menu** writes, in the user's data folder (`XDG_DATA_HOME`, or
`~/.local/share`), `applications/id.afterlab.bloom.desktop`, the icons under
`icons/hicolor/<size>/apps` and the project file type under `mime/packages`; nothing else is
changed. The entry starts the AppImage by its full path (`Exec="<file>" %f`, with `TryExec`) and
carries the line `X-Bloom-AppImage=true`, which says that Bloom wrote it. When that entry names
another file than the running AppImage (the file was moved or renamed), Bloom writes it again at
start-up without asking. An entry without the line, such as the install script's, is left alone.
The file work is a background job and never runs on the interface thread; the desktop's
`update-desktop-database` and `update-mime-database` are started afterward when they exist, and
not waited for. A path the entry cannot carry (a quote, a backquote, `$`, a backslash, `%` or a
line break) is refused with the reason shown. `bloom.ui.appimage_menu_tests` (Linux only: an
AppImage is a Linux file) covers the files and their `Exec` and `TryExec` lines, the once-only
offer, **Not Now** writing nothing, a moved file and a refused path, in folders under the
build tree.

**The install script.** A release publishes `install.sh`, written for that release from
`tools/install/install.sh.in` by `cmake/package/make_install_script.cmake`, which fills in the
version, the address of the release's files and the release's public keys. It is a POSIX shell
script and needs `tar`, `xz`, `sha256sum`, `openssl` (3.0 or later) and `curl` or `wget`.

- It downloads `SHA256SUMS` and `SHA256SUMS.sig`, checks the Ed25519 signature with `openssl`
  against a key it carries, then downloads the tar and compares its SHA-256 with the signed
  list. Only then is anything unpacked or any folder made; there is no option that skips the
  check. `--from <folder>` takes the same files from a folder and verifies them the same way.
- Without options it installs for the current user and needs no administrator rights: the
  package in `~/.local/opt/bloom`, the link `~/.local/bin/bloom-cli`, and the menu entry, the
  icons and the project file type under `~/.local/share` (or `XDG_DATA_HOME`). The menu entry
  starts the program by its full path.
- `--system` installs for every user: `/opt/bloom`, `/usr/local/bin/bloom-cli` and
  `/usr/local/share`. With `DESTDIR` it writes that layout under another folder, for a system
  image. `--prefix <folder>` names the package's folder; it must be a folder for Bloom alone,
  and the script refuses one that holds anything else, and the system's own folders.
- It names the system libraries the package expects and the system lacks, with the command
  that installs them on Debian and Ubuntu, on Fedora and on Arch Linux. The list is
  `cmake/package/linux-base-libraries.txt`; `tools/install/linux-library-packages.txt` gives
  the package of each and says whether the package is linked against it or asks for it only
  when a feature needs it. On a Debian or Ubuntu release that renamed a library package for
  its 64-bit time type (`libasound2` became `libasound2t64`), the command names the package
  the system has. The install is completed either way.
- Run again, it replaces the installed folder with a newly unpacked one and writes the link
  and the desktop files again, which updates or repairs the install. The folder is replaced by
  renames beside it; a run that is stopped between them puts the installed version back.
- It writes `share/bloom/install-kind` (`kind=script`), which tells the update service that
  the folder can be replaced by the update helper, and `share/bloom/install-manifest`, the
  list of what it wrote outside the folder. `--uninstall` removes the folder (only one that
  carries the script's marker), the version an update kept beside it, the link when it still
  points into the folder, and the listed files. Other files in the shared folders, projects,
  preferences and caches stay.

`bloom.install.script_tests` (Linux only) runs the script against a stand-in package signed
with a key made for the run by the release's own tools, in a home folder under the build tree:
a per-user install through the script's own download, each refused release (a changed
package, a changed list, another key, another release's list, no signature), the library
advice for each family, repair, update, `--prefix`, `--system` under `DESTDIR`, and
`--uninstall` beside other programs' files.

**The Arch Linux package.** `packaging/aur/PKGBUILD.in` is the source of the `bloom-bin` package
for the AUR, written for each release (with `.SRCINFO`) by `cmake/package/make_pkgbuild.cmake`
from the release's label and the checksum of its tar. The package takes the release's tar from
the release's page on GitHub, puts the tree in `/opt/bloom`, links `/usr/bin/bloom-cli` to
`/opt/bloom/bin/bloom-cli`, and installs the menu entry (started by `/opt/bloom/bin/Bloom`), the
icons and the project file type under `/usr/share`, and the license texts under
`/usr/share/licenses/bloom-bin`. Its `depends` are the Arch Linux packages of the libraries
`tools/install/linux-library-packages.txt` marks `required`, and its `optdepends` are the
`optional` ones, each with the reason. It is `x86_64` only, `provides` and `conflicts` with
`bloom`, and is not stripped, because the files of a release are checked by their bytes.
`/opt/bloom/share/bloom/install-kind` holds `kind=package` and `manager=pacman`, so Bloom sees
that pacman owns the install and its update window says to update it there; Bloom does not
replace the files. Publishing the package to the AUR is a maintainer step.
`bloom.package.pkgbuild_tests` (Linux only) generates the files for a stand-in release. It
checks them statically everywhere; where `makepkg` exists and the user is not root, it also
builds the package from the stand-in, offline, and checks every path, the link's target, the
marker and the entry's `Exec` line in the built package.

**Where it was started.** `tools/ci/linux-package/start.sh` unpacks a package on a clean
system, runs the package check's contents and run rules there, and refuses a system that has
Python, Qt's tools or a compiler. A third form, `sh start.sh <release folder>/install.sh <work>`,
takes the release's own `install.sh` from the folder that also holds `SHA256SUMS`,
`SHA256SUMS.sig` and the tar. It installs for a user whose home folder is under the work
directory (`--from <folder> --prefix <work>/installed`, which verifies the signature and the
checksum), runs the same rules with the linked `bloom-cli` as the program that runs and
renders, then runs `install.sh --uninstall` and requires that the folder and the link are gone.
The table below lists runs of the tar and the AppImage only; a run of the install script form
is listed with its system when it has been made there. The tar and the AppImage of the 0.1.0
package passed it on:

| System | C library | How |
| --- | --- | --- |
| Ubuntu 22.04 | glibc 2.35 | Official image by digest, with only the packages above |
| Ubuntu 24.04 | glibc 2.39 | Official image by digest, with only the packages above |
| Debian 12 | glibc 2.36 | Official image by digest, with only the packages above |
| Fedora 44 | glibc 2.43 | Official image by digest, with only the packages above |
| Arch Linux (a workstation with a GPU) | newer | The tar unpacked, and the AppImage mounted through FUSE and checked as the read-only tree it is; the packaged `bloom-cli` rendered a frame on the GPU through the carried Vulkan loader |

The images were entered without a container runtime, so in them the AppImage was unpacked by
its own runtime and not mounted; the FUSE mount was exercised on the workstation. The run in
the images has no display and no GPU. A window on each of those systems' own desktops was not
part of this.

The release workflow repeats the start for every release, on the same four images by digest:
`tools/ci/linux-package/clean-system.sh` installs the declared system libraries by the package
names of `tools/install/linux-library-packages.txt`, what the install script needs, and the
standalone CMake checked by its SHA-256, and `start.sh` then starts the tar, the AppImage and
the install script form.

### Windows Package

The Windows package is a folder, published as `Bloom-<version>-windows-x86_64.zip` and as the
installer `Bloom-<version>-windows-x86_64-setup.exe` (`bloom-package-archives`). The programs
are not signed. Unpacked anywhere, the zip runs from there: nothing is written outside the
folder to make it work, and nothing has to be installed first.

The installer is made with Inno Setup from `packaging/windows/bloom.iss`
(`cmake/package/make_windows_installer.cmake` runs the compiler). It is made when the
dependency prefix has the compiler. A developer build has none, makes the zip only and says so
when it is configured. The installer's welcome and finish pictures and its icon are the
ones in `apps/bloom/installer/windows` and `apps/bloom/windows`.

The installer installs for the current user only, by default into
`%LOCALAPPDATA%\Programs\Bloom`. It never asks for administrator rights and writes nothing
under `HKEY_LOCAL_MACHINE`. It adds:

- a Start menu entry and an uninstall entry, with the version;
- the project file type: `.bloom` is a "Bloom Project" with the app icon, and opening one
  starts Bloom with the file as its argument;
- the file `install-kind` with the line `kind=windows-installer`, which tells the update
  service that this copy was installed by the installer and can be updated by the next one
  ([ADR 0041](../decisions/0041-self-update.md)).

The **Add bloom-cli to PATH** option is off by default. It adds the `cli` folder of the
installation to the user's `Path` (the user's environment, never the system's) unless that
folder is an entry already. The folder holds one small program, `cli\bloom-cli.exe`. It starts
the real `bloom-cli.exe` from the installation folder with the same arguments, the same
handles and the same environment, waits for it and returns its exit code. The real program
keeps the libraries beside it, so none of the package's libraries enter the user's `PATH`. The
launcher uses the static C++ runtime and needs nothing beside it. Uninstall removes that one
entry and nothing else from `Path`.

For a deployment, the installer takes Inno Setup's own switches:

    Bloom-<version>-windows-x86_64-setup.exe /VERYSILENT /SUPPRESSMSGBOXES /NORESTART
        [/DIR="<folder>"] [/TASKS="addtopath"]

The updater runs the same with `/RELAUNCH=1`, which starts Bloom again when the install is done.
A silent install without it starts nothing. A silent uninstall is
`<folder>\unins000.exe /VERYSILENT /SUPPRESSMSGBOXES /NORESTART`.

A newer installer finds the installed Bloom by its fixed application id and upgrades it in
place, in the same folder, with the same options. First it renames the installed folder to
`<folder>.previous`, so the previous version is kept beside the new one: the update helper
puts it back when the new version does not start ([ADR 0041](../decisions/0041-self-update.md)).
When the folder cannot be renamed because a file in it is in use, the installer goes on over the
existing folder and keeps no previous version. Uninstall removes the installation, the kept
previous version, the entries, the file type and the `Path` entry.

The `bloom.package.installers` test (Windows only, because the installer is a Windows program)
makes two installers from the built package under a test identity of its own: another
application id and name, the file type `.bloomcheck<code>`, and the value
`BloomPackageCheckPath<code>` in place of `Path`, where `<code>` is made from the test's work
folder, so the checks of two build trees running at once never find each other's install (#749). It installs the first silently with the PATH option, checks the files, the
entries and the `.bloomcheck` keys, and renders a frame with `bloom-cli` from the installed
`cli` folder. It installs the second over it and checks the upgrade, the kept previous version
and that `Path` still holds the folder once. It uninstalls and checks that everything is gone.
Whatever happens, it removes the keys and the shortcut it made. Without the Inno Setup
compiler the test is reported as skipped.

The Visual C++ runtime is not part of Windows, so the package carries the redistributable
libraries of the toolset that built it (`cmake/package/stage_msvc_runtime.cmake`). A Windows
program looks for a library beside itself first, so they are beside every program: in the
package folder, with the shader tools and with each media worker. `licenses/msvc-runtime`
names them, with their checksums and the terms they are redistributed under. The universal C
runtime is part of Windows 10 and later and is not carried. The check fails when a Visual
C++ runtime library is loaded from the Windows system directory instead of the package.

The 0.1.0 package was unpacked into a second folder with a space in its name and checked
there, contents and a run, with only the Windows directories on `PATH` and none of the build
tools' variables set: every library the packaged `bloom-cli` and the packaged application
loaded came from the folder or from Windows. That was on the one Windows computer that also
built it; a computer that never had the build tools was not available, and the load record is
what stands in for it.

### Updates

Every package kind has one update path, chosen by `bloom::update::detectInstall` from the
package's records ([ADR 0041](../decisions/0041-self-update.md) holds the design and the full
table). Bloom first checks its own signed feed; only a newer version of the build's channel is
offered, and a downloaded file is used only when its size, SHA-256 and Ed25519 signature match the
feed.

| Package | Update |
| --- | --- |
| macOS bundle | The platform's updater replaces `Bloom.app` from the appcast the feed names. |
| Windows installer | The platform's updater runs the next installer silently with `/RELAUNCH=1`. |
| Windows zip | Bloom says where the next version is and changes nothing. |
| AppImage | Bloom downloads the next AppImage (a delta from the `.zsync` file when it can) and `bloom-update-helper` replaces the file. |
| Install script, tar in a folder the user can write | Bloom downloads the tar and `bloom-update-helper` swaps the folder. |
| System-wide install, package manager | Bloom says the installer must be run again, or that the package manager owns the update. |

On every platform the previous version is kept until the next update succeeds and is put back when
the new one does not confirm its start. A build folder is never updated. The user's view of this is
in the [installing guide](../user-guide/installing.md).

## Capability Reporting And Graceful Degradation

Runtime capability checks produce structured data that both the UI and headless tools can inspect.
Checks cover relevant GPU features, display behavior, codecs, color processors, plugins, and other
optional integrations.

When a capability is missing, Bloom chooses one declared outcome:

1. use a validated equivalent implementation
2. use a visible preview-only degradation that cannot affect final output silently
3. disable the operation with an actionable diagnostic while preserving project data

The application does not crash, hide controls without explanation, mutate a project to fit the
current machine, or silently render a different result.

## Definition Of Cross-Platform Complete

A user-facing feature is cross-platform complete only when:

- its semantics and fallback policy are documented
- it is implemented through portable boundaries
- Linux, macOS, and Windows pass the relevant shared tests
- projects using it survive cross-platform save/open round trips
- unsupported runtime capabilities fail visibly and preserve authoring data
- packaging includes its required dependencies and licenses on each target
- known presentation differences are intentional and documented

This definition applies from the first vertical proof onward, not as a cleanup phase before release.


## Focused GPU And Explicit CPU Fallback Gate

The manual [`gpu-qualification.yml`](../../.github/workflows/gpu-qualification.yml) workflow builds
an explicit Vulkan-OFF CPU fallback on Linux, then runs the bounded image,
source-revision, CPU evaluation, processing-budget and host-lifetime tests. It builds selected targets
and does not repeat the repository-wide quality sweep. Locked superbuild archive hashes and library
versions are reused and Linux uses the locked Clang 22 family. The default matrix is Linux. The
native job also accepts a macOS runner (the lock's Apple silicon profile, Apple clang, the locked
MoltenVK as the device). The helper configures a Windows build with MSVC in qualified mode; the
workflow has no Windows runner, and what was run by hand on a Windows machine is in the Windows
section.

Before the new workflow is on the default branch, the existing `ci.yml` dispatch entry calls it
from the selected branch with `focused_gpu_gate=true`. That explicit manual mode skips the four
normal CI jobs; pull-request/push behavior is unchanged. A concrete branch run is:

```sh
gh workflow run ci.yml --ref <branch> -f focused_gpu_gate=true -f run_native=false
```

Native execution additionally requires `run_native=true` and explicit `native_runner_labels`, for
example `["self-hosted","linux","gpu"]`. No workflow is dispatched by saving these files.

Dependency provenance remains separate from functional test results. The production lock holds a
Linux Clang profile and an Apple silicon macOS profile (Apple clang, deployment target 13.3); a run
on either consumes its source-built prefix in `qualified` mode. The lock also holds a Windows x86-64
MSVC profile (minimum Windows 10 version 1903), and a Windows run consumes its source-built prefix
in `qualified` mode the same way. Qt is locked too: every profile takes the same Qt version as
official binaries that the lock names by checksum (see Qt below).
No release qualification is fabricated: the `Qualified-pending-manifest` state is preserved rather
than upgraded by this focused gate.

The reusable [`gpu_qualification.py`](../../tools/ci/gpu_qualification.py) runner writes JSON with
separate CPU fallback, native, dependency-provenance and qualified-native fields. The fallback gate
requires Vulkan explicitly OFF, rejects an internally skipped required CPU test, verifies that the
optional resident-display binary reports its typed CPU fallback, and verifies that its
`--require-device` invocation fails. The optional device skip is recorded as **NotRun native**, never
as a passing native render. Test processes have inherited display/Wayland bindings removed and use
Qt's offscreen platform; this gate launches no desktop application.

For an existing configured fallback build:

```sh
python3 tools/ci/gpu_qualification.py --build-dir build/gpu-cpu-fallback \
  --mode fallback --build --output build/gpu-evidence/fallback.json
```

The workflow's optional native job takes explicit JSON runner labels, builds the locked Vulkan
bootstrap closure, and runs ten bounded native suites: processing-buffer, image-allocation,
display-transfer, resident-display, operation-buffer, presentation-allocation, OCIO-image storage,
queue timing, allocator memory and operation batches. Every suite receives `--require-device`.
The batch suite requires actual multi-kernel submissions, status rejection and bounded retirement;
its optional unavailable-device result cannot count as native qualification.
Device diagnostic `device_type` must identify
an integrated, discrete or virtual GPU. Missing capability and CPU/software Vulkan produce
**NotRun** and a failing exit status in native mode, including without the explicit requirement
marker. An invariant failure remains a
failure and is never relabelled device unavailability. The native gate does not qualify GUI
presentation, all production render routes, sustained playback, platform packaging or full prefix
manifests; those remain independent gates.

```sh
python3 tools/ci/gpu_qualification.py --build-dir build/gpu-native-evidence \
  --mode native --build --require-device --output build/gpu-evidence/native.json
```

A prepared workflow is not proof that a particular machine has a device or that Bloom's GPU routes
pass on it. The optional job probes the actual selected runner and retains unavailable/failure
evidence, and a platform's parity claim rests on what was measured on a real device of that
platform (the macOS and Windows sections).

## macOS

macOS has no system Vulkan. Bloom's device there is MoltenVK over Metal, a locked, source-built
component of the dependency prefix, with the locked Vulkan loader. Both are packaged with every
program that renders: inside `Bloom.app` (`Contents/Frameworks`, with the driver manifest under
`Contents/Resources/vulkan/icd.d`) and beside `bloom-cli`, `bloom-mcp` and the test programs. A
device is reached with nothing set in the environment. Metal's fast-math is turned off for Bloom,
because it breaks parity with the CPU reference. A Metal device has one memory pool shared with
the system; Bloom plans against it with its own budget
([ADR 0039](../decisions/0039-unified-memory-devices.md)).

What was measured, on an Apple M4 running macOS 26 (2026-11-12), in a qualified build at the
13.3 deployment target:

- The GPU coverage contract and the native acceptance pass with the contract's exception lists
  unchanged and all six render routes proven. The acceptance needs the desktop build, because
  the two preview route proofs are written by harnesses that drive the Viewer and RAM preview.
- `bloom.sweep.nodes` renders all 98 node types that have an image output and a buildable graph
  on the GPU: 97 are bit-identical to the CPU reference and Auto Exposure is within the
  documented 2e-6 gate.
- The Viewer uses the adopted preview route (prepared on the GPU, painted through the widget).
  The resident presentation route needs a Wayland surface and is not built for macOS; no Metal
  surface route exists. The route proof contract therefore holds the macOS preview proofs to
  the host-painted rule: every frame prepared on the device and equal to the CPU reference in
  every pixel, exactly one final display transfer per frame, no other readback and no frame on
  the CPU path. Windows is held to the same rule. Linux paints the Viewer the same way and is
  held to it too; because Linux also has the Wayland surface the resident service presents
  through, its acceptance requires the resident proofs as well, so each Linux preview route is
  proven twice, once per display, and a missing proof of either kind fails the acceptance.

Not measured: any macOS release from 13.3 to 25, and an Intel Mac. The deployment target says
where the binaries are allowed to start, not where they were tested.

The application bundle is `id.afterlab.bloom`, with its icon, the minimum system version of the
deployment target, the media workers under `Contents/libexec/bloom/media`, the color worker and
the shader tools beside the executable, and the loader and driver as above. It carries the
Python interpreter (see Scripting Interpreter above): the library in `Contents/Frameworks`, the
standard library and numpy in `Contents/Resources/python` and Bloom's Python package in
`Contents/Resources/bloom-python` ([Dependency intake](dependency-intake.md)). The
build puts the locked Qt's frameworks and plugins into the bundle exactly as the Qt Project
published and signed them (a developer build copies the host's Qt with `macdeployqt` instead).
The `bloom-deploy-macos` target signs the result ad hoc and then checks
that every program and library in the bundle resolves inside it or in the operating system and
that none was built for a newer system than the bundle declares; there is no Developer ID
signature or notarization, so a copy downloaded from the internet is stopped by Gatekeeper until
the person opening it allows it.

The macOS package is that bundle as `Bloom-<version>-macos-arm64.zip`
(`bloom-package-archives`, made with the system's `ditto` so the links inside the frameworks
and the signatures survive). `bloom-cli` and `bloom-mcp` are inside it, in `Contents/MacOS`.
The 0.1.0 zip was unpacked into a second folder with a space in its name on the Mac that built
it: the signature verifies, and the package check with a run passes there with only the
system's directories on `PATH`. A second Mac was not available.

### Test Programs And Gatekeeper

macOS scans every new program file the first time it starts (#727). The kernel finds no
Developer ID signature on an ad-hoc signed build product, and the system policy daemon then checks
it: an online notarization lookup, then an XProtect scan of the file. The program waits until
the check ends. The daemon checks one file at a time and remembers each result per file, not per
content, so a byte-identical file written again is checked again. A library a program loads is
checked the same way. In a desktop-build validation suite of 2026-11-20 that had relinked 210
test programs, each check took about 1.1 s. With four tests started together, a relinked test
waited a median 3.3 s before its first instruction (about 710 s over the run). The suite took
1,443 s of summed test time; the run before it, with nothing relinked, took 840 s. These figures come from the system
log (`com.apple.syspolicy.exec`, AMFI and XProtect messages) and the suite's own log.

The build therefore never writes a program file it does not need to:

- A test program's link goes through `cmake/BloomKeepUnchangedLink.cmake` (set by `bloom_add_test`
  on macOS only). The linker writes into the usual path, because the path goes into the program
  (the ad-hoc signature's identifier and the image UUID). When the result has the same bytes as
  the file it replaces, the old, already checked file is put back and marked as just built. A
  program whose bytes changed is installed as the linker wrote it and is checked once when it
  first starts, like any new program. `bloom.build.keep-unchanged-link` runs the launcher with a
  stand-in linker on every system.
- Generated sources are written only when their text changes. The third-party license catalog
  was rewritten by every configure, so a change to any `CMakeLists.txt` relinked every program
  that links `bloom_ui`, among them every UI test.

A kept file is not scanned again. Its times change (the build must see it as newer than its
inputs), so the system re-checks its remembered result once, which is much shorter than a scan.
The suite that first linked every test program through the launcher relinked 1,203 test programs
with unchanged bytes. Of those, 4 were scanned in full and the rest were re-checked. Summed test
time grew by about 0.3 s per relinked program over a run with nothing relinked, against about
2.8 s per relinked program in the run described above.

Nothing on the Mac is changed for this: no security setting, no developer-tool exception and
nothing installed system-wide. The check of a changed program remains, because skipping it would
take one of those.

### Installers

`bloom-package-archives` makes three files from the signed bundle, side by side in
`<build>/package`. The zip is what the updater downloads. The other two are for people.
`cmake/package/make_macos_installers.cmake` makes them. It copies the bundle with `ditto`, so the
links and signatures survive, and signs nothing again.

- `Bloom-<version>-macos-<processor>.dmg` is the disk image. Its volume is named `Bloom`. It
  holds `Bloom.app`, a link named `Applications` to `/Applications`, and the background picture
  in the hidden folder `.background` as one TIFF with both sizes (660 x 400 and 1320 x 800; the
  pictures are in `apps/bloom/installer/macos`). It also holds a `.DS_Store`, the file Finder
  reads for the window: a 660 x 400 icon view with icon size 128, the background picture, and
  the places of the icons (`Bloom.app` at 180, 180 and `Applications` at 480, 180, on either side
  of the picture's arrow). Nobody arranges the window by hand. The maker makes the image
  read-write, attaches it, runs `tools/release/dmg_layout.py write` to write the `.DS_Store`
  (with the system's own bookmark of the background file), detaches it, and only then compresses
  it. The tool is deterministic, uses only the Python standard library, and has its own test on
  all three platforms (`bloom.release.dmg_layout_tests`).
- `Bloom-<version>-macos-<processor>.pkg` installs `Bloom.app` into `/Applications` with no
  window (`installer -pkg`). The app always goes to `/Applications`, never over another copy found
  elsewhere, and an older package may be installed over a newer version. Its `postinstall` script
  (`packaging/macos/postinstall`) links `/usr/local/bin/bloom-cli` to the program in the bundle. A
  link that cannot be made never stops the installation, and a file that is not a link and has that
  name is left alone.

Help > Install Command-Line Tool... (macOS only) makes the same link from a copy of Bloom that was
dragged out of the disk image or unpacked from the zip. It links `/usr/local/bin/bloom-cli` to the
`bloom-cli` beside the running program. A link to another copy is changed, and a file that is not
a link is never deleted. When this user cannot write `/usr/local/bin`, the system's administrator
prompt makes the link; the program's path is passed to the prompt's script as an argument. The
function behind it (`src/ui/command_line_tool_link.cpp`) uses only `std::filesystem`.

The `bloom.package.installers` test (macOS here; Windows has a test of the same name) copies the
built bundle, signs the copy, makes both installers from it and looks inside them. It attaches the
image read-only and without showing it, and checks the application, the `Applications` link and its
target, the background file and its two pictures, and the window layout: it reads the image's
`.DS_Store` with `dmg_layout.py read` and requires the window size, the icon size, the two icon
places, the background's volume and path (and that the image holds that file), and the bookmark. It
also checks that the signature still verifies. It expands the package and checks the payload, the
install location, that the app is not relocatable or version checked, and that the postinstall
script is executable and is the checked-in one. It runs the script with a target volume inside its
work folder and checks the link. It never runs `installer -pkg` and never writes outside its work
folder. A linked `bloom-cli` renders a frame headless through the installed package in
`bloom.package.check`.

The real installation is done where it is safe: on the release workflow's hosted macOS machine,
which is thrown away afterwards and whose user needs no password for it
(`tools/ci/macos-package/installed.sh`, in the job after the build, in a rehearsal as well). It
runs `sudo installer -pkg` on the built package, requires `/Applications/Bloom.app` owned by
root and the `/usr/local/bin/bloom-cli` link to the program inside it, runs the package check's
contents, version and run rules on the installed application through that link, which starts
the application without a window and renders a frame headless, and attaches the built disk
image and reads its application, its link, its background file and its window layout. A failure
fails the run. The same job then takes the desktop captures of the installed application
(`tools/ci/captures`): `/Applications` and a project file in Finder, the opened disk image with
its background, the Dock with Bloom running, and the Help menu with Install Command-Line Tool.

### Worker Memory

Bloom runs media, color and export work in worker processes, each launched with a memory budget
(`ProcessOptions::addressSpaceBytes`). On Linux the budget is a kernel address-space limit and on
Windows a job limit. macOS gives a parent no way to set a limit on a child, and refuses an
address-space limit below the shared system libraries every process maps, so the budget is kept
by the parent there:

- What is measured is the worker's physical footprint, the number the system's own memory
  accounting charges to the process: memory it has written to, including what the system has
  compressed or swapped. Memory that is reserved but never touched, and unmodified pages of
  mapped files and libraries, are not part of it.
- The parent reads the footprint on every turn of its wait on the worker's pipes, at most ten
  milliseconds apart. A worker over its budget is killed with its process group, and the request
  fails with `ResourceLimit`, which the caller reports as the worker passing its memory limit.
  `bloom.platform.process-supervisor` holds this: a worker that touches four times its budget is
  killed and reported, and the same worker with a budget above its use runs to its end.

What this does not guarantee:

- It is not a kernel limit. An allocation inside the worker is never refused, so a worker cannot
  handle the limit itself; it is stopped from outside.
- A worker can be over its budget for as long as one interval between two readings, and by as
  much as it can touch in that time.
- The footprint is read only while the parent is reading from or writing to that worker. A
  worker the parent is not waiting on (a retained worker between two requests, or a worker that
  has been asked to finish) is not policed until the next request.
- A reservation larger than the budget succeeds, where Linux would refuse it. Code that relies
  on a refused reservation to stay inside its budget does not get one here.
- It protects neither the rest of the system nor Bloom's own process from memory pressure in
  the meantime; the host's memory budget and pressure monitor
  ([GPU backend](gpu-backend.md)) do that for Bloom itself.

The bundle declares Bloom's project document (`.bloom`, type `id.afterlab.bloom.project`), so
the Finder offers Bloom for a project file. When a project is double-clicked, dropped on the
Dock icon or chosen with Open With, the system sends the application a file-open event, at
launch and while it runs; `ProjectFileOpenEvents` hands the file to the project host, and the
project opens exactly as through File, Open: unsaved changes are asked about first, a request
that arrives during another project operation waits for it, and a file that cannot be opened
is reported the same way. The package check fails on a bundle that does not declare the type.

On Linux, `apps/bloom/linux/id.afterlab.bloom.xml` describes the type for shared-mime-info
(`application/x-bloom-project`, the glob `*.bloom`, a kind of zip file, with the application
icon), and a package installs it to `share/mime/packages`. The desktop entry names the type in
`MimeType=` and runs `Bloom %f`, so a file manager starts Bloom with the project's path; Windows
does the same for the registered extension. `Bloom <file>` opens a project on every platform:
`projectPathFromArguments()` takes the first argument that is not a switch or an address (a
relative path is resolved against the working directory), and `openProjectFromArguments()` hands
it to the project host once the event loop runs, so it opens exactly as the file-open event does.
Bloom has no single-instance hand-over, so a second start opens a second window. The package
check fails on a Linux package without the `MimeType=` line, the `%f` argument or the
description, and `bloom.quality.linux_file_type` checks that the description, the desktop entry
and `Info.plist.in` name the same type and extension.

### Application Icon

The layered icon `apps/bloom/icon/Bloom.icon` (a background gradient fill and the foreground
as SVG, in the macOS 26 layered icon format) is the one source of the artwork. The glass image
used outside macOS 26 is exported from it at 2048 pixels and checked in beside it
(`bloom-glass-2048.png`); `apps/bloom/icon/README.md` has the command that exports it again.

`tools/quality/generate_app_icons` takes one source per target. The Windows and Linux tile
(`bloom-tile-1024.png`, the glass tile with a small margin) gives `Bloom.ico` from 16 to 256
pixels and the Linux icons from 16 to 512 pixels that a package installs with its desktop entry.
The older-macOS icon (`bloom-macos-legacy-1024.png`, an 824-pixel body and its shadow on a
1024-pixel canvas) gives `Bloom.icns` at every size from 16 to 1024 pixels, for macOS 13.3 to 15.
The `bloom.quality.app_icons` test checks every generated file against its source and names the
stale file and its source when one differs.

On macOS 26 and later the layered icon itself is compiled into the bundle's asset catalog,
`Contents/Resources/Assets.car`, so the system draws its default, dark, clear and tinted looks.
The build step `bloom-bundle-assets` (`apps/bloom/CMakeLists.txt`) runs `actool` from Xcode 26
on `Bloom.icon`, and runs again when a file of the icon changes. It finishes before
`bloom-deploy-macos` signs the bundle. `actool` also writes a `Bloom.icns` that stops at 256
pixels; it is not used. The bundle keeps `apps/bloom/macos/Bloom.icns`. Configure finds the tool
and the macOS 26 SDK. Where either is missing, configure prints one status line and the build
still succeeds without the catalog. That bundle is for a developer's machine; the package check
refuses it as a release. `Info.plist` carries `CFBundleIconFile` and `CFBundleIconName`, both
`Bloom`. The Bloom project document type (`.bloom`) and its type declaration name the same icon
(`CFBundleTypeIconFile`, `UTTypeIconFile`), so a project file shows the app icon.

The installer pictures are in `apps/bloom/installer`: the macOS disk image background at 660 x 400
and 1320 x 800 (the logotype, an arrow from the app to the Applications folder, and a line that
says how to install), and the Windows installer's welcome and finish picture and header picture
at the seven display scales from 100 to 250 percent. Its README says which file belongs to which
scale and how the files were made. The `bloom.quality.installer_pictures` test
(`tools/quality/check_installer_pictures`) checks the names, sizes and formats, and that nothing
else is in the folders.

The package check's `icons` rule runs on every package, complete or not. On Linux it needs the
eight icon sizes, each a PNG of exactly its size, and a menu entry with
`Icon=id.afterlab.bloom`. On macOS it needs `Bloom.icns` with an image at every size from 16 to
1024 pixels, the icon keys above, and, for a complete package, `Assets.car`. On macOS 13.3 to 15
the check is the `Bloom.icns` file, not a run on that system. On Windows it needs an icon
resource in `Bloom.exe` (`tools/quality/check_program_icon`). The installer pictures inside
the installers are checked with the installers.

Media on macOS is read and encoded by the Apple frameworks first, with the bundled FFmpeg worker
behind them ([Media I/O](media-io.md)). Continuous integration builds macOS in qualified mode on
hosted Apple silicon runners; a hosted runner may have no usable GPU device, and its GPU tests
then report not run.

The immutable still-source seam uses native `std::filesystem::path` file opening, UTF-8 locator/base
identity, fixed-width digest/counters and `steady_clock` timings. Decoder input is one owned byte
vector on every platform, with independent EXR/stb streams and subtraction-checked request budgets.
A filename suffix is appended to the native path without narrowing its existing Unicode spelling.
The source ticket does not depend on filesystem timestamps, watcher semantics or an operating-system
file handle surviving external mutation. The shared fallback tests cover same-size/same-timestamp
replacement, concurrent decode budgets, cancellation, refresh generations, gesture read-only access,
final revalidation and charged worker retirement; execution on each platform is still required.

Linux video source preparation now owns a disk-backed, SHA-256-verified anonymous read-only file.
The process supervisor installs that typed host capability on a dedicated inherited descriptor; the
FFmpeg provider consumes the exact descriptor through custom AVIO. Source-byte quotas include live
captures and pending retirement until the worker closes the backing. The macOS video provider keeps
its existing fresh-verification path and does not advertise retained-video hash elision. Windows
prepares the same verified private copy, without a pathname, and hands it to the worker as a
read-only inherited handle ([Media I/O](media-io.md)). Portable descriptor plumbing alone is not
Apple video qualification. The Linux source fixture covers same-size/same-mtime replacement, removed originals,
uncached frame parity from retained bytes, cancellation, quota, final revalidation, refresh/reconnect,
gesture read-only behavior and charged worker retirement.

## Windows

Windows uses the system's Vulkan driver through the locked Vulkan loader, which is built with the
dependency prefix and found beside the programs: the build copies `vulkan-1.dll` beside the
application and installs it beside the programs of a package, and the application opens exactly
that file. (Before that the Windows application was built with its GPU preview service off, so
its Viewer and exports ran on the CPU reference path while the tests, which name the loader
themselves, used the device.) Bloom targets x86-64 Windows 10 version 1903 and
later, built with MSVC. Every executable carries a manifest that makes the process code page
UTF-8 and the process long-path aware, so a name in any script and a path longer than 260
characters reach the system as written, in Bloom's code and in the libraries it links; version
1903 is the first that honors the code page. The main thread of every executable has an 8 MiB
stack, as the other platforms' main threads do.

What was measured, on an x86-64 desktop with an NVIDIA GeForce RTX 3080 Ti running Windows 11
(2026-11-12), in a qualified build:

- The GPU coverage contract and the native acceptance pass with the contract's exception lists
  unchanged and all six render routes proven.
- `bloom.sweep.nodes` renders all 98 node types that have an image output and a buildable graph
  on the GPU with no failure, and the recorded baseline is byte-identical to the Linux x86-64
  one: the CPU reference gives the same bits on both, which `bloom.sweep.baselines-windows`
  holds.
- The Viewer uses the adopted preview route (prepared on the GPU, painted through the widget), as
  on macOS, and the route proof contract holds the Windows preview proofs to the same
  host-painted rule. The resident presentation route needs a Wayland surface and is not built
  for Windows; no Win32 surface route exists.
- Run-time shader compilation, OCIO GPU programs, LUT import and the File Transform work. The LUT
  helper is confined with a restricted token at untrusted integrity inside a job object
  ([Color management](color-management.md)).
- The focused gate above, run by hand: native mode with a required device passes its ten suites
  on the discrete GPU, and fallback mode passes in a build with Vulkan off.
- Projects, imports and renders work in folders with non-ASCII names and under paths longer than
  260 characters. A project named by a network share path (UNC) saves and renders; the share
  tried was one the machine serves to itself, not a second machine.

Not measured: any Windows release other than the one named, Windows 10 in particular; a GPU from
another vendor; a machine with no Vulkan device; the desktop file dialogs and drag and drop with
non-ASCII names, which need a person at the screen. The minimum version says where the binaries
are allowed to start, not where they were tested.

H.264 and HEVC on Windows are decoded and encoded by the system's Media Foundation transforms,
on the graphics device when the computer has one, behind the bundled FFmpeg worker's container
code; everything else, and anything the system refuses, is the bundled FFmpeg's
([Media I/O](media-io.md)). AV1 export holds SVT-AV1 to its SSE4.2 code
([Dependency intake](dependency-intake.md)). Installed fonts are the system font collection
DirectWrite reports, which includes the fonts a user installed for themselves. A developer build runs from its build folder
with Qt's and the prefix's libraries on the search path. Continuous integration has a Windows job
on a hosted runner in qualified mode, which is not a required check; a hosted runner has no usable
GPU device, and its GPU tests then report not run.

Tests that hide the GPU do it through the Vulkan loader's driver variables, which the loader
ignores in an elevated process; run the suite from an ordinary, non-elevated account.

## Export Queue Platforms

Export runs on every platform ([ADR 0035](../decisions/0035-media-provider-fallback-chain.md),
which supersedes the Linux-only statement of [ADR 0033](../decisions/0033-export-queue.md),
section 15).
The export queue model, persistence, availability, the Export panel, presets and
scripting edits are identical on Linux, macOS and Windows; a project saved on any platform opens
on every other. Preset availability is computed per codec and encoder
on every platform; there is no per-platform refusal code. User presets live in the platform user-data directory.
