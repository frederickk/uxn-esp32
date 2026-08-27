# midi-bridge

A tiny sketch for an ATmega32U4 board (Arduino Micro, Leonardo, or a
compatible clone) that turns it into a USB-MIDI adapter for the M5Stack.

## Why this exists

The M5Stack Core's ESP32 has no native USB device controller — its USB
port is only a USB-to-serial bridge chip used for flashing, and it can
never present itself as a class-compliant USB-MIDI device on its own.

The ESP32 can already send MIDI over BLE (see
`src/drivers/midi/m5stack_ble_midi.cpp`), but for a wired, driverless
connection that any DAW or MIDI host recognizes instantly, the ESP32
instead sends raw MIDI bytes over a plain UART to this board, which
_does_ have real USB hardware and relays them on via the `MIDIUSB`
library.

## Wiring

Two wires only. The Micro/Leonardo gets its own power from the USB host
it's plugged into — don't wire 5V/3V3/BAT from the M5Stack at all.

```
        M5Stack Core                          Arduino Micro / Leonardo
     (side GPIO header)                             (ATmega32U4)
    +--------------------+                    +--------------------+
    |                    |                    |                    |
    |   GPIO17 (TX2) o---+--------------------+---o  RX1  (D0)      |
    |                    |                    |                    |
    |        GND     o---+--------------------+---o  GND            |
    |                    |                    |                    |
    +--------------------+                    +----------+---------+
                                                           |
                                                           | USB
                                                           v
                                                +----------------------+
                                                |   Computer / DAW /   |
                                                |     MIDI host        |
                                                +----------------------+
```

**Do not** wire the Micro/Leonardo's TX back to the M5Stack's RX
(GPIO16). This link is one-way by design: MIDI output doesn't need
anything to come back, and a genuine Arduino Micro/Leonardo runs at 5V
logic — its TX would feed 5V into the ESP32's RX pin, which is
3.3V-only and not reliably 5V-tolerant. Skipping that wire avoids the
issue entirely rather than needing a level shifter.

If your board is a 3.3V "Pro Micro" variant instead of a genuine 5V
Micro/Leonardo, the voltage concern doesn't apply, but the link still
only needs to run one way for this use case.

## Protocol

The ESP32 side (`src/drivers/midi/m5stack_serial_midi.cpp`) writes
plain 3-byte MIDI 1.0 messages — status byte, note, velocity — at
31250 baud, the same baud rate used by real MIDI DIN hardware. This
sketch resyncs to the next status byte (high bit set) and forwards
each complete 3-byte message via `MidiUSB.sendMIDI()`, so it's
self-healing if a byte is ever dropped on the wire.

## Build & flash

```
cd midi-bridge
pio run -t upload
```

This is a separate PlatformIO project from the main M5Stack firmware —
different microcontroller architecture (AVR, not ESP32), so it needs
its own `platformio.ini` and build.
