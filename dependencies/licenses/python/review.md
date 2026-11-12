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
  linked instead, on every platform), mimalloc, and the standard library's test packages and
  test extension modules. The archive carries no libffi, zlib, SQLite or bzip2; those are locked
  components of their own.
- `sourceObligation: none`. `modified: true`: one patch, applied on Windows only, recorded in the
  lock (`dependencies/patches/python/0001-windows-build-with-locked-libraries.patch`). It adds
  three switches to four of CPython's Visual Studio project files, so that the Windows build
  links the locked zlib, expat and liblzma instead of CPython's own choices, and one header of
  one line through which the decimal project reaches the libmpdec the archive carries. The PSF
  license permits modified versions with a summary of the changes; this entry and the patch
  file are that summary. The interpreter's source code is not changed on any platform.

## Why It Is Here

Bloom embeds CPython for scripting (ADR 0012, ADR 0022) and carries the interpreter in every
package (issue #677), so scripting starts on a computer that has no Python and a script gives
the same result on Linux, macOS and Windows. An interpreter from the operating system or a
package manager is built for that system, changes with it, and loads libraries Bloom has not
locked.

## Reached Scope And Minimization

- Built on every profile from the same archive: `dependencies/superbuild/projects/python.cmake`
  (Linux and macOS, CPython's configure script) and `python-windows.cmake` (CPython's own Visual
  Studio project files, driven by MSBuild, with the patch above).
- Installed under the prefix's `python` directory: the shared interpreter library, the standard
  library with its extension modules, the headers, and the interpreter program the build's
  tests run. Recorded roles: `library`, `data` (the standard library), `executable`, `license`,
  `notice`.
- Linux and macOS: `--enable-shared --without-ensurepip --disable-test-modules
  --without-mimalloc --with-system-expat --with-system-libmpdec=no --without-readline`, with
  `-ffp-contract=off` so that a multiplication and an addition are never fused (arm64 compilers
  fuse them by default, which changed last bits). Windows: `IncludeExternals=true`,
  `IncludeTkinter=false`, `IncludeTests=false`; the Visual Studio compiler does not fuse.
- What each extension module is made of, on every platform:

  | Module | Library |
  | --- | --- |
  | `_ssl`, `_hashlib` | `openssl` (locked) |
  | `_lzma` | `xz` (locked) |
  | `_zstd` | `zstd` (locked) |
  | `zlib` | `zlib` (locked) |
  | `_bz2` | `bzip2` (locked) |
  | `_sqlite3` | `sqlite` (locked) |
  | `_ctypes` | `libffi` (locked) |
  | `pyexpat`, `_elementtree` | `expat` (locked) |
  | `_decimal` | the libmpdec CPython carries |
  | `_sha2`, `_sha3`, `_blake2`, `_md5`, `_sha1`, `_hmac` | the HACL* code CPython carries |

  Linux and macOS link all of them statically into the modules. Windows links zlib, expat and
  liblzma statically, compiles SQLite, bzip2 and Zstandard from the locked archives inside
  CPython's own projects (SQLite as `sqlite3.dll`), and carries OpenSSL and libffi as
  `libcrypto-3-x64.dll`, `libssl-3-x64.dll` and `libffi-8.dll` beside the modules.
- Not built on any platform, so that the module set is the same everywhere: `_tkinter` (no Tk is
  locked), `_gdbm`, `_dbm`, `_curses`, `_curses_panel` and `readline` (an embedded interpreter
  has no terminal). A script that imports `tkinter`, `curses`, `readline`, `dbm.gnu` or
  `dbm.ndbm` fails on every platform; `dbm` works through `dbm.sqlite3` and `dbm.dumb`. The
  private `_uuid` accelerator is built where the operating system itself has the function
  (macOS, Windows) and not on Linux, where it would need the system's libuuid; `uuid` works
  without it. Modules CPython limits to one system (`winreg`, `msvcrt`, `termios`, `fcntl` and
  the like) follow CPython. `tests/fixtures/scripting/python_module_set.txt` is the committed
  list, and a test compares it on every platform.
- Removed after the install on every platform: the standard library's `tkinter`, `idlelib`,
  `turtledemo`, `turtle.py`, `curses` and `ensurepip`, which cannot work in this interpreter.
  pip is not installed.
- `pkg-config` is replaced by a path that does not exist while CPython configures, and every
  library is named by its path, so nothing installed on the build machine can be found in place
  of the locked libraries. After the install, `python-install.cmake` imports the modules in the
  table and fails the build if a module, the interpreter library or the program loads anything
  from outside the interpreter's own directory and the operating system: `/usr/lib` and
  `/System/Library` on macOS, the C library's own libraries on Linux, the system directory on
  Windows (where the Visual C++ runtime is not counted as part of the system).
- Build host on Windows: MSBuild runs a few of CPython's source generators with an existing
  Python, the one the superbuild already needs for Meson. It contributes no file to the result.
- Certificates: the carried OpenSSL has no certificate location of its own on Linux and macOS.
  When Bloom starts the interpreter it sets `SSL_CERT_FILE` to the operating system's
  certificate file (macOS: `/etc/ssl/cert.pem`; Linux: the first that exists of the common
  distribution paths), unless `SSL_CERT_FILE` or `SSL_CERT_DIR` is already set, so
  `ssl.create_default_context()` verifies against the roots the system maintains. On Windows
  CPython reads the system certificate store itself. No certificate bundle is shipped.
- Packages carry the interpreter library, the standard library with its extension modules, the
  bundled packages (`numpy`) and the license texts. They leave out the compiled caches and the
  files for building extensions against the interpreter.
