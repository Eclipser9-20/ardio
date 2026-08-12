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
// Part of ardio. Licensed under the GNU General Public License v3.

#ifndef ARDIO_ARDUINO_H
#define ARDIO_ARDUINO_H

// ------------------------------------------------------------ integers -----
//
// The sketch compiler is freestanding: there is no <stdint.h> to include, so
// the fixed-width types are spelled out here for the AVR data model
// (char 1, short 2, int 2, long 4, long long 8, pointer 2).

typedef signed char        int8_t;
typedef unsigned char      uint8_t;
typedef signed int         int16_t;
typedef unsigned int       uint16_t;
typedef signed long        int32_t;
typedef unsigned long      uint32_t;
typedef signed long long   int64_t;
typedef unsigned long long uint64_t;

typedef uint16_t size_t;
typedef int16_t  ssize_t;
typedef uint16_t uintptr_t;
typedef int16_t  intptr_t;

typedef uint8_t  byte;
typedef uint16_t word;
typedef uint8_t  boolean;

#ifndef NULL
#define NULL 0
#endif

// ----------------------------------------------------------- constants -----

#define HIGH 1
#define LOW  0

#define INPUT        0
#define OUTPUT       1
#define INPUT_PULLUP 2

#define LSBFIRST 0
#define MSBFIRST 1

#define CHANGE  1
#define FALLING 2
#define RISING  3

#define PI         3.1415926535897932384626433832795
#define HALF_PI    1.5707963267948966192313216916398
#define TWO_PI     6.283185307179586476925286766559
#define DEG_TO_RAD 0.017453292519943295769236907684886
#define RAD_TO_DEG 57.295779513082320876798154814105

// ----------------------------------------------------- pin numbering -------
//
// Arduino Uno / Nano (ATmega328P) digital pin numbers. D0..D13 are the digital
// header; A0..A7 continue the same numbering space so that digitalWrite(A0, ..)
// works exactly as on a real board. A6/A7 are analog-input-only on the Nano and
// have no digital driver.

#define D0  0
#define D1  1
#define D2  2
#define D3  3
#define D4  4
#define D5  5
#define D6  6
#define D7  7
#define D8  8
#define D9  9
#define D10 10
#define D11 11
#define D12 12
#define D13 13

#define A0 14
#define A1 15
#define A2 16
#define A3 17
#define A4 18
#define A5 19
#define A6 20
#define A7 21

#define LED_BUILTIN 13

// Bus pins, for sketches that name them symbolically.
#define SS   10
#define MOSI 11
#define MISO 12
#define SCK  13
#define SDA  A4
#define SCL  A5

#define NUM_DIGITAL_PINS  22
#define NUM_ANALOG_INPUTS 8

// -------------------------------------------------------- core runtime -----
//
// These five bind directly to the symbols exported by runtime/core.S, so they
// are declared with C linkage: the assembler emits plain, unmangled labels.

extern "C" {

void pinMode(uint8_t pin, uint8_t mode);
void digitalWrite(uint8_t pin, uint8_t value);
int  digitalRead(uint8_t pin);

void delay(uint32_t ms);
void delayMicroseconds(uint16_t us);

// The remaining core entry points also live in the assembly runtime.
int  analogRead(uint8_t pin);
void analogWrite(uint8_t pin, int value);
void analogReference(uint8_t mode);

uint32_t millis(void);
uint32_t micros(void);

void     shiftOut(uint8_t dataPin, uint8_t clockPin, uint8_t bitOrder, uint8_t value);
uint8_t  shiftIn(uint8_t dataPin, uint8_t clockPin, uint8_t bitOrder);
uint32_t pulseIn(uint8_t pin, uint8_t state, uint32_t timeout);

void tone(uint8_t pin, uint16_t frequency, uint32_t duration);
void noTone(uint8_t pin);

void randomSeed(uint32_t seed);
long random_range(long min_value, long max_value);

void interrupts(void);
void noInterrupts(void);

} // extern "C"

// -------------------------------------------------------------- maths -----
//
// Arduino specifies min/max/abs/constrain/round as macros, and sketches rely on
// that (they are applied to mixed types). They are macros here for the same
// reason, and are guarded so that a host build including <algorithm> first is
// not broken by them.

#ifndef min
#define min(a, b) ((a) < (b) ? (a) : (b))
#endif

#ifndef max
#define max(a, b) ((a) > (b) ? (a) : (b))
#endif

#ifndef abs
#define abs(x) ((x) > 0 ? (x) : -(x))
#endif

#ifndef constrain
#define constrain(amt, low, high) \
    ((amt) < (low) ? (low) : ((amt) > (high) ? (high) : (amt)))
#endif

#ifndef round
#define round(x) ((x) >= 0 ? (long)((x) + 0.5) : (long)((x) - 0.5))
#endif

#ifndef sq
#define sq(x) ((x) * (x))
#endif

#define lowByte(w)  ((uint8_t)((w) & 0xFF))
#define highByte(w) ((uint8_t)(((w) >> 8) & 0xFF))

#define bitRead(value, bit)    (((value) >> (bit)) & 0x01)
#define bitSet(value, bit)     ((value) |= (1UL << (bit)))
#define bitClear(value, bit)   ((value) &= ~(1UL << (bit)))
#define bit(b)                 (1UL << (b))

// Re-map a value from one integer range to another, as documented for the
// Arduino map(). Integer arithmetic throughout, truncating toward zero.
inline long map(long value, long from_low, long from_high, long to_low, long to_high)
{
    long from_span = from_high - from_low;
    if (from_span == 0)
        return to_low;
    return (value - from_low) * (to_high - to_low) / from_span + to_low;
}

// Both documented forms of random(). Written as two overloads rather than one
// function with a default argument, so the sketch compiler never has to fill in
// a missing parameter.
inline long random(long max_value)            { return random_range(0, max_value); }
inline long random(long lo, long hi)          { return random_range(lo, hi); }

// -------------------------------------------------------------- String -----

#include "WString.h"

// ------------------------------------------------------- sketch shape ------
//
// Every sketch supplies these two. ardio's startup code in runtime/core.S calls
// setup() once and then loop() forever.

void setup();
void loop();

#endif // ARDIO_ARDUINO_H
