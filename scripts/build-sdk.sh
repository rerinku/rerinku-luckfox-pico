#!/usr/bin/env bash
# Cross-compile the rerinku device SDK (../rerinku-client) for RV1103/RV1106
# inside the official build container, and install it to build/rerinku-sdk:
#
#   build/rerinku-sdk/include/rerinku/   rerinku_device.h (the C API)
#   build/rerinku-sdk/lib/               librerinku.a (SDK + dependencies in one
#                                        archive), librerinku.so
#   build/rerinku-client/                build tree (examples, tests)
#
#   scripts/build-sdk.sh            Release build (size-optimized)
#   BUILD_TYPE=Debug scripts/build-sdk.sh
#   RERINKU_CLIENT_DIR=/path/to/rerinku-client scripts/build-sdk.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CLIENT="$(cd "${RERINKU_CLIENT_DIR:-$ROOT/../rerinku-client}" && pwd)"
BUILD="$ROOT/build/rerinku-client"
PREFIX="$ROOT/build/rerinku-sdk"
BUILD_TYPE="${BUILD_TYPE:-MinSizeRel}"

case "$CLIENT" in
"$(cd "$ROOT/.." && pwd)"/*) ;;
*) echo "rerinku-client must live inside the workspace mounted into the container: $CLIENT" >&2
   exit 1 ;;
esac

"$ROOT/scripts/docker-shell.sh" bash -c "
    set -euo pipefail
    cmake -S '$CLIENT' -B '$BUILD' \
        -DCMAKE_TOOLCHAIN_FILE='$CLIENT/cmake/toolchains/rv1106-uclibc.cmake' \
        -DCMAKE_BUILD_TYPE='$BUILD_TYPE' \
        -DBUILD_TESTING=ON
    cmake --build '$BUILD' -j\$(nproc)
    rm -rf '$PREFIX'
    cmake --install '$BUILD' --component sdk --strip --prefix '$PREFIX' >/dev/null
"

echo
echo "installed to $PREFIX:"
(cd "$PREFIX" && find . -type f | sort | sed 's|^\./|  |')
