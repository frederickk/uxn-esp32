# Uxn-Esp32

A port of the [Uxn](https://wiki.xxiivv.com/site/uxn.html) virtual machine and [Varvara](https://wiki.xxiivv.com/site/varvara.html) environment to the ESP32 platform, forked from [max22/uxn-esp32](https://github.com/max22-/uxn-esp32).

This fork targets the **M5Stack Core + Faces QWERTY Keyboard** ("Faces Pocket") specifically, with `main.cpp` rewritten from scratch against the _current_ Uxn core and device layout (see `git.sr.ht/~rabbits/uxn-m5`). `m5stack-core-esp32` is the only environment, and the default.

# Hardware

- M5Stack Core (ESP32, 320x240 TFT)
- M5 Faces QWERTY Keyboard module (I2C, SKU A003) — reports one key at a time. Its firmware combines Alt+letter into a single hardware-reported byte (see `github.com/m5stack/M5Faces`, `src/M5Faces_Keyboard3.hpp`); this port decodes those bytes directly to Ctrl+letter, matching Orca's own Ctrl shortcuts (Ctrl+H toggles the operator guide, Ctrl+, / Ctrl+. change playback speed, etc).
- M5Stack Core's three front buttons (A/B/C, left to right) mirror the most common shortcuts as a keyboard-free alternative: A = Ctrl+. (speed down), B = Ctrl+H (toggle guide), C = Ctrl+, (speed up).
- Optional: an ATmega32U4 board (Arduino Micro/Leonardo-class) wired one-way from GPIO17 for wired USB-MIDI output — see [`midi-bridge/README.md`](midi-bridge/README.md). Not required; BLE MIDI works standalone.

# How to build it

Install [PlatformIO Core](https://platformio.org/install/cli) if you want to use the command line only. It is also available as a plugin for several IDEs/editors (Emacs, vim, VSCode, Atom, etc).

```
git clone https://github.com/frederickk/uxn-esp32
cd uxn-esp32
git checkout m5-faces-pocket-modern-uxn
pio run
```

No `User_Setup.h`/TFT_eSPI configuration is needed — `M5Unified` handles display/board init automatically. WiFi credentials are required to compile at all (even if you don't want WiFi) — see [WiFi / NTP time sync](#wifi--ntp-time-sync) below before your first build.

Then upload everything to the device:

```
pio run -t uploadfs
pio run -t upload
```

ROMs must be in the `data/` folder (they're uploaded with `pio run -t uploadfs`). `data/orca.rom` is the current prebuilt [Orca](https://wiki.xxiivv.com/site/orca.html) binary.

# Devices implemented

System, Console, Screen, Audio (4-channel ADSR), Controller (Faces keyboard), File (SPIFFS-backed, flat namespace — no directory listing), DateTime. No Mouse device — the Faces Pocket has no touch/pointer input.

MIDI is not a dedicated device in the current Varvara spec. Orca (and the reference `shim` relay it targets) sends raw MIDI bytes out through Console/write instead; this port replicates that framing and routes decoded note on/off messages to both BLE MIDI (advertises as `"uxn-esp32"`) and, if the optional bridge board is attached, wired USB-MIDI.

## Opening a file

There's no auto-open-on-boot. Confirmed against both the current `uxncli.c` and `orca.tal` sources: even a real `uxncli orca.rom yourfile.orca` invocation doesn't auto-open the second argument anymore — that was the old (frozen-submodule-era) Orca's behavior, not the current one. Current Orca opens files through its own mouse-driven UI (Ctrl+O), which needs a Mouse device and directory-listing support neither of which this port implements yet (SPIFFS has no real directories to list in the first place). Typing over the serial monitor still works as plain console input — `loop()` forwards it into Console/read — but Orca treats that as grid-insert typing, not a path to open.

# MIDI

Orca (and any other ROM that sends MIDI the same way) reaches both transports simultaneously — nothing to choose between, both just work if connected.

## BLE

The device advertises as **`uxn-esp32`** as soon as it boots — no pairing PIN, no app required on the M5Stack side. To connect:

- **macOS**: open **Audio MIDI Setup** (Applications → Utilities) → menu bar **Window → Show MIDI Studio** → double-click the **Bluetooth** icon → **Connect** next to `uxn-esp32`. Once connected it behaves like any other Core MIDI source/destination — visible to GarageBand, Logic, Ableton, a `MIDI Monitor`-style utility, etc.
- **iOS**: any app with a "Bluetooth MIDI Devices" panel (GarageBand has one under the wrench icon) will find and connect to it the same way.
- **Windows/Android**: needs a BLE MIDI-capable app or driver, since neither has first-class OS-level BLE MIDI the way macOS/iOS do — search for "BLE MIDI" plus your platform.

If nothing shows up: confirm the device actually booted past `midi_ble_init()` (check the serial log), and that you're not still holding an old pairing from a previous session — BLE MIDI reconnects are sometimes flaky if the host cached a stale connection; forget and re-pair if a previously-working connection stops responding.

## Wired USB

For a driverless, instant-recognize connection to any DAW, build the [`midi-bridge`](midi-bridge/README.md) sketch onto a separate ATmega32U4 board (Arduino Micro/Leonardo-class) and wire it one-way from the M5Stack's GPIO17. Full wiring diagram, protocol details, and build steps are in that README — short version:

```
cd midi-bridge
pio run -t upload
```

Then wire GPIO17 (TX2) → the bridge board's RX1, and GND → GND (two wires only, nothing else). Plug the bridge board into your computer via its own USB port — it shows up immediately as a class-compliant USB-MIDI device, no drivers needed.

# WiFi / NTP time sync

WiFi exists solely to set the ESP32's system clock via NTP for the DateTime device, once at boot; nothing else in this port needs a network connection, and the radio is fully powered off again once the sync attempt finishes (see [Limitations](#limitations) for why).

1. Copy `src/wifi_credentials.sample.h` to `src/wifi_credentials.h` (gitignored — never committed) and fill in your real SSID/password. This file is required to compile at all; if you don't want WiFi, create it anyway with placeholder values and the connection attempt will just time out after 10s.
2. Adjust `WIFI_GMT_OFFSET_SEC` / `WIFI_DAYLIGHT_OFFSET_SEC` near the top of the `WiFi` section in `src/main.cpp` for your timezone — both default to `0` (UTC), since a wrong guess would be worse than leaving them alone.
3. Build and flash normally. Watch the serial monitor on boot: `Connecting to "<ssid>"` followed by either `connected` (NTP sync attempted) or `failed, continuing without WiFi/NTP` (timed out, boot continues regardless).

There's no reconnect-later mechanism — if it fails at boot, DateTime just reflects the ESP32's software clock as-is until the next power cycle.

# Updating the Uxn core

This branch's `src/main.cpp` is a hand-adapted, self-contained port — there's no submodule or vendored copy to `git pull`, so "updating" means manually re-porting against current upstream sources when the ecosystem moves. Two different things can go stale independently:

- **The ROM** (`data/orca.rom`) — just download a fresh prebuilt binary from the current [Orca](https://wiki.xxiivv.com/site/orca.html) source (`git.sr.ht/~rabbits/orca-toy`) and drop it in `data/`, then `pio run -t uploadfs`. No code changes needed for this alone.
- **The core interpreter and device dispatch** (`uxn_eval()`, `emu_dei`/`emu_deo`, and everything in the numbered `@|Device` sections) — these were adapted from `git.sr.ht/~rabbits/uxn-m5`'s `src/uxn-m5.c`, the current reference M5-targeted port, not from any frozen/pinned version. If upstream changes the opcode set or device port layout again (it's happened before — the current core added `JCI`/`JMI`/`JSI` immediate-jump opcodes at bit patterns that collided with the older k/r/2 mode-flag encoding, a genuine breaking change, not just an addition), re-diff `uxn-m5.c` and the relevant `.tal` device tables (`orca.tal`'s `|00 @System ...` header block is the authoritative absolute-address device map) against what's here, and port by hand. There's no automated way to check this is current — the safest approach if you suspect drift is to clone `uxn-m5` and `orca-toy` fresh and diff their device tables against the addresses hardcoded throughout `main.cpp`'s `emu_dei`/`emu_deo`.

# Limitations

- WiFi only runs once at boot (to sync NTP), then is fully powered off before anything else initializes. ESP32 classic has one radio and separate memory pools for WiFi and BLE; there isn't enough heap for both to stay resident alongside M5GFX/I2S audio (confirmed on hardware: WiFi's own driver init fails outright if BLE has already started). If WiFi fails or times out, the DateTime device just reflects the ESP32's software clock from wherever it last was — expect it to read as the epoch on a cold boot with no WiFi.
- No Mouse device.
