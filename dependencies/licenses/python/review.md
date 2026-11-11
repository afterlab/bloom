# CPython License And Feature Review

Reviewed: 2026-11-12

## License

- Whole-component SPDX expression: `PSF-2.0 AND BSD-2-Clause AND MIT`.
  - `PSF-2.0`: the interpreter and standard library. The archive's `LICENSE` (the PSF license
    stack with its historical BeOpen, CNRI and CWI agreements and the zero-clause BSD grant for
    documentation code) is bound beside this file.
  - `BSD-2-Clause`: `Modules/_decimal/libmpdec`, the decimal arithmetic library CPython carries
    and this build compiles into `_decimal`.
  - `MIT`: `Modules/_hacl`, the HACL* hash implementations CPython carries and compiles into
    `_sha2`, `_sha3`, `_blake2`, `_md5`, `_sha1` and `_hmac`. Its text, taken from the header of
    `Modules/_hacl/Hacl_Hash_SHA2.c`, is bound as `HACL-LICENSE`.
  - `incorporated-software.rst` is the archive's `Doc/license.rst`, CPython's own list of the
    licenses and acknowledgements for software incorporated in the distribution (libmpdec, the
    Mersenne Twister, SipHash, the `dtoa` code, the socket, cookie, tracing, UUencode and XML-RPC
    modules and others). It is bound as a notice file so the application can show it.
- Carried in the archive but not compiled by this build: expat (the locked `expat` component is
  linked instead), mimalloc (`--without-mimalloc`), libffi and zlib (the operating system's are
  used), and the standard library's test packages (`--disable-test-modules`).
- `sourceObligation: none`; `modified: false` with an empty patch set.

## Why It Is Here

Bloom embeds CPython for scripting (ADR 0012, ADR 0022). Linux embeds the system interpreter.
macOS ships none that an application may rely on, and a package manager's interpreter is built
for the newest system and loads that package manager's own libraries, so an application bundle
built against it does not start on the deployment target and is not self-contained. This
component is the interpreter for macOS, built for the locked deployment target.

## Reached Scope And Minimization

- Built on the Apple silicon profile only (`dependencies/superbuild/projects/python.cmake`).
- Installed under the prefix's `python` directory: `lib/libpython3.14.dylib` with an
  `@rpath`-relative install name, the standard library, the headers, and `bin/python3.14`, which
  the build's tests run. Recorded roles: `library`, `data` (the standard library), `executable`,
  `license`, `notice`.
- `--enable-shared --without-ensurepip --disable-test-modules --without-mimalloc
  --with-system-expat --with-system-libmpdec=no --with-readline=editline`. pip is not installed.
- What each extension module links:

  | Module | Library |
  | --- | --- |
  | `_ssl`, `_hashlib` | `openssl` (locked), static |
  | `_lzma` | `xz` (locked), static |
  | `_zstd` | `zstd` (locked), static |
  | `pyexpat`, `_elementtree` | `expat` (locked), static |
  | `_decimal` | CPython's libmpdec |
  | `_sqlite3`, `zlib`, `_bz2`, `_ctypes`, `_curses`, `readline`, `_dbm`, `_uuid` | the operating system's libsqlite3, libz, libbz2, libffi, libncurses, libedit, the C library's ndbm and uuid |

- Not built: `_tkinter` and `_gdbm`. The operating system has no library for them and none is
  locked. A script that imports `tkinter` or `dbm.gnu` fails on macOS; this is a difference from
  a Linux system interpreter that has them installed.
- `pkg-config` is replaced by a path that does not exist while CPython configures, so nothing
  installed on the build machine can be found in place of the libraries above. After the
  install, `python-install.cmake` imports every module in the table and fails the build if a
  module, the library or the program loads anything that is neither under `/usr/lib` or
  `/System/Library` nor the interpreter library itself.
- Certificates: the bundled OpenSSL has no default certificate location. When Bloom starts the
  bundled interpreter it sets `SSL_CERT_FILE` to the operating system's certificate bundle,
  `/etc/ssl/cert.pem`, unless `SSL_CERT_FILE` or `SSL_CERT_DIR` is already set, so
  `ssl.create_default_context()` verifies against the roots the system maintains. No certificate
  bundle is shipped.
- The application bundle carries the interpreter library, the standard library with its
  extension modules, and the license texts (`apps/bloom/macos/bundle_python.cmake`). It leaves
  out the compiled caches, the files for building extensions against the interpreter, the
  programs that need Tk (`idlelib`, `tkinter`, `turtle`, `turtledemo`) and `ensurepip`.
