# OpenSSL License And Feature Review

Reviewed: 2026-11-12

## License

- Whole-component SPDX expression: `Apache-2.0`. The archive's `LICENSE.txt` is bound beside this
  file as `LICENSE`.
- `sourceObligation: none`; `modified: false` with an empty patch set.

## Why It Is Here

Bloom ships its own Python interpreter on macOS (see the `python` component). On Linux the
system interpreter gives scripts `ssl` and the OpenSSL-backed `hashlib`; this component gives
the bundled interpreter the same. Bloom's own code does not call OpenSSL.

## Reached Scope And Minimization

- Built on the Apple silicon profile only (`dependencies/superbuild/projects/openssl.cmake`).
- `libssl.a` and `libcrypto.a` and their headers, into the prefix's private `python-support`
  directory. They are linked into the interpreter's `_ssl` and `_hashlib` extension modules;
  no OpenSSL file is installed into an application.
- Configured `no-shared no-tests no-apps no-docs no-module no-legacy no-engine`: no command line
  program, no loadable provider or engine, no legacy provider.
- The configuration directory is a path that does not exist, so no `openssl.cnf` and no
  certificate directory on the machine is read by default. The interpreter is pointed at the
  operating system's certificate bundle when Bloom starts it (see the `python` review).
- Assembly: OpenSSL's own perl-generated arm64 assembly, built by the system compiler. No
  external assembler.
- Recorded role: `license` (the notice travels with the interpreter that contains the code).
