# ESP32 AMBE3000 (DV3000) compatible server over Wi-Fi — `esp32` line

ESP32 (classic DevKit) firmware that answers the DV3000 / AMBE-3000R protocol over **UDP and TCP port 2460**
(BlueDV "AMBE server"), with a serial setup console for the Wi-Fi settings. D-STAR only.
Codec: mbelib-neo with a **modified encoder** and pre-processing (150 Hz high-pass, noise gate, noise reduction,
automatic equaliser, +6 dB with peak limiter).

ESP32（無印）を、Wi-Fi でつなぐ AMBE サーバーにする系統です。設定（SSID、パスワード、IP アドレス）は USB の
COM ポートの設定画面で行います。

* Overview of all lines and the open problems: [docs/OVERVIEW.md](docs/OVERVIEW.md),
  [docs/AUDIO_FINDINGS.md](docs/AUDIO_FINDINGS.md)
* Setup console, BlueDV settings, troubleshooting (Japanese): [README_WIFI.md](README_WIFI.md)
* Protocol and private tuning commands (F0–F3): [docs/PROTOCOL.md](docs/PROTOCOL.md)
* Testing: [docs/TESTING.md](docs/TESTING.md), tools in `tools/audio_test/`
* Changes per flashed version: [CHANGELOG.md](CHANGELOG.md) (tags `esp32-YYYY.MM.DD`)

## Build (ESP-IDF 5.1.6, Windows)
```
.\idf_env.bat build
.\idf_env.bat -p COM12 flash
```
`idf_env.bat` uses `IDF_516` (ESP-IDF folder) and `IDF_PY_ENV` (its Python environment) if set; otherwise edit the
defaults in the file.

## BlueDV
Setup → AMBE: **AMBE server**, IP = the ESP32's address, port **2460**.

## Local changes to mbelib-neo
`components/mbelib_neo/vendor/mbelib-neo` is mbelib-neo (upstream commit `27df6dc`) with two local changes:
* `src/internal/mbe_compiler.h`: `MBE_THREAD_LOCAL` can be defined by the build (set empty on ESP-IDF: thread-local
  storage is copied into every task's stack and made the chip reboot at start-up)
* `src/ambe/ambe3600x2400_enc.c`: pitch tracking (continuity, octave-down correction, separate pitch source),
  voicing cut-offs, magnitude/smoothing options and their tuning globals (see docs/PROTOCOL.md, F0/F2)

## License / Patent notice
GPL-2.0-or-later ([LICENSE](LICENSE)); mbelib-neo GPL-2.0-or-later. AMBE is covered by DVSI patents in some
jurisdictions — for experimentation; check your local law before use or distribution.
