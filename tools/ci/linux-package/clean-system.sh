#!/bin/sh
# Prepares a bare official image as the clean system start.sh wants
# (docs/architecture/platform-support.md, "Linux Package"). Run as root inside the image.
#
#   sh tools/ci/linux-package/clean-system.sh
#
# It installs the system libraries a Linux Bloom package expects, by the package names
# tools/install/linux-library-packages.txt gives for this family; what the install script needs
# (tar, xz, sha256sum, openssl, curl); and the standalone CMake from its publisher, checked by its
# SHA-256, which start.sh uses to unpack and to run the check. Nothing else: no Python, no Qt's
# tools and no compiler, which start.sh refuses.
set -eu
source=$(cd "$(dirname "$0")/../../.." && pwd)
table=$source/tools/install/linux-library-packages.txt
cmake_version=3.31.6
cmake_sha256=5a1133ff103c71eb5120e2cc3de922733e7d8a26a98ae716397e8676adb367bf

# shellcheck disable=SC1091
. /etc/os-release
case " ${ID:-} ${ID_LIKE:-} " in
    *" debian "* | *" ubuntu "*) column=3 ;;
    *" fedora "*) column=4 ;;
    *)
        echo "clean-system.sh: no package names for '${ID:-this system}'" >&2
        exit 2
        ;;
esac
packages=$(awk -v column="$column" '!/^#/ && NF >= 5 { print $column }' "$table" | sort -u)

if [ "$column" = 3 ]; then
    export DEBIAN_FRONTEND=noninteractive
    apt-get update
    # A release that renamed a library package for its 64-bit time type has it under the new
    # name, as the install script's advice has (tools/install/install.sh.in).
    named=
    for name in $packages; do
        if apt-cache show "${name}t64" > /dev/null 2>&1; then
            name=${name}t64
        fi
        named="$named $name"
    done
    # shellcheck disable=SC2086
    apt-get install --yes --no-install-recommends ca-certificates curl tar xz-utils openssl \
        coreutils $named
else
    tools="tar xz openssl coreutils"
    command -v curl > /dev/null 2>&1 || tools="$tools curl"
    # shellcheck disable=SC2086
    dnf install --assumeyes --setopt=install_weak_deps=False $tools $packages
fi

archive=cmake-$cmake_version-linux-x86_64.tar.gz
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
curl --fail --silent --show-error --location \
    "https://github.com/Kitware/CMake/releases/download/v$cmake_version/$archive" \
    --output "$work/$archive"
echo "$cmake_sha256  $work/$archive" | sha256sum --check --quiet -
mkdir -p /opt/cmake
tar -xzf "$work/$archive" -C /opt/cmake --strip-components=1
ln -sf /opt/cmake/bin/cmake /usr/local/bin/cmake
cmake --version | head -n 1
