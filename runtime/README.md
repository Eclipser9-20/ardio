# ardio runtime headers

`runtime/include/` is the header set a sketch gets when it writes
`#include <Arduino.h>`. These are **ardio's own headers**, written against the
published Arduino language reference and library documentation. No Arduino core
source, and no third-party library source, is used or vendored here.

They are API-compatible, not implementation-compatible: a sketch that compiles
against the real Arduino headers should compile against these, but the machinery
behind the names is entirely ardio's.

Everything in this directory is a **declaration only**. The definitions are AVR
assembly, assembled in-process by ardio's own assembler and linked into the
sketch:

| Header | Runtime source | Symbols it binds to |
|---|---|---|
| `Arduino.h` | `runtime/core.S` | `pinMode`, `digitalWrite`, `digitalRead`, `delay`, `delayMicroseconds`, plus `analogRead`/`analogWrite`/`millis`/`micros` |
| `WString.h` | `runtime/string.S` | (no external symbols; the class is self-contained) |
| `Servo.h` | `runtime/servo.S` | `servo_attach`, `servo_write`, `servo_detach` |
| `Stepper.h` | `runtime/stepper.S` | `stepper_init`, `stepper_set_speed`, `stepper_step` |
| `Wire.h` | `runtime/wire.S` | `i2c_init`, `i2c_start`, `i2c_write`, `i2c_read`, `i2c_stop` |
| `LiquidCrystal_I2C.h` | `runtime/lcd.S` | `lcd_init`, `lcd_clear`, `lcd_set_cursor`, `lcd_write_char`, `lcd_backlight`, … |
| `ezButton.h` | `runtime/button.S` | `button_init`, `button_pressed` |

Anything that crosses into assembly is declared `extern "C"`, because the
assembler emits plain unmangled labels. The classes themselves are ordinary C++
wrappers over those calls.

## Target

ATmega328P (Arduino Uno / Nano) at 16 MHz. Pin numbers, the `A0`–`A7` numbering
space, and the timer assumptions in `Servo.h` are all specific to that part.

## Which sketches these support

The set was chosen to cover the shape of a typical device sketch: read some
inputs, drive some outputs, put text on a display.

- **Blink / GPIO** — `pinMode`, `digitalWrite`, `digitalRead`, `delay`.
- **Analog** — `analogRead` on `A0`–`A7`, `analogWrite` (PWM) on the PWM-capable
  digital pins, `map` and `constrain` for scaling.
- **Timing** — `millis`, `micros`, `delayMicroseconds`, non-blocking loops.
- **Buttons** — `ezButton`, debounced, with press/release edge queries.
- **Motion** — `Servo` for RC servos, `Stepper` for four-wire steppers.
- **Displays** — `LiquidCrystal_I2C` on a PCF8574 backpack, and `Wire` directly
  for other I2C devices.
- **Text** — the fixed-capacity `String` in `WString.h`.

## What is deliberately not implemented

These names are **absent**, not stubbed. A sketch that uses one will fail to
compile with an unknown-identifier error, which is the intended outcome — a
silent no-op stub on a microcontroller is worse than a build failure.

- **`Serial` and the whole `HardwareSerial` / `Print` / `Stream` family.**
  Nothing here prints to the host. `Serial.begin`, `Serial.print`,
  `Serial.available`, `Serial.read`, `SoftwareSerial` — none exist yet. This is
  the largest gap and the next one worth closing.
- **Floating point.** There is no `float`/`double` support in the sketch
  compiler's type model, so `sin`, `cos`, `sqrt`, `pow`, and float overloads of
  `map`, `abs`, `min`, `max` are not provided. The maths macros and `map()` are
  integer-only. `PI` and friends are defined as constants for source
  compatibility but cannot be used in arithmetic yet.
- **Interrupts.** `interrupts()` and `noInterrupts()` are declared, but
  `attachInterrupt` / `detachInterrupt` / `digitalPinToInterrupt` are not, and
  there is no ISR mechanism for sketch code.
- **`EEPROM`, `SPI`, `SD`.** No headers at all.
- **`Wire` peripheral (slave) mode.** `TwoWire` is controller-only: no
  `begin(address)`, `onReceive`, or `onRequest`.
- **`String` beyond a fixed 32 characters.** Appends truncate rather than grow;
  see the long comment at the top of `WString.h` for why. No `replace()`,
  `lastIndexOf()`, or `String(float, digits)`.
- **`LiquidCrystal_I2C` scrolling and direction** — `scrollDisplayLeft`,
  `autoscroll`, `leftToRight`, and the parallel (non-I2C) `LiquidCrystal` class.
- **`Servo` past eight channels**, and `Stepper` in its two-wire and five-wire
  constructor forms.
- **`tone()` on more than one pin at a time**, and `pulseIn` with a timeout
  longer than the 16-bit microsecond counter.
- **Anything not an ATmega328P.** Boards on the roadmap (RP2040, ESP32, SAMD)
  have no runtime here.

## Conventions

Headers are kept within the C++ subset ardio's own compiler is being built to
accept: no templates, no virtual functions, no multiple inheritance, no default
arguments where an overload will do, and operator overloading confined to `+`,
`+=`, `==`, `!=`, and `=` on `String`. Where the Arduino documentation specifies
a macro (`min`, `max`, `abs`, `constrain`), it stays a macro, because sketches
apply those across mixed types.
