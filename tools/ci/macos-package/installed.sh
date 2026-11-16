#!/bin/sh
# Installs Bloom's installer package for real and checks what an artist then has
# (docs/architecture/platform-support.md, "macOS", "Installers").
#
#   sh tools/ci/macos-package/installed.sh <Bloom-...pkg> <Bloom-...dmg> <version> <work directory>
#
# For a macOS machine that is thrown away afterwards and whose user may use `sudo` without a
# password: a hosted runner. It refuses a machine that has Bloom in /Applications already, and
# one where sudo would ask. Nobody runs it on a personal computer.
#
#   1  `sudo installer -pkg <pkg> -target /`, the same program the installer window runs.
#   2  /Applications/Bloom.app is there, owned by root, and is this version.
#   3  /usr/local/bin/bloom-cli is a link to the program inside that application.
#   4  The package check's contents, version and run rules on the installed application, started
#      through the link: bloom-cli runs a script in the carried interpreter, starts the
#      application without a window, and renders the first frame of a new project headless.
#   5  The disk image attaches, holds Bloom.app with a valid signature, the Applications link, and
#      the background picture with the window layout that names it; it is detached again.
#
# Needs this source tree and a `cmake` on PATH. Every failure ends the script with one line.
set -eu

if [ $# -ne 4 ]; then
    echo "usage: installed.sh <pkg> <dmg> <version> <work directory>" >&2
    exit 2
fi
pkg=$1
dmg=$2
version=$3
work=$4
source=$(cd "$(dirname "$0")/../../.." && pwd)
app=/Applications/Bloom.app
link=/usr/local/bin/bloom-cli
program=$app/Contents/MacOS/bloom-cli

fail() {
    echo "installed.sh: $*" >&2
    exit 1
}

[ "$(uname -s)" = Darwin ] || fail "this is not macOS"
[ -f "$pkg" ] || fail "$pkg is missing"
[ -f "$dmg" ] || fail "$dmg is missing"
[ ! -e "$app" ] || fail "$app exists already: this is not a machine to install on"
sudo -n true 2> /dev/null || fail "sudo asks for a password here, so the package cannot be installed"
mkdir -p "$work"
work=$(cd "$work" && pwd)

# 1
sudo installer -pkg "$pkg" -target / || fail "the installer refused the package"

# 2
[ -d "$app" ] || fail "the installer did not put Bloom.app into /Applications"
owner=$(stat -f %Su "$app")
[ "$owner" = root ] || fail "$app is owned by $owner, not by root"

# 3
[ -L "$link" ] || fail "$link is not a link"
target=$(readlink "$link")
[ "$target" = "$program" ] || fail "$link leads to $target, not to $program"
[ -x "$program" ] || fail "$program cannot be run"

# 4
QT_QPA_PLATFORM=offscreen cmake "-DPACKAGE=$app" -DCOMPLETE=ON -DRUN=ON \
    "-DONLY=contents;version;run" "-DVERSION=$version" "-DWORK=$work/check" \
    "-DCLI_PROGRAM=$link" -P "$source/cmake/package/check_package.cmake" ||
    fail "the installed application does not pass the package check"
[ -s "$work/check/run/frame/frame.png" ] || fail "the linked bloom-cli rendered no frame"

# 5
plist=$work/attach.plist
hdiutil attach -nobrowse -readonly -plist "$dmg" > "$plist" || fail "the disk image does not attach"
volume=$(python3 -I -c '
import plistlib, sys
with open(sys.argv[1], "rb") as file:
    entities = plistlib.load(file)["system-entities"]
print(next(entity["mount-point"] for entity in entities if "mount-point" in entity))
' "$plist") || fail "the disk image has no volume"
problem=""
[ -d "$volume/Bloom.app" ] || problem="it holds no Bloom.app"
[ -n "$problem" ] || [ "$(readlink "$volume/Applications" 2> /dev/null)" = /Applications ] ||
    problem="its Applications entry is not a link to /Applications"
[ -n "$problem" ] || [ -s "$volume/.background/dmg-background.tiff" ] ||
    problem="it holds no background picture"
[ -n "$problem" ] || codesign --verify --deep --strict "$volume/Bloom.app" ||
    problem="the application in it has no valid signature"
if [ -z "$problem" ]; then
    layout=$(python3 -I "$source/tools/release/dmg_layout.py" read "$volume/.DS_Store") ||
        problem="its window layout cannot be read"
fi
if [ -z "$problem" ]; then
    case "$layout" in
        *dmg-background.tiff*) ;;
        *) problem="its window layout does not name the background picture" ;;
    esac
fi
hdiutil detach "$volume" > /dev/null || hdiutil detach -force "$volume" > /dev/null ||
    echo "installed.sh: the disk image could not be detached" >&2
[ -z "$problem" ] || fail "the disk image: $problem"

echo "installed.sh: Bloom $version installed from the package, linked, started and rendered; the disk image is whole"
