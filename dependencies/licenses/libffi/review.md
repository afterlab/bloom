# libffi License And Feature Review

Reviewed: 2026-11-12

## License

- SPDX expression: `MIT`. The archive's `LICENSE` is bound beside this file.
- Not reached and not part of this record: the build tools the archive carries under
  `LICENSE-BUILDTOOLS` (libtool, autoconf and automake files, GPL with their distribution
  exceptions). They run while libffi is configured and built; none of their code is in the
  library.
- `sourceObligation: none`; `modified: false` with an empty patch set.

## Why It Is Here

Bloom carries its own Python interpreter in every package (see the `python` component). This
component is the foreign function interface behind the interpreter's `ctypes` module on every
platform, so `ctypes` does not follow the libffi an operating system has. On macOS the
operating system has a libffi of its own; the locked one is used there as well, because the
interpreter's link edge to this component has to hold in every profile that builds the
interpreter, and callbacks through it are exercised by the build's tests. Bloom's own code does
not call it.

## Reached Scope And Minimization

- Linux and macOS: `dependencies/superbuild/projects/libffi.cmake` builds a static,
  position-independent library with the archive's configure script (`--disable-shared
  --enable-static --with-pic --disable-docs --disable-multi-os-directory`) and installs it with
  its headers into the prefix's private `python-support` directory. It is linked into the
  interpreter's `_ctypes` extension module.
- Windows: built as `libffi-8.dll` with the archive's configure script and its `msvcc.sh`
  wrapper around the Visual Studio compiler, run from the MSYS2 shell the FFmpeg build already
  needs. CPython's project files link it as a library of its own; a package carries the DLL
  beside the interpreter's extension modules.
- Not built: documentation, the test suite.
- Recorded roles: `license` where it is linked statically; `library` and `license` on Windows.
