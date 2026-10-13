# ESP32 AMBE3000 (DV3000) compatible server over Wi-Fi

ESP32 firmware that answers the DV3000 / AMBE-3000R serial protocol (`0x61 | len | type | payload`) over
**UDP and TCP port 2460** (BlueDV "AMBE server" compatible), with a serial setup console for Wi-Fi settings.
D-STAR (AMBE 3600x2400, 2400 bps voice + 1200 bps FEC) only. Codec: mbelib-neo (vendored, modified).

Full documentation (Japanese): [README_WIFI.md](README_WIFI.md)

## Build (ESP-IDF 5.1.6, Windows)
```
.\idf_env.bat build
.\idf_env.bat -p COM12 flash
```
Edit the ESP-IDF and Python environment paths in `idf_env.bat` for your PC.

ESP32-S3 (separate build directory; setup console on the built-in USB Serial/JTAG port):
```
.\idf_env.bat -B build_s3 -D SDKCONFIG=sdkconfig.s3 -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.wifi" set-target esp32s3
.\idf_env.bat -B build_s3 -D SDKCONFIG=sdkconfig.s3 -p COM5 flash
```

## License
* Firmware in `main/`, `tools/`: GPL-2.0-or-later (see [LICENSE](LICENSE)).
* `components/mbelib_neo/vendor/mbelib-neo`: mbelib-neo, GPL-2.0-or-later (see its LICENSE / LICENSES).
  The encoder (`src/ambe/ambe3600x2400_enc.c`) and `src/internal/mbe_compiler.h` contain local
  modifications for this project (documented in README_WIFI.md).
* Any firmware binary built from this repository is distributed under the GPL.

## Patent notice
The AMBE vocoder is covered by patents held by Digital Voice Systems, Inc. (DVSI) in some jurisdictions.
This software implementation is provided for experimentation; check the legal situation where you live
before using or distributing it.
