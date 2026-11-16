#!/bin/sh
# Tests the Arch Linux package description (packaging/aur/PKGBUILD.in) with a stand-in release of
# a few small files.
#
#   sh test_pkgbuild.sh <source tree> <work directory> <cmake>
#
# Generates PKGBUILD and .SRCINFO for the stand-in tar and checks the generated files: the
# syntax, every field, the checksum, the dependencies against the table of system libraries, and
# that .SRCINFO says what the PKGBUILD says. Where `makepkg` exists and the user is not root it
# also builds the package from the stand-in (every folder under the work directory, the tar
# placed where makepkg looks for a source it already has, no dependency check, no network) and
# checks every path, the link, the marker and the menu entry in the built package. It ends with
# status 77 (a skip) where `xz` is missing.
set -eu
source=$1
work=$2
cmake=$3

command -v xz > /dev/null 2>&1 || { echo "skipped: no xz"; exit 77; }

rm -rf "$work"
mkdir -p "$work"
work=$(cd "$work" && pwd)

failures=0
check() {
    # check <what it proves> <command...>
    what=$1
    shift
    if ! "$@"; then
        echo "FAILED: $what"
        failures=$((failures + 1))
    fi
}
same() {
    [ "$1" = "$2" ]
}

# --- The stand-in release ----------------------------------------------------------------------

label=0.1.0-alpha.1
name=Bloom-$label-linux-x86_64
tree=$work/release/$name
application=id.afterlab.bloom
mkdir -p "$tree/bin" "$tree/share/bloom/licenses" "$tree/share/applications" \
    "$tree/share/mime/packages"
for program in Bloom bloom-cli bloom-mcp; do
    printf '#!/bin/sh\necho "%s"\n' "$program" > "$tree/bin/$program"
    chmod +x "$tree/bin/$program"
done
echo "license" > "$tree/share/bloom/LICENSE"
echo "notice" > "$tree/share/bloom/NOTICE"
echo "component" > "$tree/share/bloom/licenses/component"
cp "$source/apps/bloom/linux/$application.desktop" "$tree/share/applications/"
cp "$source/apps/bloom/linux/$application.xml" "$tree/share/mime/packages/"
sizes=
for size in "$source"/apps/bloom/linux/icons/*; do
    sizes="$sizes ${size##*/}"
    mkdir -p "$tree/share/icons/hicolor/${size##*/}/apps"
    cp "$size/$application.png" "$tree/share/icons/hicolor/${size##*/}/apps/"
done
tar -cJf "$work/$name.tar.xz" -C "$work/release" "$name"
sum=$(sha256sum "$work/$name.tar.xz" | awk '{ print $1 }')

# --- The generated files ---------------------------------------------------------------------

generated=$work/generated
"$cmake" "-DVERSION=$label" "-DSHA256=$sum" "-DOUTPUT_DIR=$generated" \
    -P "$source/cmake/package/make_pkgbuild.cmake"
check "the PKGBUILD is written" test -f "$generated/PKGBUILD"
check "the .SRCINFO is written" test -f "$generated/.SRCINFO"
check "the PKGBUILD has valid shell syntax" bash -n "$generated/PKGBUILD"

# A bad version, checksum or license is refused and writes nothing.
for bad in "-DVERSION=1.0" "-DSHA256=abc"; do
    rm -rf "$work/refused"
    if "$cmake" "-DVERSION=$label" "-DSHA256=$sum" "-DOUTPUT_DIR=$work/refused" "$bad" \
        -P "$source/cmake/package/make_pkgbuild.cmake" > /dev/null 2>&1; then
        echo "FAILED: $bad is accepted"
        failures=$((failures + 1))
    fi
    check "$bad writes nothing" test ! -e "$work/refused/PKGBUILD"
done

field() {
    # field <name>: the value of a PKGBUILD variable, array items separated by a line break.
    bash -c 'source "$1" > /dev/null 2>&1; eval "printf \"%s\\n\" \"\${$2[@]}\""' _ \
        "$generated/PKGBUILD" "$1"
}
check "pkgname" same "$(field pkgname)" bloom-bin
check "pkgver drops what a pkgver cannot hold" same "$(field pkgver)" 0.1.0alpha1
check "the label is kept" same "$(field _version)" "$label"
check "arch" same "$(field arch)" x86_64
check "url" same "$(field url)" https://github.com/afterlab/bloom
check "license" same "$(field license)" Apache-2.0
check "provides" same "$(field provides)" bloom
check "conflicts" same "$(field conflicts)" bloom
check "options" same "$(field options)" '!strip'
check "source" same "$(field source)" \
    "https://github.com/afterlab/bloom/releases/download/v$label/$name.tar.xz"
check "sha256sums" same "$(field sha256sums)" "$sum"

# The dependencies are the table's: every required library's Arch package, and nothing optional
# that is not also required.
required=$(awk '$1 !~ /^#/ && $2 == "required" { print $5 }' \
    "$source/tools/install/linux-library-packages.txt" | sort -u)
optional=$(awk '$1 !~ /^#/ && $2 == "optional" { print $5 }' \
    "$source/tools/install/linux-library-packages.txt" | sort -u)
check "depends are the required libraries' packages" same "$(field depends | sort -u)" "$required"
optional_listed=$(field optdepends | sed 's/:.*//' | sort -u)
expected_optional=$(printf '%s\n' "$optional" | grep -v -x -F "$required" || true)
check "optdepends are the optional libraries' packages" same "$optional_listed" "$expected_optional"
check "every optdepends has a reason" test -z "$(field optdepends | grep -v '^[^:]*: .' || true)"

# .SRCINFO says what the PKGBUILD says.
srcinfo=$generated/.SRCINFO
check ".SRCINFO pkgver" grep -q -x -F "$(printf '\tpkgver = 0.1.0alpha1')" "$srcinfo"
check ".SRCINFO source" grep -q -x -F \
    "$(printf '\tsource = https://github.com/afterlab/bloom/releases/download/v%s/%s.tar.xz' "$label" "$name")" "$srcinfo"
check ".SRCINFO checksum" grep -q -x -F "$(printf '\tsha256sums = %s' "$sum")" "$srcinfo"
check ".SRCINFO depends" same "$(sed -n 's/^\tdepends = //p' "$srcinfo" | sort -u)" "$required"
check ".SRCINFO has no variable left" test -z "$(grep -F '$' "$srcinfo" || true)"

# --- The built package -------------------------------------------------------------------------

if ! command -v makepkg > /dev/null 2>&1; then
    echo "note: no makepkg on this system; the generated files were checked, no package was built"
elif [ "$(id -u)" = 0 ]; then
    echo "note: makepkg does not run as root; the generated files were checked, no package was built"
else
    make=$work/make
    mkdir -p "$make/build" "$make/packages" "$make/sources" "$make/srcpackages" "$make/pkg"
    cp "$generated/PKGBUILD" "$make/pkg/PKGBUILD"
    # Where makepkg looks for a source it already has: no download.
    cp "$work/$name.tar.xz" "$make/sources/$name.tar.xz"
    if ! (cd "$make/pkg" &&
        BUILDDIR="$make/build" PKGDEST="$make/packages" SRCDEST="$make/sources" \
            SRCPKGDEST="$make/srcpackages" LOGDEST="$make/packages" \
            makepkg --nodeps --nocheck --noconfirm --nocolor --skippgpcheck --nosign \
            > "$make/makepkg.log" 2>&1); then
        echo "FAILED: makepkg did not build the package; its log:"
        cat "$make/makepkg.log"
        failures=$((failures + 1))
    else
        # What makepkg says the PKGBUILD is, against what was generated.
        (cd "$make/pkg" && makepkg --printsrcinfo > "$make/srcinfo" 2> /dev/null) || true
        check ".SRCINFO is what makepkg prints" cmp -s "$make/srcinfo" "$srcinfo"
        built=$(ls "$make"/packages/bloom-bin-*.pkg.tar.* 2> /dev/null | head -n 1)
        check "the package was built" test -n "$built"
        if [ -n "$built" ]; then
            if command -v bsdtar > /dev/null 2>&1; then archive=bsdtar; else archive=tar; fi
            "$archive" -tvf "$built" > "$make/listing"
            "$archive" -tf "$built" > "$make/names"
            listed() {
                grep -q -x -F -e "$1" "$make/names"
            }
            for path in opt/bloom/bin/Bloom opt/bloom/bin/bloom-cli opt/bloom/bin/bloom-mcp \
                opt/bloom/share/bloom/LICENSE opt/bloom/share/bloom/licenses/component \
                opt/bloom/share/bloom/install-kind \
                usr/bin/bloom-cli \
                "usr/share/applications/$application.desktop" \
                "usr/share/mime/packages/$application.xml" \
                usr/share/licenses/bloom-bin/LICENSE usr/share/licenses/bloom-bin/NOTICE; do
                check "the package has $path" listed "$path"
            done
            for size in $sizes; do
                check "the package has the $size icon" \
                    listed "usr/share/icons/hicolor/$size/apps/$application.png"
            done
            check "usr/bin/bloom-cli is a link to the program in /opt/bloom" \
                grep -q -F "usr/bin/bloom-cli -> /opt/bloom/bin/bloom-cli" "$make/listing"
            check "nothing is installed outside /opt/bloom and /usr" \
                test -z "$(grep -v -e '^opt/' -e '^usr/' -e '^\.' "$make/names" || true)"
            member() {
                "$archive" -xOf "$built" "$1"
            }
            check "the marker has its two lines" same "$(member opt/bloom/share/bloom/install-kind)" \
                "$(printf 'kind=package\nmanager=pacman')"
            entry=$(member "usr/share/applications/$application.desktop")
            check "the entry's Exec line" sh -c 'printf "%s\n" "$1" | grep -q -x -F "Exec=/opt/bloom/bin/Bloom %f"' _ "$entry"
            check "the entry's TryExec line" sh -c 'printf "%s\n" "$1" | grep -q -x -F "TryExec=/opt/bloom/bin/Bloom"' _ "$entry"
            check "the entry keeps its icon name" sh -c 'printf "%s\n" "$1" | grep -q -x -F "Icon=id.afterlab.bloom"' _ "$entry"
            check "the program is the release's, byte for byte" \
                sh -c '"$1" -xOf "$2" opt/bloom/bin/Bloom | cmp -s - "$3"' _ "$archive" "$built" "$tree/bin/Bloom"
            check "the file type is the release's, byte for byte" \
                sh -c '"$1" -xOf "$2" "usr/share/mime/packages/$4.xml" | cmp -s - "$3"' _ "$archive" "$built" \
                "$tree/share/mime/packages/$application.xml" "$application"
            check "the package declares what it provides and conflicts with" \
                sh -c '"$1" -xOf "$2" .PKGINFO | grep -q -x -F "provides = bloom" &&
                    "$1" -xOf "$2" .PKGINFO | grep -q -x -F "conflict = bloom"' _ "$archive" "$built"
        fi
    fi
fi

if [ "$failures" -ne 0 ]; then
    echo "$failures check(s) failed"
    exit 1
fi
echo "ok"
