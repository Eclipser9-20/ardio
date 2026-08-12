# Label Maker

A tape label printer for the Arduino Nano, in the shape of the well-known
CrunchLabs Hack Pack machine: a 16x2 I2C character LCD, a five-way control, two
28BYJ-48 steppers driving an X carriage and a Y lead screw, and a servo that
lifts the pen off the tape. You scroll through an alphabet a character at a
time, confirm, and the machine plots the label with a stroke font and a
Bresenham line routine.

```
ardio build examples/label_maker/label_maker.ino
```

It builds, and the image is 30406 bytes — which fits an ATmega328P with the
stock 2 KB Nano bootloader, but with under 350 bytes to spare. Anything added
to the font from here needs something else taken out.

This is the largest sketch in the tree and it is deliberately written against
what ardio's compiler *actually* supports today rather than against C++. That
gap is the interesting part, so it is written out in full below.

## What this port changes, and why

The reference sketch is 673 lines of ordinary Arduino C++: `Wire`,
`LiquidCrystal_I2C`, `Stepper`, `ezButton`, `Servo`, the `String` class, a
`switch` over an `enum` state machine, and a `const uint8_t vector[63][14]`
font table. Almost none of that survives contact with ardio's compiler. Fed to
`ardio build` unmodified it stops immediately, at the first declaration that
uses a library type:

```
line 15: expected a type name
```

The behaviour and the structure are the same here — same four states, same
menu text, same pen-up/pen-down plotting, same Bresenham stepping, same
release-the-coils-when-idle discipline. What changed:

* **No libraries.** `#`-directives are blanked out before parsing, so nothing
  can be included and no library class exists. The display, the servo and the
  buttons are implemented in the sketch on top of `pinMode`, `digitalWrite`,
  `digitalRead`, `delay` and `delayMicroseconds`, which is all of the runtime a
  sketch can reach (see limitation 1). That means a bit-banged two-wire master
  on A4/A5, a PCF8574/HD44780 4-bit driver, a counted-pulse servo, and
  edge-detecting debounced button reads.
* **Both steppers are driven by the sketch.** ardio's stepper runtime keeps its
  state at one fixed block of SRAM addresses and can therefore only drive a
  single motor; this machine has two. The four-step full-drive sequence is done
  here instead.
* **The joystick is five buttons.** There is no `analogRead` in the runtime at
  all, so the analog X/Y axes are replaced by four direction contacts plus the
  click, each with the internal pull-up on. Same gestures, different sensor.
* **The blinking cursor is a loop counter.** There is no `millis()`, so
  `millis() % 600 < 400` becomes a tick counter — and `%` does not exist
  either.
* **The label is a fixed set of scalars.** `String text` becomes twelve
  `int text_N` globals behind `text_get`/`text_set`, because there are no
  arrays and no strings. Twelve characters rather than sixteen is a flash
  budget decision, not a language one.
* **The font is new.** Two reasons. The original's `vector[63][14]` table
  cannot be expressed — no arrays, and no aggregate initialisers — so a glyph
  is a function of (slot, index) either way; and the original's table is not
  ours to redistribute. This is an independent stroke font covering A–Z, 0–9,
  `-` and `.`, in the same hundreds/tens/ones encoding: hundreds means pen
  down, tens is x, ones is y, 200 ends a glyph and 222 stamps a dot. Lower case
  folds to upper case.
* **The fractional fudge factors are gone.** No floating point exists, so
  `y * 3.5` is `(y * 7) >> 1`, and the `pos -= (scale*4) / 1.1` kerning nudges
  for `I` and `,` are dropped.
* **Serial tracing is gone.** There is no `Serial` object reachable from a
  sketch.
* **Everything is small functions.** Not a style choice: the assembler's
  conditional branches reach ±63 words, so any `if` body or loop body larger
  than that fails to assemble. The state machine arms, the two halves of
  Bresenham, and even the *inside* of the Bresenham loop are separate functions
  for this reason alone.

## ardio limitations hit, most blocking first

1. **Only `core.S` is linked.** `src/build/compile.cpp` appends exactly
   `{"core.S"}` to the generated assembly, so `lcd_*`, `servo_*`, `stepper_*`,
   `button_*` and `i2c_*` exist in `runtime/` but are unreachable from a
   sketch: calling one fails at assembly time with `undefined label
   'lcd_write_char'`. This is the single biggest gap — the runtime for this
   exact class of machine is already written and cannot be used.
2. **No arrays.** They parse and reserve SRAM, but reading `a[i]` is
   `unsupported expression kind` and writing it is `only simple variables can
   be assigned so far`. Brace initialisers keep only the first element and are
   then dropped anyway. This is what costs the font table, the label buffer,
   and the two coil-pin lists, and it is what makes the sketch 1300 lines
   instead of 700.
3. **The assembler's conditional branches only reach ±63 words**, and the code
   generator never falls back to a jump. A perfectly ordinary `if` with six
   statements in it, or a `for` loop with a handful of calls in the body, dies
   with `branch offset 82 is out of range (-64 to 63)`. Every such site has to
   be hand-split into another function.
4. **No `switch`.** It parses, but lowers to a hidden 32-bit local and then
   fails with `only 8- and 16-bit locals are supported`. The state machine is
   an if/else chain.
5. **No division and no modulo.** `unsupported binary operator '/'`. Dividing
   by a power of two becomes a shift; the one real division left (`v / 10`, to
   split a glyph coordinate) is a subtract loop.
6. **No string literals in expressions**, so the seven fixed LCD messages are
   seven functions that push their characters one `lcd_write_char` at a time.
   That alone is a few hundred bytes of flash.
7. **No 32-bit locals or parameters.** `long over` in the original Bresenham
   becomes `int`; the values involved stay inside 16 bits here, but that is
   luck, not design.
8. **No floating point at any level** — not even in the lexer, where `3.5`
   comes out as three separate tokens.
9. **No classes in practice.** Methods parse, lay out and type-check, and are
   then never code-generated; a method call emits a call to a bare name that
   does not link.
10. **Long if-chains are expensive.** Each comparison costs roughly 25
    instructions, so a 41-entry lookup table written as an if-chain is about
    two kilobytes of flash. Both big lookups here (character menu, ASCII to
    glyph slot) had to be rewritten as arithmetic to fit in the part; without
    that the sketch overflows at 34410 bytes.
11. **No `#include`, no `#define`.** Directives are blanked before parsing, so
    the runtime's own headers in `runtime/include/` cannot be used and every
    constant is a `const int` global.
12. **Globals initialise only from integer literals.** `int angle =
    PEN_UP_ANGLE;` compiles cleanly and silently leaves `angle` at zero, which
    is the nastiest thing on this list: it is the only one with no diagnostic.
13. **No `sizeof`, no `typedef`, no function overloading, no default
    arguments.** Minor here, but all of them appear in the original.
14. **No `analogRead`, no `millis`, no `Serial`** in the runtime.

## Not implemented

Relative to the original, this port drops: serial tracing; the accented and
symbol glyphs (`Ä Ö Ü ß & + : ; " # ( ) = @ * !` and the two smileys and the
heart); the `!` and `?` glyphs, which the alphabet no longer offers either;
per-character kerning nudges; dashed-line drawing (`line()` always took 0 or 1
in the original too, so this only removes an unused branch); and analog
joystick input. Label length is twelve characters. Everything else is here.
