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

9248 bytes of flash: 30% of the 30720 bytes an ATmega328P has behind the
stock 2 KB Nano bootloader.

This is the largest sketch in the tree, and it is the one that leans hardest
on the compiler: a four-state machine, a stroke font in a flat byte table,
pointer walking over a text buffer, and integer division in the inner loop. It
is written against what ardio's compiler actually supports, which is still
short of C++ in a few places that matter.

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
* **The font is new, and it is one flat table.** The original's table is not
  ours to redistribute, so the glyphs here are an independently drawn stroke
  font covering A–Z, 0–9, `-`, `.`, `!` and `?`. One byte per pen move: bit 6
  means pen down, bits 3..5 are x, bits 0..2 are y, and 126 stamps a dot; lower
  case folds to upper. `stroke[242]` holds every glyph end to end and
  `glyph_start[41]` says where each one begins, so the entry after a slot is
  where that slot ends and no terminator byte is needed. The offsets are
  stored biased by -121 to keep them inside a signed `char`.

  This was forty small functions behind a `switch` until aggregate
  initialisers landed in the compiler; the shapes are unchanged, only the
  storage. It was worth doing: the table form is 6272 bytes of flash smaller
  than the functions were, which is most of this sketch's total saving.
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

1. **No `#include` and no `#define`.** Directives are blanked before parsing,
   so the runtime's own headers in `runtime/include/` cannot be used, and
   neither can the library classes they declare.
2. **No `enum`.** `enum State { ... };` is `line 2: expected a type name`, so
   the two state enumerations are `const char` groups. `switch`/`case` over
   those constants works.
3. **No floating point at any level** — not even in the lexer, where `3.5`
   comes out as three separate tokens.
4. **No 32-bit locals or parameters**: `only 8- and 16-bit locals are
   supported`. The original's `long over`, `long dx`, `long dy` in Bresenham
   are `int` here. The values stay inside 16 bits at this machine's scale, but
   that is arithmetic that had to be checked by hand rather than a guarantee.
5. **`char` is signed and there is no `unsigned char`.** Bytes only reach 127,
   which is why the font's stroke bytes are packed into seven bits and its
   offsets are stored biased.
6. **The runtime is thinner than its headers.** `LiquidCrystal_I2C.h` declares
   `lcd_write_str`, `lcd_home`, `lcd_create_char` and friends, but `lcd.S`
   defines only a handful; calling one of the others fails at assembly time
   with `undefined label 'lcd_write_str'`. Printing a string is therefore a
   loop in the sketch. Worth knowing before designing against the headers.
7. **No `analogRead` and no `millis`/`micros`** in the runtime.
8. **The stepper runtime is single-instance** — fixed SRAM addresses, one set
   of coil pins — so a two-axis machine cannot use it.
9. **No `sizeof`**, no `typedef`. Both appear in the original; neither is
   load-bearing.
10. **Long if-chains are expensive in flash** (see above). Not a correctness
    limit, but it decides what fits in the part.

Four things that blocked an earlier draft of this port have since been fixed in
the compiler and are no longer limitations: array indexing and assignment; `/`
and `%`; aggregate initialisers, which is what let the font become a table;
and globals initialised from a named constant rather than a literal, which
used to compile cleanly and silently leave the global at zero.

Conditional branches used to be emitted without relaxation, so any `if` or loop
body over 63 words failed to assemble (`branch offset 82 is out of range`) and
had to be split into extra functions; that is fixed too, and the state machine
is now written inline the way the original is.

## Where the flash went

Measured with `ardio build`, summing the data records in the generated hex.
The sketch started at 16660 bytes and finished at 9248, a 44% cut, with no
feature removed and no glyph redrawn:

| change | bytes | after |
| --- | ---: | ---: |
| forty glyph functions and their `switch` become `stroke[]` + `glyph_start[]` | -6272 | 10388 |
| `coil_pattern` switch becomes a table; one `drive_coils` for both motors | -118 | 10270 |
| the redundant `angle` global, and `pen_up`/`pen_down` | -62 | 10208 |
| `setup` walks pin tables instead of writing out thirteen calls | -182 | 10026 |
| `show_choices` and `blink_cursor` replace three and two copies of themselves | -214 | 9812 |
| `glyph_start` stored as biased bytes rather than words | -236 | 9576 |
| small `const int` constants become `const char` | -40 | 9536 |
| `drive_coils` indexes the pin table instead of taking four pin arguments | -288 | 9248 |

Four things that looked like wins and measured as losses, so they are not in
the sketch: a `marks[4]` table replacing the four punctuation comparisons in
`alphabet` and `glyph_slot` (+12), a `go(next, from)` helper for the four state
transitions (+46), reading the buttons through `button_pins[]` at the five call
sites in `loop` (+140), and dropping `button_pins` in favour of five written-out
`button_init` calls (+68). On this compiler an array index costs more than a
comparison and a call costs more than three inline statements, so a table only
pays when it replaces a lot of code at once — which is exactly why the font,
which replaced forty functions, paid so enormously.

## Not implemented

Relative to the original this port drops: serial tracing; the accented and
symbol glyphs (`Ä Ö Ü ß & + : ; " # ( ) = @ *` and the two smileys and the
heart); the per-character kerning nudges; dashed-line drawing (`line()` was
only ever called with 0 or 1 in the original, so this removes an unused
branch); and analog joystick input. Everything else is here.
