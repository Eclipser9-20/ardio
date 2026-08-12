# ardio

A command-line tool for building, flashing, monitoring, and (eventually) emulating
Arduino boards.

`ardio` implements the bootloader protocols itself. Nothing goes over the serial wire
that `ardio` did not construct — there is no `arduino-cli`, `avrdude`, `esptool`, or
`bossac` under the hood, and no third-party libraries anywhere in the build.

It also ships its own **AVR assembler**, so `.S` sources are built entirely in-process:

```sh
ardio push examples/blink.S     # no external toolchain required
```

C and C++ sketches still need an AVR compiler, which `ardio toolchain` can fetch or
build for you. Assembly needs nothing at all.

**Status:** early. The AVR upload path (ATmega328P over STK500v1) works end to end.
See [Roadmap](#roadmap).

## Install

```sh
git clone <this-repo> && cd ardio
./install.sh
```

Requires CMake 3.20+ and a C++20 compiler. Nothing else.

```sh
./install.sh --prefix ~/.local   # install somewhere specific
./install.sh --uninstall         # remove it again
```

Then check your setup:

```sh
ardio doctor
```

## Usage

```
ardio push [sketch]      build and upload
ardio build [sketch]     compile only
ardio flash <file.hex>   upload a prebuilt image
ardio monitor            open the serial monitor
ardio ports              list serial ports
ardio boards             list supported boards
ardio doctor             diagnose toolchains and ports

  --port <device>    serial port (default: auto-detect)
  --board <id>       board id (default: from USB id)
  --baud <n>         monitor baud rate
  -m, --monitor      open the monitor after a successful push
```

The common case needs no arguments and no configuration:

```sh
ardio push blink.cpp -m
```

`ardio` finds the connected board, identifies it by USB vendor/product ID, picks the
protocol, uploads, and drops into the serial monitor.

### Things it handles so you don't have to

- **Bootloader baud ambiguity.** Nano clones ship with either the "new" 115200 bootloader
  or the "old" 57600 one. `ardio` tries both and tells you which worked.
- **Ambiguous ports.** With two boards plugged in, `ardio` refuses to guess — it lists
  them and asks for `--port`.
- **Missing toolchains.** Errors name the tool *and* every directory that was searched.
- **Failed uploads.** Errors name the protocol stage that failed — sync, signature,
  write — rather than reporting a generic failure.

## Configuration

Optional, at `~/.ardio/config.toml`. Every key has a working default.

```toml
default_board = "nano"
default_port  = "/dev/cu.usbserial-110"
monitor_baud  = 9600

# Where to look for compilers and uploaders, in order.
# "$PATH" is a sentinel meaning "search the PATH environment variable".
search_roots = ["~/.ardio/tools", "/usr/local/ardio", "$PATH"]
```

### Toolchain search path

Compiling a sketch needs a target compiler (`avr-g++`). `ardio` locates it by walking a
list of roots in order, stopping at the first hit — the same idea as `/usr/local/include`,
applied to build tools. The default order is:

```
~/.ardio/tools/          ardio's own tool directory
/usr/local/ardio/        system-wide install
~/.arduino-create/       Arduino Web Editor agent cache
~/Library/Arduino15/     Arduino IDE cache  (Linux: ~/.arduino15)
$PATH                    last resort
```

`ardio doctor` shows which root each tool resolved from. `ardio` never downloads anything
without asking first.

## Supported boards

| Board | MCU | Protocol | Status |
|---|---|---|---|
| Arduino Nano / Uno clones | ATmega328P | STK500v1 | working |
| Pi Pico | RP2040 | UF2 | planned |
| ESP32 / ESP8266 | Xtensa / RISC-V | ESP ROM loader | planned |
| Zero, MKR, Nano 33 IoT | SAMD | SAM-BA | planned |

## Platform support

| Platform | Status |
|---|---|
| macOS | supported |
| Linux | builds; serial layer not implemented yet |
| Windows | builds; serial layer not implemented yet |

On unsupported platforms the hardware-independent commands (`boards`, `build`, `doctor`)
work normally; anything touching a serial port reports the limitation.

## Roadmap

1. **AVR upload** — ATmega328P over STK500v1. *Done.*
2. **The chip** — a cycle-accurate ATmega328P emulator, so `ardio emulate` can run a real
   `.hex` and print `Serial` output without any hardware.
3. **The board** — ADC, PWM, SPI, I2C, and scriptable virtual devices, with a TUI showing
   live pin state.
4. **Everything else** — RP2040, ESP32, SAMD; Linux and Windows serial layers.

## Building from source

```sh
cmake -S . -B build
cmake --build build
./build/ardio_tests
```

The test suite runs entirely without hardware: the protocol encoders are pure functions
tested against known-good byte sequences, and uploads are tested against a scripted
in-memory bootloader.

## Licence

GPLv3. See [LICENSE](LICENSE).

## Library

The CLI is a thin shell over `libardio`, which is installed alongside it. The board
description model, protocol encoders, and HEX parser are all usable directly.

```cpp
#include <ardio/board.h>
#include <ardio/protocol/programmer.h>
```
