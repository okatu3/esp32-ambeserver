# 概要 / Overview

> **English summary** — ESP32 firmware that behaves like a DVSI **DV3000 / AMBE-3000R** vocoder for **D-STAR**
> (AMBE 3600x2400: 2400 bps voice + 1200 bps FEC), using the open-source codec **mbelib-neo**. A PC program such as
> **BlueDV** talks to it with the DV3000 packet protocol, either over **Wi-Fi (UDP/TCP 2460, "AMBE server")** or over
> **USB (serial, 460800 bps)**. It works end to end (transmit and receive), but the transmit audio quality of the
> mbelib-neo *encoder* is still clearly worse than a real DV3000. **We are looking for help** with the encoder
> (pitch tracking and voicing), see [AUDIO_FINDINGS.md](AUDIO_FINDINGS.md).

## 何をするものか
アマチュア無線の D-STAR では、音声を AMBE という方式で符号化します。PC で D-STAR を使うソフト（BlueDV など）は、
本来は DVSI 社の AMBE チップ（DV3000、ThumbDV、DVstick など）を USB や LAN でつないで使います。

このファームウェアは、**ESP32 をその AMBE チップの代わりにする**ものです。符号化・復号はオープンソースの
**mbelib-neo** で行います。

```
 マイク → BlueDV ──(DV3000 パケット: 音声 PCM)──→ ESP32 ──(AMBE 9 バイト)──→ BlueDV → リフレクター
 スピーカー ← BlueDV ←─(音声 PCM)── ESP32 ←──(AMBE 9 バイト)── BlueDV ← リフレクター
```
* 1 フレーム = 20 ms = 160 サンプル（8 kHz、16 ビット）↔ AMBE 72 ビット（9 バイト）
* BlueDV は 100 ms ごとに 5 フレームずつまとめて送ってくる

## 3 つの開発系統
| 系統（ブランチ） | ボード | PC との接続 | 音声処理 | 状態 |
|---|---|---|---|---|
| `esp32` | ESP32（無印、DevKit） | Wi-Fi（UDP/TCP 2460） | mbelib-neo（符号化器を改良）＋ 前処理（低域カット、ゲート、雑音抑圧、自動イコライザー） | 動作する。音質の改良を続けていた |
| `s3` | ESP32-S3（内蔵 USB だけの小型ボード） | Wi-Fi、または USB（起動時に GPIO 4 を GND で USB モード） | `esp32` と同じ | 動作する |
| `usb-esp32` | ESP32（無印、DevKit） | USB だけ（ボードの USB シリアル、460800 bps） | **公開版の mbelib-neo そのまま** ＋ ノイズゲート ＋ 話し始めのピッチ合わせ | 比較の基準として開発中 |

`main` ブランチは、この説明の入口です（ファームウェアは各系統のブランチにあります）。

## 必要なもの
* ESP32 DevKit（無印）または ESP32-S3 ボード
* ESP-IDF **5.1.6**
* BlueDV（Windows）など、DV3000 を使える D-STAR ソフト
  * Wi-Fi の系統：BlueDV の「AMBE server」に ESP32 の IP アドレスとポート 2460
  * USB の系統：BlueDV のシリアル接続の AMBE に、ボードの COM ポート（460800 bps）

## ビルドと書き込み
各系統のフォルダーで：
```
.\idf_env.bat build
.\idf_env.bat -p COMx flash
```
`idf_env.bat` の ESP-IDF と Python の場所は、自分の PC に合わせて変えてください（環境変数 `IDF_516` と
`IDF_PY_ENV` でも指定できます）。mbelib-neo は `components/mbelib_neo/vendor/mbelib-neo` に入っています。

## 資料の一覧
* [PROTOCOL.md](PROTOCOL.md)：実装している DV3000 パケットと、調整用の独自コマンド
* [AUDIO_FINDINGS.md](AUDIO_FINDINGS.md)：音質について調べたこと、わかったこと、**未解決の課題（助けてほしいこと）**
* [TESTING.md](TESTING.md)：試験のしかたと、試験用のツール（`tools/audio_test/`）
* [BlueDV_D-STAR_hosts_fix.pdf](BlueDV_D-STAR_hosts_fix.pdf)：BlueDV の D-STAR のホスト一覧のエラーの回避手順

## ライセンスと特許
* このリポジトリのファームウェアとツール：GPL-2.0-or-later
* mbelib-neo：GPL-2.0-or-later（`components/mbelib_neo/vendor/mbelib-neo/LICENSE`）
* AMBE 方式には、DVSI 社の特許がある国・地域があります。実験用として公開しています。使う・配る前に、
  自分の国の法律を確認してください。
