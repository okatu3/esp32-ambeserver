# ESP32 AMBE3000 (DV3000) compatible vocoder over USB, plain mbelib-neo — `usb-esp32` line

Reference line: an ESP32 DevKit acts as a **DV3000 on a serial port** (the board's USB-UART bridge, **460800 bps**),
with the **published mbelib-neo, unmodified**. No Wi-Fi, no console, no logs on the link. Only two things are added
in front of the encoder, both in `main/dv3000_protocol.c`:

1. a **noise gate** (background noise is otherwise encoded as voice: a warble between words), and
2. **onset pitch priming** — at each word start the encoder is reset and its pitch memory is set with dry runs on a
   tone at the period we measure, because cold it takes ~2 x the first formant for the pitch and then can only glide
   down 25 % per frame (a chirp at every word start). See [docs/AUDIO_FINDINGS.md](docs/AUDIO_FINDINGS.md) 1.2.

公開版の mbelib-neo をそのまま使い、USB だけでつなぐ、比較の基準のための系統です。

* Overview and open problems: [docs/OVERVIEW.md](docs/OVERVIEW.md), [docs/AUDIO_FINDINGS.md](docs/AUDIO_FINDINGS.md)
* Protocol: [docs/PROTOCOL.md](docs/PROTOCOL.md)
* Testing and scoring tools: [docs/TESTING.md](docs/TESTING.md), `tools/audio_test/`
* Changes: [CHANGELOG.md](CHANGELOG.md) (tags `usb-esp32-YYYY.MM.DD`)

## Build (ESP-IDF 5.1.6, Windows)
```
.\idf_env.bat build
.\idf_env.bat -p COM9 flash
```

## BlueDV
Setup → AMBE: serial DV3000 on the board's COM port, 460800 bps.

## Notes
* The DevKit's auto-reset circuit holds the ESP32 in reset while **RTS alone** is asserted; after a reset the ROM
  prints ~700 bytes of boot text at another baud rate before the firmware starts (it cannot be disabled in software).
* `components/mbelib_neo/vendor/mbelib-neo` is upstream commit `27df6dc`; the encoder file is byte-identical to
  upstream. The only local change is the `MBE_THREAD_LOCAL` guard in `src/internal/mbe_compiler.h` (thread-local
  storage is copied into every task's stack on ESP-IDF and made the chip reboot at start-up).

## License / Patent notice
GPL-2.0-or-later ([LICENSE](LICENSE)); mbelib-neo GPL-2.0-or-later. AMBE is covered by DVSI patents in some
jurisdictions — for experimentation; check your local law before use or distribution.
