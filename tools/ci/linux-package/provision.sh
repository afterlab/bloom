#!/bin/sh
# Provisions the build environment of the Linux Bloom package inside a bare Ubuntu 22.04 root
# file system (docs/architecture/platform-support.md, "Packages"): the system's own C library
# (glibc 2.35) and C++ library (libstdc++ 12), the locked Clang 22 from the LLVM project's
# archive, and CMake from Kitware's archive, because 22.04's own is older than Bloom needs.
#
# Run as root inside the image (the Dockerfile beside this file does) or inside the same root
# file system entered without a container runtime. Both archives' signing keys are compared with
# their fingerprints before they are trusted.
set -eu
export DEBIAN_FRONTEND=noninteractive
# A root file system entered as a single user cannot switch to the package tool's own user.
APT="apt-get -o APT::Sandbox::User=root"
$APT update
$APT install --yes --no-install-recommends ca-certificates curl gnupg

llvm_key=$(mktemp)
curl --fail --silent --show-error --location https://apt.llvm.org/llvm-snapshot.gpg.key \
    --output "$llvm_key"
fingerprint=$(gpg --show-keys --with-colons "$llvm_key" | awk -F: '$1 == "fpr" {print $10; exit}')
test "$fingerprint" = 6084F3CF814B57C1CF12EFD515CF4D18AF4F7421
gpg --dearmor < "$llvm_key" > /usr/share/keyrings/llvm-snapshot.gpg
echo 'deb [signed-by=/usr/share/keyrings/llvm-snapshot.gpg] https://apt.llvm.org/jammy/ llvm-toolchain-jammy-22 main' \
    > /etc/apt/sources.list.d/llvm-22.list

kitware_key=$(mktemp)
curl --fail --silent --show-error --location \
    https://apt.kitware.com/keys/kitware-archive-latest.asc --output "$kitware_key"
fingerprint=$(gpg --show-keys --with-colons "$kitware_key" | awk -F: '$1 == "fpr" {print $10; exit}')
test "$fingerprint" = 4DBEBE3EEC96E7B8C6EC5BE99E92FDC6C5B9BA75
gpg --dearmor < "$kitware_key" > /usr/share/keyrings/kitware-archive-keyring.gpg
echo 'deb [signed-by=/usr/share/keyrings/kitware-archive-keyring.gpg] https://apt.kitware.com/ubuntu/ jammy main' \
    > /etc/apt/sources.list.d/kitware.list
rm -f "$llvm_key" "$kitware_key"

$APT update
# The compiler and its sanitizer runtime; the build tools; the headers of the declared system
# libraries Bloom and the locked Qt are linked against; the D-Bus library Qt's own is linked
# to; and what makes the two published forms of the package.
$APT install --yes --no-install-recommends \
    clang-22 lld-22 libclang-rt-22-dev cmake ninja-build meson make pkg-config git patchelf \
    perl python3 xz-utils bzip2 unzip file binutils libc6-dev libstdc++-12-dev \
    libgl-dev libegl-dev libfontconfig1-dev libxkbcommon-dev libva-dev libdbus-1-3 \
    squashfs-tools desktop-file-utils
clang-22 --version | head -n 1
cmake --version | head -n 1
ninja --version
ldd --version | head -n 1
