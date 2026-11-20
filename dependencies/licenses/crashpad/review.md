# Crashpad License And Feature Review

Reviewed: 2026-11-20

## License

- Whole-component SPDX expression: `Apache-2.0 AND MIT AND LicenseRef-getopt-public-domain`.
- `LICENSE` is Crashpad's Apache License 2.0. The two pieces of third-party code inside the
  archive that the build compiles carry their own texts, bound beside it:
  `third_party-mpack-LICENSE` (MPack 1.1.1, MIT, compiled into `libcrashpad_mpack.a` on every
  platform) and `third_party-getopt-LICENSE` (Gregory Pietsch's getopt, placed in the public domain
  with a request for credit, compiled into the handler on Windows only).
- The rest of the archive's `third_party` tree (googletest, cpp-httplib, libfuzzer, edo, fuchsia,
  ninja, xnu, the zlib submodule) is not compiled.
- `sourceObligation: none`. `modified: true`: two reviewed patches under
  `dependencies/patches/crashpad/`, described below.

## Why It Is Required

Issue #736: a crash of the application, `bloom-cli` or `bloom-mcp` must leave a minidump with every
thread's stack and a short text report on the machine, written by a process that did not crash, so
that a crash on any thread, an abort, a stack overflow or a crash inside a GPU driver is still
recorded. Crashpad is the established open-source library for exactly that on Linux, macOS and
Windows.

## Reached Scope And Minimization

- Static libraries only (`crashpad_client`, `crashpad_handler_lib`, `crashpad_snapshot`,
  `crashpad_minidump`, `crashpad_util`, `crashpad_compat`, `crashpad_tools`, `crashpad_mpack`,
  and `crashpad_getopt` on Windows), with headers under `include/crashpad` and the CMake package
  `lib/cmake/crashpad`.
- Bloom's programs link the client. Bloom's own handler program, `bloom-crash-handler`, links the
  handler library and ships in every package; the fork's `crashpad_handler` program is built by the
  recipe and is not shipped.
- `CRASHPAD_ENABLE_STACKTRACE` (client-side stack walking with libunwind) and the fork's tools are
  off. zlib is the prefix's own.

## Patches

`0001-build-without-an-http-transport.patch` replaces each platform's HTTP transport (libcurl on
Linux, NSURLSession on macOS, WinHTTP on Windows) with a transport that does not exist:
`HTTPTransport::Create()` returns nothing, which the upload thread already treats as a permanent
failure. Bloom never gives the handler an upload address; with the patch, a report cannot leave
the machine even if one were given. It also removes libcurl, which no other Bloom component needs,
from the Linux build.

`0002-declare-the-targets-before-the-system-zlib.patch` reorders the installed
`crashpad-config.cmake`: it includes the targets file first and only then gives the imported
`crashpad::zlib` target the system zlib (`CRASHPAD_ZLIB_SYSTEM`). Upstream's order sets properties on
a target that does not exist yet, so a consumer's `find_package(crashpad)` fails. Nothing compiled
changes.
