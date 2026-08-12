// Arduino.h — ardio's own Arduino-compatible core header.
//
// This is an independent re-implementation of the published Arduino core API
// for the ATmega328P. It declares the same names, with the same meanings, as
// the documented Arduino language reference; none of it is derived from the
// Arduino core sources.
//
// Everything here is a declaration. The definitions live in ardio's assembly
// runtime (runtime/core.S and friends), which is assembled by ardio's own
// in-process AVR assembler and linked into the sketch.
//
// ---------------------------------------------------------------------------
// WHY THIS HEADER LOOKS THE WAY IT DOES
//
// ardio compiles sketches with its own C++ front end, not with avr-gcc, and
// that front end accepts a subset of C++. This header is written inside that
// subset so it actually compiles, rather than being faithful but unusable.
// The deliberate divergences from the published Arduino API are:
//
//   * No `extern "C"` blocks. The parser has no notion of linkage
//     specifications; the assembly runtime already exports plain, unmangled
//     labels, so C linkage was never doing any work here.
//   * No typedefs. `typedef` is parsed and discarded, so a name introduced by
//     one is not a type afterwards. uint8_t, byte, word, size_t and friends
//     are therefore gone; every declaration below uses a built-in type.
//     Where the Arduino API says uint8_t, this header says int.
//   * No function-like macros. Directives are stripped before compilation, so
//     min/max/abs/constrain/sq/bitRead/... are ordinary functions here. That
//     costs their type genericity: they are int-typed.
//   * bitSet/bitClear/bitWrite return the modified value instead of mutating
//     their argument in place, because the subset has no usable reference
//     parameters.
//   * No overloads. Semantic analysis keys functions by name alone, so two
//     functions sharing a name silently collide. Names that the Arduino API
//     overloads are split: random() becomes random_max()/random_range(),
//     attach() becomes attach()/attach_limits(), and so on.
//   * No floating point. PI, HALF_PI, DEG_TO_RAD and friends are omitted
//     entirely rather than approximated.
//   * `long` is accepted and is 4 bytes to the type system, but the code
//     generator currently evaluates expressions 16 bits wide. Long-typed
//     runtime entry points (millis, delay, pulseIn) keep their documented
//     shape; their upper half is not yet meaningful in generated code.
//
// Part of ardio. Licensed under the GNU General Public License v3.

#ifndef ARDIO_ARDUINO_H
#define ARDIO_ARDUINO_H

// ----------------------------------------------------------- constants -----
//
// Named constants are `const int` rather than macros. The compiler strips
// preprocessor directives, so a #define here would simply vanish and every use
// would fail as an undeclared identifier.

const int HIGH = 1;
const int LOW = 0;

const int INPUT = 0;
const int OUTPUT = 1;
const int INPUT_PULLUP = 2;

const int LSBFIRST = 0;
const int MSBFIRST = 1;

const int CHANGE = 1;
const int FALLING = 2;
const int RISING = 3;

// ----------------------------------------------------- pin numbering -------
//
// Arduino Uno / Nano (ATmega328P) digital pin numbers. D0..D13 are the digital
// header; A0..A7 continue the same numbering space so that digitalWrite(A0, ..)
// works exactly as on a real board. A6/A7 are analog-input-only on the Nano and
// have no digital driver.

const int D0 = 0;
const int D1 = 1;
const int D2 = 2;
const int D3 = 3;
const int D4 = 4;
const int D5 = 5;
const int D6 = 6;
const int D7 = 7;
const int D8 = 8;
const int D9 = 9;
const int D10 = 10;
const int D11 = 11;
const int D12 = 12;
const int D13 = 13;

const int A0 = 14;
const int A1 = 15;
const int A2 = 16;
const int A3 = 17;
const int A4 = 18;
const int A5 = 19;
const int A6 = 20;
const int A7 = 21;

const int LED_BUILTIN = 13;

// Bus pins, for sketches that name them symbolically. SDA/SCL repeat the A4/A5
// numbers rather than aliasing them, since a global cannot be initialised from
// another global's value in this subset.
const int SS = 10;
const int MOSI = 11;
const int MISO = 12;
const int SCK = 13;
const int SDA = 18;
const int SCL = 19;

const int NUM_DIGITAL_PINS = 22;
const int NUM_ANALOG_INPUTS = 8;

// -------------------------------------------------------- core runtime -----
//
// These bind directly to the symbols exported by runtime/core.S. Pin numbers,
// modes and levels are plain ints; the runtime narrows them.

void pinMode(int pin, int mode);
void digitalWrite(int pin, int value);
int digitalRead(int pin);

void delay(long ms);
void delayMicroseconds(int us);

int analogRead(int pin);
void analogWrite(int pin, int value);
void analogReference(int mode);

long millis();
long micros();

void shiftOut(int dataPin, int clockPin, int bitOrder, int value);
int shiftIn(int dataPin, int clockPin, int bitOrder);
long pulseIn(int pin, int state, long timeout);

void tone(int pin, int frequency, long duration);
void noTone(int pin);

void randomSeed(long seed);

void interrupts();
void noInterrupts();

// -------------------------------------------------------------- maths -----
//
// Arduino specifies these as macros so that they apply to any type. The
// compiler has no macros, so they are int-typed functions. Divergence: mixed
// or long arguments are narrowed to int at the call.

int min(int a, int b);
int max(int a, int b);
int abs(int x);
int constrain(int amt, int low, int high);
int sq(int x);

// Byte access. `w` is a 16-bit value.
int lowByte(int w);
int highByte(int w);

// Bit access. bitSet/bitClear/bitWrite return the modified value rather than
// writing through their argument: `flags = bitSet(flags, 3);`
int bitRead(int value, int bit_index);
int bitSet(int value, int bit_index);
int bitClear(int value, int bit_index);
int bitWrite(int value, int bit_index, int bit_value);
int bit(int bit_index);

// Re-map a value from one integer range to another, as documented for the
// Arduino map(). Integer arithmetic throughout, truncating toward zero. This
// lives in the runtime rather than being an inline definition here, because the
// code generator has no division yet.
long map(long value, long from_low, long from_high, long to_low, long to_high);

// Both documented forms of random(), under distinct names. Divergence: the
// Arduino API spells both `random`, which would collide here.
long random_max(long max_value);
long random_range(long lo, long hi);

// ------------------------------------------------------- sketch shape ------
//
// Every sketch supplies these two. ardio's startup code in runtime/core.S calls
// setup() once and then loop() forever.

void setup();
void loop();

#endif // ARDIO_ARDUINO_H
