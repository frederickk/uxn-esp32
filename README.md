# Uxn-Esp32

A port of the [Uxn](https://wiki.xxiivv.com/site/uxn.html) virtual machine and [Varvara](https://wiki.xxiivv.com/site/varvara.html) environment to the ESP32 platform, forked from [max22/uxn-esp32](https://github.com/max22-/uxn-esp32).

This fork targets the **M5Stack Core + Faces QWERTY Keyboard** ("Faces Pocket") specifically, with `main.cpp` rewritten from scratch against the _current_ Uxn core and device layout (see `git.sr.ht/~rabbits/uxn-m5`), rather than the frozen `uxn` submodule the rest of this repo still carries. The submodule and the `esp32dev` / `m5stack-core2` PlatformIO environments are leftovers from upstream and are **not maintained on this branch** — they don't build against the current `src/main.cpp` (missing lib_deps for `M5Unified`/`ESP32-BLE-MIDI`, and no M5 hardware to run against on `esp32dev` anyway). `m5stack-core-esp32` is the only supported target here, and is the default environment.

# Hardware

- M5Stack Core (ESP32, 320x240 TFT)
- M5 Faces QWERTY Keyboard module (I2C, SKU A003) — reports one key at a time; Alt is remapped as a sticky Ctrl toggle (press once to arm, press a key to send Ctrl+key and auto-disarm, or press Alt again to cancel)
- Optional: an ATmega32U4 board (Arduino Micro/Leonardo-class) wired one-way from GPIO17 for wired USB-MIDI output — see [`midi-bridge/README.md`](midi-bridge/README.md). Not required; BLE MIDI works standalone.

# How to build it

Install [PlatformIO Core](https://platformio.org/install/cli) if you want to use the command line only. It is also available as a plugin for several IDEs/editors (Emacs, vim, VSCode, Atom, etc).

```
git clone https://github.com/frederickk/uxn-esp32
cd uxn-esp32
git checkout m5-faces-pocket-modern-uxn
pio run
```

No `User_Setup.h`/TFT_eSPI configuration or WiFi credentials are needed — `M5Unified` handles display/board init automatically, and this branch doesn't use WiFi/NTP (see Limitations below).

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

There's no dedicated "open" device or keyboard shortcut. On boot, `open_on_boot` (in `src/main.cpp`) is typed character-by-character into the Console device, exactly like a second `argv` entry to `uxncli`/`uxnemu` — this is how Orca actually expects to receive a filename to open. You can also type a filename over the serial monitor at any time; `loop()` forwards it into Console/read the same way.

# Limitations

- No WiFi/NTP time sync. The DateTime device reflects the ESP32's internal software clock, which starts unset on every boot — expect it to read as the epoch, not the real time, unless something else calls `settimeofday()`.
- No Mouse device.
- `esp32dev` and `m5stack-core2` PlatformIO environments are present in `platformio.ini` but not buildable on this branch (see above).
