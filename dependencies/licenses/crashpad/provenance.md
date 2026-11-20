# Crashpad Provenance

Reviewed: 2026-11-20

## Source

- Upstream project: <https://github.com/getsentry/crashpad>, Sentry's fork of Crashpad
  (<https://chromium.googlesource.com/crashpad/crashpad>) that adds a CMake build of the same code
- Source ref: exact commit `0bd6ca43a0958a86939a37569b61162f92f9a022` (2026-10-19), the revision
  sentry-native 0.17.6 pins as its `external/crashpad` submodule
- Archive: `https://codeload.github.com/getsentry/crashpad/tar.gz/0bd6ca43a0958a86939a37569b61162f92f9a022`
- SHA-256 of the acquired archive:
  `d5c4eb0a553be3f7d2bee4723851652aee6fe6067835a498915e39e73ca3a618`
- Exact archive size: `1885404` bytes

## Acquisition Record

- Acquired 2026-11-20 over HTTPS (curl). GitHub reports the commit's signature as verified.
- Archive inspection: 1,410 entries, **0 symbolic links, 0 absolute paths, 0 `..` paths**.
- GitHub publishes no detached signature for this commit archive (`provenancePolicy: not-published`).
- The archive leaves the fork's three submodule directories empty. Two of them are separate lock
  components at the commits this revision names: `mini-chromium` (`third_party/mini_chromium/
  mini_chromium`) and `linux-syscall-support` (`third_party/lss/lss`, Linux only). The third,
  `third_party/zlib/zlib`, is not used: the build takes the prefix's locked zlib.

## Why The Fork

Upstream Crashpad builds only with GN and depot_tools, which the superbuild would have to lock
and run beside CMake on every platform. The fork's CMake files are translations of the upstream
`BUILD.gn` files, and the fork otherwise carries upstream Crashpad with a short list of changes
(`README.getsentry.md`): file attachments on macOS and Windows, which Bloom uses for its text
report, MinGW support, a Windows screenshot option and C++17. sentry-native, a widely used crash
reporting library, builds this same revision with CMake on Linux, macOS and Windows.

## Status

- NOT QUALIFIED. Acquisition provenance only. No production prefix manifest exists.
