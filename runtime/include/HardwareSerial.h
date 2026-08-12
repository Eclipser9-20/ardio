// HardwareSerial.h — ardio's own serial port class, API-compatible with the
// documented Arduino `Serial` object.
//
// An independent implementation: the USART0 driving lives in ardio's assembly
// runtime (runtime/serial.S), written from the ATmega328P datasheet. Polled,
// not interrupt-driven, so there is no receive buffer: available() reports the
// one byte the hardware holds, and nothing is lost that a buffer would have
// caught only by also adding an ISR the sketch cannot see.
//
// ---------------------------------------------------------------------------
// WHAT THE RUNTIME ACTUALLY PROVIDES
//
// runtime/serial.S exports seven labels: serial_begin(ubrr), serial_write(c),
// serial_print(s), serial_print_int(v), serial_available(), serial_read() and
// serial_flush(). Every method below forwards to one of them. ardio has no
// linker and no symbol aliasing, so a method carrying only a declaration
// compiles and then fails at assembly time as "nothing implements
// 'HardwareSerial__print'"; there are no declaration-only methods here.
//
// Divergences from the published API:
//
//   * begin() takes an `unsigned int`, not a `long`: this compiler passes
//     method arguments in registers and supports 8- and 16-bit ones only. The
//     standard rates are recognised by value and turned into the baud divisor
//     the runtime wants. 115200 does not fit 16 bits, so it arrives truncated
//     to its low half, 49664 -- which is recognised too, so
//     `Serial.begin(115200)` written the ordinary way does work. Any rate not
//     in the table below falls back to 9600, which is what an unconfigured
//     monitor expects anyway.
//   * print()/println() are overloaded on const char*, int and char. There is
//     no long-taking overload for the same argument-width reason; the free
//     functions serial_print_long()/serial_println_long() cover 32-bit values.
//   * No print(value, BASE): the runtime formats decimal only.
//
// Part of ardio. Licensed under the GNU General Public License v3.

#ifndef ARDIO_HARDWARE_SERIAL_H
#define ARDIO_HARDWARE_SERIAL_H

#include "Arduino.h"

// The runtime takes a baud *divisor*, not a baud rate: turning a rate into a
// divisor is a 32-bit division, and the rate is a constant in every real
// sketch. UBRR0 = F_CPU / (16 * baud) - 1 in normal mode at 16 MHz.
#define SERIAL_UBRR_9600    103
#define SERIAL_UBRR_19200   51
#define SERIAL_UBRR_38400   25
#define SERIAL_UBRR_57600   16
#define SERIAL_UBRR_115200  8

// 115200 does not fit an unsigned int; this is what is left of it when it does.
#define SERIAL_BAUD_115200_TRUNCATED 49664u

// Runtime entry points, exported by runtime/serial.S as plain labels.
void serial_begin(unsigned int ubrr);      // baud divisor, see the table above
void serial_write(int c);
void serial_print(const char* s);
void serial_print_int(int v);
int serial_available();
int serial_read();
void serial_flush();

// Send a 32-bit value in decimal. A free function rather than a method,
// because a method cannot take a 32-bit argument here.
inline void serial_print_long(long value) {
    char digits[12];                    // "-2147483648" plus headroom
    if (value == 0) {
        serial_write('0');
        return;
    }
    bool negative = value < 0;
    if (negative) value = -value;
    int count = 0;
    while (value > 0 && count < 12) {
        digits[count] = (char)('0' + (int)(value % 10));
        value = value / 10;
        count = count + 1;
    }
    if (negative) serial_write('-');
    while (count > 0) {
        count = count - 1;
        serial_write((int)digits[count]);
    }
}

inline void serial_println_long(long value) {
    serial_print_long(value);
    serial_write(13);
    serial_write(10);
}

class HardwareSerial {
public:
    // Standard rates are recognised by value; anything else falls back to
    // 9600. See the divergence note above for 115200.
    void begin(unsigned int baud) {
        unsigned int ubrr = SERIAL_UBRR_9600;
        if (baud == 19200u) ubrr = SERIAL_UBRR_19200;
        if (baud == 38400u) ubrr = SERIAL_UBRR_38400;
        if (baud == 57600u) ubrr = SERIAL_UBRR_57600;
        if (baud == SERIAL_BAUD_115200_TRUNCATED) ubrr = SERIAL_UBRR_115200;
        serial_begin(ubrr);
    }

    // Nothing to release on an ATmega328P beyond switching the USART off,
    // which begin() does for itself; end() is here for source compatibility.
    void end() { serial_flush(); }

    // Send one raw byte. Returns the number of bytes sent, as Arduino's does.
    int write(int c) {
        serial_write(c);
        return 1;
    }

    void print(const char* s) { serial_print(s); }
    void print(int v) { serial_print_int(v); }
    void print(char c) { serial_write((int)c); }

    void println() {
        serial_write(13);
        serial_write(10);
    }
    void println(const char* s) {
        serial_print(s);
        println();
    }
    void println(int v) {
        serial_print_int(v);
        println();
    }
    void println(char c) {
        serial_write((int)c);
        println();
    }

    // 1 when the hardware holds an unread byte, 0 otherwise.
    int available() { return serial_available(); }

    // The next byte, or -1 if none is waiting.
    int read() { return serial_read(); }

    // Blocks until the last character has finished shifting out.
    void flush() { serial_flush(); }
};

// ardio compiles one translation unit, so this is a definition rather than an
// `extern` declaration -- there is no second file for the object to live in.
HardwareSerial Serial;

#endif // ARDIO_HARDWARE_SERIAL_H
