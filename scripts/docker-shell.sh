#!/usr/bin/env bash
# 进入 Luckfox 官方编译环境（Ubuntu 22.04 镜像 luckfoxtech/luckfox_pico:1.0）。
#
#   scripts/docker-shell.sh                 交互式 shell
#   scripts/docker-shell.sh <命令...>       在容器里执行一条命令后退出
#   DOCKER_ROOT=1 scripts/docker-shell.sh   以 root 运行（整包固件编译 ./build.sh 需要）
#
# 整个 workspace 按宿主机同一路径挂载，容器里能直接访问旁边的 rerinku-client，
# 报错里的路径也和宿主机一致。默认用宿主机 uid/gid 运行，避免留下 root 拥有的文件。
# 交叉工具链来自 SDK 源码（scripts/fetch-sdk.sh 拉取），已加进 PATH。
set -euo pipefail

IMAGE="${LUCKFOX_IMAGE:-luckfoxtech/luckfox_pico:1.0}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORKSPACE="$(cd "$ROOT/.." && pwd)"
SDK="$ROOT/sdk/luckfox-pico"
TOOLCHAIN="$SDK/tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf"

if [ ! -d "$TOOLCHAIN/bin" ]; then
    echo "找不到交叉工具链：$TOOLCHAIN/bin，先运行 scripts/fetch-sdk.sh" >&2
    exit 1
fi

args=(--rm --init
      -v "$WORKSPACE:$WORKSPACE"
      -w "$(pwd)"
      -e "LUCKFOX_SDK_PATH=$SDK"
      -e "PATH=$TOOLCHAIN/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin")

if [ "${DOCKER_ROOT:-0}" != 1 ]; then
    # 镜像里没有对应用户，给一个可写的 HOME
    args+=(--user "$(id -u):$(id -g)" -e HOME=/tmp)
fi

if [ -t 0 ] && [ -t 1 ]; then
    args+=(-it)
fi

if [ $# -eq 0 ]; then
    set -- bash
fi

exec docker run "${args[@]}" "$IMAGE" "$@"
