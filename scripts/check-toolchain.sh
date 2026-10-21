#!/usr/bin/env bash
# 验证交叉编译环境：在官方容器里用 SDK 工具链编译 tools/toolchain-smoke，
# 给了板子地址就推上去运行。
#
#   scripts/check-toolchain.sh                    只编译
#   scripts/check-toolchain.sh root@192.168.1.100 编译并在板子上运行（需能免交互 ssh）
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$ROOT/build/toolchain-smoke"
CROSS=arm-rockchip830-linux-uclibcgnueabihf
mkdir -p "$OUT"

# C++ 运行库静态链接：不依赖板子上 libstdc++.so 的版本，和以后 SDK 的发布方式一致
"$ROOT/scripts/docker-shell.sh" bash -c "
    set -e
    ${CROSS}-gcc --version | head -1
    ${CROSS}-g++ -std=c++17 -O2 -Wall -Wextra -pthread \
        -static-libstdc++ -static-libgcc \
        -o '$OUT/smoke' '$ROOT/tools/toolchain-smoke/smoke.cpp'
    ${CROSS}-strip '$OUT/smoke'
"
file "$OUT/smoke"
ls -l "$OUT/smoke"

if [ $# -ge 1 ]; then
    target="$1"
    # 用 cat 管道传，不依赖板子上有 scp/sftp-server
    ssh "$target" 'cat > /tmp/rerinku-smoke && chmod +x /tmp/rerinku-smoke' < "$OUT/smoke"
    ssh "$target" '/tmp/rerinku-smoke; rc=$?; rm -f /tmp/rerinku-smoke; exit $rc'
fi
