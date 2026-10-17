# 試験のしかた / Testing

> **English summary** — Judge quality on a **real receiver** (we use the Peanut app), not only through the ESP32's own
> decoder (mbelib-neo's decoder hides or adds artefacts). For repeatable numbers, `tools/audio_test/` feeds a
> synthetic test signal with known pitch through the board over the serial link and scores what the encoder sent
> (pitch read straight from the AMBE bits), plus latency and link counters.

## 1. 耳で聞く（いちばん大事）
* 送信の音は、**本物の受信機**で聞いて判断します（私たちは Peanut のスマホアプリで、自分の送信を聞いています）。
  ESP32 自身の受信側（mbelib-neo の復号）を通して聞いた音は、Peanut で聞いた音と判断が分かれることがありました。
* 聞き比べは、同じマイク・同じ話し方で、設定を 1 つだけ変えて行います（Wi-Fi の系統では、F2 の命令で
  動いたまま切り替えられます。[PROTOCOL.md](PROTOCOL.md)）。
* 先に、届いている声の大きさを確かめます（[AUDIO_FINDINGS.md](AUDIO_FINDINGS.md) の 1.8）。

## 2. 通信の記録（Wi-Fi の系統）
BlueDV と ESP32 の間の UDP（ポート 2460）を Wireshark / tshark で記録すると、BlueDV から届いた音声（PCM）と、
ESP32 が返した AMBE を、あとから取り出せます。
```
tshark -i <Wi-Fi のインターフェース> -f "udp port 2460" -w session.pcapng
```
* 検証は **UDP** で行います（BlueDV が UDP を使うため）。
* 録音には自分や他局の声が入るので、**リポジトリには入れません**（`.gitignore` で `*.wav` を除外）。

## 3. 試験用のツール（`tools/audio_test/`）
Python 3 と pyserial が必要です（ESP-IDF の Python 環境に入っています）。

| ツール | 中身 |
|---|---|
| `mktest.py` | 合成した試験音を作る。12 語（100〜250 Hz、半分は息から始まる）を無音で区切り、最後に 1 kHz の音。各フレームの正解（ピッチ・無音・息・音）を `.truth` に書く。`bg` を付けると、390 Hz の連続音と雑音の背景を足す |
| `score.py` | 試験音をボードで符号化し、符号化器が送ったピッチを **AMBE のデータから直接**読んで採点する。ボードで復号した音も保存できる |
| `latency.py` | BlueDV と同じ送り方（100 ms ごとに 5 フレーム）で WAV を送り、1 フレームごとの返事までの時間と取りこぼしを測る |
| `stats.py` | `s3` 系統の F4（届いたバイト数・パケット数・声の大きさ）を、COM ポートか UDP で読む |
| `onset_pitch.py` | 2 つの WAV（入力と復号した音）の、話し始めの数フレームのピッチを並べる |

使い方の例（USB の系統、ボードが COM9 のとき）：
```
python tools/audio_test/mktest.py t_clean.wav clean
python tools/audio_test/mktest.py t_bg.wav bg
python tools/audio_test/score.py COM9 t_clean.wav decoded.wav
python tools/audio_test/latency.py COM9 t_clean.wav
```
`score.py` の結果の読み方：
```
SCORE onset_avg 0.00 frames  onset_max 0  glides 0/12  mid_err 63/201  gap_open 0  level +9.9 dB  tone +4.7 dB
```
| 項目 | 意味 | よい値 |
|---|---|---|
| `onset_avg` / `onset_max` | 語の始まりから、ピッチが正しく（12% 以内）なるまでのフレーム数 | 0 |
| `glides` | 1 フレーム目が 25% 以上外れた語の数 | 0 |
| `mid_err` | 語の途中（最初と最後の 2 フレームを除く）でピッチが 12% 以上外れたフレーム | 0 |
| `gap_open` | 無音の区間（語の終わりの 10 フレームを除く）で、声として送ったフレーム | 0 |
| `level` | ボードで復号した音と入力の、大きさの差 | 0 dB 付近 |
| `tone` | 1 kHz の音の、大きさの差 | 0 dB 付近 |

* Wi-Fi の系統では、`score.py` / `latency.py` はそのままでは使えません（シリアル用）。UDP で同じパケットを
  送る形に直せば使えます（`stats.py` は UDP に対応）。
* ボードの USB シリアルは、PC が RTS だけ ON で開くとリセットされます。ツールは DTR・RTS とも OFF で開きます。

## 4. 現在の基準値（`usb-esp32`、2026-10-17）
| | 背景なし | 背景あり |
|---|---|---|
| onset_avg / glides | 0.00 / 0 | 0.00 / 0 |
| mid_err | 63 / 201 | 63 / 201 |
| gap_open | 0 | 0 |
| level / tone | +9.9 dB / +4.7 dB | +10.0 dB / +2.2 dB |
| 処理の遅れ（5 フレーム目の返事まで） | 最大 47 ms | |
