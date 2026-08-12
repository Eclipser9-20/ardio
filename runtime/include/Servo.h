// Servo.h — ardio's own Servo class, API-compatible with the documented
// Arduino Servo library.
//
// An independent declaration set: the pulse generation lives in ardio's
// assembly runtime (runtime/servo.S), which drives a hardware timer to emit the
// standard ~50 Hz RC servo pulse train.
//
// Written inside the C++ subset ardio's own compiler accepts. Divergences from
// the published API, and why, are listed in Arduino.h; the ones that bite here
// are: no `extern "C"`, no typedefs (uint8_t/uint16_t become int), no macros
// for the constants, and no overloading — attach() with explicit endpoints is
// spelled attach_limits().
//
// Part of ardio. Licensed under the GNU General Public License v3.

#ifndef ARDIO_SERVO_H
#define ARDIO_SERVO_H

// The pulse widths, in microseconds, that 0 degrees and 180 degrees map to.
const int SERVO_MIN_PULSE_US = 544;
const int SERVO_MAX_PULSE_US = 2400;
const int SERVO_MAX_SERVOS = 8;

// Runtime entry points, exported by runtime/servo.S as plain labels.
void servo_attach(int pin, int min_us, int max_us);
void servo_write(int pin, int us);
void servo_detach(int pin);

class Servo {
public:
    Servo();

    // Claim a pin and begin emitting pulses. Returns the channel index, or -1
    // if all SERVO_MAX_SERVOS channels are in use.
    int attach(int pin);

    // As attach(pin), but with explicit endpoint pulse widths in microseconds.
    // Divergence: the Arduino API overloads attach(); this compiler keys
    // functions by name alone, so the three-argument form has its own name.
    int attach_limits(int pin, int min_us, int max_us);

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
    char pin_;
    char attached_;
    int min_us_;
    int max_us_;
    int pulse_us_;
};

#endif // ARDIO_SERVO_H
