#!/bin/sh
# Tests the Linux install script (tools/install/install.sh.in) with a stand-in package of a few
# small files, signed by a key made for the run with the release's own tools.
#
#   sh test_install_script.sh <source tree> <work directory> <cmake> <python3>
#
# Every run of the script has a home folder under the work directory and a loader list of its
# own, so the test neither reads nor writes anything of the user's. It ends with status 77
# (a skip) where `openssl` cannot make or check an Ed25519 signature, or `xz` is missing.
set -eu
source=$1
work=$2
cmake=$3
python=$4

command -v xz > /dev/null 2>&1 || { echo "skipped: no xz"; exit 77; }
command -v openssl > /dev/null 2>&1 || { echo "skipped: no openssl"; exit 77; }

rm -rf "$work"
mkdir -p "$work"
work=$(cd "$work" && pwd)

openssl genpkey -algorithm ed25519 -outform DER -out "$work/key.der" 2> /dev/null ||
    { echo "skipped: this openssl has no Ed25519"; exit 77; }
openssl genpkey -algorithm ed25519 -outform DER -out "$work/other-key.der" 2> /dev/null
public=$("$python" -I "$source/tools/release/sign_release" public-key --key-file "$work/key.der")

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
has() {
    grep -q -F -e "$2" "$1"
}
lacks() {
    ! grep -q -F -e "$2" "$1"
}
absent() {
    [ ! -e "$1" ] && [ ! -L "$1" ]
}

# --- A release: the stand-in package, the list of checksums and its signature ----------------

# make_release <version> <folder> <signing key>
make_release() {
    name=Bloom-$1-linux-x86_64
    tree=$work/trees/$1/$name
    rm -rf "$work/trees/$1" "$2"
    mkdir -p "$tree/bin" "$tree/share/bloom/licenses" "$tree/share/applications" \
        "$tree/share/mime/packages" "$2"
    for program in Bloom bloom-cli bloom-mcp; do
        # Says which file really ran, the way a program finds its own package.
        printf '#!/bin/sh\necho "%s %s at $(readlink -f "$0")"\n' "$program" "$1" \
            > "$tree/bin/$program"
        chmod +x "$tree/bin/$program"
    done
    echo "license" > "$tree/share/bloom/licenses/LICENSE"
    echo "$1" > "$tree/share/bloom/only-in-$1"
    cp "$source/apps/bloom/linux/id.afterlab.bloom.desktop" "$tree/share/applications/"
    cp "$source/apps/bloom/linux/id.afterlab.bloom.xml" "$tree/share/mime/packages/"
    for size in "$source"/apps/bloom/linux/icons/*; do
        mkdir -p "$tree/share/icons/hicolor/${size##*/}/apps"
        cp "$size/id.afterlab.bloom.png" "$tree/share/icons/hicolor/${size##*/}/apps/"
    done
    tar -cJf "$2/$name.tar.xz" -C "$work/trees/$1" "$name"
    "$python" -I "$source/tools/release/sums.py" write --out "$2/SHA256SUMS" "$2/$name.tar.xz"
    "$python" -I "$source/tools/release/sign_release" detached --key-file "$3" \
        --out "$2/SHA256SUMS.sig" "$2/SHA256SUMS"
}

# make_script <version> <release folder> <output>
make_script() {
    "$cmake" "-DVERSION=$1" "-DBASE_URL=file://$2" "-DPUBLIC_KEYS=$public" "-DOUTPUT=$3" \
        -P "$source/cmake/package/make_install_script.cmake"
}

first=0.1.0-alpha.1
second=0.1.0-alpha.2
make_release "$first" "$work/release-1" "$work/key.der"
make_release "$second" "$work/release-2" "$work/key.der"
make_script "$first" "$work/release-1" "$work/install-1.sh"
make_script "$second" "$work/release-2" "$work/install-2.sh"
if ! openssl pkeyutl -verify -pubin -keyform DER \
    -inkey "$(openssl pkey -inform DER -in "$work/key.der" -pubout -outform DER \
        -out "$work/public.der" && echo "$work/public.der")" \
    -rawin -in "$work/release-1/SHA256SUMS" -sigfile "$work/release-1/SHA256SUMS.sig" \
    > /dev/null 2>&1; then
    echo "skipped: this openssl cannot check an Ed25519 signature over a file"
    exit 77
fi

# --- The system a run sees ---------------------------------------------------------------------

# A loader list with every library the script asks about, without the ones named in
# <work>/missing; and the file that says which distribution this is.
mkdir -p "$work/tools"
: > "$work/missing"
cat > "$work/tools/ldconfig" << END
#!/bin/sh
grep -v -e '^#' -e '^\$' "$source/tools/install/linux-library-packages.txt" |
    while read -r library rest; do
        grep -q -x -F "\$library" "$work/missing" ||
            printf '\\t%s (libc6,x86-64) => /usr/lib/%s\\n' "\$library" "\$library"
    done
END
chmod +x "$work/tools/ldconfig"
# A package list that has renamed the packages named in <work>/renamed, and no other.
: > "$work/renamed"
cat > "$work/tools/apt-cache" << END
#!/bin/sh
[ "\$1" = show ] && grep -q -x -F "\$2" "$work/renamed"
END
chmod +x "$work/tools/apt-cache"
echo 'ID=ubuntu' > "$work/os-release"

# run <home> <log> <script> [options...]: the status is the script's.
run() {
    run_home=$1
    run_log=$2
    run_script=$3
    shift 3
    mkdir -p "$run_home"
    env -u XDG_DATA_HOME -u DESTDIR HOME="$run_home" PATH="$work/tools:$PATH" \
        BLOOM_INSTALL_OS_RELEASE="$work/os-release" ${run_destdir:+DESTDIR="$run_destdir"} \
        sh "$run_script" "$@" > "$run_log" 2>&1
}
run_destdir=

# installed <root> <bin folder> <data folder> <prefix of every file on disk> <version>
installed() {
    [ "$("$4$2/bloom-cli" 2> /dev/null)" = "bloom-cli $5 at $4$1/bin/bloom-cli" ] &&
        [ "$(readlink "$4$2/bloom-cli")" = "$1/bin/bloom-cli" ] &&
        [ -x "$4$1/bin/Bloom" ] &&
        grep -q -x 'kind=script' "$4$1/share/bloom/install-kind" &&
        grep -q -x "link $2/bloom-cli" "$4$1/share/bloom/install-manifest" &&
        grep -q -x -F "Exec=\"$1/bin/Bloom\" %f" "$4$3/applications/id.afterlab.bloom.desktop" &&
        grep -q -x -F "TryExec=$1/bin/Bloom" "$4$3/applications/id.afterlab.bloom.desktop" &&
        [ -f "$4$3/mime/packages/id.afterlab.bloom.xml" ] &&
        [ "$(ls "$4$3"/icons/hicolor/*/apps/id.afterlab.bloom.png | wc -l)" -eq 8 ] &&
        [ -z "$(find "$(dirname "$4$1")" -maxdepth 1 -name '*.new.*')" ]
}

# --- Per user: downloaded, verified, installed, and nothing is said about libraries ------------

home=$work/home
download=
if command -v curl > /dev/null 2>&1; then
    # The script's own download, from the release's address (a folder here).
    check "per user: the script ends well" run "$home" "$work/log-user" "$work/install-1.sh"
    download=yes
else
    check "per user: the script ends well" run "$home" "$work/log-user" "$work/install-1.sh" \
        --from "$work/release-1"
fi
check "per user: the package, the link, the menu entry, the icons and the file type are there" \
    installed "$home/.local/opt/bloom" "$home/.local/bin" "$home/.local/share" "" "$first"
check "per user: a complete system is told nothing about libraries" \
    lacks "$work/log-user" "lacks libraries"
check "per user: nothing but the home folder's .local was written" \
    [ "$(ls -A "$home")" = .local ]

# --- The libraries -----------------------------------------------------------------------------

printf '%s\n' libxkbcommon.so.0 libGL.so.1 libjack.so.0 > "$work/missing"
check "libraries: the install still ends well" \
    run "$work/home-libraries" "$work/log-debian" "$work/install-1.sh" --from "$work/release-1"
check "libraries: the missing ones are named" \
    has "$work/log-debian" "This system lacks libraries Bloom needs: libGL.so.1 libxkbcommon.so.0"
check "libraries: Debian and Ubuntu get the apt command with their packages" \
    has "$work/log-debian" "  sudo apt install libgl1 libxkbcommon0"
check "libraries: one that only a feature uses is named apart" \
    has "$work/log-debian" "  sudo apt install libjack-jackd2-0"
printf '%s\n' libasound.so.2 libGL.so.1 > "$work/missing"
echo libasound2t64 > "$work/renamed"
run "$work/home-libraries" "$work/log-renamed" "$work/install-1.sh" --from "$work/release-1" || true
check "libraries: a package the system has renamed is asked for by its new name" \
    has "$work/log-renamed" "  sudo apt install libasound2t64"
check "libraries: a package the system has not renamed keeps its name" \
    has "$work/log-renamed" "  sudo apt install libgl1"
: > "$work/renamed"
printf '%s\n' libxkbcommon.so.0 libGL.so.1 libjack.so.0 > "$work/missing"
echo 'ID=nobara
ID_LIKE="fedora"' > "$work/os-release"
run "$work/home-libraries" "$work/log-fedora" "$work/install-1.sh" --from "$work/release-1" || true
check "libraries: Fedora and its relatives get the dnf command" \
    has "$work/log-fedora" "  sudo dnf install libglvnd-glx libxkbcommon"
echo 'ID=arch' > "$work/os-release"
run "$work/home-libraries" "$work/log-arch" "$work/install-1.sh" --from "$work/release-1" || true
check "libraries: Arch Linux gets the pacman command" \
    has "$work/log-arch" "  sudo pacman -S --needed libglvnd libxkbcommon"
echo 'ID=something-else' > "$work/os-release"
run "$work/home-libraries" "$work/log-other" "$work/install-1.sh" --from "$work/release-1" || true
check "libraries: another system gets the packages of all three" \
    has "$work/log-other" "  Fedora:         libglvnd-glx libxkbcommon"
echo 'ID=ubuntu' > "$work/os-release"
: > "$work/missing"

# --- Verification: nothing is unpacked from a release that is not the signed one ---------------

refused() {
    # refused <case> <release folder> <the reason the script must give>
    home_refused=$work/home-$1
    if run "$home_refused" "$work/log-$1" "$work/install-1.sh" --from "$2"; then
        echo "FAILED: $1: the script ended well"
        failures=$((failures + 1))
    fi
    check "$1: the reason is given" has "$work/log-$1" "$3"
    check "$1: nothing was installed" [ -z "$(ls -A "$home_refused")" ]
}
name=Bloom-$first-linux-x86_64
# The package changed after the list was signed.
cp -r "$work/release-1" "$work/tampered"
printf 'x' >> "$work/tampered/$name.tar.xz"
refused tampered "$work/tampered" "does not have the checksum the signed list gives"
# The list changed with it, so the checksum agrees and the signature does not.
cp -r "$work/tampered" "$work/relisted"
"$python" -I "$source/tools/release/sums.py" write --out "$work/relisted/SHA256SUMS" \
    "$work/relisted/$name.tar.xz"
refused relisted "$work/relisted" "is not signed by Bloom's release key"
# Signed, by a key the script does not carry.
make_release "$first" "$work/other-key" "$work/other-key.der"
refused other-key "$work/other-key" "is not signed by Bloom's release key"
# A signed list of another release, which does not name this package.
cp -r "$work/release-2" "$work/other-release"
cp "$work/release-1/$name.tar.xz" "$work/other-release/"
refused other-release "$work/other-release" "does not name $name.tar.xz"
# No signature at all.
cp -r "$work/release-1" "$work/unsigned"
rm "$work/unsigned/SHA256SUMS.sig"
refused unsigned "$work/unsigned" "has no SHA256SUMS.sig"

# --- Repair and update: running it again -------------------------------------------------------

rm "$home/.local/opt/bloom/bin/bloom-cli" "$home/.local/bin/bloom-cli" \
    "$home/.local/share/applications/id.afterlab.bloom.desktop"
check "repair: the script ends well" \
    run "$home" "$work/log-repair" "$work/install-1.sh" --from "$work/release-1"
check "repair: what was missing is back" \
    installed "$home/.local/opt/bloom" "$home/.local/bin" "$home/.local/share" "" "$first"
check "update: the script of the next release ends well" \
    run "$home" "$work/log-update" "$work/install-2.sh" --from "$work/release-2"
check "update: the next version is installed in the same places" \
    installed "$home/.local/opt/bloom" "$home/.local/bin" "$home/.local/share" "" "$second"
check "update: a file only the earlier version had is gone" \
    absent "$home/.local/opt/bloom/share/bloom/only-in-$first"

# --- --prefix ----------------------------------------------------------------------------------

custom="$work/a folder/of Bloom"
check "--prefix: the script ends well" \
    run "$work/home-prefix" "$work/log-prefix" "$work/install-1.sh" --from "$work/release-1" \
    --prefix "$custom"
check "--prefix: the package is in that folder, and the link and the menu entry lead there" \
    installed "$custom" "$work/home-prefix/.local/bin" "$work/home-prefix/.local/share" "" "$first"
check "--prefix: the default folder was not made" absent "$work/home-prefix/.local/opt"
# A folder that holds something else is never taken over, and a system folder never.
mkdir -p "$work/not-bloom"
echo mine > "$work/not-bloom/notes.txt"
if run "$work/home-prefix" "$work/log-taken" "$work/install-1.sh" --from "$work/release-1" \
    --prefix "$work/not-bloom"; then
    echo "FAILED: --prefix: a folder with someone else's files was accepted"
    failures=$((failures + 1))
fi
check "--prefix: a folder with someone else's files keeps them and gains nothing" \
    [ "$(ls -A "$work/not-bloom")" = notes.txt ]
if run "$work/home-prefix" "$work/log-system-folder" "$work/install-1.sh" \
    --from "$work/release-1" --prefix /usr/local; then
    echo "FAILED: --prefix: /usr/local was accepted"
    failures=$((failures + 1))
fi
check "--prefix: a folder of the system is refused by name" \
    has "$work/log-system-folder" "is not a folder Bloom can have to itself"
if run "$work/home-prefix" "$work/log-remove-foreign" "$work/install-1.sh" --uninstall \
    --prefix "$work/not-bloom"; then
    echo "FAILED: --uninstall: a folder this script did not install was removed"
    failures=$((failures + 1))
fi
check "--uninstall: a folder this script did not install keeps its files" \
    [ -f "$work/not-bloom/notes.txt" ]

# --- --system, into a system image that is being prepared ---------------------------------------

image=$work/image
run_destdir=$image
check "--system: the script ends well" \
    run "$work/home-system" "$work/log-system" "$work/install-1.sh" --system \
    --from "$work/release-1"
run_destdir=
check "--system: /opt/bloom, /usr/local/bin/bloom-cli and /usr/local/share are filled" \
    [ -x "$image/opt/bloom/bin/Bloom" ]
check "--system: the link names the installed place, not the image's folder" \
    [ "$(readlink "$image/usr/local/bin/bloom-cli")" = /opt/bloom/bin/bloom-cli ]
check "--system: the menu entry starts the installed program" \
    has "$image/usr/local/share/applications/id.afterlab.bloom.desktop" \
    'Exec="/opt/bloom/bin/Bloom" %f'
check "--system: the user's home folder was not written" [ -z "$(ls -A "$work/home-system")" ]
run_destdir=$image
check "--system --uninstall: the script ends well" \
    run "$work/home-system" "$work/log-system-remove" "$work/install-1.sh" --system --uninstall
run_destdir=
check "--system --uninstall: the package, the link and the menu entry are gone" \
    [ -z "$(find "$image" -name '*loom*')" ]
# Without the rights it says so and writes nothing. (Not asked of a user who has them.)
if [ "$(id -u)" -ne 0 ] && [ ! -w /opt ] && [ ! -e /opt/bloom ]; then
    if run "$work/home-system" "$work/log-system-rights" "$work/install-1.sh" --system \
        --from "$work/release-1"; then
        echo "FAILED: --system: an install without the rights ended well"
        failures=$((failures + 1))
    fi
    check "--system: without the rights, the script says what it needs" \
        has "$work/log-system-rights" "needs administrator rights"
fi

# --- --uninstall: what the script wrote, and nothing else ---------------------------------------

mkdir -p "$home/.local/share/icons/hicolor/48x48/apps"
echo theirs > "$home/.local/bin/another-tool"
echo theirs > "$home/.local/share/applications/another.desktop"
echo theirs > "$home/.local/share/icons/hicolor/48x48/apps/another.png"
echo theirs > "$home/.local/share/mime/packages/another.xml"
# The version an update kept for going back is the script's too.
cp -r "$home/.local/opt/bloom" "$home/.local/opt/bloom.previous"
check "--uninstall: the script ends well" \
    run "$home" "$work/log-remove" "$work/install-2.sh" --uninstall
check "--uninstall: nothing of Bloom is left" [ -z "$(find "$home" -name '*loom*')" ]
check "--uninstall: other programs' files are all there" \
    [ "$(find "$home" -type f -exec cat {} + | grep -c -x theirs)" -eq 4 ]
if run "$home" "$work/log-remove-again" "$work/install-2.sh" --uninstall; then
    echo "FAILED: --uninstall: removing what is not installed ended well"
    failures=$((failures + 1))
fi
# A program of the user's own under the link's name is not the script's link.
check "--uninstall: an install to remove again" \
    run "$home" "$work/log-again" "$work/install-2.sh" --from "$work/release-2"
rm "$home/.local/bin/bloom-cli"
echo theirs > "$home/.local/bin/bloom-cli"
check "--uninstall: the script ends well beside a file of the user's" \
    run "$home" "$work/log-remove-own" "$work/install-2.sh" --uninstall
check "--uninstall: a file of the user's under the link's name stays" \
    [ "$(cat "$home/.local/bin/bloom-cli")" = theirs ]

if [ "$failures" -ne 0 ]; then
    echo "$failures check(s) failed; the logs are in $work"
    exit 1
fi
rm -rf "$work"
if [ -n "$download" ]; then
    echo "install script: every check passed, with the script's own download"
else
    echo "install script: every check passed (no curl: the files were taken with --from)"
fi
