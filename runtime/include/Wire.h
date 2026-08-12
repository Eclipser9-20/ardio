// Wire.h — ardio's own TwoWire class, API-compatible with the documented
// Arduino Wire (I2C) library.
//
// Controller (master) mode only. The bit-level I2C sequencing lives in ardio's
// assembly runtime (runtime/wire.S), driving the ATmega328P's hardware TWI unit
// on A4 (SDA) and A5 (SCL).
//
// Part of ardio. Licensed under the GNU General Public License v3.

#ifndef ARDIO_WIRE_H
#define ARDIO_WIRE_H

#include "Arduino.h"

// endTransmission() status codes, matching the documented Arduino values.
#define WIRE_SUCCESS       0
#define WIRE_TOO_LONG      1  // data did not fit in the transmit buffer
#define WIRE_NACK_ADDRESS  2  // no device acknowledged the address
#define WIRE_NACK_DATA     3  // device stopped acknowledging mid-transfer
#define WIRE_OTHER_ERROR   4

#define WIRE_BUFFER_LENGTH 32

extern "C" {
void    i2c_init(uint32_t frequency_hz);
uint8_t i2c_start(uint8_t address, uint8_t read_flag);  // returns 0 on ACK
uint8_t i2c_write(uint8_t value);                       // returns 0 on ACK
uint8_t i2c_read(uint8_t send_ack);
void    i2c_stop(void);
}

class TwoWire {
public:
    TwoWire();

    // Join the bus as controller at the default 100 kHz.
    void begin();

    // Bus clock in hertz — 100000 or 400000 on this part.
    void setClock(uint32_t frequency_hz);

    // Open a write to a 7-bit address. Bytes handed to write() are buffered
    // until endTransmission().
    void beginTransmission(uint8_t address);

    // Queue one byte. Returns the number of bytes accepted (0 if the buffer is
    // already full).
    uint8_t write(uint8_t value);

    // Queue a run of bytes; returns how many were accepted.
    uint8_t write(const uint8_t *data, uint8_t count);

    // Send the queued bytes and release the bus. Returns one of the WIRE_*
    // status codes above.
    uint8_t endTransmission();

    // Request bytes from a device; returns how many were actually received.
    uint8_t requestFrom(uint8_t address, uint8_t count);

    // Reading back what requestFrom() collected.
    int available();
    int read();
    int peek();

private:
    uint8_t buffer_[WIRE_BUFFER_LENGTH];
    uint8_t length_;
    uint8_t index_;
    uint8_t address_;
    uint8_t transmitting_;
};

// The single global instance every sketch uses.
extern TwoWire Wire;

#endif // ARDIO_WIRE_H
