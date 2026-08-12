// Arduino.h — ardio's own Arduino-compatible core header.
//
// This is an independent re-implementation of the published Arduino core API
// for the ATmega328P. It declares the same names, with the same meanings, as
// the documented Arduino language reference; none of it is derived from the
// Arduino core sources.
//
// ---------------------------------------------------------------------------
// EVERY NAME HERE IS BACKED BY SOMETHING
//
// ardio has no linker and no symbol aliasing: a name a sketch calls has to
// resolve to a label that exists, either one the assembly runtime exports or
// one generated from a definition in this header. A bare declaration with
// nothing behind it does not fail at compile time -- it fails much later, as
// "nothing implements 'foo'" out of the assembler. So this header contains
// exactly two kinds of entry:
//
//   * declarations of routines runtime/core.S and runtime/adc.S really export
//     (pinMode, digitalWrite, digitalRead, delay, delayMicroseconds,
//     analogRead, adc_init), and
//   * definitions, written here, of everything else -- built on top of those
//     routines and nothing more.
//
// Anything the runtime cannot back is not declared at all. runtime/README.md
// lists what was left out and why.
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
//   * min/max/abs/constrain/sq/bitRead/... are ordinary functions rather than
//     the function-like macros the Arduino reference specifies. That costs
//     their type genericity: they are int-typed.
//   * bitSet/bitClear/bitWrite return the modified value instead of mutating
//     their argument in place, because the subset has no usable reference
//     parameters.
//   * No floating point. PI, HALF_PI, DEG_TO_RAD and friends are omitted
//     entirely rather than approximated.
//   * `long` is 4 bytes and 32-bit arithmetic works, but a function may only
//     take as many arguments as fit the register-passed ABI, so map() takes
//     ints and returns a long rather than taking five longs.
//
// Part of ardio. Licensed under the GNU General Public License v3.

#ifndef ARDIO_ARDUINO_H
#define ARDIO_ARDUINO_H

// ----------------------------------------------------------- constants -----

const int HIGH = 1;
const int LOW = 0;

const int INPUT = 0;
const int OUTPUT = 1;
const int INPUT_PULLUP = 2;

const int LSBFIRST = 0;
const int MSBFIRST = 1;

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
// These five bind directly to labels exported by runtime/core.S. Pin numbers,
// modes and levels are plain ints; the runtime narrows them.
//
// delay() and delayMicroseconds() take an `unsigned int`, not the `unsigned
// long` of the published API: the routines in core.S read a 16-bit argument
// out of r24:r25, so a 32-bit argument would arrive in the wrong registers and
// wait for the wrong length of time. The ceiling is 65535 -- 65 seconds, or
// 65 milliseconds.

void pinMode(int pin, int mode);
void digitalWrite(int pin, int value);
int digitalRead(int pin);

void delay(unsigned int ms);
void delayMicroseconds(unsigned int us);

// ------------------------------------------------------- analog input ------
//
// Both are labels in runtime/adc.S. adc_init() powers the converter up and
// selects the AVcc reference; analogRead() blocks for the ~104 us a conversion
// takes and returns 0..1023.
//
// Divergence: the published API has no adc_init(), because the Arduino core
// enables the ADC in its own startup code. ardio's startup calls setup()
// directly and nothing else, so the sketch has to do it:
//
//     void setup() { adc_init(); }
//
// Without it ADEN stays clear, no conversion ever completes, and analogRead()
// spins forever waiting for one.

void adc_init();
int analogRead(int pin);

// ------------------------------------------------------ analog output ------
//
// There is no PWM hardware driver in the runtime, so this is a software PWM
// pulse: one period, bit-banged, then a return. The period is 2.04 ms -- about
// 490 Hz, the frequency the Arduino core's own timer-driven analogWrite runs
// most pins at -- and `value` is the usual 0..255 duty cycle.
//
// Divergence, and it matters: the published analogWrite() sets a timer up and
// returns, leaving the output running on its own. This one holds the level for
// exactly one period and then stops, so a sketch must call it every time round
// loop() to hold a brightness, the same way this runtime's servo_write emits
// one pulse per call. Calling it once and walking away gives one 2 ms flash.
inline void analogWrite(int pin, int value) {
    if (value < 0) value = 0;
    if (value > 255) value = 255;
    pinMode(pin, OUTPUT);
    unsigned int high_us = (unsigned int)value * 8;
    unsigned int low_us = (unsigned int)(255 - value) * 8;
    if (high_us > 0) {
        digitalWrite(pin, HIGH);
        delayMicroseconds(high_us);
    }
    if (low_us > 0) {
        digitalWrite(pin, LOW);
        delayMicroseconds(low_us);
    }
}

// ---------------------------------------------------------- bit banging ----
//
// Both are written against digitalWrite/digitalRead exactly as the Arduino
// reference describes them, so they behave the same; they are simply slower,
// since every edge goes through the runtime's pin decode.

inline void shiftOut(int dataPin, int clockPin, int bitOrder, int value) {
    for (int i = 0; i < 8; i = i + 1) {
        int bit_index = bitOrder == LSBFIRST ? i : 7 - i;
        digitalWrite(dataPin, (value >> bit_index) & 1);
        digitalWrite(clockPin, HIGH);
        digitalWrite(clockPin, LOW);
    }
}

inline int shiftIn(int dataPin, int clockPin, int bitOrder) {
    int value = 0;
    for (int i = 0; i < 8; i = i + 1) {
        digitalWrite(clockPin, HIGH);
        int bit_value = digitalRead(dataPin) != 0 ? 1 : 0;
        if (bitOrder == LSBFIRST) value = value | (bit_value << i);
        else                      value = value | (bit_value << (7 - i));
        digitalWrite(clockPin, LOW);
    }
    return value;
}

// -------------------------------------------------------------- maths -----
//
// Arduino specifies these as macros so that they apply to any type. The
// compiler has no function-like macros of its own to lean on here, so they are
// int-typed functions. Divergence: mixed or long arguments are narrowed to int
// at the call.

inline int min(int a, int b) { return a < b ? a : b; }
inline int max(int a, int b) { return a > b ? a : b; }
inline int abs(int x) { return x < 0 ? -x : x; }
inline int constrain(int amt, int low, int high) {
    if (amt < low) return low;
    if (amt > high) return high;
    return amt;
}
inline int sq(int x) { return x * x; }

// Byte access. `w` is a 16-bit value.
inline int lowByte(int w) { return w & 255; }
inline int highByte(int w) { return (w >> 8) & 255; }

// Bit access. bitSet/bitClear/bitWrite return the modified value rather than
// writing through their argument: `flags = bitSet(flags, 3);`
inline int bit(int bit_index) { return 1 << bit_index; }
inline int bitRead(int value, int bit_index) { return (value >> bit_index) & 1; }
inline int bitSet(int value, int bit_index) { return value | (1 << bit_index); }
inline int bitClear(int value, int bit_index) { return value & ~(1 << bit_index); }
inline int bitWrite(int value, int bit_index, int bit_value) {
    if (bit_value != 0) return value | (1 << bit_index);
    return value & ~(1 << bit_index);
}

// Re-map a value from one integer range to another, as documented for the
// Arduino map(). The arithmetic is done in 32 bits so that a wide scaling like
// map(reading, 0, 1023, 0, 20000) does not overflow on the way through.
//
// Divergence: the published map() takes five longs. Five 32-bit arguments do
// not fit the registers this ABI passes arguments in, so the inputs are ints
// -- which is what every real call passes anyway -- and only the result is a
// long.
inline long map(int value, int from_low, int from_high, int to_low, int to_high) {
    long from_span = (long)from_high - (long)from_low;
    if (from_span == 0) return (long)to_low;
    long offset = (long)value - (long)from_low;
    long to_span = (long)to_high - (long)to_low;
    return offset * to_span / from_span + (long)to_low;
}

// ------------------------------------------------------------- random ------
//
// A 32-bit linear congruential generator, seeded to 1 so that an unseeded
// sketch is repeatable rather than uninitialised. The multiplier and increment
// are the ones the C standard prints as an example generator; the top bits are
// the good ones, so the result is taken from bit 16 upwards.
//
// Both published forms are here, overloaded as the Arduino API spells them.

long __ardio_random_state = 1;

inline long __ardio_random_next() {
    __ardio_random_state = __ardio_random_state * 1103515245 + 12345;
    return (__ardio_random_state >> 16) & 32767;
}

inline void randomSeed(long seed) {
    if (seed == 0) seed = 1;   // 0 would not be a fixed point, but 1 is tidier
    __ardio_random_state = seed;
}

// 0 .. max_value-1. A max_value of 0 or less gives 0, as the reference says.
inline long random(long max_value) {
    if (max_value <= 0) return 0;
    return __ardio_random_next() % max_value;
}

// lo .. hi-1.
inline long random(long lo, long hi) {
    if (hi <= lo) return lo;
    return lo + __ardio_random_next() % (hi - lo);
}

// ------------------------------------------------------- sketch shape ------
//
// Every sketch supplies these two. ardio's startup code in runtime/core.S calls
// setup() once and then loop() forever.

void setup();
void loop();

#endif // ARDIO_ARDUINO_H
