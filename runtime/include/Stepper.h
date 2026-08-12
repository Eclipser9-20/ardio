// Stepper.h — ardio's own Stepper class, API-compatible with the documented
// Arduino Stepper library.
//
// Four-wire unipolar/bipolar stepper control. The coil sequencing and the
// step-to-step timing live in ardio's assembly runtime (runtime/stepper.S).
//
// Written inside the C++ subset ardio's own compiler accepts; see Arduino.h for
// the full list of divergences. Here: no `extern "C"`, and the fixed-width
// types of the published API become int.
//
// Part of ardio. Licensed under the GNU General Public License v3.

#ifndef ARDIO_STEPPER_H
#define ARDIO_STEPPER_H

// Runtime entry points, exported by runtime/stepper.S as plain labels.
void stepper_init(int p1, int p2, int p3, int p4, int steps_per_rev);
void stepper_set_speed(int rpm);
void stepper_step(int steps);

class Stepper {
public:
    // Four-wire form. steps_per_rev is the motor's full-step count for one
    // output revolution (after any internal gearbox).
    Stepper(int steps_per_rev, int p1, int p2, int p3, int p4);

    // Rotation speed in revolutions per minute. This sets the delay between
    // steps; it does not itself move the motor. Divergence: the published API
    // takes a long; an int is the honest width for an RPM figure here, and
    // avoids a 32-bit argument the code generator would truncate anyway.
    void setSpeed(int rpm);

    // Step the motor. Positive turns one way, negative the other. Blocking:
    // returns only once every step has been issued.
    void step(int steps);

    // Steps per revolution as given to the constructor.
    int stepsPerRevolution();

private:
    int steps_per_rev_;
    int rpm_;
    char pin1_;
    char pin2_;
    char pin3_;
    char pin4_;
};

#endif // ARDIO_STEPPER_H
