# ESP32-S3 AMBE3000 (DV3000) compatible vocoder — `s3` line

The `esp32` line ported to small **ESP32-S3** boards that only have the chip's built-in USB connector.
Same audio processing as `esp32`, plus a **USB mode**:

| GPIO 4 at boot | Mode | PC link |
|---|---|---|
| open (internal pull-up) | Wi-Fi | UDP/TCP 2460 (BlueDV "AMBE server"); the built-in USB COM port is the setup console |
| connected to GND | **USB** | the built-in USB COM port carries only the DV3000 protocol (BlueDV serial AMBE); Wi-Fi, console and logs are off |

ESP32-S3 の小型ボード用の系統です。起動するときに GPIO 4 を GND につなぐと USB モード（BlueDV からシリアル接続の
AMBE として使う）、つながなければ Wi-Fi モードになります。

* Overview and open problems: [docs/OVERVIEW.md](docs/OVERVIEW.md), [docs/AUDIO_FINDINGS.md](docs/AUDIO_FINDINGS.md)
* Setup console, BlueDV settings (Japanese): [README_WIFI.md](README_WIFI.md)
* Protocol, private commands (F0–F4; F4 = link and input-level counters): [docs/PROTOCOL.md](docs/PROTOCOL.md)
* Testing: [docs/TESTING.md](docs/TESTING.md), tools in `tools/audio_test/` (`stats.py` reads F4 over COM or UDP)
* Changes per flashed version: [CHANGELOG.md](CHANGELOG.md) (tags `s3-YYYY.MM.DD`)

## Build (ESP-IDF 5.1.6, Windows)
The `sdkconfig` of this branch targets the ESP32-S3:
```
.\idf_env.bat build
.\idf_env.bat -p COM5 flash
```
In Wi-Fi mode the console shows a line each time GPIO 4 is connected or released, and `status` prints its level —
handy to find the pin on an unfamiliar board.

## Notes
* Opening the built-in USB COM port with **DTR off and RTS on** resets the S3 (USB Serial/JTAG reset logic).
  BlueDV does not do this.
* Local changes to mbelib-neo: as in the `esp32` line (see its README).

## License / Patent notice
GPL-2.0-or-later ([LICENSE](LICENSE)); mbelib-neo GPL-2.0-or-later. AMBE is covered by DVSI patents in some
jurisdictions — for experimentation; check your local law before use or distribution.
