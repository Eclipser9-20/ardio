// Wire.h — ardio's own TwoWire class, API-compatible with the documented
// Arduino Wire (I2C) library.
//
// Controller (master) mode only. The bit-level I2C sequencing lives in ardio's
// assembly runtime (runtime/wire.S), driving the ATmega328P's hardware TWI unit
// on A4 (SDA) and A5 (SCL).
//
// Written inside the C++ subset ardio's own compiler accepts; see Arduino.h for
// the full list. Here: no `extern "C"`, no typedefs, status codes are `const
// int` instead of macros, and write() is split into write()/write_bytes()
// because the compiler keys functions by name alone and would otherwise let the
// two overloads collide silently.
//
// Part of ardio. Licensed under the GNU General Public License v3.

#ifndef ARDIO_WIRE_H
#define ARDIO_WIRE_H

// endTransmission() status codes, matching the documented Arduino values.
const int WIRE_SUCCESS = 0;
const int WIRE_TOO_LONG = 1;       // data did not fit in the transmit buffer
const int WIRE_NACK_ADDRESS = 2;   // no device acknowledged the address
const int WIRE_NACK_DATA = 3;      // device stopped acknowledging mid-transfer
const int WIRE_OTHER_ERROR = 4;

const int WIRE_BUFFER_LENGTH = 32;

// Runtime entry points, exported by runtime/wire.S as plain labels.
// Divergence: the bus frequency is given in kilohertz rather than hertz, so it
// fits an int; 400 kHz is the fastest this part supports.
void i2c_init(int frequency_khz);
int i2c_start(int address, int read_flag);   // returns 0 on ACK
int i2c_write(int value);                    // returns 0 on ACK
int i2c_read(int send_ack);
void i2c_stop();

class TwoWire {
public:
    TwoWire();

    // Join the bus as controller at the default 100 kHz.
    void begin();

    // Bus clock in kilohertz — 100 or 400 on this part.
    void setClock(int frequency_khz);

    // Open a write to a 7-bit address. Bytes handed to write() are buffered
    // until endTransmission().
    void beginTransmission(int address);

    // Queue one byte. Returns the number of bytes accepted (0 if the buffer is
    // already full).
    int write(int value);

    // Queue a run of bytes; returns how many were accepted. Divergence: the
    // Arduino API names this write() too.
    int write_bytes(const char *data, int count);

    // Send the queued bytes and release the bus. Returns one of the WIRE_*
    // status codes above.
    int endTransmission();

    // Request bytes from a device; returns how many were actually received.
    int requestFrom(int address, int count);

    // Reading back what requestFrom() collected.
    int available();
    int read();
    int peek();

private:
    // Array lengths must be literals: the parser only records a bound when the
    // subscript is an integer literal, so WIRE_BUFFER_LENGTH cannot be used.
    char buffer_[32];
    int length_;
    int index_;
    int address_;
    int transmitting_;
};

// The single global instance every sketch uses.
extern TwoWire Wire;

#endif // ARDIO_WIRE_H
