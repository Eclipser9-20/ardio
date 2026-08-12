// Wire.h — ardio's own TwoWire class, API-compatible with the documented
// Arduino Wire (I2C) library.
//
// Controller (master) mode, transmit only. The bit-level I2C sequencing lives
// in ardio's assembly runtime (runtime/wire.S), driving the ATmega328P's
// hardware TWI unit on A4 (SDA) and A5 (SCL).
//
// ---------------------------------------------------------------------------
// WHAT THE RUNTIME ACTUALLY PROVIDES
//
// runtime/wire.S exports four labels and no more:
//
//     i2c_init()        bus enabled, 100 kHz, prescaler 1
//     i2c_start()       START (or repeated START)
//     i2c_write(byte)   shift one byte out; returns the masked TWSR status
//     i2c_stop()        STOP
//
// Every method below is an inline forward built from those. ardio has no
// linker and no symbol aliasing, so a method carrying only a declaration
// compiles and then fails at assembly time as "nothing implements
// 'TwoWire__write'"; there are no declaration-only methods here.
//
// WHAT IS MISSING, AND WHY IT IS NOT DECLARED
//
//   * requestFrom(), read(), peek(), available() — reading a device needs a
//     receive routine (TWCR with TWEA, then TWDR) that wire.S does not have.
//     Rather than declare them and fail at assembly time, they are absent.
//   * setClock() — the bit rate is TWBR, which only i2c_init() writes. The bus
//     runs at 100 kHz.
//   * onReceive/onRequest and peripheral (slave) mode — controller only.
//
// runtime/README.md carries the same list.
//
// Divergence: the transmit buffer is a file-scope array rather than a member,
// because this compiler cannot read an array field from inside a method. With
// one TwoWire instance -- the global Wire below, which is all any sketch uses
// -- that makes no observable difference.
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

// TWSR status codes, masked to their top five bits, as i2c_write returns them.
const int WIRE_TWSR_SLA_W_ACK = 0x18;
const int WIRE_TWSR_DATA_ACK = 0x28;

// Runtime entry points, exported by runtime/wire.S as plain labels.
void i2c_init();
void i2c_start();
int i2c_write(int value);   // returns the masked TWSR status
void i2c_stop();

// The transmit buffer. See the divergence note above.
char __ardio_wire_buffer[32];

class TwoWire {
public:
    TwoWire() {
        length_ = 0;
        address_ = 0;
        transmitting_ = 0;
    }

    // Join the bus as controller. The runtime fixes the clock at 100 kHz.
    void begin() {
        length_ = 0;
        transmitting_ = 0;
        i2c_init();
    }

    // Open a write to a 7-bit address. Bytes handed to write() are buffered
    // until endTransmission().
    void beginTransmission(int address) {
        address_ = address;
        length_ = 0;
        transmitting_ = 1;
    }

    // Queue one byte. Returns the number of bytes accepted (0 if the buffer is
    // already full).
    int write(int value) {
        if (length_ >= WIRE_BUFFER_LENGTH) return 0;
        __ardio_wire_buffer[length_] = (char)value;
        length_ = length_ + 1;
        return 1;
    }

    // Queue a run of bytes; returns how many were accepted.
    int write(const char *data, int count) {
        int accepted = 0;
        for (int i = 0; i < count; i = i + 1) {
            if (write((int)data[i]) == 0) return accepted;
            accepted = accepted + 1;
        }
        return accepted;
    }

    // Queue a NUL-terminated string; returns how many bytes were accepted.
    int write(const char *text) {
        int accepted = 0;
        int i = 0;
        while (text[i] != 0) {
            if (write((int)text[i]) == 0) return accepted;
            accepted = accepted + 1;
            i = i + 1;
        }
        return accepted;
    }

    // Send the queued bytes and release the bus. Returns one of the WIRE_*
    // status codes above.
    int endTransmission() {
        transmitting_ = 0;
        i2c_start();
        if (i2c_write((address_ << 1) & 0xFE) != WIRE_TWSR_SLA_W_ACK) {
            i2c_stop();
            length_ = 0;
            return WIRE_NACK_ADDRESS;
        }
        for (int i = 0; i < length_; i = i + 1) {
            if (i2c_write((int)__ardio_wire_buffer[i] & 255) != WIRE_TWSR_DATA_ACK) {
                i2c_stop();
                length_ = 0;
                return WIRE_NACK_DATA;
            }
        }
        i2c_stop();
        length_ = 0;
        return WIRE_SUCCESS;
    }

    // Bytes currently queued for the open transmission.
    int queued() { return length_; }

private:
    int length_;
    int address_;
    int transmitting_;
};

// The single global instance every sketch uses. ardio compiles one translation
// unit, so this is a definition rather than an `extern` declaration -- there is
// no second file for the object to live in.
TwoWire Wire;

#endif // ARDIO_WIRE_H
