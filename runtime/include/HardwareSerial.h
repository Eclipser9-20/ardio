// HardwareSerial.h — ardio's own serial port class, API-compatible with the
// documented Arduino `Serial` object.
//
// An independent declaration set: the USART0 driving lives in ardio's assembly
// runtime (runtime/serial.S), written from the ATmega328P datasheet. Polled,
// not interrupt-driven, so there is no receive buffer: available() reports the
// one byte the hardware holds, and nothing is lost that a buffer would have
// caught only by also adding an ISR the sketch cannot see.
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

extern "C" {
void serial_begin(unsigned int ubrr);      // baud divisor, see the table above
void serial_write(unsigned char c);
void serial_print(const char* s);
void serial_print_int(int v);
unsigned char serial_available();
int serial_read();
void serial_flush();
}

class HardwareSerial {
public:
    // Standard rates are recognised by value; anything else falls back to
    // 9600, which is what an unconfigured monitor expects anyway.
    void begin(long baud) {
        unsigned int ubrr = SERIAL_UBRR_9600;
        if (baud == 19200L)  ubrr = SERIAL_UBRR_19200;
        if (baud == 38400L)  ubrr = SERIAL_UBRR_38400;
        if (baud == 57600L)  ubrr = SERIAL_UBRR_57600;
        if (baud == 115200L) ubrr = SERIAL_UBRR_115200;
        serial_begin(ubrr);
    }

    // Nothing to release on an ATmega328P beyond switching the USART off,
    // which begin() does for itself; end() is here for source compatibility.
    void end() { serial_flush(); }

    // Send one raw byte. Returns the number of bytes sent, as Arduino's does.
    int write(int c) { serial_write((unsigned char)c); return 1; }

    void print(const char* s) { serial_print(s); }
    void print(int v)         { serial_print_int(v); }

    void println() { serial_write(13); serial_write(10); }
    void println(const char* s) { serial_print(s); println(); }
    void println(int v)         { serial_print_int(v); println(); }

    // 1 when the hardware holds an unread byte, 0 otherwise.
    int available() { return (int)serial_available(); }

    // The next byte, or -1 if none is waiting.
    int read() { return serial_read(); }

    // Blocks until the last character has finished shifting out.
    void flush() { serial_flush(); }
};

extern HardwareSerial Serial;

#endif // ARDIO_HARDWARE_SERIAL_H
