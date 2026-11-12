# SQLite License And Feature Review

Reviewed: 2026-11-12

## License

- SQLite's code is dedicated to the public domain by its authors; the lock records it with the
  SPDX identifier `blessing`, the name SPDX gives SQLite's notice. The archive has no license
  file for the library, so `LICENSE` beside this file is the notice itself, lines 2 to 10 of the
  archive's `sqlite3.h`, byte for byte.
- Not reached and not part of this record: the archive's `autosetup` build scripts (their own
  BSD-style license) and the Tcl extension under `tea/`. Neither is run, compiled or installed.
- `sourceObligation: none`; `modified: false` with an empty patch set. The recipe adds a build
  file of Bloom's own beside the sources and changes no source file.

## Why It Is Here

Bloom carries its own Python interpreter in every package (see the `python` component). This
component gives the interpreter's `sqlite3` module, and `dbm.sqlite3`, one SQLite on every
platform, so a script's database does not follow the version the operating system has. Bloom's
own code does not call SQLite.

## Reached Scope And Minimization

- Linux and macOS: `dependencies/superbuild/projects/sqlite.cmake` compiles the one file
  `sqlite3.c` into a static library with `dependencies/superbuild/projects/sqlite/CMakeLists.txt`
  and installs it with `sqlite3.h` and `sqlite3ext.h` into the prefix's private `python-support`
  directory. It is linked into the interpreter's `_sqlite3` extension module.
- Windows: CPython's own project file compiles the same `sqlite3.c` into `sqlite3.dll`, which
  the package carries beside the interpreter.
- Compile-time options, the same on every platform and the ones CPython's Windows build uses:
  `SQLITE_ENABLE_MATH_FUNCTIONS`, `SQLITE_ENABLE_FTS4`, `SQLITE_ENABLE_FTS5`,
  `SQLITE_ENABLE_RTREE`, `SQLITE_OMIT_AUTOINIT`, and thread safety on.
- Not built: the `sqlite3` shell program, the archive's configure script, the Tcl extension.
- Recorded roles: `license` where it is linked statically; `library` and `license` on Windows.
