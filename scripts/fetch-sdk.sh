#!/usr/bin/env bash
# 把 Luckfox Pico 官方 SDK 浅克隆到 sdk/luckfox-pico，并检出 sdk/luckfox-pico.lock 钉住的提交。
# SDK 约 1GB、自带 git 仓库，不进本仓库版本控制（见 .gitignore）。
# 重复执行是幂等的：已在钉住的提交上就什么都不做。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=../sdk/luckfox-pico.lock
source "$ROOT/sdk/luckfox-pico.lock"
DEST="$ROOT/sdk/luckfox-pico"

if [ -d "$DEST/.git" ]; then
    current="$(git -C "$DEST" rev-parse HEAD 2>/dev/null || true)"
    if [ "$current" = "$SDK_COMMIT" ]; then
        echo "SDK 已在 ${SDK_COMMIT:0:12}，无需更新"
        exit 0
    fi
else
    mkdir -p "$DEST"
    git -C "$DEST" init -q
    git -C "$DEST" remote add origin "$SDK_URL"
fi

echo "拉取 $SDK_URL @ ${SDK_COMMIT:0:12} ..."
git -C "$DEST" fetch --depth 1 --progress origin "$SDK_COMMIT"
git -C "$DEST" -c advice.detachedHead=false checkout -q FETCH_HEAD
echo "完成：$(git -C "$DEST" log -1 --format='%h %cd %s' --date=short)"
