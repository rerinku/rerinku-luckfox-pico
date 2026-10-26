# rerinku-luckfox-pico

Luckfox Pico（RV1103 / RV1106）上的 rerinku 摄像头主程序 `rerinku-luckfox`：替代瑞芯微参考程序 rkipc，
初始化 sensor、ISP 和硬件编码器，通过 rerinku SDK 把画面推到 rerinku 平台，并响应平台的快照、
信息查询和重启命令。它同时是硬件厂商集成 rerinku SDK 的完整参考实现。

已在 Luckfox Pico Plus（RV1103，SC3336 摄像头）上验证：开机自启、注册上线、2304×1296 H.264 实时画面、
硬件 JPEG 快照、平台下发重启。

## 编译环境

官方只支持 Ubuntu 22.04 x86_64，这里统一用 Luckfox 官方 Docker 镜像
`luckfoxtech/luckfox_pico:1.0`，宿主机系统无关。交叉工具链
`arm-rockchip830-linux-uclibcgnueabihf`（GCC 8.3，uClibc）在官方 SDK 源码里，不在镜像里。

```sh
docker pull luckfoxtech/luckfox_pico:1.0
scripts/fetch-sdk.sh          # 浅克隆官方 SDK 到 sdk/luckfox-pico（约 1GB 下载，解开 4.6GB）
scripts/check-toolchain.sh    # 在容器里编译 C++17 冒烟测试
scripts/docker-shell.sh       # 进入编译容器（工具链已在 PATH）
```

- SDK 版本钉在 `sdk/luckfox-pico.lock`，升级时改那里再重跑 `fetch-sdk.sh`。
- SDK 目录不进版本控制。瑞芯微的媒体库（rockit、rkaiq、rga、mpp）是闭源二进制，
  只在编译时从 SDK 取头文件和库，运行时链接板子固件 `/oem/usr/lib` 里自带的那份。
- `docker-shell.sh` 默认以宿主机用户运行，整个 workspace 按原路径挂载，
  容器里可以直接访问旁边的 `rerinku-client`。整包固件编译（SDK 的 `./build.sh`）
  需要 root：`DOCKER_ROOT=1 scripts/docker-shell.sh`。

## 编译

```sh
scripts/build-sdk.sh    # rerinku SDK（../rerinku-client，纯 C）→ build/rerinku-sdk/
scripts/build-app.sh    # 主程序 → out/（缺 SDK 时先自动编 SDK）
```

`out/` 里是要装到板子上的全部东西：

```
out/bin/rerinku-luckfox                   主程序，静态链接 SDK，动态链接 librockit / librkaiq / uClibc
out/bin/rerinku-launch.sh                 启动脚本：后台运行，程序退出 3 秒后自动拉起
out/share/rerinku/rerinku.conf.example    配置模板
```

## 安装到板子

```sh
scripts/deploy.sh --endpoint http://<onboard 地址>:18070 --reboot
```

通过 SSH（默认 `root@192.168.1.100`，`LUCKFOX_HOST` 可改）完成：

1. 程序和启动脚本装到 `/oem/usr/bin/`；
2. 首次安装时生成 `/userdata/rerinku/rerinku.conf`（之后保留，`--endpoint` 只改那一行）；
3. 开机脚本 `/oem/usr/bin/RkLunch.sh` 里只把启动 rkipc 的那一行换成 `rerinku-launch.sh`，
   加载摄像头驱动（`insmod_ko.sh`）、网络等其余流程原样保留；`RkLunch-stop.sh` 同步加上停止命令。
   原文件备份为 `*.rkipc`，`scripts/deploy.sh --restore-rkipc --reboot` 恢复 rkipc。

rkipc 占着摄像头，而且停掉后无法干净地重新拉起，所以切换在重启后生效。我们的程序可以随时停止和
重新启动（`/oem/usr/bin/rerinku-launch.sh stop` / `rerinku-launch.sh`），升级程序后不用重启板子。

设备序列号默认是 `LUCKFOX-<CPU 序列号>`，例如 `LUCKFOX-8176C2C64E1ECB46`；平台上第一次注册即出现在设备列表。
日志进 syslog：`grep rerinku /var/log/messages`。

## 配置

`/userdata/rerinku/rerinku.conf`，`key = value`，完整说明见 [board/rerinku.conf.example](board/rerinku.conf.example)。
命令行上 `key=value` 可以临时覆盖任一项，例如手动调试：

```sh
/oem/usr/bin/rerinku-launch.sh stop
/oem/usr/bin/rerinku-luckfox log_level=debug bitrate=4096
```

| 键 | 默认 | |
|---|---|---|
| `endpoint` | （必填） | rerinku 平台（onboard）地址 |
| `sn` / `name` | CPU 序列号 / 空 | 设备身份与显示名 |
| `codec` | `h265` | 主码流编码；不支持 H.265 的浏览器由平台改放子码流。关掉子码流时建议改 `h264` |
| `width` × `height` @ `fps` | 2304×1296@25 | SC3336 全幅 |
| `bitrate` / `gop` | 2048 kbit/s CBR / 50 帧 | |
| `wrap` | 1 | VI→VENC 行缓冲（wrap）模式，2304×1296 下省约 4 MB 媒体内存，RV1103 必须开 |
| `jpeg_quality` | 70 | 快照 JPEG 质量，0 关闭快照 |
| `sub` | 1 | 子码流（流畅）：多分屏、手机、不支持 H.265 的浏览器看它 |
| `sub_codec` | `h264` | |
| `sub_width` × `sub_height` | 640×360 | 偶数，不大于主码流 |
| `sub_bitrate` | 512 kbit/s CBR | |

## 程序结构

```
src/camera.c    rkaiq ISP（3A、IQ 文件）→ VI 通道 0 → VENC 0（主码流 H.264/H.265 CBR）+ VENC 1（JPEG，与 VENC 0 combo）
                                    → VI 通道 1 → VENC 2（子码流，ISP 第二路输出直接缩放，不经软件）
src/main.c      SDK 接入：设备配置与回调、快照线程、信号与关机顺序
src/config.c    配置文件与命令行覆盖
src/sysinfo.c   CPU 序列号、型号；静态描述（SoC/CPU/内存/系统/MAC，ext_json）与运行状态（温度/CPU/内存，get_runtime）
src/log.c       日志：手动运行打 stderr，开机运行进 syslog；SDK 日志同一出口
board/          板上的启动脚本与配置模板
```

和 SDK 的对接点：

- 编码器一直在跑，每帧交给 `rerinku_device_write()`；没人观看时 SDK 直接丢弃，代价很小。
- 两路码流是同一个推流会话里的两个通道（stream id `main` / `sub`）。平台在 `push_sfu` 里
  说要哪几路，SDK 只发要的那几路；中途加一路从它的下一个关键帧开始发，不重建连接。
- 不开 SDK 的 GOP 缓存（`gop_cache_max_bytes = 0`，3 MP 的一个 GOP 太占内存）；新观众进来时
  SDK 回调 `on_keyframe_request`，程序调 `RK_MPI_VENC_RequestIDR` 立即出 IDR。
- 快照：`on_snapshot` 只排队，快照线程让 JPEG 通道编下一帧，一张图同时回复期间到达的所有请求。
- 命令：`reboot` 先回复成功，再停 SDK（通知平台下线）、关摄像头、重启；其他命令回复不支持。
- 关机顺序：先停 SDK，再停快照线程，最后关编码器和 ISP。

## 实测（Luckfox Pico Plus，RV1103，2304×1296@25 H.264 2 Mbit/s）

| | rerinku-luckfox | rkipc（出厂） |
|---|---|---|
| 程序大小（去符号） | 396 KB（含 SDK） | — |
| 系统可用内存 | 15.2 MB | 10.9 MB |
| 媒体内存池（CMA 24 MB）占用 | 13.8 MB | 19.9 MB |
| 程序 CPU，空闲 / 推流 | 11% / 18% | — |
| 开机到平台在线 | 约 7 秒（含等网络） | — |
| 打开实时画面到首帧 | 1–2 秒 | — |

空闲时的 CPU 大部分是 rkaiq 的 3A 线程（约 8%，rkipc 也一样），取流线程约 2%。

加开子码流（640×360 H.264 512 kbit/s，两路都 25 帧）后：媒体内存池占用 +1.4 MB，系统可用内存
基本不变，程序 CPU 空闲 +1%、两路同时推流 +3%。两路共用一个硬件编码核，主码流帧率不受影响。
rkipc 同时跑主码流、子码流、智能分析和 RTSP，所以内存数字不是同口径对比，只说明替换后余量更大。

## 目录

```
CMakeLists.txt           主程序构建（链接 build/rerinku-sdk 与 SDK 里的瑞芯微媒体库）
cmake/rv1106-uclibc.cmake 交叉编译工具链文件
src/                     主程序源码
board/                   板上文件：启动脚本、配置模板
scripts/                 环境与构建脚本（fetch-sdk / docker-shell / check-toolchain / build-sdk / build-app / deploy）
sdk/luckfox-pico.lock    官方 SDK 来源与钉住的提交
sdk/luckfox-pico/        官方 SDK（fetch-sdk.sh 拉取，git 忽略）
tools/toolchain-smoke/   工具链冒烟测试
build/  out/             构建产物（git 忽略）
```
