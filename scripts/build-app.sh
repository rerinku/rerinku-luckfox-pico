#!/usr/bin/env bash
# Cross-compile the camera program (rerinku-luckfox) in the official build
# container and install it to out/:
#
#   out/bin/rerinku-luckfox                  the program (stripped)
#   out/bin/rerinku-launch.sh                start/restart script for the board
#   out/share/rerinku/rerinku.conf.example   settings template
#
#   scripts/build-app.sh                     builds the SDK first when missing
#   BUILD_TYPE=Debug scripts/build-app.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$ROOT/build/app"
PREFIX="$ROOT/out"
BUILD_TYPE="${BUILD_TYPE:-MinSizeRel}"

if [ ! -f "$ROOT/build/rerinku-sdk/lib/librerinku_device.a" ]; then
    "$ROOT/scripts/build-sdk.sh"
fi

"$ROOT/scripts/docker-shell.sh" bash -c "
    set -euo pipefail
    cmake -S '$ROOT' -B '$BUILD' \
        -DCMAKE_TOOLCHAIN_FILE='$ROOT/cmake/rv1106-uclibc.cmake' \
        -DCMAKE_BUILD_TYPE='$BUILD_TYPE'
    cmake --build '$BUILD' -j\$(nproc)
    rm -rf '$PREFIX'
    cmake --install '$BUILD' --strip --prefix '$PREFIX' >/dev/null
"

echo
echo "installed to $PREFIX:"
(cd "$PREFIX" && find . -type f | sort | while read -r f; do
    printf '  %-40s %s\n' "${f#./}" "$(du -h "$f" | cut -f1)"
done)
