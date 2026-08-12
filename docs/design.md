# ardio — Design

**Date:** 2026-08-12
**Status:** Approved, ready for implementation planning

## Summary

`ardio` is a cross-platform command-line tool for building, flashing, monitoring, and
emulating Arduino boards. It is a single native binary written in C++ (with C and
assembly where they earn their place), built on a reusable core library, `libardio`.

`ardio` implements every bootloader protocol itself. Nothing goes over the serial wire
that `ardio` did not construct. It does not depend on `arduino-cli`, `avrdude`,
`esptool`, or `bossac`.

`ardio` also contains a cycle-accurate ATmega328P emulator, so sketches can be built and
run by users who do not own the hardware.

## Constraints

**`ardio` is a public, standalone project.** It contains no third-party or privately-held
code, and depends on no external toolchain of its own. Everything in this repository is
original work written for `ardio`. It stands on its own.

**macOS is the first platform.** Linux and Windows follow. The platform layer is designed
for all three from the start so later ports fill in interfaces rather than restructure
the codebase.

**No silent downloads.** `ardio` never fetches a toolchain without asking first.

## Ownership Split

`ardio` owns the wire and the machine. It borrows only target code generation, because
shipping a public tool that depends on a private compiler is not an option, and writing
an AVR compiler backend is a separate multi-month project.

| Layer | Owner |
|---|---|
| Port enumeration, board identification by USB VID/PID | `ardio` (IOKit / libudev / SetupAPI) |
| Serial I/O, DTR/RTS reset sequencing, baud control | `ardio` (termios / Win32 `SetCommState`) |
| Bootloader protocols (STK500v1/v2, ESP ROM, SAM-BA, UF2) | `ardio`, implemented from scratch |
| AVR instruction set and peripheral emulation | `ardio`, implemented from scratch |
| Sketch → `.hex` / `.elf` code generation | `avr-gcc` / `xtensa-gcc`, located via search path |
| Arduino core libraries (`Serial`, `digitalWrite`, …) | Vendor cores, located via search path |

## Architecture

```
libardio/
  platform/     port enumeration, serial I/O, USB VID/PID
  toolchain/    search path resolution, config parsing
  build/        sketch discovery, compiler invocation, image output
  protocol/     stk500v1 first; esp_rom, sam_ba, uf2 later
  emu/          AVR core: decoder, execution, cycle timing, SRAM/flash/EEPROM
  emu/periph/   USART, GPIO, timers first; ADC, PWM, SPI, I2C later
  emu/board/    virtual board: devices wired to pins from ardio.toml
ardio/          CLI — a thin shell over libardio
```

The boundary that matters most: `protocol/` and `emu/` consume a single shared board
description model — pin map, fuses, memory layout, USB VID/PID, bootloader parameters.
The emulated Nano and the physical Nano are described once, so they cannot drift apart.

`libardio` is a real library with a public header, not an implementation detail. The CLI,
and later the GUI, are consumers of it.

## Toolchain Search Path

Toolchains and cores are located by walking an ordered list of roots until the needed
tool is found. This generalizes the `/usr/local/include` model to build tools.

Default order:

```
~/.ardio/tools/          ardio's own, if it has fetched anything
/usr/local/ardio/        system-wide install
~/.arduino-create/       Arduino Web Editor agent cache
~/Library/Arduino15/     Arduino IDE cache  (Linux: ~/.arduino15; Windows: %LOCALAPPDATA%\Arduino15)
$PATH                    last resort
```

Every entry is overridable in `~/.ardio/config.toml`. `ardio doctor` reports which root
each tool resolved from and what is missing. If a required tool is absent, `ardio` offers
to fetch it into `~/.ardio/tools/` and does nothing until the user agrees.

On a machine with the Arduino Web Editor agent installed, this resolves with zero
downloads.

## Command Surface

```
ardio push [sketch]        build + upload, optionally then monitor
ardio build [sketch]       compile only
ardio flash <file.hex>     upload a prebuilt image, no compile
ardio monitor [-b BAUD]    serial monitor
ardio emulate [sketch]     build and run in the emulator
ardio ports                list ports with VID:PID and identified board
ardio boards               supported targets and toolchain availability
ardio toolchain <...>      list / fetch / pin
ardio doctor               diagnose resolution, permissions, drivers
```

Zero configuration is the default path. Bare `ardio push` finds the single connected
board, identifies it by USB VID/PID, selects the protocol, and uploads. `--port`,
`--board`, and `--baud` override auto-detection. An optional per-project `ardio.toml`
pins them, and later describes virtual hardware for the emulator.

## Sketch Input

`ardio` accepts C++ (the priority) and C. `.ino` files are treated as C++ with the
standard Arduino preamble applied. No other source languages in this design.

## Error Handling

Auto-detection is a convenience that must fail legibly, never silently:

- **No port found** — say so, list what *was* enumerated, and name the likely cause
  (board unplugged, or a missing USB-serial driver).
- **Multiple boards** — never guess. List them with VID:PID and require `--port`.
- **Unknown VID:PID** — upload is still possible; require an explicit `--board` rather
  than assuming a target.
- **Bootloader baud ambiguity** — Nano clones split between 115200 ("new") and 57600
  ("old") bootloaders. `ardio` tries both automatically and reports which succeeded. The
  user is never expected to know which clone they own.
- **Missing toolchain** — name the tool, name every root searched, and offer to fetch.
- **Upload failure** — report the protocol stage that failed (sync, page write, verify),
  not a generic error.

## Testing

- **Protocol encoders** are pure functions from a command and payload to bytes. Tested
  against captured known-good STK500v1 transcripts, with no hardware involved.
- **The AVR core** is tested per instruction: a small program, a known start state, an
  asserted register/flag/cycle-count end state. This is the bulk of the emulator's test
  suite and is fully deterministic.
- **The emulator validates the uploader.** A sketch compiled by `ardio` and run in
  `ardio emulate` exercises the same board model the real upload path uses.
- **Platform code** (IOKit enumeration, termios) is verified against real hardware and
  kept behind a narrow interface so the rest of the codebase tests without a board.

## Phasing

Each phase produces a working tool, not a step toward one. Each phase gets its own
implementation plan; the first plan covers Phase 1 only.

**Phase 1 — Real tool, real board.**
`libardio` skeleton, macOS port enumeration and board identification, config and search
path, `avr-gcc` build, native STK500v1 upload, serial monitor. Target: ATmega328P Nano
over CH340 (`0x1A86:0x7523`), the CrunchLabs Hack Pack board.

**Phase 2 — The chip.**
Full ATmega328P instruction set, cycle-accurate, with USART, GPIO, and timers sufficient
for correct `delay()` and `millis()`. `ardio emulate` runs a real `.hex` and prints
`Serial` output. This is the phase that makes `ardio` usable by people who do not own a
board.

**Phase 3 — The board.**
ADC, PWM, SPI, I2C, and timer fidelity sufficient for the Servo library. Scriptable
virtual devices declared in `ardio.toml`, with a TUI showing live pin state.

**Phase 4 — Everything else.**
RP2040 (UF2), ESP32 (ROM loader with DTR/RTS reset), SAMD (SAM-BA), STK500v2 for Mega.
Linux and Windows platform layers.

**Later, out of scope for this spec.**
A macOS `.app` with a GUI, built on `libardio`.

## Explicitly Out of Scope

- An AVR compiler backend. `ardio` invokes `avr-gcc`.
- Any dependency on `arduino-cli`, `avrdude`, `esptool`, or `bossac`.
- Source languages beyond C and C++.
- The GUI application, until the CLI and emulator are complete.
