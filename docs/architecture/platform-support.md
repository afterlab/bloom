# Cross-Platform Support Contract

Status: working

Implementation status: the portable staged-artifact interface has tested Linux, macOS and Windows
providers with checked writer close, no-follow exact-stage reopen, bounded verifier reads, explicit
acceptance or rejection, and publication blocked until acceptance. Any other system fails
explicitly as unsupported. Format-specific semantic validators and cancellation/progress
integration remain pending. What was measured on each platform is in the macOS and Windows
sections below.

Updated: 2026-11-12

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
in `qualified` mode the same way; Qt comes from its official binaries there and is not locked.
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
  the CPU path. Every other platform proves the resident route, and a host-painted proof is
  refused there.

Not measured: any macOS release from 13.3 to 25, and an Intel Mac. The deployment target says
where the binaries are allowed to start, not where they were tested.

The application bundle is `org.kinetik.bloom`, with its icon, the minimum system version of the
deployment target, the media workers under `Contents/libexec/bloom/media`, the color worker and
the shader tools beside the executable, and the loader and driver as above. It carries the
Python interpreter (see Scripting Interpreter above): the library in `Contents/Frameworks`, the
standard library and numpy in `Contents/Resources/python` and Bloom's Python package in
`Contents/Resources/bloom-python` ([Dependency intake](dependency-intake.md)). The
`bloom-deploy-macos` target adds Qt with `macdeployqt`, signs the result ad hoc, and then checks
that every program and library in the bundle resolves inside it or in the operating system and
that none was built for a newer system than the bundle declares; there is no Developer ID
signature or notarization, so a copy downloaded from the internet is stopped by Gatekeeper until
the person opening it allows it.

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
dependency prefix and found beside the programs. Bloom targets x86-64 Windows 10 version 1903 and
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
DirectWrite reports, which includes the fonts a user installed for themselves. There is no installer or package yet: Bloom runs from a build folder
with Qt's and the prefix's libraries on the search path. Continuous integration has a Windows job
on a hosted runner in qualified mode, which is not a required check; a hosted runner has no usable
GPU device, and its GPU tests then report not run.

Tests that hide the GPU do it through the Vulkan loader's driver variables, which the loader
ignores in an elevated process; run the suite from an ordinary, non-elevated account.

## Export Queue Platforms

Export runs on every platform ([ADR 0035](../decisions/0035-media-provider-fallback-chain.md),
which supersedes the Linux-only statement of [ADR 0033](../decisions/0033-export-queue.md),
section 15).
The export queue model, persistence (document 1.29), availability, the Export panel, presets and
scripting edits are identical on Linux, macOS and Windows; a 1.29 project saved on any platform opens
on every other. Preset availability is computed per codec and encoder
on every platform; there is no per-platform refusal code. User presets live in the platform user-data directory.
