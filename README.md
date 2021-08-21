# Uxn


An assembler and emulator for the [Uxn stack-machine](https://wiki.xxiivv.com/site/uxn.html), written in ANSI C.


---
## Build

### Linux

To build the Uxn emulator, you must have [SDL2](https://wiki.libsdl.org/).

```sh
./build.sh
	--debug # Add debug flags to compiler
```

### Plan 9

To build the Uxn emulator on [9front](http://9front.org/), via [npe](https://git.sr.ht/~ft/npe):

```rc
mk
```

If the build fails on 9front because of missing headers or functions, try again after `rm -r /sys/include/npe`.

### Windows

Uxn can be built on Windows with [MSYS2](https://www.msys2.org/). Install by downloading from their website or with Chocolatey with `choco install msys2`. In the MSYS shell, type:

```sh
pacman -S git mingw-w64-x86_64-gcc mingw64/mingw-w64-x86_64-SDL2
export PATH="${PATH}:/mingw64/bin"
git clone https://git.sr.ht/~rabbits/uxn
cd uxn
./build.sh
```


---
## Getting Started

Begin by building the assembler and emulator by running the build script. The assembler(`uxnasm`) and emulator(`uxnemu`) are created in the `/bin` folder.

```
./build.sh
```

### Assembler

The following command will create an Uxn-compatible rom from an [uxntal file](https://wiki.xxiivv.com/site/uxntal.html), point to a different .tal file in `/projects` to assemble a different rom.

```
bin/uxnasm projects/examples/demos/life.tal bin/life.rom
```

### Emulator

To start the rom, point the emulator to the newly created rom:

```
bin/uxnemu bin/life.rom
```

You can also use the emulator without graphics by using `uxncli`. You can find additional roms [here](https://sr.ht/~rabbits/uxn/sources).

### I/O

You can send events from Uxn to another application, or another instance of uxn, with the Unix pipe. For a companion application that translates notes data into midi, see the [shim](https://git.sr.ht/~rabbits/shim).

```
uxnemu orca.rom | shim
```


---
## Emulator Controls

- `F1` toggle zoom
- `F2` toggle debug
- `F3` capture screen


---
## Need a hand?

Find us in `#uxn`, on irc.esper.net


---
## Flashing ESP32 Hardware

Compile and flash ESP32 hardware using [platformio](https://platformio.org/).

The steps to compile a `.tal` (e.g. run `build.sh`) into a ROM, flash firmware, and flash ROM, are easily done by executing `flash.sh` with the appropriate environment flag.

| Board              | Command                         |
|--------------------|---------------------------------|
| ESP WROVER Kit     | `./flash.sh esp-wrover-kit`     |
| M5Stack Core ESP32 | `./flash.sh m5stack-core-esp32` |

Otherwise, you can run the tasks individually.

**ESP WROVER Kit**

| Task       | Command                                     |
|------------|---------------------------------------------|
| Compile    | `pio run -e esp-wrover-kit`                 |
| Flash      | `pio run -e esp-wrover-kit -t upload`       |
| Flash ROM  | `pio run -e esp-wrover-kit -t uploadfs`     |


**M5Stack and M5Core ESP32**

| Task       | Command                                     |
|------------|---------------------------------------------|
| Compile    | `pio run -e m5stack-core-esp32`             |
| Flash      | `pio run -e m5stack-core-esp32 -t upload`   |
| Flash ROM  | `pio run -e m5stack-core-esp32 -t uploadfs` |


