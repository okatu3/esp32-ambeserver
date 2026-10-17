# ESP32 AMBE3000 (DV3000) compatible vocoder for D-STAR

ESP32 firmware that behaves like a DVSI **DV3000 / AMBE-3000R** for **D-STAR** (AMBE 3600x2400), using the open-source
codec **mbelib-neo**. PC software such as **BlueDV** uses it over **Wi-Fi** (UDP/TCP 2460, "AMBE server") or **USB**
(serial, 460800 bps).

**Status:** transmit and receive work end to end. Receive audio is good; **transmit audio (the mbelib-neo encoder) is
still clearly worse than a real DV3000** — pitch octave errors, onset chirps, voicing. Help is very welcome:
see [docs/AUDIO_FINDINGS.md](docs/AUDIO_FINDINGS.md) (section 3, *Help wanted*).

ESP32 を DVSI 社の AMBE チップ（DV3000）の代わりにする、D-STAR 用のファームウェアです。BlueDV から Wi-Fi または USB で
使えます。送受信はできますが、**送信の音質（mbelib-neo の符号化器）が本物の DV3000 に及ばず、改良に協力してくれる方を
探しています。**

## Branches / 開発系統
| Branch | Board | Link to the PC | Audio processing |
|---|---|---|---|
| [`esp32`](../../tree/esp32) | ESP32 DevKit | Wi-Fi (UDP/TCP 2460) | mbelib-neo with a modified encoder + pre-processing (HPF, gate, noise reduction, auto EQ) |
| [`s3`](../../tree/s3) | ESP32-S3 (single built-in USB) | Wi-Fi, or USB (GPIO 4 to GND at boot) | same as `esp32` |
| [`usb-esp32`](../../tree/usb-esp32) | ESP32 DevKit | USB only (USB-UART, 460800 bps) | **unmodified mbelib-neo** + noise gate + onset pitch priming (reference line) |

`main` is this landing page only; the firmware is in the branches above. Each version that was flashed and tested is
tagged `<line>-YYYY.MM.DD` and described in that branch's `CHANGELOG.md`.

## Documents / 資料
* [docs/OVERVIEW.md](docs/OVERVIEW.md) — what it is, the three lines, build and flash
* [docs/PROTOCOL.md](docs/PROTOCOL.md) — DV3000 packets implemented, private tuning commands
* [docs/AUDIO_FINDINGS.md](docs/AUDIO_FINDINGS.md) — measurements, causes found, what failed, **open problems**
* [docs/TESTING.md](docs/TESTING.md) — how we test (Peanut A/B by ear, captures, `tools/audio_test/` scoring)
* [docs/BlueDV_D-STAR_hosts_fix.pdf](docs/BlueDV_D-STAR_hosts_fix.pdf) — BlueDV D-STAR host list workaround

## License / Patent notice
* Firmware and tools: GPL-2.0-or-later ([LICENSE](LICENSE)). mbelib-neo: GPL-2.0-or-later.
* The AMBE vocoder is covered by DVSI patents in some jurisdictions. This is provided for experimentation; check the
  legal situation where you live before using or distributing it.
