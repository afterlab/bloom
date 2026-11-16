#!/bin/sh
# Starts a Linux Bloom package on a clean system and checks what it loaded
# (docs/architecture/platform-support.md, "Packages"): the system has the declared system
# libraries and nothing else of Bloom's: no Python, no Qt, no compiler.
#
#   sh start.sh <Bloom-...tar.xz | Bloom-...AppImage | <release folder>/install.sh> <work directory>
#
# The install script form takes the release's own install.sh, in the folder that also holds
# SHA256SUMS, SHA256SUMS.sig and the tar. It installs for a user whose home folder is
# <work>/home (`install.sh --from <folder> --prefix <work>/installed`, which verifies the
# signature and the checksum), runs the same check on the installed folder, and then removes it
# (`install.sh --uninstall`) and requires that the folder and the link are gone. The check starts
# the linked bloom-cli, so the link the script made is what runs and renders. The work directory
# must not be inside the release folder, and it is emptied first.
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
mkdir -p "$work"
work=$(cd "$work" && pwd)
case "$package" in
    */install.sh | install.sh)
        folder=$(cd "$(dirname "$package")" && pwd)
        case "$folder/" in
            "$work"/* | "$work"/)
                echo "start.sh: the work directory must not hold the release folder" >&2
                exit 2
                ;;
        esac
        ;;
esac
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
    */install.sh | install.sh)
        user=$work/home
        mkdir -p "$user"
        HOME=$user sh "$package" --from "$folder" --prefix "$work/installed"
        tree=$work/installed
        test -x "$tree/bin/Bloom"
        cli=$user/.local/bin/bloom-cli
        test -L "$cli"
        ;;
    *)
        echo "start.sh: $package is neither the tar, the AppImage nor install.sh" >&2
        exit 2
        ;;
esac
cmake "-DPACKAGE=$tree" -DCOMPLETE=ON -DRUN=ON "-DONLY=contents;run" "-DWORK=$work/check" \
    ${cli:+"-DCLI_PROGRAM=$cli"} -P "$source/cmake/package/check_package.cmake"
if [ -n "${cli:-}" ]; then
    HOME=$user sh "$package" --uninstall --prefix "$work/installed"
    if [ -e "$work/installed" ] || [ -L "$cli" ] || [ -e "$cli" ]; then
        echo "start.sh: the uninstall left the folder or the link behind" >&2
        exit 1
    fi
    echo "start.sh: installed, checked and uninstalled"
fi
