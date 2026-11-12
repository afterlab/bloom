#!/bin/sh
# Builds the Linux Bloom package inside the build environment (provision.sh): the dependency
# superbuild, Bloom against it, the package tree with its check, and the tar and the AppImage.
#
#   sh tools/ci/linux-package/build.sh [work directory, default build/linux-package]
#
# The two files are left in <work directory>/bloom/package.
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
    "-DBLOOM_PACKAGE_FLOOR=GLIBC=2.35;GLIBCXX=3.4.30;CXXABI=1.3.13"
cmake --build "$work/bloom" --target bloom-package-archives
