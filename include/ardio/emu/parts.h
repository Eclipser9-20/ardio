#pragma once
#include "ardio/emu/machine.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// The virtual breadboard's component library.
//
// A Part sees pins changing and nothing else, which is what a real component
// sees. That is what makes these usable on any board: an LED does not know
// whether the level on its pin came from a port bit on an ATmega328P or from
// something else entirely, and it should not have to.
//
// HOW A PART LEARNS THE CLOCK RATE, and why it is done this way.
//
// pin_changed carries a cycle count, not a time. A servo cannot turn a pulse
// width into an angle, and a buzzer cannot turn an interval into a frequency,
// without knowing how long a cycle lasts. Three ways to supply that were
// available:
//
//   * Hardcode a rate. Rejected outright. A 8 MHz board would then decode
//     every 1500 us servo pulse as 3000 us and report 180 degrees for a
//     centred servo -- a plausible-looking wrong answer, which is the failure
//     mode this codebase treats as worse than no answer at all.
//
//   * Have the Machine push the rate into a part when it is wired. That is the
//     tidiest at the call site, but it means a part is only correct once it has
//     been wired, and a test -- or a future breadboard UI -- that constructs a
//     part and drives it directly gets one silently running at whatever the
//     default was. It also needs a hook on the Part interface that is not
//     there.
//
//   * Require the rate at construction. That is what is done here. Every part
//     takes clock_hz as its first constructor argument with no default, so a
//     part cannot exist without one, and the registry below refuses to build a
//     part for a clock of zero rather than inventing a rate. The cost is that
//     the caller must know f_cpu, which it does: it comes from the Board.
//
// set_clock_hz is offered as well, and the by-name registry needs it: a caller
// naming a part on a command line has not chosen a board yet at that point, so
// create_part builds one with its clock still unknown and the caller sets the
// rate once the board is resolved. A part in that state does not guess. It
// reports a clock of zero, returns zero from every time-valued accessor, and
// says so in describe(), so a caller that forgets the second step gets an
// obviously empty answer instead of one computed against a made-up rate.
//
// One thing the Part interface does not provide, which several parts here need:
// there is no way for a part to be told that time has passed without one of its
// pins changing. A button whose script says "press at cycle 32000" has to learn
// that cycle 32000 has arrived somehow, and no pin of its own moves at that
// moment. So parts that need it expose their own advance(cycles), which the
// board's run loop should call whenever it advances peripherals. pin_changed
// also folds an advance in, so a part is never behind the last edge it saw.
namespace ardio::emu {

// Common state for the parts here: the clock rate they measure time against,
// and a display label.
class PartBase : public Part {
public:
    explicit PartBase(uint32_t clock_hz, std::string label = {})
        : clock_hz_(clock_hz), label_(std::move(label)) {}

    uint32_t clock_hz() const { return clock_hz_; }
    void set_clock_hz(uint32_t hz) { clock_hz_ = hz; }

    const std::string& label() const { return label_; }
    void set_label(std::string label) { label_ = std::move(label); }

    // Cycles as microseconds at this part's clock. Returns 0 for a clock of
    // zero rather than dividing by it; a part built through the registry can
    // never have one, but set_clock_hz is public.
    double cycles_to_us(uint64_t cycles) const {
        if (clock_hz_ == 0) return 0.0;
        return double(cycles) * 1e6 / double(clock_hz_);
    }

    // Told that time has passed without a pin moving. The default does
    // nothing; parts that generate their own events override it.
    void advance(uint64_t cycles) override { (void)cycles; }

protected:
    uint32_t clock_hz_ = 0;
    std::string label_;
};

// ================================================================== LED ====

// An LED. Counts how many times it lit and how long it stayed lit.
//
// A floating pin counts as off: no drive means no current, which is the whole
// difference between a pin configured as an output and one left as an input.
class Led : public PartBase {
public:
    // active_low models the other common wiring -- anode to Vcc, cathode to the
    // pin -- where the sketch pulls the pin down to light it. Both are ordinary
    // wiring, and guessing wrong inverts every reading this part produces.
    Led(uint32_t clock_hz, bool active_low = false, std::string colour = {})
        : PartBase(clock_hz, std::move(colour)), active_low_(active_low) {}

    std::string kind() const override { return "led"; }
    void pin_changed(int pin, PinState state, uint64_t cycles) override;
    std::string describe() const override;

    void advance(uint64_t cycles) override;

    bool on() const { return on_; }

    // Off-to-on transitions. This is the count a blink sketch is judged by, so
    // it deliberately counts lightings rather than edges of either direction.
    uint32_t blinks() const { return blinks_; }
    uint32_t transitions() const { return transitions_; }

    // Time lit, up to `as_of`. An LED that is still on has an interval that has
    // not closed yet, so the caller has to say when "now" is; asking for the
    // total without one would report the time up to the last edge and be
    // quietly short by however long it has been lit since.
    uint64_t lit_cycles(uint64_t as_of) const;
    double lit_us(uint64_t as_of) const { return cycles_to_us(lit_cycles(as_of)); }

    // Forward voltage and colour are display-only. Nothing here simulates
    // current, and pretending to would be a model of a circuit rather than of a
    // pin.
    void set_forward_voltage(double volts) { forward_volts_ = volts; }
    double forward_voltage() const { return forward_volts_; }

private:
    bool level_lights(PinState state) const;

    bool active_low_ = false;
    bool on_ = false;
    uint32_t blinks_ = 0;
    uint32_t transitions_ = 0;
    uint64_t now_ = 0;
    uint64_t lit_since_ = 0;      // valid only while on_
    uint64_t lit_total_ = 0;      // closed intervals only
    double forward_volts_ = 0.0;
};

// =============================================================== button ====

// A momentary push button to ground, with the sketch's internal pull-up
// assumed. Pressed pulls the pin low; released drives nothing at all and lets
// the pull-up carry the pin high. Driving high on release would be wrong for
// the common wiring and would fight a sketch that drives the pin as an output.
class Button : public PartBase {
public:
    explicit Button(uint32_t clock_hz) : PartBase(clock_hz) {}

    std::string kind() const override { return "button"; }
    void pin_changed(int pin, PinState state, uint64_t cycles) override;
    PinState drive(int pin) const override;
    std::string describe() const override;

    void advance(uint64_t cycles) override;

    // Restricts this button to one pin. Left unset it answers for whatever pin
    // it is asked about, which is right for the one-pin wiring but wrong if a
    // caller ever wires one button across several pins.
    void set_pin(int pin) { pin_ = pin; }
    int pin() const { return pin_; }

    bool pressed() const { return pressed_; }

    // Scripted input. The cycle is when the human's finger arrives, so events
    // must be scheduled in increasing order; an out-of-order one is rejected
    // rather than silently sorted in, because the surrounding script has
    // probably miscomputed and reordering would hide that.
    bool press(uint64_t at_cycle);
    bool release(uint64_t at_cycle);

    // Contact bounce, off by default.
    //
    // Real switches chatter for a millisecond or so on each transition, and a
    // sketch that miscounts presses only when they do is a genuine finding. But
    // bounce on by default would make every test of every unrelated part depend
    // on chatter timing, so it is opt-in: a run without it is the clean case,
    // and a run with it is the interesting one.
    void set_bounce(bool enabled) { bounce_ = enabled; }
    void set_bounce_profile(double bounce_us, uint32_t edges);
    bool bouncing() const { return bounce_; }

    // Edges this button has actually produced, bounce included. Without bounce
    // this is one per scripted event.
    uint32_t edges() const { return uint32_t(edges_.size()); }
    uint32_t edges_delivered() const { return delivered_; }

private:
    void schedule(uint64_t at_cycle, bool pressed);

    struct Edge { uint64_t cycle; bool pressed; };

    int pin_ = -1;
    bool pressed_ = false;
    bool bounce_ = false;
    double bounce_us_ = 1500.0;
    uint32_t bounce_edges_ = 4;
    uint64_t now_ = 0;
    uint64_t last_scheduled_ = 0;
    bool has_scheduled_ = false;
    uint32_t delivered_ = 0;
    std::vector<Edge> edges_;
};

// ======================================================== potentiometer ====

// A potentiometer. Unlike everything else here it is not on a digital pin at
// all: the wiper goes to an analog input, and the sketch reads it through
// analogRead. So it reports an analog input number and a voltage rather than
// driving a PinState, and a caller feeds that to Machine::set_analog_volts.
//
// Modelling it as a digital part with a made-up level would be the wrong shape
// entirely -- there is no level on that pin to speak of.
class Potentiometer : public PartBase {
public:
    // supply_volts is the voltage across the track, which is what the wiper
    // divides. It is a property of the wiring, not of the board's clock, so a
    // default is honest here.
    Potentiometer(uint32_t clock_hz, int analog_input = 0, double supply_volts = 5.0)
        : PartBase(clock_hz), analog_input_(analog_input), supply_volts_(supply_volts) {}

    std::string kind() const override { return "potentiometer"; }
    void pin_changed(int pin, PinState state, uint64_t cycles) override;
    std::string describe() const override;

    int analog_input() const { return analog_input_; }
    void set_analog_input(int analog_input) { analog_input_ = analog_input; }

    // Wiper position, 0 at ground and 1 at the supply. Values outside that
    // range are clamped: a knob cannot be turned past its stop.
    void set_position(double fraction);
    double position() const { return position_; }

    void set_supply_volts(double volts) { supply_volts_ = volts; }
    double supply_volts() const { return supply_volts_; }
    double volts() const { return position_ * supply_volts_; }

    // What a 10-bit conversion against `vref` would return. This mirrors the
    // ADC's own rounding rather than re-deriving it, so a test can predict what
    // the sketch will read.
    uint16_t expected_reading(double vref) const;

private:
    int analog_input_ = 0;
    double supply_volts_ = 5.0;
    double position_ = 0.0;
};

// ================================================================ servo ====

// A hobby servo, decoding the standard pulse on its signal pin.
//
// The angle comes from the measured interval between the rising and falling
// edge, converted through the clock. That is the point of the exercise: if
// pin_changed's cycle stamps are wrong, this part reports the wrong angle, and
// it reports it differently at different clock rates.
class Servo : public PartBase {
public:
    // The 1000-2000 us convention is the standard one, but plenty of servos
    // want 544-2400, so it is a parameter.
    Servo(uint32_t clock_hz, double min_us = 1000.0, double max_us = 2000.0,
          double travel_degrees = 180.0)
        : PartBase(clock_hz), min_us_(min_us), max_us_(max_us), travel_(travel_degrees) {}

    std::string kind() const override { return "servo"; }
    void pin_changed(int pin, PinState state, uint64_t cycles) override;
    std::string describe() const override;

    // Last commanded angle, in degrees. Before any pulse arrives there is no
    // angle to report and this stays at the resting value, which is why
    // pulses() exists: a test should check that a pulse was seen at all rather
    // than reading 0 degrees and assuming it meant something.
    double angle() const { return angle_; }
    double last_pulse_us() const { return last_pulse_us_; }
    uint32_t pulses() const { return pulses_; }

    // Pulses whose width was nowhere near the servo range. These are counted
    // and discarded rather than clamped into the range, because a 20 ms low
    // period misread as a pulse would otherwise look like a legitimate command
    // to travel to the end stop.
    uint32_t rejected() const { return rejected_; }

    // Pulses that arrived while this servo had no clock rate. A width in cycles
    // cannot be judged against a width in microseconds, so such a pulse is
    // neither decoded nor rejected -- it is counted here and reported by
    // describe(), because the alternative is a servo that quietly reads 0
    // degrees for a sketch that is driving it perfectly well.
    uint32_t undecoded() const { return undecoded_; }

    // Interval between the last two rising edges, in microseconds -- the refresh
    // rate the sketch is producing, nominally 20000.
    double frame_us() const { return frame_us_; }

private:
    double min_us_ = 1000.0, max_us_ = 2000.0, travel_ = 180.0;
    bool high_ = false;
    uint64_t rise_cycle_ = 0;
    uint64_t last_rise_ = 0;
    bool have_last_rise_ = false;
    double angle_ = 0.0;
    double last_pulse_us_ = 0.0;
    double frame_us_ = 0.0;
    uint32_t pulses_ = 0;
    uint32_t rejected_ = 0;
    uint32_t undecoded_ = 0;
};

// ============================================================== buzzer =====

// A piezo buzzer, which here is a frequency counter: it measures the square
// wave a tone() sketch puts on its pin.
//
// Frequency is taken from the interval between consecutive rising edges. A
// single period is enough to report a frequency, and reporting it from one
// period rather than averaging means a tone change shows up on the first period
// of the new tone instead of being smeared across the old one.
class PiezoBuzzer : public PartBase {
public:
    explicit PiezoBuzzer(uint32_t clock_hz) : PartBase(clock_hz) {}

    std::string kind() const override { return "buzzer"; }
    void pin_changed(int pin, PinState state, uint64_t cycles) override;
    std::string describe() const override;

    // Frequency of the most recent complete period, in Hz. Zero until two
    // rising edges have been seen, because one edge is not a period.
    double frequency_hz() const;

    // Mean frequency over every complete period since reset. Useful for a wave
    // that is meant to be steady, where a single period could be an artefact of
    // where the run happened to stop.
    double mean_frequency_hz() const;

    uint32_t periods() const { return periods_; }
    uint32_t edges() const { return edges_; }

    // Fraction of the measured span the pin spent high. A tone() square wave is
    // near 0.5; a PWM part driving the same pin is not, and that difference is
    // the cheap way to tell them apart.
    double duty() const;

    bool audible() const { return periods_ > 0; }

private:
    bool high_ = false;
    uint64_t last_rise_ = 0;
    bool have_last_rise_ = false;
    uint64_t high_since_ = 0;
    uint64_t high_cycles_ = 0;
    uint64_t pending_high_ = 0;
    uint64_t span_cycles_ = 0;
    uint64_t period_total_ = 0;
    uint32_t periods_ = 0;
    uint32_t edges_ = 0;
    // The measurement is kept as a cycle count, not as a frequency. A part
    // built by the registry gets its clock rate after it is constructed, and a
    // frequency computed at edge time would have been divided by whatever the
    // rate was then and never corrected.
    uint64_t last_period_ = 0;
};

// ======================================================= shift register ====

// A 74HC595 serial-in, parallel-out shift register: three pins, and the byte
// only appears on the outputs when the latch is clocked.
//
// This is the multi-pin case. It matters that the data pin's level is
// remembered between clock edges rather than sampled from a pin_changed, since
// a sketch sets data once and then clocks -- the data pin does not move on the
// clock edge, so a part that only looked at the pin it was just told about
// would shift in nothing.
class ShiftRegister : public PartBase {
public:
    // Pins default to unassigned. A register whose pins were never set reports
    // an error from describe() and ignores edges, rather than treating pin 0 as
    // its data line and reconstructing a byte out of unrelated traffic.
    explicit ShiftRegister(uint32_t clock_hz, int data_pin = -1, int clock_pin = -1,
                           int latch_pin = -1, uint8_t width_bits = 8)
        : PartBase(clock_hz), data_pin_(data_pin), clock_pin_(clock_pin),
          latch_pin_(latch_pin), width_(width_bits ? width_bits : 8) {}

    std::string kind() const override { return "shift_register"; }
    void pin_changed(int pin, PinState state, uint64_t cycles) override;
    std::string describe() const override;

    void set_pins(int data_pin, int clock_pin, int latch_pin);
    bool configured() const;

    // The latched outputs -- what the eight parallel pins are actually showing.
    uint32_t output() const { return output_; }
    // The undisplayed contents of the shift register, for a test that wants to
    // check the shifting itself rather than the latching.
    uint32_t shifted() const { return shift_ & mask(); }

    uint32_t latches() const { return latches_; }
    uint32_t clocks() const { return clocks_; }

    // Set when an edge arrived on a pin this part was never told about, or
    // before its pins were assigned. Empty when nothing has gone wrong.
    const std::string& error() const { return error_; }

private:
    uint32_t mask() const {
        return width_ >= 32 ? 0xFFFFFFFFu : ((1u << width_) - 1u);
    }

    int data_pin_ = -1, clock_pin_ = -1, latch_pin_ = -1;
    uint8_t width_ = 8;
    bool data_level_ = false;
    bool clock_level_ = false;
    bool latch_level_ = false;
    uint32_t shift_ = 0;
    uint32_t output_ = 0;
    uint32_t clocks_ = 0;
    uint32_t latches_ = 0;
    std::string error_;
};

// ============================================================= registry ====

// Parts by name, because the CLI takes a name on the command line and a
// breadboard UI will offer a menu. Neither can use a compile-time list.
//
// Kind names are lowercase, and a two-word kind is joined with an underscore.
// They are user-facing CLI syntax, so they do not change once shipped, and
// there are no aliases: one spelling per part means the list in a help message
// and the list in an error message are the same list.
//
// Nothing beyond the kind is encoded in the name. Every part the registry
// builds is fully constructed with defaults, and anything else -- an LED's
// colour, which analog input a potentiometer feeds, a shift register's three
// pins -- is set afterwards through the concrete type.

// Every kind create_part accepts, for help text and error messages.
std::vector<std::string> part_kinds();

// Creates a part by kind name ("led", "button", "servo"). Returns nullptr and
// sets `error` for an unknown kind, naming what was asked for and listing what
// is available.
//
// The part comes back with its clock rate unset, since a kind on a command line
// says nothing about the board. Call set_part_clock before running anything
// whose timing matters.
std::unique_ptr<Part> create_part(std::string_view kind, std::string& error);

// As above, at a known clock. This is the form to prefer wherever the board is
// already in hand, because it leaves no second step to forget.
std::unique_ptr<Part> create_part(std::string_view kind, uint32_t clock_hz,
                                  std::string& error);

// Sets the clock rate on any part from this library without the caller having
// to know its concrete type. Returns false for a part that is not one of these,
// which is the only way a caller can tell that its rate went nowhere.
bool set_part_clock(Part* part, uint32_t clock_hz);

} // namespace ardio::emu
