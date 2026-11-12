#!/bin/sh
# Starts a Linux Bloom package on a clean system and checks what it loaded
# (docs/architecture/platform-support.md, "Packages"): the system has the declared system
# libraries and nothing else of Bloom's: no Python, no Qt, no compiler.
#
#   sh start.sh <Bloom-...tar.xz | Bloom-...AppImage> <work directory>
#
# Needs this source tree (for the check and the script the packaged programs run) and a
# `cmake` on PATH, which unpacks the tar and runs the check; the standalone CMake from its
# publisher needs nothing but the C library. The AppImage is unpacked by its own runtime
# (`--appimage-extract`), so the start works where FUSE is not available, as in a container.
#
# The check is cmake/package/check_package.cmake with its contents and run rules: the packaged
# bloom-cli runs a script in the carried interpreter, starts the packaged application without
# a window, and every library either of them loaded must come from the package or be a
# declared system library.
set -eu
package=$1
work=$2
source=$(cd "$(dirname "$0")/../../.." && pwd)
for refused in python3 python qmake6 cc gcc clang; do
    if command -v "$refused" > /dev/null 2>&1; then
        echo "start.sh: this system has $refused; it is not a clean system" >&2
        exit 2
    fi
done
rm -rf "$work"
mkdir -p "$work/unpacked"
case "$package" in
    *.tar.xz)
        (cd "$work/unpacked" && cmake -E tar xf "$package")
        tree=$(echo "$work"/unpacked/Bloom-*)
        ;;
    *.AppImage)
        (cd "$work/unpacked" && "$package" --appimage-extract > /dev/null)
        tree=$work/unpacked/squashfs-root
        test -x "$tree/AppRun"
        ;;
    *)
        echo "start.sh: $package is neither the tar nor the AppImage" >&2
        exit 2
        ;;
esac
cmake "-DPACKAGE=$tree" -DCOMPLETE=ON -DRUN=ON "-DONLY=contents;run" "-DWORK=$work/check" \
    -P "$source/cmake/package/check_package.cmake"
