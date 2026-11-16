# Arch Linux package

This folder holds the source of the `bloom-bin` package for the Arch User Repository (AUR). The
package installs a release's Linux tar under `/opt/bloom`, with the menu entry, the icons and the
project file type under `/usr/share`, and a link `/usr/bin/bloom-cli`.

- `PKGBUILD.in` is the package description with the release's values left open.
- `test_pkgbuild.sh` is the test `bloom.package.pkgbuild_tests`.

## Making a release's files

The release's tar must be published first, because the package carries its checksum.

```sh
cmake -DVERSION=0.1.0-alpha.1 -DSHA256=<checksum of the tar> -DOUTPUT_DIR=<folder> \
      -P cmake/package/make_pkgbuild.cmake
```

This writes `PKGBUILD` and `.SRCINFO` into the folder. The label `0.1.0-alpha.1` becomes the
package version `0.1.0alpha1`, because a package version cannot hold a hyphen. The dependencies
come from `tools/install/linux-library-packages.txt`: the libraries marked `required` are
`depends`, and the ones marked `optional` are `optdepends`.

## What the package tells Bloom

The package writes `/opt/bloom/share/bloom/install-kind` with the lines `kind=package` and
`manager=pacman`. Bloom reads it, sees that pacman owns the files, and leaves updates to pacman.

## Publishing

Publishing the files to the AUR is a maintainer step. Nothing in the build or the release does it.
