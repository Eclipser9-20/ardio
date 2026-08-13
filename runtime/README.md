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

The assembly no longer names a part. Every register address, the top of RAM and
the whole digital pin numbering reach it as generated symbols, so one runtime
serves every AVR in ardio's device table. The clock is still assumed to be
16 MHz by the cycle-counted delays in `ir.S`, `servo.S`, `stepper.S`, `lcd.S`,
`button.S` and `delayMicroseconds`; `delay()` and the TWI bit rate are computed
from `AD_F_CPU` and are correct at any clock.

### How the device description reaches the assembly

`src/avr/device.cpp` holds one row per AVR part. Before the runtime is
assembled, the row for the board's MCU is rendered into a block of assembly and
emitted alongside it (`device_prelude`, declared in
`include/ardio/avr/device.h`, which carries the authoritative contract). That
block defines:

- **Constants.** `AD_RAMEND`, `AD_RAMSTART`, `AD_F_CPU`, `AD_NUM_PINS`,
  `AD_NUM_ANALOG`, `AD_LED_BUILTIN`, the USART registers `AD_UCSRA`…`AD_UDR`,
  the ADC registers `AD_ADMUX`…`AD_ADCH`, timer 0's `AD_TCCR0A`…`AD_TIFR0`, the
  two-wire block `AD_TWBR`…`AD_TWCR`, and `AD_SREG` / `AD_SPL` / `AD_SPH`.

  All of these are **data-space** addresses, reached with `lds`/`sts` — except
  `AD_SREG`, `AD_SPL` and `AD_SPH`, which are **I/O** addresses, because those
  three are identical on every AVR8 part and `in`/`out` is the only way the
  runtime ever touches them.

- **`__ardio_pinmap`.** Two bytes per digital pin in flash: the data-space
  address of that pin's `PINx` register, then its bit number within the port. A
  pin the part does not have is stored as address 0, and every caller treats a
  zero address as "no such pin" and does nothing. Because a label is a word
  address and `lpm` addresses bytes, pin *n*'s entry is at byte address
  `__ardio_pinmap * 2 + n * 2`.

- **`__ardio_analogmap`.** One byte per analog input: the ADC channel `An`
  selects, or `0xFF` for a number the part does not have.

Only `PINx` is stored for a pin because on every AVR the three port registers
sit together as `PINx`, `DDRx = PINx+1`, `PORTx = PINx+2`. One `Z` pointer at
`PINx` therefore reaches all three through the displacement forms `Z+0`, `Z+1`
and `Z+2`, which is how `core.S`, `ir.S`, `button.S`, `servo.S` and `stepper.S`
drive a pin whose number is only known at run time.

Note that `.equ` and `.org` are resolved as the assembler walks the file, before
the generated block has been read, so a runtime file may use an `AD_*` name in
an **instruction operand** but must not define a `.equ` in terms of one.

### Supporting a new part

1. Add a row to the table in `src/avr/device.cpp`: memory sizes, the register
   addresses from the datasheet's register summary, the board's digital pin
   numbering and its analog channels.
2. Point a board at it through `Board::mcu` in `src/board.cpp`.

There is no step three. No assembly file changes, because none of them contain
a part-specific number — with these exceptions, all of them documented at the
point they occur:

- `runtime/timer.S` still has the ATmega328P's vector table addresses in its
  `.org` directives. `.org` has to be resolved while the assembler is still
  measuring where code lands, which is before the generated block at the end of
  the image has been read, so the vector number cannot be a symbol as things
  stand. Timer 0's overflow vector is elsewhere on every other part in the
  table, and this is why `timer.S` is excluded from the runtime the build
  assembles.
- `analogRead` in `runtime/adc.S` folds a digital pin number down to an analog
  input number by subtracting 14, the digital pin number of `A0`. The device
  description does not carry that number and it cannot be derived from the ones
  it does carry: `AD_NUM_PINS - AD_NUM_ANALOG` is 14 on most parts but 12 on the
  ATmega328P, whose table lists two ADC-only inputs that have no digital pin.
  Guessing there would read a different pin and say nothing about it.
- The delays listed above are calibrated for 16 MHz.
- Each file's SRAM state is at a fixed address chosen by hand, since there is no
  linker and no `.bss`. Those addresses fit inside the SRAM of every part in the
  table, but a part with very little RAM would need them revisited.

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
- **Anything that is not an AVR.** Boards on the roadmap (RP2040, ESP32, SAMD)
  have no runtime here: the device description abstracts over AVR parts, not
  over instruction sets.

## Conventions

Headers are kept within the C++ subset ardio's own compiler is being built to
accept: no templates, no virtual functions, no multiple inheritance, no default
arguments where an overload will do, and operator overloading confined to `+`,
`+=`, `==`, `!=`, and `=` on `String`. Where the Arduino documentation specifies
a macro (`min`, `max`, `abs`, `constrain`), it stays a macro, because sketches
apply those across mixed types.
