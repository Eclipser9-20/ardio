# What still stops ardio compiling the *original* Label Maker sketch

`label_maker.ino` next to this file is a hand-written port of the CrunchLabs
Label Maker firmware: same machine, same behaviour, but written in the subset
ardio understands. It builds and it flashes.

The goal beyond that is the upstream sketch itself — 674 lines,
`003-label-maker/LabelMakerStarterCode/LabelMakerStarterCode.ino` in the
CrunchLabs hackpack repository — compiled byte for byte as its author wrote
it. That sketch is not vendored here; it is not ours to redistribute. What is
recorded below is each limitation it runs into, in the order a front-to-back
compile hits them, with the smallest fragment that reproduces it.

Every entry has a matching test in `tests/test_original_label_maker.cpp`,
named `original_label_maker_still_cannot_…`, asserting the limitation is still
there. When one of those tests starts failing, the limitation is gone: delete
the test and tick the entry off here.

## Where it stands

Measured by fetching the sketch, compiling it, applying the smallest possible
change to get past each error, and repeating.

* **Roughly 70% of the sketch's lines already compile untouched** — 472 of
  674 lines needed no change at all. Of the 202 that did, 74 are the character
  table (item 5) and most of the rest are the mechanical `Serial.print` /
  `lcd.print` renames of item 12.
* The whole sketch **parses and type-checks** once items 1–12 are worked
  around. There is no construct in it that the front end fundamentally cannot
  represent.
* It still does not produce a `.hex`. With every language workaround applied
  the build dies in the assembler on item 13, purely because the program is
  bigger than a short jump can reach. Blank out any one of the larger
  functions and the same source assembles and links cleanly — so item 13 is
  the *only* thing standing between a workaround-free front end and a real
  binary.

Items 1–12 are front-end work and can be done in any order. Item 13 is the one
that must be done regardless of everything else. Items 14–15 are runtime, not
compiler.

---

## 1. `sizeof`

Not recognised at all — the parser does not know the keyword.

```
error: line 187: expected an expression
```

```cpp
int n = sizeof(int);
```

In the sketch: `int alphabetSize = sizeof(alphabet) - 1;`

**Difficulty: easy.** A unary operator over a type name or an expression,
folded to a constant at compile time. Needs a type-size query, which the
codegen already has for layout.

## 2. Array fields cannot be used from a member function

Declaring `char b[33]` inside a class is accepted. *Reading* it from a method
is not: the field then goes down a path that only handles 8- and 16-bit
scalars.

```
error: field 'b' of class 'C': only 8- and 16-bit fields are supported
```

```cpp
class C { public: char f() { return b[0]; } char b[33]; };
C c;
```

Not in the sketch directly — but it is what stops `runtime/include/WString.h`
compiling, and so what makes item 3 unfixable on its own.

**Difficulty: medium.** An array field needs to decay to `this + offset`
rather than be loaded as a value.

## 3. `String`

The sketch's label text is a `String`. There is no `String` type in scope:
`Arduino.h` includes nothing, and including `WString.h` by hand fails on
item 2.

```
error: line 187: expected a type name
  187 | String text;
```

```cpp
#include <Arduino.h>
String text;
```

Beyond making the header compile, the sketch uses `text += c`,
`text.length()`, `text.remove(n)`, `text = ""`, `plotText(String &str, …)`,
`str.charAt(i)` and `Serial.println(str)`. `WString.h` deliberately offers
`concat`/`setString` in place of the operators, so `operator+=` and
`operator=` on a class are needed too — neither parses today:

```
error: line 190: expected ';' after a declaration
```

```cpp
class C { public: C& operator+=(char c) { return *this; } };
```

**Difficulty: hard.** Item 2, then `Arduino.h` pulling in `WString.h`, then
operator overloading. This is the single largest item on the list.

## 4. The Arduino type spellings: `boolean`, `byte`, `uint8_t`

None of the three are known type names.

```
error: line 187: expected a type name
```

```cpp
boolean b = false;
uint8_t x = 1;
byte b = 1;
```

In the sketch: `boolean pPenOnPaper = false;`, `void plot(boolean …)`,
`const uint8_t vector[63][14]`, `if (byte(c) != 195)`.

**Difficulty: easy.** Three aliases — `boolean` → `bool`, `byte`/`uint8_t` →
`unsigned char` — plus the rest of `<stdint.h>`'s AVR set while you are there.

## 5. Aggregate initialisers -- DONE

All three forms now compile, at global and at block scope.

```cpp
int v[4] = {1, 2, 3, 4};
int v[2][2] = {{1, 2}, {3, 4}};
const char a[] = "ABC";
```

Braced lists are typed against the target type, element by element, with
nested braces recursing into the element type; too many initialisers is an
error and too few zero-fill, as C promises. A global's initialiser is folded
to a byte image and stored before the entry point runs; a local's is lowered
into one store per element.

The 63x14 table is 1764 bytes. It fits in the ATmega328P's SRAM, but only just
-- globals are limited to 1792 bytes so that the stack has room -- so a global
array that does not fit is now a diagnostic naming the sizes rather than a
run-time overwrite of the stack.

## 6. Functional-style casts

```
error: line 190: expected an expression
```

```cpp
void f() { int i = 65; char c = char(i); }
```

In the sketch: `char(str.charAt(i))`, `byte(c)`, and thirty-odd `uint8_t(c)`
in `plotCharacter`. C-style `(char)i` works fine.

**Difficulty: easy.** In the expression parser, a type name followed by `(`
is a cast, not a call.

## 7. Floating point

The sketch declares no float, but it divides by one, and the parser reads the
`.1` of `1.1` as a member access.

```
error: line 190: expected a member name
error: line 187: expected a type name        (for `float`)
```

```cpp
void f() { int x = 10; x = x / 1.1; }
float f = 1.0;
```

In the sketch: `pos -= (scale*4) / 1.1;`, `/ 1.2`, and
`y + cy*y_scale*3.5`.

**Difficulty: hard if done properly** (soft-float on an 8-bit core is a
runtime library), **easy if done narrowly.** All three uses are
integer-times-constant; constant-folding a float literal in an integer
context into a rational multiply-then-divide would cover the sketch exactly
and cost nothing at runtime. Worth deciding deliberately rather than by
default.

## 8. The alternative operator spellings

```
error: line 190: expected ')' after an if condition
```

```cpp
void f() { int a = 1, b = 2; if (a and b) a = 0; }
```

In the sketch: `if (uint8_t(c) > 64 and uint8_t(c) < 91)`, four times.

**Difficulty: trivial.** Lex `and`/`or`/`not` (and the rest of the set) as the
corresponding punctuation.

## 9. `extern "C"` linkage blocks

```
error: line 187: expected a type name
```

```cpp
extern "C" { void ff(unsigned char c); }
```

Nothing in the sketch writes one — but `runtime/include/HardwareSerial.h`
does, so the header does not compile, so `Serial` does not exist:

```
error: cannot compile 'HardwareSerial.h' yet -- ardio's compiler rejects the
header itself: line 204: expected a type name
```

**Difficulty: trivial.** ardio does not mangle names anyway; parse the
construct and emit the contents as if the wrapper were not there.

## 10. `Arduino.h` includes nothing

Even with item 9 fixed, the sketch never includes `HardwareSerial.h` itself —
no Arduino sketch does. The real core header pulls in the serial port and the
string class for you. ardio's `Arduino.h` has no `#include` lines at all.

```
error: line 626: undeclared identifier 'Serial'
```

**Difficulty: trivial**, once items 3 and 9 land. Until then, adding the
include would only move the error.

## 11. Library objects have no constructors

Every library header the sketch includes declares its class, and every one of
those classes compiles. None of the constructors is implemented, so a global
object of one does not link:

```
error: nothing implements 'LiquidCrystal_I2C__ctor'. It is declared by a
header the sketch includes, but ardio's runtime does not define it yet.
```

```cpp
#include <LiquidCrystal_I2C.h>
LiquidCrystal_I2C lcd(0x27, 16, 2);
```

The sketch constructs four kinds: `lcd`, `button1`, `xStepper`/`yStepper`,
`servo`. The port sidesteps all of them by calling the C runtime directly
(`lcd_init`, `button_init`, `stepper_step`, `servo_write`).

Two sub-problems, one bigger than it looks:

* **Constructors with arguments, running before `setup()`.** There is no
  static-initialisation pass; something has to call each constructor at
  startup, in declaration order.
* **`Stepper` cannot be instantiated twice.** The runtime's stepper is a
  singleton — `stepper_init(…)` and `stepper_set_speed(rpm)` take no handle —
  but the sketch has an X stepper and a Y stepper and drives them
  independently. This one is a runtime redesign, not a compiler fix.

**Difficulty: medium for the constructors, medium for the two-stepper
runtime.**

## 12. Method overloading by argument type

Overloaded *free functions* now work. The library headers, though, were
written around the limitation and expose `print_str` / `print_int` /
`print_char` instead of one `print`, so the sketch's calls do not resolve:

```
error: line 622: undeclared method 'LiquidCrystal_I2C::print'
```

The sketch calls `lcd.print(…)` sixteen times and `Serial.print`/`println`
twenty-two times, with `const char*`, `int` and `char` arguments — 38 lines,
the bulk of the mechanical edits.

**Difficulty: easy-to-medium.** Extend overload resolution to methods, then
give `HardwareSerial` and `LiquidCrystal_I2C` real `print`/`println`
overloads forwarding to the existing primitives.

## 13. The assembler never relaxes a jump — the sketch is too big

This is the blocker that survives everything else. `rjmp`, `rcall` and the
conditional branches all reach ±2K words, and nothing is ever promoted to the
32-bit `jmp`/`call`. A program larger than that cannot be assembled.

```
error: line 2: jump offset 3000 is out of range (-2048 to 2047)
error: line 202: relaxed branch offset 4200 is out of range (-2048 to 2047)
error: branch relaxation did not settle
```

Which of the three you get depends on how the layout iterates; the third is
the relaxation loop oscillating rather than converging, and it is what the
full sketch produces.

```cpp
int g;
void f() {
    if (g == 0) {
        g = 0;  // ... repeated ~700 times
    }
}
```

or, at the assembler directly:

```
start:
    rjmp far
    nop        ; x3000
far:
    ret
```

With items 1–12 worked around by hand, the sketch reaches this and stops.
Empty out `line()`, `plotText()` or any one of the switch-case bodies and the
very same source assembles, links and writes a `.hex` — the code is correct,
there is just too much of it to reach with a short jump.

**Difficulty: medium, and unavoidable.** The assembler already has a
relaxation pass; it needs to grow the ability to widen `rjmp`→`jmp` and
`rcall`→`call` (and to invert a conditional branch over a `jmp` where the
condition itself is out of reach), and to iterate monotonically so it always
converges.

## 14. `millis()`

Declared, nothing behind it.

```
error: nothing implements 'millis'.
```

The sketch uses it four times, always as `millis() % 600 < 400` — a 32-bit
modulo, which also wants `__ardio_udivmod32`:

```
error: nothing implements '__ardio_udivmod32'.
```

**Difficulty: medium.** A timer0 overflow ISR and a 32-bit counter, plus the
32-bit division helper.

## 15. `abs()`

```
error: nothing implements 'abs'.
```

In the sketch: `dx = abs(dx); dy = abs(dy);` in `line()`.

**Difficulty: trivial.** Three instructions, or a builtin.

---

## Things that used to be on this list and are not any more

Re-measured over the course of the work, these went from failing to passing:

* `#include` and `#define` — the sketch's five includes and five macros now
  resolve; the first error is no longer at line 4.
* Enums, and enum values as `switch` labels — `enum State { MainMenu, … };`
  and the four-case state machine parse and generate.
* Overloading of free functions.
* `long` locals and `long` arithmetic.
* `analogRead`.
* Reference parameters (`void f(int &x)`), ternary operators, `A0`–`A7`,
  `LED_BUILTIN`, string literals in block comments, macro-sized array bounds,
  and array fields that are declared but never read.
