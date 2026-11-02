#!/usr/bin/env bash
# Cross-compile the Rerinku Device SDK (../rerinku-device-sdk) for RV1103/RV1106
# inside the official build container, and install it to build/rerinku-sdk:
#
#   build/rerinku-sdk/include/rerinku/   rerinku_device.h (the C API), rerinku_version.h
#   build/rerinku-sdk/lib/               librerinku_device.a (SDK + dependencies in
#                                        one archive), librerinku_device.so
#   build/rerinku-sdk/lib/cmake/         find_package(rerinku_device)
#   build/rerinku-device-sdk/            build tree (examples, tests)
#
#   scripts/build-sdk.sh            Release build (size-optimized)
#   BUILD_TYPE=Debug scripts/build-sdk.sh
#   RERINKU_DEVICE_SDK_DIR=/path/to/rerinku-device-sdk scripts/build-sdk.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CLIENT="$(cd "${RERINKU_DEVICE_SDK_DIR:-$ROOT/../rerinku-device-sdk}" && pwd)"
BUILD="$ROOT/build/rerinku-device-sdk"
PREFIX="$ROOT/build/rerinku-sdk"
BUILD_TYPE="${BUILD_TYPE:-MinSizeRel}"

case "$CLIENT" in
"$(cd "$ROOT/.." && pwd)"/*) ;;
*) echo "rerinku-device-sdk must live inside the workspace mounted into the container: $CLIENT" >&2
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
