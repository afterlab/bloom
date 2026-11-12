# Linux runtime packages

A Linux Bloom package carries six small X11 helper libraries beside its Qt. The Qt Project's
Linux binaries load them, and a desktop does not always have them installed: a Wayland-only
installation has none, and `libxcb-cursor` in particular is missing from many.

They are not built here. Each is the library from one Ubuntu 22.04 binary package, and
`packages.json` is the only place they are written: the package and its version, the places
it can be downloaded from, its SHA-256 and size, the library file taken from it with that
file's SHA-256, and the package's own `copyright` file, which is checked in under `licenses`
and compared with the one inside the package when it is unpacked.

| Package | Library | License |
| --- | --- | --- |
| `libxcb-cursor0` 0.1.1-4ubuntu1 | `libxcb-cursor.so.0` | MIT |
| `libxcb-icccm4` 0.4.1-1.1build2 | `libxcb-icccm.so.4` | MIT |
| `libxcb-image0` 0.4.0-2 | `libxcb-image.so.0` | MIT |
| `libxcb-keysyms1` 0.4.0-1build3 | `libxcb-keysyms.so.1` | MIT |
| `libxcb-render-util0` 0.3.9-1build3 | `libxcb-render-util.so.0` | MIT |
| `libxcb-util1` 0.4.0-1build2 | `libxcb-util.so.1` | MIT |

Ubuntu 22.04 is the oldest system a package runs on, so these libraries need nothing newer
than that system's C library. The superbuild puts them into the dependency prefix's
`linux-runtime` directory (`dependencies/superbuild/projects/linux-runtime-fetch.cmake`), and
`cmake/package/stage_linux_runtime.cmake` copies them beside Qt, where Qt's own search path
finds them.

One thing is changed in each library: it is given its own directory as its library search
path, so that it finds the others beside itself and not the system's. Nothing else in the file
changes. The inventory's library checksum is of the file as packaged, before that change.

Everything else a package loads from outside itself is a declared system library
(`cmake/package/linux-base-libraries.txt`). The X protocol library itself (`libxcb` and the
extensions that are part of it) is one of those: it has to be the one the system's own X
libraries use.
