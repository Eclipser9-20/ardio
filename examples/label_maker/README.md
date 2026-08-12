# Label Maker

A tape label printer for the Arduino Nano: a 16x2 I2C character LCD, a
five-way control, two 28BYJ-48 steppers driving an X carriage and a Y lead
screw, and a servo that lifts the pen off the tape. You scroll through an
alphabet a character at a time, confirm, and the machine plots the label with
a stroke font and a Bresenham line routine.

```
$ ardio build examples/label_maker/label_maker.ino
built .ardio-build/label_maker.hex
```

25060 bytes of flash, so it fits an ATmega328P with room to spare even behind
the stock 2 KB Nano bootloader.

This is the largest sketch in the tree, and it is the one that leans hardest
on the compiler: a four-state machine, forty glyph routines, pointer walking
over a text buffer, and integer division in the inner loop. It is written
against what ardio's compiler actually supports, which is still short of C++
in a few places that matter.

## What this port changes, and why

The starting point is the familiar 673-line Arduino label maker sketch:
`Wire`, `LiquidCrystal_I2C`, `Stepper`, `ezButton` and `Servo`, the `String`
class, a `switch` over an `enum` state machine, and a `const uint8_t
vector[63][14]` font table. Fed to `ardio build` unmodified, it stops at the
first declaration that names a library type:

```
line 15: expected a type name
```

The behaviour and the structure here are the same as that sketch — same four
states, same menu text, same pen-up/pen-down plotting, same Bresenham
stepping, same release-the-coils-when-idle discipline. What changed:

* **No libraries, no includes.** `#`-directives are blanked out before
  parsing, so `#include` brings in nothing and `#define` leaves an undeclared
  identifier behind. Every runtime entry point is declared at the top of the
  sketch instead, and every constant is a `const int`. The library *objects*
  are gone with them: `lcd.print(...)` becomes `print_str(...)` over the
  runtime's `lcd_write_char`, and so on.
* **Both steppers are driven by the sketch.** ardio's stepper runtime keeps its
  state at one fixed block of SRAM addresses, so it can only ever drive a
  single motor; this machine has two. The four-step full-drive sequence is
  written out here instead, over `digitalWrite`.
* **The joystick is five buttons.** There is no `analogRead` in the runtime, so
  the analog X/Y axes become four direction contacts plus the click, through
  the runtime's debounced `button_init`/`button_pressed`. Same gestures,
  different sensor.
* **The blinking cursor is a pass counter.** There is no `millis()`, so
  `millis() % 600 < 400` becomes `blink_tick % 12 < 8`.
* **`String text` is `char text[17]`.** No heap, no `String`; the display is
  sixteen columns wide anyway, and the label is capped to match.
* **The font is new, and it is code rather than data.** Two separate reasons.
  The original's table is not ours to redistribute, so the glyphs here are an
  independently drawn stroke font covering A–Z, 0–9, `-`, `.`, `!` and `?` in
  the same encoding: hundreds digit means pen down, tens is x, ones is y, 200
  ends a glyph, 222 stamps a dot, lower case folds to upper. And it is forty
  small functions behind a `switch` rather than a `const int[40][14]` because
  ardio has no aggregate initialisers — an array can be declared and indexed,
  but nothing can put values in it at compile time, and filling a table this
  size at run time would cost 1120 of the part's 2048 bytes of SRAM.
* **The fractional fudge factors are gone.** There is no floating point, so
  `y * 3.5` is `y * 7 / 2`, and the `pos -= (scale*4) / 1.1` kerning nudges for
  `I` and `,` are dropped.
* **Serial tracing is gone.** `serial_*` routines do exist in the runtime, but
  the original's tracing is `Serial.print` on mixed types, which needs
  overloading and `String`.
* **Two big lookups are arithmetic, not tables.** The character menu and the
  ASCII-to-glyph-slot mapping would each be a forty-entry if-chain, and each
  comparison costs roughly twenty-five instructions; written as chains they
  added about four kilobytes of flash between them.

## ardio limitations hit, most blocking first

Ordered by how much they cost this sketch.

1. **No aggregate initialisers.** `int t[3] = {1,2,3};` fails with `cannot
   initialise int[3] from int`, at global and local scope alike, and
   `char s[] = "AB"` fails with `cannot initialise char[0] from char[3]` (no
   size deduction either). Arrays are otherwise fine now — declared, indexed,
   assigned — so this is the one gap that reshapes the design, and it is what
   turns a 63x14 font table into forty functions.
2. **No `#include` and no `#define`.** Directives are blanked before parsing,
   so the runtime's own headers in `runtime/include/` cannot be used, and
   neither can the library classes they declare.
3. **No `enum`.** `enum State { ... };` is `line 2: expected a type name`, so
   the two state enumerations are `const int` groups. `switch`/`case` over
   those constants works.
4. **No floating point at any level** — not even in the lexer, where `3.5`
   comes out as three separate tokens.
5. **No 32-bit locals or parameters**: `only 8- and 16-bit locals are
   supported`. The original's `long over`, `long dx`, `long dy` in Bresenham
   are `int` here. The values stay inside 16 bits at this machine's scale, but
   that is arithmetic that had to be checked by hand rather than a guarantee.
6. **Globals initialise only from integer literals.** `int angle =
   PEN_UP_ANGLE;` compiles cleanly and silently leaves `angle` at zero. It is
   the only item on this list with no diagnostic at all, and it is the one
   that cost the most debugging.
7. **The runtime is thinner than its headers.** `LiquidCrystal_I2C.h` declares
   `lcd_write_str`, `lcd_home`, `lcd_create_char` and friends, but `lcd.S`
   defines only a handful; calling one of the others fails at assembly time
   with `undefined label 'lcd_write_str'`. Printing a string is therefore a
   loop in the sketch. Worth knowing before designing against the headers.
8. **No `analogRead` and no `millis`/`micros`** in the runtime.
9. **The stepper runtime is single-instance** — fixed SRAM addresses, one set
   of coil pins — so a two-axis machine cannot use it.
10. **No `sizeof`**, no `typedef`, no function overloading, no default
    arguments. All four appear in the original; none is load-bearing.
11. **Long if-chains are expensive in flash** (see above). Not a correctness
    limit, but it decides what fits in the part.

Two things that blocked an earlier draft of this port have since been fixed in
the compiler and are no longer limitations: array indexing and assignment, and
`/` and `%`. Conditional branches used to be emitted without relaxation, so any
`if` or loop body over 63 words failed to assemble (`branch offset 82 is out of
range`) and had to be split into extra functions; that is fixed too, and the
state machine is now written inline the way the original is.

## Not implemented

Relative to the original this port drops: serial tracing; the accented and
symbol glyphs (`Ä Ö Ü ß & + : ; " # ( ) = @ *` and the two smileys and the
heart); the per-character kerning nudges; dashed-line drawing (`line()` was
only ever called with 0 or 1 in the original, so this removes an unused
branch); and analog joystick input. Everything else is here.
