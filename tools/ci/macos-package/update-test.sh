#!/bin/sh
# The end-to-end update test of the macOS files, on the machine that built them
# (docs/development/releasing.md, "The end-to-end update test").
#
#   sh tools/ci/macos-package/update-test.sh <build directory> <version> <work directory>
#
# <build directory> is the configured release build whose bloom-package-archives made
# <build directory>/package/Bloom-<version>-macos-<processor>.zip. The script keeps that zip as the
# candidate, configures the same build directory again as the test build of an older version
# (BLOOM_UPDATE_TEST_BUILD=ON, the test's public key, <version's numbers>-alpha.0), builds its
# zip, and runs tools/release/update_e2e.py: the test build is updated to the candidate by the
# platform's updater, and then put back by the update helper when a candidate cannot start.
#
# It runs where the build ran because the platform's updater needs the login session of a
# desktop: its installer waits for a helper that shows progress, and that helper does not come up
# in a remote session without one. A hosted machine has the session. The build directory is the
# test build's afterwards, so everything of the release is uploaded before this runs.
set -eu

if [ $# -ne 3 ]; then
    echo "usage: update-test.sh <build directory> <version> <work directory>" >&2
    exit 2
fi
build=$1
version=$2
work=$3
source=$(cd "$(dirname "$0")/../../.." && pwd)
processor=$(uname -m)
older=${version%%-*}-alpha.0
if [ "$older" = "$version" ]; then
    echo "update-test.sh: $version has no older label to make a test build of" >&2
    exit 2
fi

mkdir -p "$work/candidate"
candidate=$work/candidate/Bloom-$version-macos-$processor.zip
cp "$build/package/Bloom-$version-macos-$processor.zip" "$candidate"

# The signing program for the test's own release: the machine's openssl is not OpenSSL 3.
cmake --build "$build" --target bloom-ed25519-tool --parallel
BLOOM_ED25519_TOOL=$build/src/update/bloom-ed25519-tool
export BLOOM_ED25519_TOOL
key=$(python3 -I "$source/tools/release/update_e2e.py" --print-test-key)

cmake -S "$source" -B "$build" "-DBLOOM_PACKAGE_VERSION=$older" -DBLOOM_UPDATE_TEST_BUILD=ON \
    "-DBLOOM_UPDATE_PUBLIC_KEYS=$key" -DBLOOM_UPDATE_FEED_URL=
cmake --build "$build" --target bloom-package-archives --parallel

python3 -I "$source/tools/release/update_e2e.py" \
    --installed "$build/package/Bloom-$older-macos-$processor.zip" \
    --candidate "$candidate" --work "$work/run"
