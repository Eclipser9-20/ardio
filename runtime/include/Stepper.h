// Stepper.h — ardio's own Stepper class, API-compatible with the documented
// Arduino Stepper library.
//
// Four-wire unipolar/bipolar stepper control. The coil sequencing and the
// step-to-step timing live in ardio's assembly runtime (runtime/stepper.S),
// which drives a 28BYJ-48 through a ULN2003 in 4-step full-drive mode.
//
// ---------------------------------------------------------------------------
// WHAT THE RUNTIME ACTUALLY PROVIDES
//
// runtime/stepper.S exports three labels: stepper_init(p1, p2, p3, p4),
// stepper_set_speed(rpm) and stepper_step(steps). Every method below is an
// inline forward to one of them -- ardio has no linker, so a declared-only
// method fails at assembly time as "nothing implements 'Stepper__step'"
// rather than at compile time.
//
// Divergences from the published API:
//
//   * stepper_init takes the four coil pins and nothing else. The runtime's
//     rpm-to-delay conversion is fixed at the 28BYJ-48's 2048 steps per
//     output revolution (see the derivation in stepper.S), so the constructor
//     keeps steps_per_rev only to report it back from stepsPerRevolution().
//     Give a motor with a different step count an rpm scaled by
//     2048 / its_steps_per_rev if the speed has to be right.
//   * setSpeed() takes an int rather than the published long: rpm reaches the
//     runtime as a 16-bit value, and an rpm figure that needs 32 bits does not
//     exist.
//   * One stepper at a time. stepper.S keeps its state in fixed SRAM, so a
//     second Stepper takes the motor over from the first.
//   * step() blocks until every step has been issued, as the Arduino library's
//     does.
//
// Part of ardio. Licensed under the GNU General Public License v3.

#ifndef ARDIO_STEPPER_H
#define ARDIO_STEPPER_H

// The step count per output revolution that stepper.S's speed arithmetic
// assumes: a 28BYJ-48 through its 64:1 gearbox.
const int STEPPER_RUNTIME_STEPS_PER_REV = 2048;

// Runtime entry points, exported by runtime/stepper.S as plain labels.
void stepper_init(int p1, int p2, int p3, int p4);
void stepper_set_speed(int rpm);
void stepper_step(int steps);

class Stepper {
public:
    // Four-wire form. steps_per_rev is the motor's full-step count for one
    // output revolution (after any internal gearbox). The pins are made
    // outputs and de-energised, and the speed is left at the runtime's
    // 10 rpm default.
    Stepper(int steps_per_rev, int p1, int p2, int p3, int p4) {
        steps_per_rev_ = steps_per_rev;
        rpm_ = 10;
        pin1_ = (char)p1;
        pin2_ = (char)p2;
        pin3_ = (char)p3;
        pin4_ = (char)p4;
        stepper_init(p1, p2, p3, p4);
    }

    // Rotation speed in revolutions per minute. This sets the delay between
    // steps; it does not itself move the motor. The runtime clamps the delay
    // so that a 28BYJ-48 is never asked to follow steps it would skip.
    void setSpeed(int rpm) {
        rpm_ = rpm;
        stepper_set_speed(rpm);
    }

    // Step the motor. Positive turns one way, negative the other. Blocking:
    // returns only once every step has been issued.
    void step(int steps) { stepper_step(steps); }

    // Steps per revolution as given to the constructor.
    int stepsPerRevolution() { return steps_per_rev_; }

    // The speed last set, in rpm.
    int speed() { return rpm_; }

private:
    int steps_per_rev_;
    int rpm_;
    char pin1_;
    char pin2_;
    char pin3_;
    char pin4_;
};

#endif // ARDIO_STEPPER_H
