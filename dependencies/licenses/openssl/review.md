# OpenSSL License And Feature Review

Reviewed: 2026-11-12

## License

- Whole-component SPDX expression: `Apache-2.0`. The archive's `LICENSE.txt` is bound beside this
  file as `LICENSE`.
- `sourceObligation: none`; `modified: false` with an empty patch set.

## Why It Is Here

Bloom carries its own Python interpreter in every package (see the `python` component). This
component gives that interpreter `ssl` and the OpenSSL-backed `hashlib` from one OpenSSL on
every platform, so a script's TLS does not follow the version an operating system has. Bloom's
own code does not call OpenSSL.

## Reached Scope And Minimization

- Built on every profile (`dependencies/superbuild/projects/openssl.cmake`), configured
  `no-tests no-apps no-docs no-module no-legacy no-engine`: no command line program, no loadable
  provider or engine, no legacy provider.
- Linux and macOS: also `no-shared`. `libssl.a` and `libcrypto.a` and their headers go into the
  prefix's private `python-support` directory and are linked into the interpreter's `_ssl` and
  `_hashlib` extension modules; no OpenSSL file is installed into a package. The configuration
  directory is a path that does not exist, so no `openssl.cnf` and no certificate directory on
  the machine is read by default.
- Windows: CPython's project files link OpenSSL as libraries of its own, so it is built as
  `libcrypto-3-x64.dll` and `libssl-3-x64.dll` (target `VC-WIN64A`, the Visual Studio compiler),
  which a package carries beside the interpreter's extension modules. The configuration
  directory is OpenSSL's default for Windows, under the Common Files directory that only an
  administrator can write: a directory an ordinary user could create would let that user plant
  a configuration file, and a configuration file an administrator installs there is read.
- Certificates: the interpreter is pointed at the operating system's certificates when Bloom
  starts it (see the `python` review).
- Assembly: OpenSSL's own perl-generated assembly. arm64 and x86-64 Linux assemble it with the
  system compiler; Windows with the locked `nasm` component, a build edge.
- Build host: Perl. On Windows a Perl that runs as a Windows program is needed (MSYS2's
  `mingw-w64-ucrt-x86_64-perl` or Strawberry Perl); OpenSSL's Configure refuses the MSYS2
  shell's own Perl.
- Recorded roles: `license` where it is linked statically (the notice travels with the
  interpreter that contains the code); `library` and `license` on Windows.
