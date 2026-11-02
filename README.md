<div align="center">

# rerinku-luckfox-pico

**An open source camera program for the Luckfox Pico: one executable of about 400 KB that streams video to Rerinku View over WebRTC.**

[Quick start](#quick-start) · [Configuration](#configuration) · [Building from source](#building-from-source) · [FAQ](#faq) · [Issues](https://github.com/rerinku/rerinku-luckfox-pico/issues)

English · [简体中文](README.zh-CN.md) · [日本語](README.ja.md)

</div>

`rerinku-luckfox` replaces `rkipc`, the program the Luckfox Pico ships with. It sets up the camera sensor, the ISP and the hardware encoder, then uses the [Rerinku Device SDK](https://github.com/rerinku/rerinku-device-sdk) to push H.265 or H.264 video to [Rerinku View](https://github.com/rerinku/rerinku-view), a self-hosted NVR. Open the device in Rerinku View to watch it live, record it, play it back and run AI detection on it.

It is also a complete reference for hardware vendors integrating the SDK: capture, encoding, streaming and command handling are all readable C code.

## Features

- **Low-latency live view:** encoded video goes straight to the SDK and out over WebRTC. The first frame usually appears within 1 to 2 seconds.
- **Two streams:** the main stream defaults to H.265 at 2304×1296, 25 fps and 2 Mbit/s. The sub stream defaults to H.264 at 640×360 and 512 kbit/s, for split-screen walls, phones and browsers without H.265. The SDK doesn't send a stream that nobody is watching.
- **Hardware encoding and snapshots:** the SoC's hardware encoder handles both video and JPEG snapshots.
- **Small footprint:** the program is about 400 KB including the SDK, and leaves about 4 MB more system memory free than the stock rkipc. See [Measurements](#measurements).
- **Automatic registration:** the serial number comes from the CPU serial by default, and the device appears in Rerinku View's device list the first time it connects.
- **Device information and status:** Rerinku View shows the SoC, CPU, memory and OS version, plus live temperature, CPU and memory use.
- **Restart at any time, easy to undo:** you can stop and start the program whenever you like, and one command restores rkipc.
- **Automatic restart:** the launch script restarts the program 3 seconds after it exits.

**Not included:** rkipc's RTSP server, audio, smart analysis and web admin page.

## Supported hardware

| Board | SoC | Camera | Status |
| --- | --- | --- | --- |
| Luckfox Pico Plus | RV1103 | SC3336 (2304×1296) | Verified |
| Luckfox Pico Pro / Max and other RV1106 boards | RV1106 | — | Untested; the code uses interfaces common to RV1103 and RV1106 |

Verified on the Luckfox Pico Plus: autostart at boot, registration, 2304×1296 live video, hardware JPEG snapshots and rebooting from the platform.

## How it works

```
                          rerinku-luckfox (one process on the board)
 ┌─────────┐  ┌───────┐  ┌───────────────────────────────────────────┐
 │ SC3336  │→ │  ISP  │→ │ VI channel 0 → VENC 0  main, H.265/H.264  │
 │ sensor  │  │ rkaiq │  │              ↘ VENC 1  JPEG snapshots     │──┐
 └─────────┘  └───────┘  │ VI channel 1 → VENC 2  sub, H.264 640×360 │  │
                         └───────────────────────────────────────────┘  │ encoded frames
                                                                        ↓
                                  ┌─────────────────────────────────────────────┐
                                  │ Rerinku Device SDK                          │
                                  │ HTTP registration · MQTT control · WebRTC   │
                                  └──────────────────────┬──────────────────────┘
                                                         ↓
                                                   Rerinku View
                                           (watch in a browser, record)
```

- The encoders always run and every frame goes to the SDK. When nobody is watching, the SDK drops it at very little cost.
- The two streams are two channels (`main` and `sub`) in one streaming session. The platform says which ones it wants and the SDK sends only those. A stream added midway starts at its next keyframe, without rebuilding the connection.
- The SDK's GOP cache is off, because one GOP at 3 megapixels takes too much memory. When a new viewer joins, the SDK calls back and the program requests an IDR frame right away.
- Snapshot requests only queue. A snapshot thread has the JPEG channel encode the next frame, and one image answers every request that arrived in the meantime.
- On a `reboot` command from the platform, the program replies first, then stops the SDK, closes the camera and reboots the board.

## Quick start

You need a Luckfox Pico Plus with the stock image, an SC3336 camera and Ethernet; a computer that can SSH into the board; and a running Rerinku View.

**1. Run Rerinku View** (skip this if you already have it)

```sh
docker run -d --name rerinku-view \
  --network host \
  --restart unless-stopped \
  -v rerinku-view-data:/data \
  rerinku/view:latest
```

Open `http://<host-ip>:8080` in a browser to check that the console loads. The board must be able to reach this machine on ports 8080 (HTTP) and 1883 (MQTT).

**2. Build and install on the board**

Set up the build environment first; see [Building from source](#building-from-source). Then run:

```sh
scripts/build-app.sh                                          # build; output goes to out/
scripts/deploy.sh --endpoint http://<rerinku-view-host>:8080 --reboot
```

`deploy.sh` connects over SSH to `root@192.168.1.100` by default. Set `LUCKFOX_HOST` to change that.

**3. Watch**

The board comes online about 7 seconds after it reboots. `LUCKFOX-<CPU serial>` appears on the Devices page of Rerinku View. Open it to see live video.

## Configuration

Settings live in `/userdata/rerinku/rerinku.conf` on the board, as `key = value` lines with `#` comments. See [board/rerinku.conf.example](board/rerinku.conf.example) for the full reference. The first deploy creates the file using `--endpoint`, and later deploys keep it.

A `key=value` argument on the command line overrides any setting for that run, which is handy for debugging:

```sh
/oem/usr/bin/rerinku-launch.sh stop
/oem/usr/bin/rerinku-luckfox log_level=debug bitrate=4096
```

| Key | Default | Description |
| --- | --- | --- |
| `endpoint` | (required) | Rerinku View address |
| `sn` / `name` | CPU serial / empty | Serial number and display name. An empty serial number becomes `LUCKFOX-<CPU serial>` |
| `codec` | `h265` | Main stream codec. With the sub stream off, use `h264` so every browser can play the main stream |
| `width` × `height` @ `fps` | 2304×1296@25 | SC3336 full frame |
| `bitrate` / `gop` | 2048 kbit/s (CBR) / 50 frames | |
| `wrap` | `1` | VI-to-VENC line buffer mode. Saves about 4 MB of media memory at 2304×1296; required on RV1103 |
| `jpeg_quality` | `70` | Snapshot quality; `0` turns snapshots off |
| `sub` | `1` | Whether the sub stream is on |
| `sub_codec` | `h264` | Sub stream codec |
| `sub_width` × `sub_height` | 640×360 | Even numbers, no larger than the main stream |
| `sub_bitrate` | 512 kbit/s (CBR) | |
| `nack_history` | `256` | Sent packets kept for retransmission (about 1.2 KB each) |
| `log_level` | `info` | `error`, `warn`, `info` or `debug` |

The template also describes `secret`, `ca_file`, `tls_insecure` and `iq_dir`.

## Everyday use

```sh
# Read the logs (syslog, tag "rerinku")
grep rerinku /var/log/messages

# Stop and start the program without rebooting the board
/oem/usr/bin/rerinku-launch.sh stop
/oem/usr/bin/rerinku-launch.sh

# Upgrade: rebuild, deploy again (settings are kept), then restart the program
scripts/deploy.sh --no-autostart
ssh root@192.168.1.100 '/oem/usr/bin/rerinku-launch.sh stop; /oem/usr/bin/rerinku-launch.sh'

# Go back to the stock rkipc
scripts/deploy.sh --restore-rkipc --reboot
```

`deploy.sh` changes very little on the system. It installs the program and launch script in `/oem/usr/bin/` and creates the settings file on first install. In the boot script `RkLunch.sh`, it replaces only the line that starts rkipc with `rerinku-launch.sh`, so camera drivers, networking and the rest of the boot sequence stay as they were. The original files are kept as `*.rkipc`.

rkipc holds the camera and can't be cleanly restarted once stopped, so the first switch takes effect after a reboot.

## Building from source

Cross-compilation runs in Luckfox's official Docker image, so the host OS doesn't matter (the vendor officially supports only Ubuntu 22.04 x86_64).

This repository and the [Rerinku Device SDK](https://github.com/rerinku/rerinku-device-sdk) (Apache-2.0) must sit side by side:

```
workspace/
├── rerinku-device-sdk/       the device SDK
└── rerinku-luckfox-pico/     this repository
```

```sh
git clone https://github.com/rerinku/rerinku-device-sdk.git
git clone https://github.com/rerinku/rerinku-luckfox-pico.git
cd rerinku-luckfox-pico

docker pull luckfoxtech/luckfox_pico:1.0
scripts/fetch-sdk.sh          # fetch the Luckfox SDK to sdk/luckfox-pico (about 1 GB download, 4.6 GB unpacked)
scripts/check-toolchain.sh    # compile a small C++17 program in the container to check the toolchain

scripts/build-sdk.sh          # build the device SDK → build/rerinku-sdk/
scripts/build-app.sh          # build the program → out/
```

`out/` holds everything that goes on the board:

```
out/bin/rerinku-luckfox                  the program (SDK linked statically; librockit, librkaiq and uClibc dynamically)
out/bin/rerinku-launch.sh                launch script: runs in the background, restarts the program 3 seconds after it exits
out/share/rerinku/rerinku.conf.example   settings template
```

- The Luckfox SDK version is pinned in [sdk/luckfox-pico.lock](sdk/luckfox-pico.lock). To upgrade, change it there and run `fetch-sdk.sh` again.
- The Luckfox SDK directory isn't under version control. Rockchip's media libraries (rockit, rkaiq, rga, mpp) are closed-source binaries: headers and link stubs come from the SDK at build time, and at run time the program uses the copies in the board's firmware under `/oem/usr/lib`.
- `scripts/docker-shell.sh` opens a shell in the build container with the toolchain on `PATH`. It runs as your host user and mounts the parent directory at its original path.

## Measurements

Test setup: Luckfox Pico Plus (RV1103), 2304×1296@25, H.264 at 2 Mbit/s, main stream only.

| | rerinku-luckfox | rkipc (stock) |
| --- | --- | --- |
| Program size (stripped) | about 400 KB, including the SDK | — |
| Available system memory | 15.2 MB | 10.9 MB |
| Media memory pool used (of 24 MB) | 13.8 MB | 19.9 MB |
| Program CPU, idle / streaming | 11% / 18% | — |
| Boot to online | about 7 seconds, including waiting for the network | — |
| Opening live view to first frame | 1 to 2 seconds | — |

Most of the idle CPU goes to rkaiq's 3A threads (about 8%, the same under rkipc). The stream-reading thread takes about 2%.

Turning on the sub stream (640×360 H.264 at 512 kbit/s, both streams at 25 fps) adds 1.4 MB to the media memory pool and leaves available system memory about the same. Program CPU rises by 1% when idle and 3% with both streams sending. The two streams share one hardware encoder core, and the main stream's frame rate isn't affected.

rkipc runs a main stream, a sub stream, smart analysis and RTSP at once, so these memory figures aren't a like-for-like comparison. They only show that the system has more headroom with this program.

## FAQ

**The board won't come online and the log keeps reporting refused connections.**
Check that `endpoint` has the right Rerinku View address and port (8080 by default in Docker), and that the board can reach that machine on ports 8080 and 1883.

**The board is online, but the video won't open.**
Video uses WebRTC over UDP, so the board, the browser and Rerinku View must be able to reach each other. This usually just works on one LAN. Across networks, it depends on Rerinku View's network settings.

**A browser can't play the main stream, but H.264 devices work.**
The main stream is H.265 by default, and only some browsers decode it. The platform plays the sub stream to those that don't. If you turned the sub stream off (`sub = 0`), set `codec` to `h264`.

**It can't connect to an https server address.**
TLS certificate checks depend on the board's clock. Make sure the board's time is correct and NTP is working.

**The log shows `XCORE:E:invalid main scene len!`.**
rkaiq prints this every time it initializes. It's harmless.

**The log shows `errno=97`.**
The board's kernel has no IPv6. The SDK's ICE component tries an IPv6 socket first and falls back to IPv4, so it's harmless.

## Project layout

```
src/camera.c     ISP (3A, IQ files) → VI → VENC: main stream, JPEG snapshots, sub stream
src/main.c       SDK integration: device config and callbacks, snapshot thread, signals and shutdown order
src/config.c     settings file and command-line overrides
src/sysinfo.c    CPU serial; static device description (SoC, CPU, memory, OS, MAC) and live status (temperature, CPU, memory)
src/log.c        logging: stderr when run by hand, syslog at boot; SDK logs use the same path
board/           launch script and settings template for the board
scripts/         environment, build and deploy scripts
cmake/           cross-compilation toolchain file
tools/           helpers: toolchain-smoke (toolchain check) and view-relay (see below)
sdk/             lock file for the Luckfox SDK version; fetch-sdk.sh downloads the SDK itself
```

**view-relay** is a small reverse proxy written in Go. Use it when Rerinku View is reachable only through something like Tailscale and the board, on another LAN, can't reach it. Run it on a machine that can reach both, and point the board's `endpoint` at it. It forwards HTTP and MQTT and rewrites the addresses in the registration response. Media doesn't pass through it: the board and Rerinku View connect directly over WebRTC.

## Reporting issues

Please open an [issue](https://github.com/rerinku/rerinku-luckfox-pico/issues) for bugs and ideas. For bugs, include:

- Board model and firmware version
- Camera model
- Relevant logs (`grep rerinku /var/log/messages`, with `log_level=debug` if needed)
- Rerinku View version

## License

[Apache License 2.0](LICENSE). Third-party components are listed in [NOTICE](NOTICE) and keep their own licenses. Rockchip's media libraries are closed-source binaries that ship in the board's firmware; they aren't part of this repository.
