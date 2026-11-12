# AppImage runtime

A Bloom AppImage is the package tree as a zstd-compressed SquashFS image, joined to the
AppImage project's type 2 runtime: a small static program that mounts the image and starts
`AppRun` in it. `runtime.json` is the only place the runtime is written: the release, its
SHA-256 and size, its detached signature and the key that made it, the source commit it was
built from, and what is linked into it with each part's license files as checked in here.
`cmake/package/make_appimage.cmake` reads it.

The runtime is taken as published, by checksum, and is not built here: the superbuild puts it
into the dependency prefix's `appimage-runtime` directory
(`dependencies/superbuild/projects/appimage-runtime-fetch.cmake`). Nothing of Bloom links it; it
is present only in the AppImage form of a Linux package. The tar form of the same tree has none
of it.

What is inside the runtime, from its build scripts at the recorded commit
(`scripts/common/install-dependencies.sh`, `scripts/docker/Dockerfile`, `src/runtime/Makefile`):

| Part | Version | License | Note |
| --- | --- | --- | --- |
| The runtime itself | release 20251108 | MIT | |
| libfuse | 3.15.0 | LGPL-2.1-only | Linked statically, with the runtime project's `mount.c.diff` applied. The exact source archive and that patch are recorded, and the patch is checked in |
| squashfuse | 0.5.2 | BSD-2-Clause | |
| zlib | 1.3.1 | Zlib | The version is the one the binary reports |
| zstd, mimalloc, musl | as packaged by Alpine Linux 3.21 | BSD-3-Clause, MIT, MIT | The build takes them from the distribution; the runtime's release does not record their versions |

The signature was verified against `signing-pubkey.asc` (fingerprint
`570C77ACEA40C0F1B758902CBF96CCA56490F695`), which is the key in the runtime's source at the
recorded commit. That ties the binary to the project's release process, not to an independent
trust root: the key came from the same place as the binary.

An AppImage needs FUSE on the machine that runs it (`fusermount3` or `fusermount`); without
it, `--appimage-extract-and-run` unpacks and runs, and the tar is the form meant for such a
machine.
