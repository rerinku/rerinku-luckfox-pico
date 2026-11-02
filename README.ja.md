<div align="center">

# rerinku-luckfox-pico

**Luckfox Pico 向けのオープンソースカメラプログラム。約 400 KB の実行ファイル 1 つで、映像を WebRTC で Rerinku View に送信します。**

[クイックスタート](#クイックスタート) · [設定](#設定) · [ソースからのビルド](#ソースからのビルド) · [よくある質問](#よくある質問) · [Issues](https://github.com/rerinku/rerinku-luckfox-pico/issues)

[English](README.md) · [简体中文](README.zh-CN.md) · 日本語

</div>

`rerinku-luckfox` は、Luckfox Pico に標準で入っている `rkipc` を置き換えるプログラムです。カメラセンサー、ISP、ハードウェアエンコーダーを初期化し、[Rerinku Device SDK](https://github.com/rerinku/rerinku-device-sdk) を使って H.265 / H.264 の映像をセルフホスト型 NVR の [Rerinku View](https://github.com/rerinku/rerinku-view) に送信します。Rerinku View でデバイスを開くと、ライブ表示、録画、再生、AI 認識を利用できます。

ハードウェアメーカーが SDK を組み込む際の完全なリファレンス実装でもあります。キャプチャ、エンコード、配信、コマンド処理のすべてを読みやすい C コードで確認できます。

## 特長

- **低遅延のライブ表示:** エンコード済みの映像をそのまま SDK に渡し、WebRTC で送信します。最初のフレームは通常 1〜2 秒で表示されます。
- **2 つのストリーム:** メインストリームの既定は H.265、2304×1296、25 fps、2 Mbit/s です。サブストリームの既定は H.264、640×360、512 kbit/s で、分割表示、スマートフォン、H.265 に対応しないブラウザー向けです。視聴者のいないストリームは SDK が送信しません。
- **ハードウェアエンコードとスナップショット:** 映像と JPEG スナップショットの両方を SoC のハードウェアエンコーダーで処理します。
- **小さなフットプリント:** プログラムは SDK を含めて約 400 KB で、標準の rkipc より約 4 MB 多くシステムメモリーが空きます。[計測結果](#計測結果)をご覧ください。
- **自動でオンライン:** シリアル番号は既定で CPU シリアルから作られ、初めて接続した時点で Rerinku View のデバイス一覧に表示されます。
- **デバイス情報と状態:** Rerinku View で SoC、CPU、メモリー、OS のバージョンのほか、温度、CPU、メモリー使用量をリアルタイムで確認できます。
- **いつでも再起動でき、元に戻すのも簡単:** プログラムはいつでも停止・起動でき、コマンド 1 つで rkipc に戻せます。
- **自動再起動:** 起動スクリプトが、プログラムの終了から 3 秒後に再起動します。

**含まれないもの:** rkipc の RTSP サーバー、音声、スマート分析、Web 管理ページ。

## 対応ハードウェア

| ボード | SoC | カメラ | 状況 |
| --- | --- | --- | --- |
| Luckfox Pico Plus | RV1103 | SC3336(2304×1296) | 検証済み |
| Luckfox Pico Pro / Max などの RV1106 ボード | RV1106 | — | 未テスト。コードは RV1103 / RV1106 共通のインターフェースを使用 |

Luckfox Pico Plus で検証済みの内容:起動時の自動実行、登録、2304×1296 のライブ映像、ハードウェア JPEG スナップショット、プラットフォームからの再起動。

## 仕組み

```
                          rerinku-luckfox(ボード上の 1 プロセス)
 ┌─────────┐  ┌───────┐  ┌───────────────────────────────────────────┐
 │ SC3336  │→ │  ISP  │→ │ VI channel 0 → VENC 0  main, H.265/H.264  │
 │ sensor  │  │ rkaiq │  │              ↘ VENC 1  JPEG snapshots     │──┐
 └─────────┘  └───────┘  │ VI channel 1 → VENC 2  sub, H.264 640×360 │  │
                         └───────────────────────────────────────────┘  │ エンコード済みフレーム
                                                                        ↓
                                  ┌─────────────────────────────────────────────┐
                                  │ Rerinku Device SDK                          │
                                  │ HTTP registration · MQTT control · WebRTC   │
                                  └──────────────────────┬──────────────────────┘
                                                         ↓
                                                   Rerinku View
                                              (ブラウザーで視聴、録画)
```

- エンコーダーは常に動作し、すべてのフレームを SDK に渡します。視聴者がいないときは SDK が破棄するので、コストはわずかです。
- 2 つのストリームは、1 つの配信セッション内の 2 つのチャンネル(`main` と `sub`)です。SDK はプラットフォームが要求したものだけを送信します。途中で追加されたストリームは次のキーフレームから送信され、接続は作り直しません。
- SDK の GOP キャッシュはオフにしています。300 万画素の GOP はメモリーを使いすぎるためです。新しい視聴者が来ると SDK がコールバックし、プログラムがすぐに IDR フレームを要求します。
- スナップショット要求はキューに入れるだけです。スナップショットスレッドが JPEG チャンネルに次のフレームをエンコードさせ、1 枚の画像でその間に届いたすべての要求に応答します。
- プラットフォームから `reboot` コマンドを受けると、先に応答してから SDK を停止し、カメラを閉じ、ボードを再起動します。

## クイックスタート

必要なもの:標準イメージの Luckfox Pico Plus(SC3336 カメラと有線 LAN を接続)、ボードに SSH でログインできるパソコン、動作中の Rerinku View。

**1. Rerinku View を起動**(すでにある場合は不要)

```sh
docker run -d --name rerinku-view \
  --network host \
  --restart unless-stopped \
  -v rerinku-view-data:/data \
  rerinku/view:latest
```

ブラウザーで `http://<ホストの IP>:8080` を開き、コンソールが表示されることを確認します。ボードからこのマシンのポート 8080(HTTP)と 1883(MQTT)に接続できる必要があります。

**2. ビルドしてボードにインストール**

先にビルド環境を準備してください。手順は[ソースからのビルド](#ソースからのビルド)にあります。そのうえで次を実行します:

```sh
scripts/build-app.sh                                          # ビルド。成果物は out/ に出力
scripts/deploy.sh --endpoint http://<Rerinku View のアドレス>:8080 --reboot
```

`deploy.sh` は既定で `root@192.168.1.100` に SSH で接続します。変更するには `LUCKFOX_HOST` を設定してください。

**3. 視聴**

ボードは再起動の約 7 秒後にオンラインになります。Rerinku View のデバイスページに `LUCKFOX-<CPU シリアル>` が表示されるので、開くとライブ映像を見られます。

## 設定

設定はボードの `/userdata/rerinku/rerinku.conf` にあり、`key = value` 形式で、`#` はコメントです。詳しくは [board/rerinku.conf.example](board/rerinku.conf.example) をご覧ください。初回のデプロイで `--endpoint` を使って作成され、以降のデプロイでは保持されます。

コマンドラインの `key=value` で、その実行に限って任意の設定を上書きできます。デバッグに便利です:

```sh
/oem/usr/bin/rerinku-launch.sh stop
/oem/usr/bin/rerinku-luckfox log_level=debug bitrate=4096
```

| キー | 既定値 | 説明 |
| --- | --- | --- |
| `endpoint` | (必須) | Rerinku View のアドレス |
| `sn` / `name` | CPU シリアル / 空 | シリアル番号と表示名。シリアル番号が空の場合は `LUCKFOX-<CPU シリアル>` |
| `codec` | `h265` | メインストリームのコーデック。サブストリームをオフにする場合は、すべてのブラウザーで再生できる `h264` を推奨 |
| `width` × `height` @ `fps` | 2304×1296@25 | SC3336 のフルフレーム |
| `bitrate` / `gop` | 2048 kbit/s(CBR) / 50 フレーム | |
| `wrap` | `1` | VI から VENC へのラインバッファーモード。2304×1296 でメディアメモリーを約 4 MB 節約。RV1103 では必須 |
| `jpeg_quality` | `70` | スナップショットの品質。`0` でオフ |
| `sub` | `1` | サブストリームの有効・無効 |
| `sub_codec` | `h264` | サブストリームのコーデック |
| `sub_width` × `sub_height` | 640×360 | 偶数で、メインストリーム以下 |
| `sub_bitrate` | 512 kbit/s(CBR) | |
| `nack_history` | `256` | 再送用に保持する送信済みパケット数(1 つ約 1.2 KB) |
| `log_level` | `info` | `error`、`warn`、`info`、`debug` |

`secret`、`ca_file`、`tls_insecure`、`iq_dir` についてはテンプレートをご覧ください。

## 日常の操作

```sh
# ログを見る(syslog、タグは rerinku)
grep rerinku /var/log/messages

# ボードを再起動せずにプログラムを停止・起動
/oem/usr/bin/rerinku-launch.sh stop
/oem/usr/bin/rerinku-launch.sh

# アップグレード:再ビルドして再度デプロイ(設定は保持)し、プログラムを再起動
scripts/deploy.sh --no-autostart
ssh root@192.168.1.100 '/oem/usr/bin/rerinku-launch.sh stop; /oem/usr/bin/rerinku-launch.sh'

# 標準の rkipc に戻す
scripts/deploy.sh --restore-rkipc --reboot
```

`deploy.sh` がシステムに加える変更はわずかです。プログラムと起動スクリプトを `/oem/usr/bin/` にインストールし、初回のみ設定ファイルを作成します。起動スクリプト `RkLunch.sh` では rkipc を起動する行だけを `rerinku-launch.sh` に置き換えるので、カメラドライバーの読み込み、ネットワーク設定など、ほかの処理はそのままです。元のファイルは `*.rkipc` として残ります。

rkipc はカメラを占有し、停止後にきれいに再起動できないため、最初の切り替えはボードの再起動後に有効になります。

## ソースからのビルド

クロスコンパイルは Luckfox 公式の Docker イメージ内で行うので、ホストの OS は問いません(ベンダーが公式にサポートするのは Ubuntu 22.04 x86_64 のみです)。

このリポジトリと [Rerinku Device SDK](https://github.com/rerinku/rerinku-device-sdk)(Apache-2.0)を同じディレクトリに並べて置く必要があります:

```
workspace/
├── rerinku-device-sdk/       デバイス SDK
└── rerinku-luckfox-pico/     このリポジトリ
```

```sh
git clone https://github.com/rerinku/rerinku-device-sdk.git
git clone https://github.com/rerinku/rerinku-luckfox-pico.git
cd rerinku-luckfox-pico

docker pull luckfoxtech/luckfox_pico:1.0
scripts/fetch-sdk.sh          # Luckfox SDK を sdk/luckfox-pico に取得(ダウンロード約 1 GB、展開後約 4.6 GB)
scripts/check-toolchain.sh    # コンテナー内で小さな C++17 プログラムをコンパイルしてツールチェーンを確認

scripts/build-sdk.sh          # デバイス SDK をビルド → build/rerinku-sdk/
scripts/build-app.sh          # プログラムをビルド → out/
```

`out/` にはボードにインストールするものがすべて入っています:

```
out/bin/rerinku-luckfox                  プログラム(SDK は静的リンク、librockit / librkaiq / uClibc は動的リンク)
out/bin/rerinku-launch.sh                起動スクリプト:バックグラウンドで動作し、終了から 3 秒後に再起動
out/share/rerinku/rerinku.conf.example   設定テンプレート
```

- Luckfox SDK のバージョンは [sdk/luckfox-pico.lock](sdk/luckfox-pico.lock) で固定しています。アップグレードするときはここを変更し、`fetch-sdk.sh` を再度実行してください。
- Luckfox SDK のディレクトリはバージョン管理の対象外です。Rockchip のメディアライブラリー(rockit、rkaiq、rga、mpp)はクローズドソースのバイナリで、ビルド時は SDK のヘッダーとライブラリーを使い、実行時はボードのファームウェア内 `/oem/usr/lib` のものを使います。
- `scripts/docker-shell.sh` でビルドコンテナーのシェルに入れます(ツールチェーンは `PATH` に設定済み)。ホストのユーザーで動作し、親ディレクトリを元のパスのままマウントします。

## 計測結果

テスト条件:Luckfox Pico Plus(RV1103)、2304×1296@25、H.264 2 Mbit/s、メインストリームのみ。

| | rerinku-luckfox | rkipc(標準) |
| --- | --- | --- |
| プログラムサイズ(ストリップ後) | 約 400 KB(SDK を含む) | — |
| システムの空きメモリー | 15.2 MB | 10.9 MB |
| メディアメモリープールの使用量(全 24 MB) | 13.8 MB | 19.9 MB |
| プログラムの CPU、アイドル / 配信中 | 11% / 18% | — |
| 起動からオンラインまで | 約 7 秒(ネットワーク待ちを含む) | — |
| ライブ表示を開いてから最初のフレームまで | 1〜2 秒 | — |

アイドル時の CPU の大半は rkaiq の 3A スレッドです(約 8%、rkipc でも同じ)。ストリーム取得スレッドは約 2% です。

サブストリーム(640×360 H.264 512 kbit/s、どちらも 25 fps)をオンにすると、メディアメモリープールの使用量は 1.4 MB 増え、システムの空きメモリーはほぼ変わりません。プログラムの CPU はアイドル時に 1%、2 つ同時の配信時に 3% 増えます。2 つのストリームは 1 つのハードウェアエンコーダーコアを共有し、メインストリームのフレームレートは影響を受けません。

rkipc はメインストリーム、サブストリーム、スマート分析、RTSP を同時に動かしているので、これらのメモリーの数値は同じ条件での比較ではありません。このプログラムに置き換えるとシステムの余裕が増えることを示すものです。

## よくある質問

**ボードがオンラインにならず、ログに接続拒否が繰り返し出ます。**
`endpoint` が Rerinku View の正しいアドレスとポート(Docker の既定は 8080)になっているか、ボードからそのマシンのポート 8080 と 1883 に接続できるかを確認してください。

**オンラインになるのに、映像が開きません。**
映像は WebRTC(UDP)で送られるので、ボード、ブラウザー、Rerinku View の間で通信できる必要があります。同じ LAN 内なら通常は問題ありません。ネットワークをまたぐ場合は、Rerinku View 側のネットワーク設定によります。

**メインストリームを再生できないブラウザーがありますが、H.264 のデバイスは再生できます。**
メインストリームの既定は H.265 で、デコードできるブラウザーは一部です。対応していないブラウザーには、プラットフォームがサブストリームを再生します。サブストリームをオフ(`sub = 0`)にしている場合は、`codec` を `h264` にしてください。

**https のサーバーアドレスに接続できません。**
TLS 証明書の検証はボードの時計に依存します。時刻が正しく、NTP が動作していることを確認してください。

**ログに `XCORE:E:invalid main scene len!` が出ます。**
rkaiq が初期化のたびに出力するもので、問題ありません。

**ログに `errno=97` が出ます。**
ボードのカーネルに IPv6 がないためです。SDK の ICE コンポーネントは先に IPv6 ソケットを試し、失敗すると IPv4 に切り替えるので、問題ありません。

## プロジェクト構成

```
src/camera.c     ISP(3A、IQ ファイル)→ VI → VENC:メインストリーム、JPEG スナップショット、サブストリーム
src/main.c       SDK との連携:デバイス設定とコールバック、スナップショットスレッド、シグナルと終了順序
src/config.c     設定ファイルとコマンドラインの上書き
src/sysinfo.c    CPU シリアル、デバイスの静的情報(SoC、CPU、メモリー、OS、MAC)とリアルタイムの状態(温度、CPU、メモリー)
src/log.c        ログ:手動実行時は stderr、起動時は syslog。SDK のログも同じ経路
board/           ボード用の起動スクリプトと設定テンプレート
scripts/         環境準備、ビルド、デプロイのスクリプト
cmake/           クロスコンパイルのツールチェーンファイル
tools/           補助ツール:toolchain-smoke(ツールチェーンの確認)、view-relay(下記)
sdk/             Luckfox SDK のバージョン固定ファイル。SDK 本体は fetch-sdk.sh が取得
```

**view-relay** は Go で書かれた小さなリバースプロキシです。Rerinku View に Tailscale などでしか接続できず、別の LAN にあるボードからは届かない場合に使います。両方に接続できるマシンで実行し、ボードの `endpoint` をそこに向けてください。HTTP と MQTT を転送し、登録応答内のアドレスを書き換えます。メディアは経由せず、ボードと Rerinku View が WebRTC で直接接続します。

## 問題の報告

不具合やご意見は [Issues](https://github.com/rerinku/rerinku-luckfox-pico/issues) にお寄せください。不具合の報告には次の情報を添えてください:

- ボードの型番とファームウェアのバージョン
- カメラの型番
- 関連するログ(`grep rerinku /var/log/messages`、必要に応じて `log_level=debug`)
- Rerinku View のバージョン

## ライセンス

[Apache License 2.0](LICENSE)。サードパーティーコンポーネントは [NOTICE](NOTICE) に記載しており、それぞれのライセンスが適用されます。Rockchip のメディアライブラリーはボードのファームウェアに含まれるクローズドソースのバイナリで、このリポジトリには含まれません。
