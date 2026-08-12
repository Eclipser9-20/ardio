// Servo.h — ardio's own Servo class, API-compatible with the documented
// Arduino Servo library.
//
// An independent declaration set: the pulse generation lives in ardio's
// assembly runtime (runtime/servo.S), which drives a hardware timer to emit the
// standard ~50 Hz RC servo pulse train.
//
// Part of ardio. Licensed under the GNU General Public License v3.

#ifndef ARDIO_SERVO_H
#define ARDIO_SERVO_H

#include "Arduino.h"

// The pulse widths, in microseconds, that 0 degrees and 180 degrees map to.
#define SERVO_MIN_PULSE_US 544
#define SERVO_MAX_PULSE_US 2400
#define SERVO_MAX_SERVOS   8

extern "C" {
void servo_attach(uint8_t pin, uint16_t min_us, uint16_t max_us);
void servo_write(uint8_t pin, uint16_t us);
void servo_detach(uint8_t pin);
}

class Servo {
public:
    Servo();

    // Claim a pin and begin emitting pulses. Returns the channel index, or -1
    // if all SERVO_MAX_SERVOS channels are in use.
    int attach(int pin);

    // As attach(pin), but with explicit endpoint pulse widths in microseconds.
    int attach(int pin, int min_us, int max_us);

    // Angle in degrees, 0..180. Values outside that range are clamped.
    void write(int angle);

    // Set the pulse width directly, bypassing the angle mapping.
    void writeMicroseconds(int us);

    // The last angle written. Not a position reading — an RC servo cannot
    // report where it actually is.
    int read();

    // The last pulse width written, in microseconds.
    int readMicroseconds();

    bool attached();

    // Stop pulsing and release the pin. The servo goes limp.
    void detach();

private:
    uint8_t  pin_;
    uint8_t  attached_;
    uint16_t min_us_;
    uint16_t max_us_;
    uint16_t pulse_us_;
};

#endif // ARDIO_SERVO_H
