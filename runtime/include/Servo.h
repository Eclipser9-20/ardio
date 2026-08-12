// Servo.h — ardio's own Servo class, API-compatible with the documented
// Arduino Servo library.
//
// An independent implementation: the pulse generation lives in ardio's
// assembly runtime (runtime/servo.S), which bit-bangs one RC servo pulse per
// call on any digital pin.
//
// ---------------------------------------------------------------------------
// WHAT THE RUNTIME ACTUALLY PROVIDES
//
// runtime/servo.S exports exactly three labels -- servo_attach(pin),
// servo_write(pin, angle) and servo_detach(pin) -- and every method below is
// written in terms of those. ardio has no linker and no symbol aliasing, so a
// method with no body would compile and then fail at assembly time as
// "nothing implements 'Servo__attach'"; each one therefore carries an inline
// definition here.
//
// Two consequences worth knowing before wiring anything up:
//
//   * ONE SERVO. servo.S keeps its state in three fixed SRAM bytes, so the
//     runtime tracks a single servo at a time. Attaching a second Servo takes
//     the channel over from the first. SERVO_MAX_SERVOS says so.
//   * ONE PULSE PER CALL. servo_write emits a single pulse of the requested
//     width and returns; it does not start a background pulse train. Call
//     write() about every 20 ms -- i.e. once round loop() -- to hold a
//     position, exactly as servo.S documents.
//
// Divergences from the published API, and why:
//
//   * attach() ignores its optional endpoint arguments. The runtime maps angle
//     to pulse width itself, on the fixed 1000..2000 us span below, and takes
//     no endpoints; accepting them and silently doing nothing with them would
//     be worse than the overload not existing, so the two- and three-argument
//     forms are gone. SERVO_MIN_PULSE_US/SERVO_MAX_PULSE_US report the span
//     the runtime really uses.
//   * writeMicroseconds() converts back to an angle and goes through the same
//     path, since the runtime has no width-taking entry point. It is therefore
//     quantised to whole degrees, about 5.5 us.
//   * attach() returns 0 rather than a channel index: there is one channel.
//
// Part of ardio. Licensed under the GNU General Public License v3.

#ifndef ARDIO_SERVO_H
#define ARDIO_SERVO_H

// The pulse widths, in microseconds, that 0 degrees and 180 degrees map to.
// These are servo.S's own numbers, not the Arduino library's 544/2400.
const int SERVO_MIN_PULSE_US = 1000;
const int SERVO_MAX_PULSE_US = 2000;

// The runtime holds one servo's state, so this is 1.
const int SERVO_MAX_SERVOS = 1;

// Runtime entry points, exported by runtime/servo.S as plain labels.
void servo_attach(int pin);
void servo_write(int pin, int angle);
void servo_detach(int pin);

class Servo {
public:
    Servo() {
        pin_ = -1;
        attached_ = 0;
        angle_ = 90;
    }

    // Claim a pin and park the recorded angle at centre. Returns 0, the only
    // channel this runtime has. No pulse is emitted until write() is called.
    int attach(int pin) {
        pin_ = (char)pin;
        attached_ = 1;
        angle_ = 90;
        servo_attach(pin);
        return 0;
    }

    // Angle in degrees, 0..180. Values outside that range are clamped. Emits
    // one pulse; call it about every 20 ms to hold the position.
    void write(int angle) {
        if (angle < 0) angle = 0;
        if (angle > 180) angle = 180;
        angle_ = angle;
        if (attached_ != 0) servo_write((int)pin_, angle);
    }

    // Pulse width in microseconds, converted to the nearest whole degree on
    // the runtime's 1000..2000 us span.
    void writeMicroseconds(int us) {
        if (us < SERVO_MIN_PULSE_US) us = SERVO_MIN_PULSE_US;
        if (us > SERVO_MAX_PULSE_US) us = SERVO_MAX_PULSE_US;
        write((us - SERVO_MIN_PULSE_US) * 180 / (SERVO_MAX_PULSE_US - SERVO_MIN_PULSE_US));
    }

    // The last angle written. Not a position reading — an RC servo cannot
    // report where it actually is.
    int read() { return angle_; }

    // The last pulse width written, in microseconds.
    int readMicroseconds() {
        return SERVO_MIN_PULSE_US +
               angle_ * (SERVO_MAX_PULSE_US - SERVO_MIN_PULSE_US) / 180;
    }

    bool attached() { return attached_ != 0; }

    // Stop pulsing and release the pin. The servo goes limp.
    void detach() {
        if (attached_ != 0) servo_detach((int)pin_);
        attached_ = 0;
    }

private:
    char pin_;
    char attached_;
    int angle_;
};

#endif // ARDIO_SERVO_H
