// Stepper.h — ardio's own Stepper class, API-compatible with the documented
// Arduino Stepper library.
//
// Four-wire unipolar/bipolar stepper control. The coil sequencing and the
// step-to-step timing live in ardio's assembly runtime (runtime/stepper.S).
//
// Part of ardio. Licensed under the GNU General Public License v3.

#ifndef ARDIO_STEPPER_H
#define ARDIO_STEPPER_H

#include "Arduino.h"

extern "C" {
void stepper_init(uint8_t p1, uint8_t p2, uint8_t p3, uint8_t p4, uint16_t steps_per_rev);
void stepper_set_speed(uint16_t rpm);
void stepper_step(int16_t steps);
}

class Stepper {
public:
    // Four-wire form. steps_per_rev is the motor's full-step count for one
    // output revolution (after any internal gearbox).
    Stepper(int steps_per_rev, int p1, int p2, int p3, int p4);

    // Rotation speed in revolutions per minute. This sets the delay between
    // steps; it does not itself move the motor.
    void setSpeed(long rpm);

    // Step the motor. Positive turns one way, negative the other. Blocking:
    // returns only once every step has been issued.
    void step(int steps);

    // Steps per revolution as given to the constructor.
    int stepsPerRevolution();

private:
    uint16_t steps_per_rev_;
    uint16_t rpm_;
    uint8_t  pin1_;
    uint8_t  pin2_;
    uint8_t  pin3_;
    uint8_t  pin4_;
};

#endif // ARDIO_STEPPER_H
