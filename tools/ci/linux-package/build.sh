#!/bin/sh
# Builds the Linux Bloom package inside the build environment (provision.sh): the dependency
# superbuild, Bloom against it, the package tree with its check, and the tar and the AppImage.
#
#   sh tools/ci/linux-package/build.sh [work directory, default build/linux-package]
#
# The files are left in <work directory>/bloom/package: the tar, the AppImage and the
# AppImage's control file. BLOOM_PACKAGE_VERSION in the environment names a pre-release's
# files (0.1.0-alpha.1); without it they carry the source tree's version. A release also gives
# its update settings in the environment, and each that is set is passed to the build:
# BLOOM_UPDATE_FEED_URL, BLOOM_UPDATE_PUBLIC_KEYS and BLOOM_UPDATE_APPIMAGE_INFORMATION
# (docs/decisions/0041-self-update.md). Without them the package never looks for updates.
set -eu
source=$(cd "$(dirname "$0")/../../.." && pwd)
work=${1:-$source/build/linux-package}
export CC="${CC:-clang-22}" CXX="${CXX:-clang++-22}" LC_ALL=C TZ=UTC SOURCE_DATE_EPOCH=0

cmake -S "$source/dependencies/superbuild" -B "$work/superbuild" -G Ninja \
    -DBLOOM_DEPENDENCY_PREFIX="$work/prefix" \
    -DBLOOM_DEPENDENCY_DOWNLOAD_DIR="$work/downloads" \
    -DBLOOM_FFMPEG_ENABLE_VAAPI=ON
cmake --build "$work/superbuild"

cmake -S "$source" -B "$work/bloom" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DBLOOM_DEPENDENCY_MODE=qualified -DBLOOM_DEPENDENCY_PREFIX="$work/prefix" \
    -DBLOOM_BUILD_PYTHON=ON -DBLOOM_BUILD_DESKTOP=ON -DBLOOM_RENDER_ENABLE_VULKAN=ON \
    "-DBLOOM_PACKAGE_FLOOR=GLIBC=2.35;GLIBCXX=3.4.30;CXXABI=1.3.13" \
    ${BLOOM_PACKAGE_VERSION:+"-DBLOOM_PACKAGE_VERSION=$BLOOM_PACKAGE_VERSION"} \
    ${BLOOM_UPDATE_FEED_URL:+"-DBLOOM_UPDATE_FEED_URL=$BLOOM_UPDATE_FEED_URL"} \
    ${BLOOM_UPDATE_PUBLIC_KEYS:+"-DBLOOM_UPDATE_PUBLIC_KEYS=$BLOOM_UPDATE_PUBLIC_KEYS"} \
    ${BLOOM_UPDATE_APPIMAGE_INFORMATION:+"-DBLOOM_UPDATE_APPIMAGE_INFORMATION=$BLOOM_UPDATE_APPIMAGE_INFORMATION"}
cmake --build "$work/bloom" --target bloom-package-archives
