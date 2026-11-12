# Dependency Intake And Qualification

Status: working

Updated: 2026-10-31

## Purpose

Bloom's dependency graph is part of the product. It affects decoded pixels, color transforms,
project safety, cross-platform behavior, installation size, licensing, and the ability to reproduce
a release. This contract turns a third-party library from a convenient local package into a
qualified Bloom build input.

ADR 0019 fixes the mechanism: a separate offline-capable superbuild produces a qualified prefix,
and the application build consumes it. This document defines the implementation and acceptance
boundary. The mechanism is accepted; the superbuild, lock, prefixes, and individual dependency
profiles remain unqualified until they pass the gates below. The version 1 artifact grammar,
identity, resource, and qualification rules below are frozen; production component values and
Unicode digests remain pending verified intake and must not be fabricated to populate the shape.

Implementation status: the two exact Draft 2020-12 lock/prefix schema families, including reviewed
lock schemas 1.2, 1.3 and 1.4, the offline checker for synthetic contract fixtures, and production lock
validation are implemented — including the reviewed Unicode 15.1 bootstrap tables with two-source
digest equality, repository-artifact digest binding, and an ASCII-strict v1 string tightening that
defers NFC machinery. The reviewed production lock contains the static baseline and FFmpeg 8.1.2,
the first shared component. FFmpeg builds offline from its digest-verified official archive as a
worker-only LGPL candidate with a corresponding-source obligation, an explicit configure-argument
allow-list, and an optional VA-API feature. `cmake/BloomDependencyPrefix.cmake` implements
explicit `qualified`/`developer-system` consumption with restricted package search, the
ABI-relevant toolchain-agreement subset, and Linux shared-library SONAME inventory checks;
`tests/dependency-consumption` exercises both the FFmpeg package wrapper and the desktop boundary.
The supervised worker now exists with a synthetic provider only. Its
[worker rpath policy](media-io.md#worker-rpath-policy) keeps each provider generation’s shared
dependencies private; MEDIA-3 owns FFmpeg linkage and installed-package verification.
Still pending: the production prefix manifest and its validator (with complete filesystem
inventory and no-follow/hardlink/link-chain evidence), a trusted qualified identity capability,
full Unicode collision data behind the ASCII tightening, exact tool-identity capture at the
qualify phase, and mode-absence enforcement once the first in-tree target consumes a package.

## Repository Shape

The dependency implementation should enter the repository as one cohesive boundary:

```text
dependencies/
  dependencies.lock.json       canonical reviewed lock
  superbuild/CMakeLists.txt     dependency-only composition root
  superbuild/projects/          one recipe per direct dependency
  patches/<component>/          immutable, documented patches
  licenses/                     reviewed license/source metadata
  schemas/                      lock and prefix-manifest schemas
cmake/
  BloomDependencyPrefix.cmake   normal-build prefix validation
```

Recipe, provenance, license, and test subdirectories may expand as components enter, but the lock,
schema, and prefix-manifest paths fixed below do not change within v1. The ownership separation may
not change: the normal root CMake build must not contain a parallel web of `ExternalProject`,
`FetchContent`, or dependency-specific patch logic.

The production lock path is exactly `dependencies/dependencies.lock.json`. A qualified prefix
contains its manifest at exactly
`share/bloom/dependencies/prefix-manifest-v1.json`, relative to the prefix root. Schema artifacts
live at `dependencies/schemas/dependency-lock-1.1.schema.json` and
`dependencies/schemas/prefix-manifest-1.0.schema.json`. None of those paths may be redirected by a
value inside an artifact.

`dependencies/tests/fixtures/` is reserved for synthetic schema, canonical-byte, and hostile-input
fixtures. A file below that directory is never a production lock, prefix, provenance record, or
qualification result even if its bytes are otherwise valid. Production validation and CMake
consumption reject a lock or prefix manifest resolved below that directory. Fixture URLs use the
reserved `example.invalid` domain, fixture digests bind only checked-in synthetic bytes, and no
fixture may make an upstream, security, license, or conformance claim.

## Dependency Artifact Canonical JSON v1

The lock and prefix manifest use the same closed canonical JSON profile. Schema artifacts remain
readable repository JSON and are not themselves instance bytes in this profile.

- Instance bytes are shortest-form UTF-8 without a BOM, insignificant whitespace, or a trailing
  newline. Every decoded string and object key is Unicode 15.1 NFC.
- The only JSON numbers are unsigned integers in the inclusive `uint64` range, written in shortest
  decimal form. Schema versions and `applyOrder` additionally fit `uint32`. Floating point,
  exponent notation, a sign, and negative zero are invalid.
- Duplicate decoded object keys are invalid. Every object has exactly the members declared by its
  v1 schema, in the order declared below; no member is optional and unknown members are invalid.
  A conditionally absent value is represented by the declared `null`, empty array, or empty string
  form rather than by omitting its member.
- Strings emit non-control Unicode directly as UTF-8. They escape `"` and `\\`, use `\b`, `\f`,
  `\n`, `\r`, and `\t`, encode other U+0000–U+001F controls as lowercase `\u00xx`, and never escape
  `/` or a non-control scalar. Escaped surrogate pairs and alternative spellings of direct
  non-control Unicode are therefore non-canonical even when generic JSON would decode them.
- Arrays that are sets are strictly ordered by unsigned UTF-8 bytes of their declared identity
  field and contain no duplicate identity. Component dependencies order by `name`, then
  `relationship`; CMake options order by `name`; qualification results order by `gateId`;
  installed paths order by `path`. Patch arrays instead use contiguous `applyOrder` values starting
  at zero and occur in that order. The five Unicode data records use the fixed order below.
- A schema-specific decoder validates types, closed enums, ordering, bounds, and cross-references,
  then reproduces the complete canonical byte stream. Byte inequality is non-canonical input. A
  generic JSON parser or JSON Schema validator alone cannot establish acceptance.

Digest text is always lowercase `sha256:` followed by exactly 64 lowercase hexadecimal digits.
Dates are `YYYY-MM-DD`; qualification timestamps are UTC `YYYY-MM-DDTHH:MM:SSZ` with no fractional
seconds. A namespaced ID is ASCII `[a-z0-9][a-z0-9._-]{0,127}`. A CMake target name is 1–256
printable ASCII bytes excluding whitespace, semicolon, quote, backslash, and control bytes. A CMake
option name is ASCII `[A-Za-z_][A-Za-z0-9_.-]{0,255}`. Logical paths use the portable path profile
below and are at most 1,024 UTF-8 bytes. URLs, tool identities, option values, versions, reasons,
and artifact-reference paths are at most 4,096 UTF-8 bytes and non-empty unless their field is
explicitly nullable. The absolute decoded-string ceiling remains 1 MiB.

The schemas use JSON Schema Draft 2020-12 with absolute IDs
`urn:kinetik:bloom:schema:dependency-lock:1.1` and
`urn:kinetik:bloom:schema:dependency-prefix-manifest:1.0`. They use repository-local `$ref` values,
declare every required member, set `unevaluatedProperties: false` on every object, and carry the
closed lexical, enum, and count constraints expressible in that dialect. A standard-library-only
offline repository checker verifies the exact schema artifacts and schema-specific fixture values;
it never resolves a remote meta-schema. Member order, canonical bytes, Unicode table identity,
cross-record equality, graph closure, and filesystem evidence remain explicit checker rules rather
than claims delegated to JSON Schema.

Lock schema 1.1 retains the v1 canonical ordering rules while permitting the reviewed optional
component fields needed by non-CMake and shared-library dependencies: exact `configureArguments`,
source archive size/retrieval date, `shared` linkage, and corresponding-source archive digest.
Components that do not need those fields continue to use the 1.0-compatible static shape.

Lock schema 1.3 is the minimal honest-vocabulary extension. It copies the 1.2 shape, ordering,
limits, and closure unchanged and alters only the `profileBuild` contract: `linkage` gains
`header-only` (for a header/data-only dependency whose install is headers, registry data, and a
CMake package config) and `executable` (for a non-shipping build- or qualify-time tool).
`shippingRoles` may be an empty array only for `executable` linkage; every other linkage still
declares at least one role. Empty roles mean the tool's installed binary is build/qualification
support and is not part of the end-user application package; they do not exempt any installed file
from the prefix inventory and they make no qualified-prefix claim. A reviewed executable tool that
is a genuine runtime product instead declares its real roles: the pinned `glslangValidator` and
`spirv-val` GPU color-compilation tools record `["executable", "license"]`, ship beside the
desktop, CLI, and MCP executables through `cmake/BloomGpuShaderTools.cmake`, and travel with their
reviewed license texts and a generated digest inventory rather than claiming build-only status. The frozen 1.0, 1.1, and 1.2
schema bytes are unchanged, and a 1.2 lock remains closed to the new enums. The generator selects
1.3 the moment a reviewed record carries a header-only or executable linkage and never downgrades
an existing 1.3.

Lock schema 1.4 adds one optional member to the `profileBuild` contract and changes nothing else:
`prebuilt`, for a component a profile takes as official upstream binaries instead of building it.
It is a non-empty array of at most 64 archives, ordered by URL and free of duplicates; each entry
is a closed object with the archive's HTTPS `url`, its `archiveSha256` and its `archiveSizeBytes`
(greater than zero). A build that carries the list has `shared` or `executable` linkage: official
binaries are used as published and never relinked. The component's `source` record stays the
exact source archive the binaries were built from, so the license, provenance and
corresponding-source rules apply to it unchanged. The frozen 1.0 to 1.3 schema bytes are
unchanged and a 1.3 lock remains closed to the list. The generator selects 1.4 the moment a
profile build carries `prebuilt` and never downgrades an existing 1.4.

Whoever fetches a `prebuilt` archive compares its size and SHA-256 with the lock before
unpacking it. `dependencies/superbuild/projects/prebuilt-fetch.cmake` does this for the
superbuild; the `bloom.quality.prebuilt_fetch` test proves that an archive with a wrong checksum,
a wrong size or missing from an offline cache is refused and nothing is unpacked.

### Runtime-fetched binary components

Lock schema 1.2 adds the optional `runtimeFetch` record for a component whose distributable binary
must be obtained by the end user's machine. It binds an HTTPS URL pattern, per-platform archive
and decompressed-library SHA-256 values, the installed library name, and the vendor license
artifact. The generated lock is still the only production lock; records under
`dependencies/lock-components/` remain the generator inputs.

No production component uses `runtimeFetch` today. OpenH264, the one component that did, is built
from its pinned source archive and linked into the worker's `libavcodec` instead (ADR 0036): Bloom
never downloads, verifies or installs a codec binary on the end user's machine. The schema record
stays available for a future component whose binary cannot be redistributed; such a component must
still be a one-click in-app action, never fetched at startup or from an unverified mirror.

### Bundled codec libraries

The permissively licensed codec libraries that FFmpeg's LGPL wrappers link -- OpenH264 (H.264
encode), Kvazaar (HEVC encode), dav1d (AV1 decode), SVT-AV1 (AV1 encode), libvpx (VP9 encode) and
libopus (Opus encode) -- are built from pinned source archives as static, position-independent
archives and linked into the worker's shared `libavcodec` on Linux, macOS and Windows. FFmpeg's
version script keeps their symbols private, so the worker ships no extra runtime file. Each is a lock component
with `static` linkage, a corresponding-source obligation (its archive completes `libavcodec`'s
corresponding source) and an FFmpeg `link` dependency edge. Its recipe reads an adjacent
`<name>.configure-arguments` vector (Meson or CMake options); the lock generator copies that file
into the component's `configureArguments` (`--configure-arguments <name>=<path>`), and the
production lock check compares the two line for line exactly as it does FFmpeg's vector (libvpx's
vector is its configure script's options). Meson and Ninja are build-host requirements for the
Meson-built codecs; x86 assembly uses the superbuild-private NASM (libvpx receives it through
`AS`, SVT-AV1 through `CMAKE_ASM_NASM_COMPILER`). On Windows libvpx's configure script generates a
Visual Studio solution with the static runtime, run from the same MSYS2 shell as FFmpeg's. The
Windows superbuild runs from a Visual Studio developer environment (`cl`, `link`, `nmake`) with
`MSYS2_ROOT` naming an MSYS2 installation that provides `make`, `patch`, `pkgconf` and `perl`, and
with Python (Meson, Ninja) on `PATH`. NASM builds with its own NMAKE file, so it needs no MSYS2
shell. FFmpeg's MSVC install leaves the import libraries in `bin/`; a post-install step moves them
to `lib/`. The Windows build of SVT-AV1 is used with its SSE4.2 code only (the worker passes
`asm=sse4_2`): its AVX2 code as built by this recipe wrote wrong pixels at 1918x1078 (the
reopened export differed from the source by up to 40501 of 65535), while its C and SSE4.2 code
on Windows and the AVX2 code on Linux are correct. The recipe is unchanged; the cause inside the
library is not found. A
patent grant shipped with a library (the AOM Patent License 1.0 with SVT-AV1, Google's VP8/VP9
grant with libvpx) is a notice file of its lock component and is reproduced in
`THIRD_PARTY_NOTICES.md`. `codec-static-install.cmake` appends each archive's runtime libraries to
its pkg-config record (FFmpeg reads them through `--pkg-config-flags=--static`), gives MSVC the
`<name>.lib` spelling and refuses any shared artifact.

Kvazaar fixes its sample depth at compile time, so its one lock component is built twice from the
same archive and vector: the 8-bit archive (`libkvazaar`) and a 10-bit archive (`libkvazaar10`,
issue #408) built with `KVZ_BIT_DEPTH=10` and the force-included `kvazaar10-symbols.h`, which gives
every external symbol a `kvz10_` prefix so both link into one `libavcodec`.
`kvazaar10-install.cmake` refuses an archive that still defines an unprefixed symbol (every
external symbol on ELF and Mach-O; Kvazaar's own names with MSVC, whose objects also carry shared
COMDAT helpers) and installs it under its own names (`libkvazaar10`, `include/kvazaar10/`,
`kvazaar10.pc`). FFmpeg reaches it through the reviewed `libkvazaar10` encoder patch.

NVENC uses nv-codec-headers, recorded with `header-only` linkage and an FFmpeg `build` edge: the
superbuild installs the MIT headers and `ffnvcodec.pc` (Linux and Windows), and FFmpeg's NVENC
encoders load the NVIDIA driver at run time, so no NVIDIA code is linked or shipped.

FFmpeg's x86 SIMD is enabled. NASM 2.16.03 is a source-built, build-host-only tool recorded with
`executable` linkage and no shipping roles; FFmpeg depends on it with the `build` relationship. It is
installed into a superbuild-private `tools/` directory rather than the dependency prefix and is
passed to FFmpeg through `--x86asmexe` on x86 targets only.

### Carried Python interpreter

Every Bloom package carries its own CPython, so scripting starts on a computer that has no
Python and a script gives the same result on Linux, macOS and Windows (issues #668, #677).
CPython is a lock component of every profile, built from the same release archive as a shared
library and installed under `python/` in the dependency prefix: by its configure script on Linux
and macOS (`python.cmake`), and on Windows by its own Visual Studio project files driven by
MSBuild (`python-windows.cmake`), which are given the locked components in place of the prebuilt
libraries they would fetch. One recorded patch to those project files is the only change to the
archive (`dependencies/patches/python/`); the interpreter's source is the same everywhere.

The modules are made of locked components only, from a private `python-support/` directory of
the prefix: OpenSSL (`ssl`, `hashlib`), XZ Utils' liblzma (`lzma`), Zstandard
(`compression.zstd`), SQLite (`sqlite3`), bzip2 (`bz2`) and libffi (`ctypes`); `zlib` and
`pyexpat` use the locked zlib and Expat of the prefix. Linux and macOS link them statically into
the modules. Windows carries OpenSSL, libffi and SQLite as libraries beside the modules, as
CPython's Windows build is made to. The module set is the same on every platform: `_tkinter`,
`_gdbm`, `_dbm`, `_curses` and `readline` are built nowhere, and the parts of the standard
library that need them are removed from the prefix. pkg-config is off for CPython's configure
step, so no library of the build machine can be picked up, and the recipe's install step
(`python-install.cmake`) imports the modules Bloom requires and refuses an interpreter in which
anything loads a library from outside its own directory and the operating system. Compilers are
told not to fuse a multiplication and an addition, which arm64 compilers otherwise do.

The carried interpreter has one third-party package, numpy, built from its source archive with
the Meson it carries and the locked Cython (a build tool that is not shipped), without a BLAS or
LAPACK library and without numpy's kernels for particular instruction sets, into the
interpreter's own `site-packages`. [Platform support](platform-support.md) records the module
set, the package set, how the set grows and what "the same result" covers.

OpenSSL on Linux and macOS is built with a configuration directory that does not exist, so it
trusts no certificate by accident. When Bloom starts the carried interpreter it sets
`SSL_CERT_FILE` to the operating system's certificate file (macOS: `/etc/ssl/cert.pem`; Linux:
the first that exists of the common distribution paths), unless `SSL_CERT_FILE` or `SSL_CERT_DIR`
is already set; HTTPS requests from scripts are verified against it. On Windows CPython reads
the system certificate store itself.

The Windows superbuild needs, beyond what the codec libraries need, a Perl that runs as a Windows
program for OpenSSL's Configure (MSYS2's `mingw-w64-ucrt-x86_64-perl`, or Strawberry Perl; the
MSYS2 shell's own Perl is refused) and MSBuild, which the Visual Studio developer environment
provides.

A qualified build takes CPython from the prefix (`Python3_ROOT_DIR` in the top-level
`CMakeLists.txt`) and the tests then run with it, the bench suite included. On macOS, with
`BLOOM_BUILD_PYTHON=ON` and no interpreter in the prefix, the build refuses to configure; on
Linux and Windows a prefix from before the interpreter was locked still configures against the
host's CPython and says so, and such a build is not what a package is made from. nanobind's
static core is built against the same interpreter. The shared lookup in
`src/scripting/python/interpreter_home.cpp` finds the interpreter inside a package by the
package's layout on each platform and falls back to the prefix in a build tree.
`cmake/package/stage_python.cmake` puts the interpreter into a package on every platform: on
macOS the library in `Contents/Frameworks`, the standard library with the bundled packages in
`Contents/Resources/python` and Bloom's package in `Contents/Resources/bloom-python`
(`cmake/BloomPackageLayout.cmake` has the Linux and Windows places). The license texts travel
with every other component's, in the package's `licenses` directory
(`cmake/package/stage_records.cmake`). The carried interpreter never writes into its own home:
the compiled form of the modules it imports goes to Bloom's per-user cache.
`cmake/package/check_package.cmake`, which the `bloom-deploy-macos` and `bloom-package` targets
end with, fails when a program or library in the package loads something that is neither in
the package nor a declared part of the operating system, carries a library search path that
leaves the package, or, on macOS, was built for a newer system than the bundle's
`LSMinimumSystemVersion`.

### Locked Qt

Bloom uses one exact Qt version on every platform (issue #679,
[ADR 0014](../decisions/0014-apache-license-and-qt-distribution.md)). Qt is the lock's `qt`
component. It is not built: every profile takes the Qt Project's official open-source binaries of
that version, named archive by archive, with SHA-256 and size, in the profile build's `prebuilt`
list. The component's `source` is the Qt source archive of the same version, which is the
corresponding source of those binaries; the lock records its SHA-256 and every package names it.

The superbuild's `bloom_dependency_qt` target (`qt.cmake`) reads the archives from the lock,
checks each one and unpacks them, unmodified, into `qt/` in the dependency prefix; no version,
URL or checksum is written anywhere else, and no separate installer is used. With
`BLOOM_DEPENDENCY_ALLOW_DOWNLOADS=OFF` the archives must already be in the cache when the
superbuild is configured. The directory records what it holds in `bloom-prebuilt.txt`, and a
directory that already holds the lock's archives is left untouched.

A `qualified` Bloom build takes Qt from that directory only (`cmake/BloomLockedQt.cmake`). A
prefix without it, or one that holds another version, stops the configure with the reason; a Qt
location cached by an earlier configure is dropped rather than kept. A `developer-system` build
uses the host's Qt 6.8 or later and stays Unqualified.

Bloom links Qt dynamically under LGPL-3.0-only. `dependencies/qt-modules.txt` lists every Qt
library and plugin Bloom uses, per operating system. The `bloom.quality.qt_modules` test
(`cmake/BloomQtModuleCheck.cmake`) fails when a CMake file names a Qt part that is not in the
list, and, in a qualified build, when the locked Qt's own software bill of materials (the
`sbom/*.spdx` files the official binaries carry) does not offer a listed part under
LGPL-3.0-only: a module published under the GPL or a commercial license only cannot enter the
build by being added to the list. `bloom.quality.qt_modules_self_test` proves both refusals on a
deliberately added GPL-only module.

### Linux Distribution Libraries And The AppImage Runtime

Two things in a Linux package are neither built by the superbuild nor lock components. Each
has an inventory that is the only place it is written, and each is taken by checksum.

- `dependencies/linux-runtime/packages.json`: six X11 helper libraries Qt loads, each from one
  Ubuntu 22.04 binary package named with its version, download places, SHA-256 and size, the
  library file with its own SHA-256, and the package's `copyright` file, which is checked in
  and compared with the one in the package. `linux-runtime-fetch.cmake` puts them into the
  prefix's `linux-runtime` directory; a package whose bytes differ stops the build, and a
  download place that serves other bytes is an error, not a reason to try the next.
- `dependencies/appimage-runtime/runtime.json`: the AppImage project's type 2 runtime, one
  release by SHA-256 and size, with its detached signature and signing key, the source commit
  it was built from, and everything linked into it with the license files checked in. It
  statically links libfuse under LGPL-2.1-only with one patch; the record names the exact
  libfuse source archive, and the patch is checked in. `appimage-runtime-fetch.cmake` puts it
  into the prefix's `appimage-runtime` directory. Only the AppImage form of a package contains
  it.

With downloads off, both must already be in the archive cache when the superbuild is
configured, like every other archive. Changing either is an intake: a new version, checksum
and license review, in the same change as the inventory.

### Closed Resource Limits

Limits are checked on encoded bytes before allocation and during parsing with checked arithmetic.
Deployment policy may lower but never raise these v1 ceilings.

| Resource | Lock v1 | Prefix manifest v1 |
| --- | ---: | ---: |
| encoded bytes | 16 MiB (`16777216`) | 128 MiB (`134217728`) |
| nesting depth, root included | `64` | `64` |
| total JSON values | `1000000` | `4000000` |
| one decoded string | 1 MiB (`1048576`) | 1 MiB (`1048576`) |
| one array | `200000` | `200000` |
| components | `4096` | `4096` |
| build profiles | `256` | exactly `1` resolved profile |
| dependencies, patches, options, feature decisions, or fixture sets per component/profile | `8192` | `8192` copied patch records per component |
| capabilities | `65536` | `65536` |
| CMake packages | n/a | `4096` |
| exported targets in one package | n/a | `8192` |
| installed path records | n/a | `200000` |
| one installed regular file | n/a | 4 GiB (`4294967296`) |
| aggregate installed regular-file bytes | n/a | 16 GiB (`17179869184`) |
| qualification gates/results | `4096` | `4096` |

## Lock v1 Shape

The following whitespace-expanded illustration shows every lock member in its required order.
Whitespace is added only for documentation; canonical instance bytes use the compact profile above.
Example strings and zero digests are test-fixture shape markers, not production values. The same
member order is used by the schema definitions for repeated records.

```json
{
  "format": "org.kinetik.bloom.dependencies.lock",
  "schemaVersion": {"major": 1, "minor": 0},
  "unicodeProfile": {
    "version": "15.1.0",
    "sourceUrl": "https://example.invalid/ucd.zip",
    "archiveSha256": "sha256:0000000000000000000000000000000000000000000000000000000000000000",
    "files": [
      {"path": "UnicodeData.txt", "sha256": "sha256:0000000000000000000000000000000000000000000000000000000000000000"},
      {"path": "CompositionExclusions.txt", "sha256": "sha256:0000000000000000000000000000000000000000000000000000000000000000"},
      {"path": "DerivedNormalizationProps.txt", "sha256": "sha256:0000000000000000000000000000000000000000000000000000000000000000"},
      {"path": "CaseFolding.txt", "sha256": "sha256:0000000000000000000000000000000000000000000000000000000000000000"},
      {"path": "NormalizationTest.txt", "sha256": "sha256:0000000000000000000000000000000000000000000000000000000000000000"}
    ]
  },
  "profiles": [{
    "id": "bloom.profile",
    "target": {
      "triple": "x86_64-unknown-linux-gnu",
      "operatingSystem": "linux",
      "architecture": "x86_64",
      "minimumOsVersion": null
    },
    "buildConfiguration": "release",
    "consumerAbi": {
      "cxxStandard": 20,
      "compilerFamily": "gcc",
      "compilerAbi": "gcc-abi",
      "standardLibrary": "libstdc++",
      "standardLibraryAbi": "libstdcxx-abi",
      "cxxRuntime": "libgcc",
      "cxxRuntimeAbi": "libgcc-abi",
      "cxxRuntimeLinkage": "dynamic",
      "platformRuntime": "glibc",
      "platformRuntimeAbi": "glibc-abi",
      "exceptions": true,
      "rtti": true,
      "libstdcxxCxx11Abi": 1,
      "msvcRuntime": null,
      "msvcIteratorDebugLevel": null,
      "windowsSdk": null,
      "windowsSdkVersion": null,
      "appleSdk": null,
      "appleSdkVersion": null,
      "appleDeploymentTarget": null,
      "abiFlags": []
    },
    "toolchain": {
      "cmake": {"name": "cmake", "version": "version", "identity": "identity"},
      "generator": {"name": "generator", "version": "version", "identity": "identity"},
      "buildTool": {"name": "tool", "version": "version", "identity": "identity"},
      "compiler": {"name": "gcc", "version": "version", "identity": "identity"},
      "linker": {"name": "linker", "version": "version", "identity": "identity"},
      "standardLibrary": {"name": "libstdc++", "version": "version", "identity": "identity"},
      "sdk": {"name": "linux-sysroot", "version": "version", "identity": "identity"}
    },
    "environment": {"profileId": "bloom.environment", "sourceDateEpoch": 0, "variables": []},
    "qualificationGates": [{"gateId": "bloom.gate", "disposition": "required", "reason": null}]
  }],
  "components": [{
    "name": "component",
    "version": "version",
    "source": {
      "url": "https://example.invalid/source.tar.gz",
      "archiveSha256": "sha256:0000000000000000000000000000000000000000000000000000000000000000",
      "commit": null,
      "provenancePolicy": "not-published",
      "provenanceReview": {"path": "dependencies/licenses/component/provenance.md", "sha256": "sha256:0000000000000000000000000000000000000000000000000000000000000000"},
      "provenance": []
    },
    "license": {
      "spdxExpression": "Apache-2.0",
      "licenseFiles": [{"path": "dependencies/licenses/component/LICENSE", "sha256": "sha256:0000000000000000000000000000000000000000000000000000000000000000"}],
      "copyrightFiles": [{"path": "dependencies/licenses/component/COPYRIGHT", "sha256": "sha256:0000000000000000000000000000000000000000000000000000000000000000"}],
      "noticeFiles": [{"path": "dependencies/licenses/component/NOTICE", "sha256": "sha256:0000000000000000000000000000000000000000000000000000000000000000"}],
      "sourceObligation": "none",
      "modified": false,
      "reviewRecord": {"path": "dependencies/licenses/component/review.md", "sha256": "sha256:0000000000000000000000000000000000000000000000000000000000000000"},
      "reviewedAt": "2000-01-01"
    },
    "patches": [],
    "dependencies": [],
    "profileBuilds": [{
      "profileId": "bloom.profile",
      "linkage": "dynamic",
      "cmakeOptions": [],
      "featureDecisions": [],
      "capabilities": [],
      "shippingRoles": ["library", "license", "notice"],
      "conformanceFixtureSets": []
    }],
    "securityReview": {
      "reviewedAt": "2000-01-01",
      "record": {"path": "dependencies/licenses/component/security.md", "sha256": "sha256:0000000000000000000000000000000000000000000000000000000000000000"},
      "vulnerabilities": []
    }
  }]
}
```

The repeated records not populated above have these exact member orders and vocabularies. An
artifact reference is always the exact object `path`, `sha256`; no supporting repository artifact
is represented by a bare path.

- provenance: `kind`, `evidence`, `identity`, `issuer`, `policy`, where `evidence` is an artifact
  reference, `kind` is `detached-signature`, `sigstore-bundle`, or `signed-tag`, and `issuer` is
  string or `null`;
- patch: `path`, `sha256`, `applyOrder`, `reason`;
- dependency: `name`, `relationship`, where `relationship` is `build`, `link`,
  `runtime-plugin`, or `vendored`;
- CMake option: `name`, `value`;
- feature decision: `id`, `state`, `reason`, where `state` is `enabled` or `disabled`;
- conformance fixture set: `id`, `artifact`, where `artifact` is an artifact reference;
- vulnerability: `id`, `disposition`, `record`, where `disposition` is `not-affected`,
  `mitigated`, or `accepted-risk` and `record` is an artifact reference;
- tool identity: `name`, `version`, `identity`; and
- environment variable: `name`, `value`, with names matching ASCII
  `[A-Z_][A-Z0-9_]{0,127}`.

Root profiles order by `id` and root components by `name`. Provenance records order by `kind`, then
`evidence.path`; license files, copyright files, and notice files order by artifact `path`; ABI
flags, capabilities, shipping roles, and target names order by their string value; dependencies
order by `name`, then `relationship`; profile
builds order by `profileId`; CMake options and environment variables by `name`; feature decisions,
fixture sets, vulnerabilities, and qualification gates by `id`/`gateId` as named in their records.
All are duplicate-free. Only patch order and the fixed Unicode-file order are positional.

`provenancePolicy` is `required` or `not-published`. `required` requires at least one evidence
record and successful policy verification during acquisition; `not-published` requires an empty
evidence array and a reviewed explanation at `provenanceReview`. A non-empty patch array requires
`license.modified` to be `true`; `false` is valid only when the locked source is unmodified.
`licenseFiles` is a non-empty array of artifact references; `copyrightFiles` and `noticeFiles` may
be empty only when the reviewed SPDX/license obligations permit it.

The application's Open Source Licenses window is generated from these arrays: every locked
component has an entry that carries each file its `licenseFiles` and `noticeFiles` name, whatever
the file is called. A component with no license file, or a named file that is missing, stops the
configure, and `bloom.ui.third_party_license_catalog` fails when a named file is not shown. Qt's
entry is its generated notice with the texts it names. Vendored sources that the lock does not
list are shown from the `LICENSE` file in their own folder.

`operatingSystem` is `linux`, `macos`, or `windows`; `architecture` is `x86_64` or `aarch64`;
`buildConfiguration` is `debug` or `release`; `cxxRuntimeLinkage` is `dynamic` or `static`; component
`linkage` is `static` or `shared` under lock schemas 1.0–1.2, and schemas 1.3 and 1.4 additionally
admit `header-only` and `executable`; `shippingRoles` is a set drawn from `library`, `executable`,
`plugin`, `data`, `cmake-package`, `license`, `notice`, and `source`. It must contain at least one
role for every linkage except `executable`, where the empty array is the honest record of a
non-shipping build-only tool. `sourceObligation` is `none` or `corresponding-source`.

`consumerAbi` has exactly the member order shown in the illustration, including `cxxRuntimeAbi`
between `cxxRuntime` and `cxxRuntimeLinkage`. `cxxStandard` is the unsigned integer `20` in v1.
`compilerFamily` is `gcc`, `clang`, `apple-clang`, `msvc`, or `clang-cl`; `compilerAbi` is a
non-empty exact ASCII ABI/toolset identity. `standardLibrary` is `libstdc++`, `libc++`, or
`msvc-stl`, and `standardLibraryAbi` is its non-empty exact ASCII ABI identity. `cxxRuntime` is
`libgcc`, `compiler-rt`, or `msvc`; `cxxRuntimeAbi` is a non-empty exact ASCII identity; and
`cxxRuntimeLinkage` is `dynamic` or `static`. `platformRuntime` is `glibc`, `musl`, `ucrt`, or
`macos-libsystem`, with a non-empty exact ASCII `platformRuntimeAbi`. None of those general fields
is nullable. `exceptions` and `rtti` are booleans.

`libstdcxxCxx11Abi` is the unsigned integer `0`, `1`, or `null`; it is non-null exactly when
`standardLibrary` is `libstdc++` and null otherwise. `msvcRuntime` is `dynamic-release`,
`dynamic-debug`, `static-release`, `static-debug`, or `null`, and
`msvcIteratorDebugLevel` is the unsigned integer `0`, `1`, `2`, or `null`. Both are non-null exactly
on Windows, where `standardLibrary` is `msvc-stl`, `cxxRuntime` is `msvc`, `platformRuntime` is
`ucrt`, their debug/release form agrees with `buildConfiguration`, and `cxxRuntimeLinkage` agrees
with the `static`/`dynamic` prefix of `msvcRuntime`; both are null elsewhere.

`windowsSdk` and `windowsSdkVersion` are both strings or both `null`. On Windows,
`windowsSdk` is exact string `windows-sdk`, `windowsSdkVersion` is a non-empty dotted-decimal
version, both SDK values equal the locked toolchain SDK name/version, and
`target.minimumOsVersion` is a non-null dotted-decimal version. Both SDK fields are null off
Windows. `appleSdk`, `appleSdkVersion`, and `appleDeploymentTarget` are all strings or all `null`.
On macOS, `appleSdk` is exact string `macosx`, the other two are non-empty dotted-decimal versions,
the SDK name/version equal the locked toolchain SDK name/version, and `appleDeploymentTarget`
equals `target.minimumOsVersion` byte for byte. A Windows profile requires `compilerFamily` `msvc`
or `clang-cl`; a macOS profile requires `compilerFamily` `apple-clang`, `standardLibrary` `libc++`,
`cxxRuntime` `compiler-rt`, and `platformRuntime` `macos-libsystem`; all three Apple fields are null
off macOS. Linux requires `compilerFamily` `gcc` or `clang`, requires `target.minimumOsVersion` and
every Windows/Apple field to be null, requires `platformRuntime` `glibc` or `musl`, permits
`standardLibrary` `libstdc++` or `libc++` and `cxxRuntime` `libgcc` or `compiler-rt`, and carries its
libc/kernel userspace floor in `platformRuntimeAbi`. Thus an applicable platform field is never null
and an inapplicable one is always null.

On MSVC-compatible profiles, `compilerAbi` includes the toolset ABI level. `abiFlags` is a sorted,
duplicate-free array of 1–512-byte printable ASCII single-argument tokens without whitespace,
semicolon, quote, backslash, or control bytes. It contains every additional recipe-owned flag or
macro that can change calling convention, layout, exception, RTTI, visibility, CRT, iterator, or
standard-library ABI; paths, optimization-only flags, and warnings are forbidden there. A recipe
has a closed ABI-input vocabulary and qualification rejects an ABI-affecting input not represented
by an explicit field or `abiFlags` entry.

Component and profile IDs are unique. Every `profileBuilds.profileId` names a root profile. Every
dependency names another component and may not name itself. Dependency records are direct edges;
the complete transitive graph exists because every reached dependency, including vendored code,
has its own component record. That rule is about dependencies reached THROUGH this pipeline: a lock
component carries a recipe, `profileBuilds`, a `consumerAbi`, and installed prefix artifacts the
prefix manifest validates. Third-party content whose bytes are checked into this repository and
compiled directly into a Bloom target has none of those and installs nothing, so the lock v1 shape
cannot represent it without inventing a recipe; it is inventoried in `THIRD_PARTY_NOTICES.md`
instead, and it still carries license text plus acquisition, license, and security review records --
either beside the vendored files (`src/ui/kit/third_party/<component>/`) or, when the application's
license catalog must ship its license text, in the same component-record shape under
`dependencies/licenses/<component>/` with no lock entry. `dependencies/licenses/stb_truetype/` is
that case and its own records state the determination. For each profile, components with a matching `profileBuild` form an
acyclic dependency graph that is closed over `link`, `runtime-plugin` and `vendored` edges. A `build`
edge may name a component the profile does not build: a build-host tool or header set that the
profile's recipe does not use (NASM and nv-codec-headers on an arm64 macOS target). Every provided capability has one component owner in that profile.
Every production profile has at least one participating component and one qualification gate; every
component has at least one profile build. A recipe owns a closed feature vocabulary, and each
feature in that vocabulary has exactly one enabled/disabled decision in every participating build;
an unrecognized or omitted recipe feature is a lock/recipe mismatch rather than ambient discovery.
Patch paths resolve below `dependencies/patches/<component>/`; evidence, review, fixture, copyright,
and notice artifact references resolve below their declared repository-owned dependency
directories and are regular checked-in files. Every one of those bytes matches the SHA-256 in its
reference before the lock is accepted. The lock identity therefore binds the digest of every
repository artifact supporting a source, license, security, patch, or conformance claim rather than
binding only its pathname.

Version ranges, floating branches, `latest`, mutable archive URLs, and unrecorded dependency-owned
downloads are invalid. Version and URL immutability require human review in addition to lexical
validation. A new version is a lock change even when it promises ABI compatibility. An empty
production component or profile array is invalid; test-only minimal fixtures remain subject to the
same rule.

### Unicode 15.1 Bootstrap

The `unicodeProfile.files` array has exactly the five records and fixed order shown above. The
reviewed official archive digest and each extracted-file digest are copied into a small
dependency-validator bootstrap allowlist outside the lock. The validator first hashes those raw
checked-in files at `dependencies/unicode/15.1.0/<path>` and compares the allowlist, then uses only
those verified tables to check lock NFC, full Default Case Folding, and normalization fixtures. The
allowlist constants live in the offline dependency-artifact checker and are covered by its golden
self-tests. The checker finally requires the lock's source URL, archive digest, filenames, and file
digests to equal the bootstrap values. This deliberate two-source equality prevents the lock from
authorizing the tables used to validate itself.

Full Default Case Folding uses the Unicode 15.1 common and full mappings, excludes Turkic-only
mappings, and applies Unicode 15.1 NFC after folding. A Unicode update changes the bootstrap
allowlist, lock, fixtures, and format/profile review together. Ambient ICU, Python, operating-system,
or filesystem Unicode behavior is never an acceptance oracle. Until the real official bytes and
digests are reviewed and checked in, no production lock or prefix can be called valid; schemas use
synthetic fixture digests rather than placeholders that resemble a production claim.

### Lock Identity

The lock identity is lowercase `sha256:` plus SHA-256 over the following bytes, with no terminator
after the lock:

```text
ASCII "bloom.dependencies.lock.v1"
u8 0
u64be canonicalLockByteCount
bytes[canonicalLockByteCount] canonicalLock
```

The count is checked before hashing. Identity is available only from a successfully decoded and
byte-reproduced lock; no public or CMake path computes a trusted identity for arbitrary bytes.

## Portable Archive Path Profile

Every source member name passes the versioned `bloom.portable-archive-path.v1` profile before any
filesystem object is created. The profile pins Unicode 15.1 normalization and full Default Case
Folding data; it never uses a host locale, filesystem collation, or platform case conversion. The
table source and digest are lock inputs and prefix-manifest evidence.

A member name must be shortest-form valid UTF-8. `/` is the only separator; any `\` is rejected
rather than translated. A directory entry may have one terminal `/`, which is removed before path
validation, while a file entry may not. After that removal the path must be relative and contain at
least one segment. Leading `/`, `//`, a drive-qualified or drive-relative prefix such as `C:/` or
`C:foo`, a UNC or extended-device prefix, an empty segment, and repeated or trailing separators are
invalid. Percent escapes and other textual encodings are never decoded into separators.

Each segment is normalized to Unicode 15.1 NFC and must satisfy all of these rules:

- it is neither `.` nor `..`, contains no U+0000, C0 control, or U+007F, and contains none of the
  portable-invalid ASCII characters `<`, `>`, `:`, `"`, `|`, `?`, or `*`;
- its normalized UTF-8 encoding is at most 255 bytes, and the normalized slash-joined path is at
  most 1,024 UTF-8 bytes;
- it does not end in U+0020 SPACE or U+002E FULL STOP; and
- the part before its first U+002E, after Unicode 15.1 full Default Case Folding and NFC, is not
  `con`, `prn`, `aux`, `nul`, `clock$`, `conin$`, `conout$`, `com1` through `com9`, `lpt1` through
  `lpt9`, `com¹`, `com²`, `com³`, `lpt¹`, `lpt²`, or `lpt³`.

Rejecting `:` forbids both drive syntax and NTFS alternate-data-stream names rather than relying on
the extraction host to interpret them. The reserved-name rule applies even when the segment has an
extension, so `NUL.txt` and `com1.anything` are invalid.

The normalized path is the extraction path. Its portable collision key is formed by applying
Unicode 15.1 full Default Case Folding and then NFC to each normalized segment and joining the
results with `/`. Before extraction, Bloom rejects duplicate keys, file/directory type disagreement
at one key, and any non-directory key that is a prefix of another member key. These checks include
explicit and implicit directory entries and therefore catch NFC aliases, case aliases, trailing-name
aliases, and archive-order tricks consistently on Linux, macOS, and Windows.

## Prefix Manifest v1 Shape And Identity

The prefix manifest root has exactly these members in this order:

1. `format`, exact string `org.kinetik.bloom.dependencies.prefix`
2. `schemaVersion`, exact object `major`, `minor` with value `1`, `0`
3. `lockIdentity`, the validated lock identity
4. `profile`, one complete profile object copied from the lock with the same member order
5. `unicodeProfile`, the complete Unicode object copied from the lock with the same member order
6. `components`, sorted component-result records
7. `capabilities`, sorted capability records
8. `cmakePackages`, sorted exported-package records
9. `installedFiles`, sorted installed-path records
10. `qualificationResults`, sorted gate-result records

All copied values compare equal to the validated lock; copying does not create a second source of
truth. Prefix component results have exact member order `name`, `version`,
`sourceArchiveSha256`, `sourceCommit`, `patches`, `buildOptionsIdentity`. `sourceCommit` is string
or `null`, and `patches` copies the matching lock patch records exactly. `buildOptionsIdentity` is
lowercase `sha256:` plus SHA-256 over:

```text
ASCII "bloom.dependencies.component-build.v1"
u8 0
u64be canonicalProfileBuildByteCount
bytes[canonicalProfileBuildByteCount] canonical profileBuild object from the lock
```

There is exactly one component result for every component participating in the selected profile
and no other component. Component results order by `name` and reproduce the lock version, source,
patch, and build identity.

A capability record has member order `id`, `providerComponent`; IDs are unique, sorted, and equal
the union of the participating components' locked capabilities. An exported CMake-package record
has member order `name`, `version`, `configPath`, `targets`; records order by `name`, package names
are unique, `configPath` names a recorded regular file with role `cmake-package`, and `targets` is a
strictly UTF-8-byte-sorted unique array of CMake target names. A package target required by Bloom
must be declared here before `find_package` executes.

An installed-path record has exact member order `path`, `type`, `component`, `role`, `size`,
`sha256`, `permissions`, `linkTarget`. `component` is a participating component name or `null` for
prefix-wide Unicode or generated qualification evidence. The closed vocabularies and valid field
tuples are:

| `type` | `size` | `sha256` | `permissions` | `linkTarget` |
| --- | --- | --- | --- | --- |
| `regular-file` | exact `uint64` byte count | content digest | `regular` or `executable` | `null` |
| `directory` | `null` | `null` | `none` | `null` |
| `symbolic-link` | `null` | `null` | `none` | normalized relative target string |

`role` is `directory`, `library`, `executable`, `plugin`, `data`, `cmake-package`, `license`,
`notice`, `source`, or `qualification-evidence`. A directory uses `directory`; a regular file or
symbolic link never does. Every directory, regular file, and permitted symbolic link below the clean
prefix is recorded exactly once except the prefix manifest itself, whose self-inventory would be
circular. No other unrecorded path is allowed. Component-owned non-directory roles equal the
matching locked `shippingRoles` except that `qualification-evidence` is non-shipping support data;
each declared shipping role has at least one path. A copied component artifact keeps that component
name even when its role is `qualification-evidence`; prefix-wide Unicode and generated gate
evidence use a null component. Regular-file hashes are taken after installation and path remapping,
before qualification. Checked accumulation enforces both the per-file and aggregate installed-byte
limits above. Qualification evidence paths are regular files in the inventory.
Package signing and application bundling occur later and produce a separate package inventory; they
never mutate a qualified prefix in place.

Every lock artifact reference begins with `dependencies/` and is repository-root-relative. The five
`unicodeProfile.files` entries are archive-member records rather than repository artifact
references; their checked-in and installed mappings are fixed separately above and below. Every
such reference reachable from a participating component—patches, provenance evidence and review,
license, copyright, notice, license review, security review, vulnerability disposition, and
conformance fixture evidence—is copied into the clean prefix at exactly
`share/bloom/dependencies/evidence/<path-after-dependencies/>`. It has a matching regular-file
inventory record and the same SHA-256. The five Unicode inputs are copied separately to exactly
`share/bloom/dependencies/unicode/15.1.0/<unicode-file-path>` with their locked digests.
License and copyright artifacts use `license`; notice artifacts use `notice`; patches, Unicode data,
reviews, provenance, security, vulnerability, fixtures, and gate evidence use
`qualification-evidence`. The `source` role is reserved for corresponding-source or source-offer
payloads selected by a locked `shippingRoles` entry, not for a patch merely because it modifies
source. A missing copy, digest disagreement, path-mapping disagreement, component disagreement, or
role disagreement makes the prefix unqualified. Artifact references inside prefix qualification
results are prefix-root-relative rather than repository-relative and directly match installed
records.

Source-archive extraction still materializes no links; its only tolerance is the
provenance-recorded skip defined under acquisition. A qualified Linux or macOS prefix may contain a symbolic
link only when its link text is relative, uses `/`, is already normalized, passes the portable path
segment rules, and resolves from the containing directory within the prefix to another recorded
path. All installed paths have unique portable collision keys, and directory/non-directory prefix
conflicts are invalid before the host filesystem is consulted. Link chains are
bounded to 32 records and must end at a recorded regular file or directory; dangling links, cycles,
absolute targets, and targets containing an empty, `.`, or `..` component are invalid. This admits
ordinary shared-library aliases and represents a macOS `.framework` entirely as recorded
directories, regular files, and bounded internal relative links—there is no opaque framework
exception. Windows prefixes reject symbolic links, junctions, mount points, and every other reparse
point. Hard links are invalid on all platforms. Qualification uses no-follow identity checks to
detect them rather than trusting manifest type text.

### Qualification Results

A qualification-result record has exact member order `gateId`, `status`, `evidence`,
`completedAt`. `status` is exactly `passed`, `failed`, or `not-applicable`; there is no trusted
`qualified: true` field. `evidence` is a sorted, unique, non-empty array of artifact references.
Each reference exactly matches an installed regular file with role `qualification-evidence`.
`completedAt` is the canonical UTC rendering of the selected profile's `sourceDateEpoch`, not the
qualification machine's wall clock. `sourceDateEpoch` is at most `253402300799`
(`9999-12-31T23:59:59Z`). Installed evidence is a deterministic canonical summary without host
paths, ambient timestamps, random IDs, or unordered tool output; verbose ephemeral CI logs remain
outside the prefix. Repeating the same locked build and gate inputs can therefore reproduce the
same manifest identity.

Results and the selected profile's `qualificationGates` have the same unique, sorted `gateId` set.
A `required` gate is accepted only with `passed`. A `not-applicable` gate is accepted only with
`not-applicable` and a non-null reason in the lock. `failed`, a missing or extra result, a status
that disagrees with the locked disposition, missing evidence, or an unknown enum makes the prefix
unqualified. A failed prefix manifest remains useful as immutable diagnostic output, but CMake may
not consume it in qualified mode.

### Prefix Identity

The prefix-manifest identity is lowercase `sha256:` plus SHA-256 over the following bytes, with no
terminator after the complete, successfully reproduced prefix manifest:

```text
ASCII "bloom.dependencies.prefix.v1"
u8 0
u64be canonicalPrefixManifestByteCount
bytes[canonicalPrefixManifestByteCount] canonical prefix manifest
```

The manifest does not store its own identity. This avoids a self-digest and leaves every byte,
including qualification results and installed-file digests, covered. Every structurally valid
manifest has a prefix-manifest identity; it is a `qualifiedPrefixIdentity` and may enter Bloom build
or runtime provenance only when all qualification rules above pass. Prefix manifest v1 is
checksummed by this identity and contains no signature field. A future detached signature may sign
the exact textual identity only after its trust and revocation contract is accepted. Application
packages retain the qualified identity and may be signed independently, but package signature
acceptance is not prefix qualification.

### Exact Matching And Consumer Compatibility

Qualification compares the selected prefix `profile`, `unicodeProfile`, participating components,
patch records, build-options identities, capabilities, and every provenance tool identity exactly
with the validated lock. No range, host discovery, or “compatible enough” comparison is permitted.

Consumption compares the requested profile ID, target triple, operating system, architecture,
minimum OS version including `null`, build configuration, and the entire `consumerAbi` object for
exact equality. Version 1 has only this `exact-abi-v1` policy: C++ standard, compiler ABI identity,
standard-library identity and ABI, runtime linkage, and ABI-affecting flags cannot use a range or
family heuristic. A compatible relaxation requires a later schema version and qualification data.

The CMake, generator, build-tool, linker, SDK, and exact compiler executable identities describe
how the prefix was built and must match the lock during qualification. They are not compared to the
paths or patch versions of tools running the later Bloom consumer build; those tools do not alter
an already-built dependency ABI. Any consumer compiler/standard-library fact that can affect ABI
belongs in `consumerAbi` and therefore compares exactly. The consumer also rehashes the manifest
and installed paths before resolving packages; a mismatch never falls back to host packages.

## Acquisition, Build, And Consumption

The pipeline has four explicit phases:

1. `acquire` downloads locked archives and provenance into a content-addressed cache and verifies
   every digest and published signature. It extracts only after verification into an empty staging
   directory through Bloom's bounded archive reader and the exact portable path profile above.
   Version 1 also rejects devices, FIFOs, and hardlinks. A symbolic-link entry is rejected unless
   the component's provenance review record enumerates that exact entry path and link target; a
   recorded link must have a relative target that resolves within the archive root, and the reader
   skips it — validating it against the record and materializing nothing — so an extracted staging
   tree never contains links. Unrecorded, absolute, or root-escaping links remain hard failures,
   and a recorded link's subtree must be outside every locked build's consumed sources. It permits
   at most 4 GiB
   source bytes, 200,000 entries, 16 GiB
   aggregate expanded bytes, 4 GiB per file, 1,024 UTF-8 bytes per logical path, and a 1,000:1
   aggregate expansion ratio. Checked counters and final path-containment verification are
   mandatory; a tighter component ceiling may be locked but may not exceed these hard maxima.
   With `BLOOM_DEPENDENCY_ALLOW_DOWNLOADS=OFF` the superbuild is strictly offline: configuring it
   fails, naming each recipe, when a locked archive is missing from `BLOOM_DEPENDENCY_DOWNLOAD_DIR`
   or does not match its locked SHA-256, so the build step never reaches the network. The check
   runs over every recipe's archive, including recipes added later.
2. `build` runs with networking disabled, applies only locked patches, and installs into a clean
   staging prefix. Dependency-owned auto-fetch is a hard failure. The profile starts from a
   documented environment allowlist, fixes locale and timezone, records `SOURCE_DATE_EPOCH`,
   removes user package registries, and remaps or rejects build/staging paths embedded in outputs.
3. `qualify` runs component, integration, malformed-input, license, and platform gates and writes
   the canonical checksummed prefix manifest defined above.
4. `consume` configures Bloom against that prefix. It validates schema, lock digest, target,
   toolchain/ABI, configuration, and required capabilities before resolving imported CMake targets.

The normal application build has no acquisition step and performs no network access in either
dependency mode. `qualified` mode is mandatory for release, package, conformance, and CI builds. It
requires an explicit prefix and validated production lock, runs the offline validator before any
dependency `find_package`, restricts package search to that prefix, disables package registries and
network-backed discovery, and fails configuration on every missing, unqualified, mismatched, or
mutated input. It never searches the host as a fallback.

An explicitly selected `developer-system` mode may use `find_package` outside a qualified prefix
for local iteration. It is a separate mode, never an automatic fallback from `qualified`. The
application About/diagnostic report and build metadata label it `Unqualified`; it cannot produce a
release package, conformance result, prefix manifest, qualified capability, or lock update. CI does
not use it. Absence of an explicit mode becomes a configuration error once dependency-prefix
consumption is implemented.

## Feature Minimization

Each recipe enables only the capabilities Bloom owns. For the first frame-output slice:

- OpenImageIO builds only the library functionality and image plugins required for PNG and OpenEXR;
- command-line tools, tests, examples, Python bindings, video, camera RAW, texture-system extras, and
  unrelated format plugins remain off;
- OpenEXR and Imath use locked external dependencies or explicitly qualified vendored components;
  their build may not silently fetch libdeflate, OpenJPH, or another package;
- OpenColorIO tools, Python bindings, tests, and optional integrations remain off unless a Bloom
  capability separately requires them; and
- all third-party targets are private implementation dependencies and treated as system headers for
  Bloom warning policy without suppressing Bloom adapter warnings.

The lock, not this prose, is the authority on the exact enabled graph.

## Initial Qualification Candidates

The following current releases are the first candidates for the color/output work. OpenColorIO,
OpenEXR, and Imath align with families named by the VFX Reference Platform CY2026. CY2026 does not
specify OpenImageIO, so Bloom selects and qualifies that candidate independently rather than calling
it platform-mandated. None is qualified or shipping until exact archives, hashes, transitives,
options, licenses, and all target builds exist in the lock:

| Component | Candidate | Reason for starting point |
| --- | --- | --- |
| OpenColorIO | 2.5.2 | Current 2.5 security/bug-fix release and CY2026 family |
| OpenImageIO | 3.1.16.0 | Current supported 3.1 candidate with recent malformed-input hardening; not a CY2026-listed component |
| OpenEXR | 3.4.15 | Current 3.4 security/bug-fix release and CY2026 family |
| Imath | 3.2.3 | Current 3.2 release and CY2026 family |

Upgrading a candidate before its first lock is expected if security review finds a newer fix. Once
locked, no automated job rewrites these versions; update automation may open a report or change for
human review.

## Qualification Matrix

Every locked profile must pass:

- GCC and Clang on supported Linux plus the production MSVC and Apple Clang toolchains;
- x86-64 Linux/Windows and Apple Silicon macOS, with deployment targets matching Bloom packages;
- Debug diagnostics and release-like optimized builds without unresolved symbols or accidental
  host-library references;
- exported package relocation and an install-prefix smoke consumer;
- Bloom's deterministic color and image-output fixtures, error translation, cancellation behavior,
  and resource limits;
- malformed and hostile input corpora under sanitizers where supported;
- license/notice/source completeness and automated comparison of actual packaged files to the
  declared distribution graph; and
- vulnerability scanning with manual disposition for reachable findings.

The VFX Reference Platform informs family alignment, especially on Linux. It does not override
Bloom's Windows/macOS parity gate or authorize an unreviewed patch version.

## SBOM And Release Evidence

Each native package carries or is accompanied by:

- an SPDX 2.3 JSON SBOM naming the application and all resolved shipped components;
- `LICENSE`, `NOTICE`, per-component license texts, and required attribution;
- exact corresponding source or a durable source offer where the selected license requires it;
- the dependency lock, prefix-manifest identity, Bloom revision, toolchain identity, and package-file
  digest inventory; and
- Qt replacement instructions and other obligations required by accepted
  [ADR 0014](../decisions/0014-apache-license-and-qt-distribution.md).

The SBOM is generated from the prefix and final packaging graph, then checked against the actual
artifact. A source-only inventory that omits dynamically loaded plugins or bundled tools fails the
gate.

## Upgrade And Incident Policy

An upgrade starts with a reviewed lock proposal, release/security notes, API/ABI assessment, and
license delta. It rebuilds the clean prefix and reruns all affected conformance and package gates.
Bloom never mutates a qualified prefix in place.

When a vulnerability affects a locked component, the response records reachability, exposure to
untrusted content, affected package versions, mitigation, and the replacement lock. Suppressing a
scanner finding without this record does not qualify a release.

### Security release tracking

The carried interpreter runs users' scripts against the network and their files, so its
components are followed for security releases as part of intake (issue #677). This covers
CPython, every library its modules are made of (OpenSSL, liblzma, Zstandard, SQLite, bzip2,
libffi, zlib, Expat), every bundled package (numpy) and what a bundled package links.

- Each of these components' security record (`dependencies/licenses/<component>/security.md`)
  has a "Security Release Tracking" section with a line `Advisory source: <https://...>` naming
  where its upstream publishes security releases. The production lock check
  (`bloom.quality.dependency_artifacts`) fails when the record of such a component has no such
  line; the set is computed from the lock: the `python` component, the components linked
  against it, and everything those link or carry.
- A review reads each advisory source and writes the date and what it found in the record's
  "Bounded Review" section; the lock's `securityReview.reviewedAt` carries the same date. Every
  release of Bloom is preceded by such a review of these components.
- A security release of the locked series of any of them moves the lock: a new component input
  with the new archive digest, review and provenance records, a rebuilt prefix on every
  profile, and the module-set and same-result tests on every platform. When a fix cannot be
  taken at once, the incident record above says what is reachable and how it is mitigated.

Primary references:

- [ADR 0019](../decisions/0019-reproducible-dependency-intake.md)
- [VFX Reference Platform CY2026](https://vfxplatform.com/)
- [OpenColorIO 2.5.2](https://github.com/AcademySoftwareFoundation/OpenColorIO/releases/tag/v2.5.2)
- [OpenImageIO 3.1.16.0](https://github.com/AcademySoftwareFoundation/OpenImageIO/releases/tag/v3.1.16.0)
- [OpenEXR 3.4.15](https://github.com/AcademySoftwareFoundation/openexr/releases/tag/v3.4.15)
- [Imath 3.2.3](https://github.com/AcademySoftwareFoundation/Imath/releases/tag/v3.2.3)
