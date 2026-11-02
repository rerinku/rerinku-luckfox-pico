<div align="center">

# rerinku-luckfox-pico

**Luckfox Pico 上的开源摄像头程序:一个约 400 KB 的可执行文件,把画面通过 WebRTC 推给 Rerinku View。**

[快速开始](#快速开始) · [配置](#配置) · [从源码编译](#从源码编译) · [常见问题](#常见问题) · [Issues](https://github.com/rerinku/rerinku-luckfox-pico/issues)

简体中文

</div>

`rerinku-luckfox` 替代 Luckfox Pico 出厂的 `rkipc`:初始化摄像头传感器、ISP 和硬件编码器,通过 Rerinku 设备 SDK 把 H.265 / H.264 画面推送到 [Rerinku View](https://hub.docker.com/r/rerinku/view)。在 Rerinku View 的网页里打开设备就能看到实时画面,支持录像、回放和 AI 识别。

它也是硬件厂商集成 Rerinku 设备 SDK 的完整参考实现:从采集、编码到推流、命令响应,全部是可读的 C 代码。

## 特性

- **低延迟实时画面:** 编码后的码流直接交给 SDK,通过 WebRTC 推送,打开画面通常在 1 到 2 秒内出首帧。
- **双码流:** 主码流默认 H.265、2304×1296@25、2 Mbit/s;子码流默认 H.264、640×360、512 kbit/s,供多分屏、手机和不支持 H.265 的浏览器使用。没人观看某一路时,SDK 不发送它。
- **硬件编码和快照:** 视频编码和 JPEG 快照都由 SoC 的硬件编码器完成,编码不占用 CPU。
- **占用小:** 程序约 400 KB(含 SDK),比出厂的 rkipc 多留出约 4 MB 系统内存,详见[实测数据](#实测数据)。
- **自动上线:** 设备序列号默认取 CPU 序列号,第一次连上 Rerinku View 就出现在设备列表里,不用手动添加。
- **设备信息和运行状态:** 在 Rerinku View 里能看到 SoC、CPU、内存、系统版本,以及实时的温度、CPU 和内存占用。
- **可热重启、可回退:** 程序随时可以停止和重新启动;一条命令即可恢复 rkipc。
- **崩溃自动拉起:** 启动脚本在程序退出 3 秒后自动重启它。

**不包含:** rkipc 自带的 RTSP 服务、音频、智能分析和 Web 管理页。

## 支持的硬件

| 板子 | 芯片 | 摄像头 | 状态 |
| --- | --- | --- | --- |
| Luckfox Pico Plus | RV1103 | SC3336(2304×1296) | 已验证 |
| Luckfox Pico Pro / Max 等 RV1106 板子 | RV1106 | — | 未测试,代码按 RV1103 / RV1106 通用接口编写 |

已在 Luckfox Pico Plus 上验证:开机自启、注册上线、2304×1296 实时画面、硬件 JPEG 快照、平台下发重启。

## 工作原理

```
                          rerinku-luckfox(板子上的一个进程)
 ┌─────────┐  ┌──────┐  ┌────────────────────────────────────────────┐
 │ SC3336  │→ │ ISP  │→ │ VI 通道 0 → VENC 0  主码流 H.265 / H.264   │
 │ 传感器  │  │rkaiq │  │          ↘ VENC 1  JPEG 快照               │──┐
 └─────────┘  └──────┘  │ VI 通道 1 → VENC 2  子码流 H.264(640×360)  │  │
                        └────────────────────────────────────────────┘  │ 编码后的帧
                                                                        ↓
                                         ┌──────────────────────────────────┐
                                         │ Rerinku 设备 SDK                  │
                                         │ HTTP 注册 · MQTT 控制 · WebRTC 推流 │
                                         └────────────────┬─────────────────┘
                                                          ↓
                                                  Rerinku View
                                                (浏览器里看画面、录像)
```

- 编码器一直在跑,每一帧都交给 SDK;没人观看时,SDK 直接丢弃,代价很小。
- 两路码流是同一次推流会话里的两个通道(`main` 和 `sub`)。平台说要哪几路,SDK 就只发那几路;中途加一路,从它的下一个关键帧开始发,不重建连接。
- 不开 SDK 的 GOP 缓存(300 万像素的一个 GOP 太占内存)。新观众进来时,SDK 回调程序,程序立即请求一个 IDR 帧。
- 快照请求只排队,由快照线程让 JPEG 通道编下一帧,一张图同时回复这期间到达的所有请求。
- 平台下发 `reboot` 时,程序先回复成功,再停止 SDK、关闭摄像头,最后重启板子。

## 快速开始

你需要:一块 Luckfox Pico Plus(接好 SC3336 摄像头和网线,系统是出厂镜像)、一台能 SSH 登录板子的电脑,以及一个正在运行的 Rerinku View。

**1. 运行 Rerinku View**(已有的话跳过)

```sh
docker run -d --name rerinku-view \
  --network host \
  --restart unless-stopped \
  -v rerinku-view-data:/data \
  rerinku/view:latest
```

在浏览器里打开 `http://<这台机器的 IP>:8080` 确认能看到控制台。板子需要能访问这台机器的 8080(HTTP)和 1883(MQTT)端口。

**2. 编译并安装到板子**

```sh
scripts/build-app.sh                                       # 编译,产物在 out/
scripts/deploy.sh --endpoint http://<Rerinku View 地址>:8080 --reboot
```

`deploy.sh` 默认用 `root@192.168.1.100` 通过 SSH 连板子,用环境变量 `LUCKFOX_HOST` 可以改。首次编译需要先准备环境,见[从源码编译](#从源码编译)。

**3. 看画面**

板子重启后约 7 秒上线。在 Rerinku View 的「设备」页里会出现 `LUCKFOX-<CPU 序列号>`,点开即可看到实时画面。

## 配置

配置文件在板子的 `/userdata/rerinku/rerinku.conf`,格式是 `key = value`,`#` 开头是注释。完整说明见 [board/rerinku.conf.example](board/rerinku.conf.example)。第一次部署时由 `--endpoint` 生成,之后的部署会保留它。

命令行上的 `key=value` 可以临时覆盖任何一项,适合手动调试:

```sh
/oem/usr/bin/rerinku-launch.sh stop
/oem/usr/bin/rerinku-luckfox log_level=debug bitrate=4096
```

| 键 | 默认值 | 说明 |
| --- | --- | --- |
| `endpoint` | (必填) | Rerinku View 的地址 |
| `sn` / `name` | CPU 序列号 / 空 | 设备序列号和显示名。序列号留空时用 `LUCKFOX-<CPU 序列号>` |
| `codec` | `h265` | 主码流编码。关掉子码流时建议改成 `h264`,这样所有浏览器都能直接播放 |
| `width` × `height` @ `fps` | 2304×1296@25 | SC3336 全幅 |
| `bitrate` / `gop` | 2048 kbit/s(CBR) / 50 帧 | |
| `wrap` | `1` | VI 到 VENC 的行缓冲模式,2304×1296 下省约 4 MB 媒体内存,RV1103 必须开 |
| `jpeg_quality` | `70` | 快照质量,`0` 关闭快照 |
| `sub` | `1` | 是否开启子码流 |
| `sub_codec` | `h264` | 子码流编码 |
| `sub_width` × `sub_height` | 640×360 | 偶数,不大于主码流 |
| `sub_bitrate` | 512 kbit/s(CBR) | |
| `nack_history` | `256` | 保留多少个已发送的包用于重传(每个约 1.2 KB) |
| `log_level` | `info` | `error`、`warn`、`info` 或 `debug` |

另有 `secret`、`ca_file`、`tls_insecure`、`iq_dir` 几项,含义见配置模板。

## 日常使用

```sh
# 看日志(进 syslog,标签是 rerinku)
grep rerinku /var/log/messages

# 停止和重新启动程序,不用重启板子
/oem/usr/bin/rerinku-launch.sh stop
/oem/usr/bin/rerinku-launch.sh

# 升级程序:重新编译,再部署一次(配置会保留),然后重启程序
scripts/deploy.sh --no-autostart
ssh root@192.168.1.100 '/oem/usr/bin/rerinku-launch.sh stop; /oem/usr/bin/rerinku-launch.sh'

# 恢复出厂的 rkipc
scripts/deploy.sh --restore-rkipc --reboot
```

`deploy.sh` 对系统做的改动很小:程序和启动脚本装到 `/oem/usr/bin/`,首次安装时生成配置;开机脚本 `RkLunch.sh` 里只把启动 rkipc 的那一行换成 `rerinku-launch.sh`,加载摄像头驱动、配置网络等其他流程原样保留。原文件备份为 `*.rkipc`。

rkipc 占着摄像头,而且停掉后无法干净地重新拉起,所以第一次切换要重启板子才生效。

## 从源码编译

交叉编译在 Luckfox 官方的 Docker 镜像里进行,和宿主机系统无关(官方只支持 Ubuntu 22.04 x86_64)。

**目录布局:** 本仓库和 [Rerinku Device SDK](https://github.com/rerinku/rerinku-device-sdk)(Apache-2.0)需要并排放在同一个目录下:

```
workspace/
├── rerinku-device-sdk/       设备 SDK
└── rerinku-luckfox-pico/     本仓库
```

```sh
git clone https://github.com/rerinku/rerinku-device-sdk.git
git clone https://github.com/rerinku/rerinku-luckfox-pico.git
cd rerinku-luckfox-pico

docker pull luckfoxtech/luckfox_pico:1.0
scripts/fetch-sdk.sh          # 拉取 Luckfox 官方 SDK 到 sdk/luckfox-pico(下载约 1 GB,解开约 4.6 GB)
scripts/check-toolchain.sh    # 在容器里编译一个 C++17 小程序,验证工具链可用

scripts/build-sdk.sh          # 编译设备 SDK → build/rerinku-sdk/
scripts/build-app.sh          # 编译主程序 → out/
```

`out/` 里是要装到板子上的全部内容:

```
out/bin/rerinku-luckfox                  主程序(静态链接 SDK,动态链接 librockit / librkaiq / uClibc)
out/bin/rerinku-launch.sh                启动脚本:后台运行,程序退出 3 秒后自动拉起
out/share/rerinku/rerinku.conf.example   配置模板
```

- Luckfox 官方 SDK 的版本钉在 [sdk/luckfox-pico.lock](sdk/luckfox-pico.lock) 里,升级时改那里,再重新运行 `fetch-sdk.sh`。
- SDK 目录不进版本控制。瑞芯微的媒体库(rockit、rkaiq、rga、mpp)是闭源二进制,只在编译时从 SDK 里取头文件和库;运行时链接的是板子固件 `/oem/usr/lib` 里自带的那一份。
- `scripts/docker-shell.sh` 可以进入编译容器(工具链已在 `PATH` 里)。默认以宿主机用户运行,整个工作目录按原路径挂载。

## 实测数据

测试条件:Luckfox Pico Plus(RV1103),2304×1296@25,H.264 2 Mbit/s,单路主码流。

| | rerinku-luckfox | rkipc(出厂) |
| --- | --- | --- |
| 程序大小(去符号) | 约 400 KB(含 SDK) | — |
| 系统可用内存 | 15.2 MB | 10.9 MB |
| 媒体内存池占用(共 24 MB) | 13.8 MB | 19.9 MB |
| 程序 CPU,空闲 / 推流 | 11% / 18% | — |
| 开机到平台在线 | 约 7 秒(含等网络) | — |
| 打开实时画面到首帧 | 1 到 2 秒 | — |

空闲时的 CPU 大部分是 rkaiq 的 3A 线程(约 8%,rkipc 也一样),取流线程约 2%。

加开子码流(640×360 H.264 512 kbit/s,两路都是 25 帧)后:媒体内存池占用增加 1.4 MB,系统可用内存基本不变,程序 CPU 空闲时增加 1%,两路同时推流时增加 3%。两路共用一个硬件编码核,主码流帧率不受影响。

rkipc 同时运行主码流、子码流、智能分析和 RTSP,所以这些内存数字不是同一口径的对比,只能说明换用本程序后,系统余量更大。

## 常见问题

**板子上线不了,日志里反复报连接被拒绝?**
先确认 `endpoint` 写的是 Rerinku View 的地址和端口(Docker 默认是 8080),并且板子能访问那台机器的 8080 和 1883 端口。

**能上线,但画面打不开?**
画面走 WebRTC(UDP),板子和浏览器、Rerinku View 之间的网络要能互通。同一个局域网内一般没有问题;跨网络访问要看 Rerinku View 一侧的网络设置。

**网页里播不了画面,但 H.264 的设备可以?**
默认主码流是 H.265,只有部分浏览器能解码。不用担心:平台会给不支持的浏览器播放子码流。如果你关掉了子码流(`sub = 0`),请把 `codec` 改成 `h264`。

**使用 https 的服务器地址时连不上?**
TLS 证书校验依赖板子的系统时钟。先确认板子的时间正确(NTP 是否生效)。

**日志里有 `XCORE:E:invalid main scene len!`?**
这是 rkaiq 每次初始化都会打印的信息,不影响使用。

**板子的内核没有 IPv6,日志里有 `errno=97`?**
SDK 的 ICE 组件会先尝试创建 IPv6 套接字,失败后自动退回 IPv4,无害。

## 项目结构

```
src/camera.c     ISP(3A、IQ 文件)→ VI → VENC:主码流、JPEG 快照、子码流
src/main.c       SDK 接入:设备配置与回调、快照线程、信号处理与关机顺序
src/config.c     配置文件与命令行覆盖
src/sysinfo.c    CPU 序列号;设备静态信息(SoC、CPU、内存、系统、MAC)和运行状态(温度、CPU、内存)
src/log.c        日志:手动运行输出到 stderr,开机运行进 syslog;SDK 的日志走同一个出口
board/           板子上用的启动脚本和配置模板
scripts/         环境准备、编译和部署脚本
cmake/           交叉编译工具链文件
tools/           辅助工具:toolchain-smoke(工具链冒烟测试)、view-relay(见下)
sdk/             Luckfox 官方 SDK 的版本锁定文件;SDK 本体由 fetch-sdk.sh 拉取,不进版本控制
```

**view-relay:** 一个小型的 Go 反向代理。当 Rerinku View 只能通过 Tailscale 之类的方式访问,而板子在另一个局域网里连不上它时,在两边都能访问到的机器上运行它,把板子的 `endpoint` 指向它即可。它转发 HTTP 和 MQTT,并改写注册响应里的地址;媒体仍由板子和 Rerinku View 通过 WebRTC 直连,不经过它。

## 反馈问题

遇到问题或有想法,请到 [Issues](https://github.com/rerinku/rerinku-luckfox-pico/issues) 提出。报告问题时请附上:

- 板子型号和固件版本
- 摄像头型号
- 相关日志(`grep rerinku /var/log/messages`,必要时加上 `log_level=debug`)
- Rerinku View 的版本

## 许可证

[Apache License 2.0](LICENSE)。

本项目链接并分发的第三方组件各自遵循自己的许可证:瑞芯微的媒体库(rockit、rkaiq、rga、mpp)是闭源二进制,Luckfox 官方 SDK 遵循其仓库中的许可;设备 SDK 及其依赖见 [rerinku-device-sdk](https://github.com/rerinku/rerinku-device-sdk) 的 `NOTICE`。
