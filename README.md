# Uxn-Esp32

A port of [Uxn](https://wiki.xxiivv.com/site/uxn.html)/[Varvara](https://wiki.xxiivv.com/site/varvara.html) to the ESP32, forked from [max22/uxn-esp32](https://github.com/max22-/uxn-esp32), targeting the **M5Stack Core + Faces QWERTY Keyboard** ("Faces Pocket"). `src/main.cpp` is a from-scratch, self-contained port against the current Uxn core and device layout (see `git.sr.ht/~rabbits/uxn-m5`) — no submodule, no vendored copy. `m5stack-core-esp32` is the only environment.

# Hardware

- M5Stack Core (ESP32, 320x240 TFT)
- M5 Faces QWERTY Keyboard (I2C, SKU A003). Its firmware combines Alt+letter into one byte in hardware; this port decodes that straight to Ctrl+letter, matching Orca's shortcuts (Ctrl+H = operator guide, Ctrl+,/Ctrl+. = playback speed).
- Front buttons A/B/C mirror the same: A = Ctrl+. , B = Ctrl+H, C = Ctrl+,
- Optional: ATmega32U4 board for wired USB-MIDI (see [`midi-bridge/`](midi-bridge/README.md)) — not required, BLE MIDI works standalone.

# Build

```
git clone https://github.com/frederickk/uxn-esp32 && cd uxn-esp32
cp src/wifi_credentials.sample.h src/wifi_credentials.h  # edit with real (or dummy) SSID/password -- required to compile
pio run
pio run -t uploadfs   # uploads data/ (ROMs)
pio run -t upload
```

`M5Unified` handles display/board init automatically — no TFT_eSPI config needed. ROMs live in `data/`; `data/orca.rom` is the current prebuilt [Orca](https://wiki.xxiivv.com/site/orca.html).

# Devices implemented

System, Console, Screen, Audio (4-channel ADSR), Controller (Faces keyboard), File (SPIFFS, flat namespace — no dir listing), DateTime. No Mouse (no touch/pointer input on this hardware).

# MIDI

Not a dedicated device — Orca sends raw MIDI bytes via Console/write, which this port decodes and forwards to **both** transports at once:

- **BLE**: advertises as `uxn-esp32`, no pairing PIN. macOS: Audio MIDI Setup → Window → Show MIDI Studio → Bluetooth icon → Connect. iOS: any app with a "Bluetooth MIDI Devices" panel (e.g. GarageBand).
- **USB**: build/flash [`midi-bridge/`](midi-bridge/README.md) onto an ATmega32U4 board, wire GPIO17 → its RX1 and GND → GND (one-way, two wires), plug it into your computer — shows up as class-compliant USB-MIDI, no drivers.

# WiFi / NTP

Used once at boot to set the clock for DateTime, nothing else — the radio is fully powered off again once the sync attempt finishes (see Limitations).

1. `src/wifi_credentials.h` (gitignored) needs real credentials to actually sync; placeholder values just make the boot-time attempt time out after 10s and continue.
2. Adjust `WIFI_GMT_OFFSET_SEC` / `WIFI_DAYLIGHT_OFFSET_SEC` in `src/main.cpp`'s WiFi section for your timezone (default UTC).
3. No reconnect-later — a failed/skipped sync means DateTime just holds the ESP32's software clock until next power cycle.

# Opening a file

No auto-open-on-boot — confirmed against current `uxncli.c` and `orca.tal` that even real `uxncli orca.rom file.orca` doesn't do this anymore (that was the old Orca's behavior). Current Orca opens files through its own mouse-driven UI (Ctrl+O), which needs a Mouse device and directory listing this port doesn't implement yet.

# Updating the Uxn core

No submodule to `git pull` — this is a hand port. Two independent things can go stale:

- **ROM**: download a fresh `orca.rom` from `git.sr.ht/~rabbits/orca-toy`, drop it in `data/`, `pio run -t uploadfs`.
- **Core/device dispatch** (`uxn_eval`, `emu_dei`/`emu_deo`): re-diff `git.sr.ht/~rabbits/uxn-m5`'s `src/uxn-m5.c` and the `.tal` device tables (`orca.tal`'s `|00 @System ...` header is the authoritative port map) against `main.cpp`, port by hand. No automated drift detection.

# Limitations

- WiFi and BLE can't stay resident together on this hardware — one radio, separate memory pools, not enough heap for both alongside M5GFX/I2S audio (confirmed on hardware: WiFi init fails if BLE already started). Hence WiFi-then-fully-off before BLE starts.
- No Mouse device.
