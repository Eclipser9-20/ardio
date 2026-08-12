# IR Turret

A port of the CrunchLabs Hack Pack **IR Turret** sketch to ardio's own C++
compiler and runtime. An Arduino Nano drives three servos -- yaw (continuous
rotation, spins the base), pitch (positional, tilts the head) and roll
(continuous rotation, advances the dart barrel) -- and takes commands from an
infrared remote.

    ardio build examples/ir_turret/ir_turret.ino
    ardio push  examples/ir_turret/ir_turret.ino

**Status: it builds.** `ardio build` exits 0 and writes `ir_turret.hex`
(about 5 kB of code, comfortably inside the Nano's 30 kB). It has not been run
on hardware; the timing notes below are the parts most likely to need a trim
against a real turret.

The original is MIT-licensed code belonging to CrunchLabs and is not copied into
this repository. It is at
`rglidden/hackpack`, `001-ir-turret/IRturretStarterCode/IRturretStarterCode.ino`.

## What the port keeps

Same wiring (yaw on 10, pitch on 11, roll on 12, IR receiver on 9), same NEC
command bytes for the bundled remote, same servo constants -- `yawStopSpeed`,
`yawPrecision`, `rollPrecision`, `pitchMin`/`pitchMax`, all unchanged -- and the
same six actions bound to the same buttons: up, down, left, right, OK to fire
one dart, `*` to fire all six. `shakeHeadYes` and `shakeHeadNo` are carried over
too; as in the original, nothing calls them.

## What the port changes, and why

### The Servo library is gone; the sketch pulses the servos itself

ardio has no library manager and no linker, and `runtime/servo.S` (which does
have `servo_attach` / `servo_write` / `servo_detach`) is **not linked into a
sketch build** -- `src/build/compile.cpp` appends only `core.S`. So the sketch
generates the 50 Hz servo frame directly out of `digitalWrite` and
`delayMicroseconds`, which are in `core.S`.

That forced one real behavioural change. The Servo library refreshes its servos
from a timer interrupt in the background, so the original can say
`yawServo.write(180); delay(150);` and the continuous-rotation servo keeps
turning for the whole 150 ms. Without a background refresh a single pulse turns
the motor for one frame and then it stops. Every timed movement therefore goes
through `servoHold(pin, angle, ms)`, which re-pulses at 50 Hz for the requested
duration. The motion is the same; the code that produces it is not.

### The IRremote library is gone; the sketch decodes NEC itself

Same reason. `runtime/ir.S` does not exist yet, so declaring `ir_begin` /
`ir_available` / `ir_decode` / `ir_read_command` and calling them would leave
undefined labels and fail to assemble. Instead the sketch contains a small
polling NEC receiver: `measure()` counts 10 us steps while the receiver output
holds a level, `ir_bit()` turns a burst/space pair into a bit, `ir_byte()` reads
eight of them, and `ir_decode()` checks the 9 ms leader and 4.5 ms space and
verifies the command against its ones complement.

Two consequences worth knowing:

- Timing is measured by counting `delayMicroseconds(10)` calls rather than with
  a timer, because ardio has no timer runtime. The tolerances are wide
  (a 9 ms leader is accepted anywhere in 6--12 ms) to absorb the loop overhead,
  but this is the part most likely to need adjustment on real hardware.
- NEC *repeat* frames (held buttons) are rejected rather than decoded, so a held
  button does not auto-repeat. The original had the same practical behaviour,
  since a repeat frame carries no command byte.

If `runtime/ir.S` does land and is wired into the build, the four functions
named `measure`, `ir_bit`, `ir_byte` and `ir_decode` here are the ones to
delete; nothing else in the sketch depends on how a command is obtained.

### Serial output is gone

The original prints protocol diagnostics and a line per action. `runtime/serial.S`
exists but, like `servo.S`, is not linked into a sketch build, and `Serial` is
not a name the compiler knows. Every `Serial.print*` call was dropped. Nothing
else changes; they were diagnostics only.

### Functions were split up

`ir_decode`, `loop`, `leftMove`, `rightMove`, `upMove`, `downMove`,
`shakeHeadYes` and `shakeHeadNo` were each split, so that no loop body and no
`if` body is more than a single call. See limitation 1 below. The helpers
(`yawStep`, `pitchStep`, `rollBurst`, `nodFrame`, `nodOnce`, `shakeOnce`,
`pollRemote`, `handleCommand`, `fireAllAndPause`) exist only for that reason and
have no counterpart in the original.

### Miscellaneous

- `shakeHeadYes(int moves = 3)` lost its default argument -- ardio does not
  support default arguments -- so callers pass the count.
- The `#define`d button codes became `int` globals, because ardio's sketch
  preprocessor strips `#` lines entirely and does not expand macros.
- `pitchMoveSpeed` is negated at the call site (`pitchStep(0 - pitchMoveSpeed)`)
  rather than in the callee, which keeps up and down sharing one helper.

## ardio limitations hit, most blocking first

1. **Conditional branches have no long-branch expansion.** The assembler emits
   `breq`/`brne` with the AVR's native +-63 word reach, so any `if` or loop body
   larger than about 60 words of generated code fails with
   `branch offset N is out of range (-64 to 63)`. Since the code generator is
   verbose -- a two-argument call is roughly a dozen instructions -- this bites
   at around three or four statements. It was hit five separate times while
   porting and is the single reason the sketch is carved into so many one-line
   helpers. A relaxation pass that rewrites an out-of-range `brXX target` into
   `brYY .+2 / rjmp target` would remove this entirely and is the highest-value
   fix here.

2. **Only `core.S` is linked.** `src/build/compile.cpp` appends exactly one
   runtime file to the generated assembly. `servo.S`, `serial.S`, `wire.S`,
   `lcd.S`, `stepper.S` and `button.S` are unreachable from a sketch: calling
   `servo_write` leaves an undefined label. That is why the servo driver and the
   IR decoder are written out in the sketch, and why there is no serial output.
   Appending every runtime file and letting the assembler drop what is
   unreferenced -- or a real symbol-driven link step -- would let this sketch
   shrink by roughly half.

3. **Locals are capped at 62 bytes per frame** (`needs N bytes of locals; the
   frame pointer reaches only 62`). An `int[32]` alone is 64 bytes, so the
   obvious "buffer the 32 IR bits, then interpret them" structure is not
   expressible; the port consumes the frame a byte at a time instead.

4. **No 32-bit locals.** `long` is in the type system and in `Type::size()`, but
   `codegen_stmt.cpp` refuses to allocate one: `only 8- and 16-bit locals are
   supported`.

5. **No floating point.** `float x = 1.5;` fails in the lexer/parser. Not needed
   here, but worth recording.

6. **`#include` and `#define` are stripped, not processed.** The sketch
   preprocessor removes `#` lines wholesale, so every runtime entry point must be
   declared by hand in the sketch and macro constants must become variables.

7. **No default arguments.** `void f(int a = 3)` parses, but calling `f()`
   reports `'f' expects 1 arguments, got 0`.

8. **No `Serial`, and no `String`.** Neither is a name the compiler knows.
   `String` was not needed here -- unlike the Label Maker sketch -- so it cost
   nothing, but any sketch that prints is currently out of reach.

Two limitations were hit during this port and then fixed in the compiler while
it was underway, so they are recorded only as history: `switch` used to lower
its controlling expression into a 32-bit temporary that the code generator
refused to allocate, so no switch ever built; and `/` used to emit a call to an
`__ardio_divmod16` helper that the linked runtime did not define. Both work now,
and the sketch uses both.

Things that were expected to be missing and turned out to be fine: `%`, `&&`,
`||`, compound assignment, unary minus, arrays, `do`/`while`, and calling a
function before it is defined (the sketch preprocessor hoists prototypes
correctly).

## Test

`tests/test_ir_turret.cpp` reads this sketch off disk, runs it through
`preprocess_sketch` and `compile_avr`, and checks both succeed, skipping if the
example cannot be located. It is not yet listed in `CMakeLists.txt`'s
`ardio_tests` sources, which enumerates test files explicitly; adding
`tests/test_ir_turret.cpp` to that list wires it into `ctest`.
