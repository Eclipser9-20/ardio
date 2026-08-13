// The virtual breadboard's parts.
//
// These are written against pins, not registers, and the discipline that keeps
// them honest is that none of them may look at anything else. A servo here
// decodes a pulse the same way a servo on a desk does: it watches its signal
// line go up and come back down and measures how long that took. If the board
// mistimes the edge, the angle is wrong, and that is the correct outcome -- a
// part that reached into the timer to find out what the sketch meant would
// report the right angle on a broken board and be useless as a test.
//
// Two conventions run through the whole file.
//
// A floating pin is not a low pin. Nothing is driving it, which for an output
// device means no current and for an input device means the pull-up decides. So
// an LED with a floating pin is dark rather than lit, and a servo treats a pin
// that stops being driven as the end of a pulse rather than as a level to
// measure. Collapsing Floating into Low would make a sketch that forgot
// pinMode(OUTPUT) look like a sketch that works.
//
// Time is cycles until the last possible moment. Every interval is accumulated
// as a cycle count and converted to microseconds only when someone asks for a
// number, so nothing accumulates rounding across a long run, and a part whose
// clock rate is not known yet still records everything it saw and can report it
// correctly once the rate arrives.

#include "ardio/emu/parts.h"

#include <algorithm>
#include <cmath>

namespace ardio::emu {
namespace {

// A pin's level for a part that only cares whether current flows. Floating is
// not high: see the note above.
bool driven_high(PinState state) { return state == PinState::High; }

std::string round_to_string(double value, int places) {
    double scale = std::pow(10.0, places);
    double r = std::round(value * scale) / scale;
    std::string s = std::to_string(r);
    // std::to_string always gives six decimals; trim to the ones asked for so a
    // describe() line does not read "1500.000000".
    std::string::size_type dot = s.find('.');
    if (dot == std::string::npos) return s;
    if (places <= 0) return s.substr(0, dot);
    if (s.size() > dot + 1 + std::string::size_type(places))
        s.resize(dot + 1 + std::string::size_type(places));
    return s;
}

} // namespace

// ================================================================== LED ====

bool Led::level_lights(PinState state) const {
    if (state == PinState::Floating) return false;
    return active_low_ ? (state == PinState::Low) : driven_high(state);
}

void Led::advance(uint64_t cycles) {
    if (cycles > now_) now_ = cycles;
}

void Led::pin_changed(int pin, PinState state, uint64_t cycles) {
    (void)pin;
    advance(cycles);
    bool lit = level_lights(state);
    if (lit == on_) return;   // a change on the pin that does not change the LED

    ++transitions_;
    if (lit) {
        ++blinks_;
        lit_since_ = cycles;
    } else {
        // Close the interval here rather than at whatever "now" turns out to
        // be. The LED went dark at this cycle and no later one.
        lit_total_ += cycles - lit_since_;
    }
    on_ = lit;
}

uint64_t Led::lit_cycles(uint64_t as_of) const {
    uint64_t total = lit_total_;
    if (on_ && as_of > lit_since_) total += as_of - lit_since_;
    return total;
}

std::string Led::describe() const {
    std::string s = "led";
    if (!label_.empty()) s += " (" + label_ + ")";
    s += on_ ? ": on" : ": off";
    s += " (blinked " + std::to_string(blinks_) + "x)";
    if (clock_hz_ == 0) s += ", clock rate not set";
    return s;
}

// =============================================================== button ====

void Button::set_bounce_profile(double bounce_us, uint32_t edges) {
    bounce_us_ = bounce_us < 0.0 ? 0.0 : bounce_us;
    bounce_edges_ = edges;
}

void Button::schedule(uint64_t at_cycle, bool pressed) {
    // Bounce is a burst of chatter that settles on the commanded state. The
    // final edge is placed at the commanded cycle and the chatter is put
    // BEFORE it, so enabling bounce never moves the moment the button actually
    // settles. A script that presses at cycle N and expects the sketch to see a
    // press by cycle N therefore still holds, and the only difference bounce
    // makes is the mess on the way in -- which is the difference worth testing.
    if (bounce_ && bounce_edges_ > 0 && clock_hz_ != 0 && bounce_us_ > 0.0) {
        uint64_t span = uint64_t(bounce_us_ * double(clock_hz_) / 1e6);
        uint64_t step = span / (bounce_edges_ + 1);
        if (step == 0) step = 1;
        uint64_t start = at_cycle > step * bounce_edges_ ? at_cycle - step * bounce_edges_ : 0;
        bool level = pressed;
        for (uint32_t i = 0; i < bounce_edges_; ++i) {
            edges_.push_back({start + step * i, level});
            level = !level;
        }
    }
    edges_.push_back({at_cycle, pressed});
}

bool Button::press(uint64_t at_cycle) {
    if (has_scheduled_ && at_cycle <= last_scheduled_) return false;
    schedule(at_cycle, true);
    last_scheduled_ = at_cycle;
    has_scheduled_ = true;
    return true;
}

bool Button::release(uint64_t at_cycle) {
    if (has_scheduled_ && at_cycle <= last_scheduled_) return false;
    schedule(at_cycle, false);
    last_scheduled_ = at_cycle;
    has_scheduled_ = true;
    return true;
}

void Button::advance(uint64_t cycles) {
    if (cycles < now_) return;
    now_ = cycles;
    // Every edge at or before now has happened. Applying them all rather than
    // one per call means a coarse run loop still ends up at the right final
    // state, though it will step over the chatter in between -- which is what a
    // sketch that only samples the pin occasionally would also do.
    while (delivered_ < edges_.size() && edges_[delivered_].cycle <= cycles) {
        pressed_ = edges_[delivered_].pressed;
        ++delivered_;
    }
}

void Button::pin_changed(int pin, PinState state, uint64_t cycles) {
    (void)state;
    // A button does not read its pin -- the sketch drives the pull-up on it and
    // the button either shorts it to ground or does not. The edge is still
    // useful as a clock: it says what cycle it is, which is how a scripted
    // press gets applied on a board that never calls advance.
    if (pin_ < 0 || pin == pin_) advance(cycles);
}

PinState Button::drive(int pin) const {
    if (pin_ >= 0 && pin != pin_) return PinState::Floating;
    // Released means driving nothing, not driving high: the switch is open and
    // the sketch's pull-up is what carries the pin.
    return pressed_ ? PinState::Low : PinState::Floating;
}

std::string Button::describe() const {
    std::string s = "button";
    if (pin_ >= 0) s += " d" + std::to_string(pin_);
    s += pressed_ ? ": pressed" : ": released";
    if (bounce_) s += " (bounce on)";
    return s;
}

// ======================================================== potentiometer ====

void Potentiometer::set_position(double fraction) {
    position_ = std::clamp(fraction, 0.0, 1.0);
}

void Potentiometer::pin_changed(int pin, PinState state, uint64_t cycles) {
    // Nothing to do. A potentiometer's wiper sits on an analog input, and the
    // sketch reads it through the ADC rather than through a pin level, so a
    // digital edge on it says nothing about the knob.
    (void)pin;
    (void)state;
    (void)cycles;
}

uint16_t Potentiometer::expected_reading(double vref) const {
    if (vref <= 0.0) return 0;
    // Truncating, and clamping at full scale rather than wrapping -- the same
    // arithmetic the ADC does, so this predicts the sketch's reading instead of
    // approximating it.
    double raw = std::floor(volts() / vref * 1024.0);
    if (raw < 0.0) return 0;
    if (raw > 1023.0) return 1023;
    return uint16_t(raw);
}

std::string Potentiometer::describe() const {
    return "potentiometer a" + std::to_string(analog_input_) + ": " +
           round_to_string(volts(), 2) + "V of " +
           round_to_string(supply_volts_, 2) + "V";
}

// ================================================================ servo ====

void Servo::pin_changed(int pin, PinState state, uint64_t cycles) {
    (void)pin;
    bool high = driven_high(state);
    if (high == high_) return;

    if (high) {
        if (have_last_rise_ && cycles > last_rise_)
            frame_us_ = cycles_to_us(cycles - last_rise_);
        last_rise_ = cycles;
        have_last_rise_ = true;
        rise_cycle_ = cycles;
        high_ = true;
        return;
    }

    high_ = false;
    if (cycles < rise_cycle_) return;
    if (clock_hz_ == 0) { ++undecoded_; return; }
    double us = cycles_to_us(cycles - rise_cycle_);

    // A pulse well outside the servo's range is not a command. Real signal
    // lines carry all sorts of things -- a pin held high through setup, a
    // sketch driving the same pin for something else -- and clamping those into
    // the travel range would report a confident angle for a signal that was
    // never a servo pulse at all. The margin is half the range on either side,
    // which is wide enough to accept a sloppy sketch and narrow enough to
    // reject a 20 ms level.
    double margin = (max_us_ - min_us_) * 0.5;
    if (us < min_us_ - margin || us > max_us_ + margin) {
        ++rejected_;
        return;
    }

    last_pulse_us_ = us;
    ++pulses_;
    double span = max_us_ - min_us_;
    double fraction = span > 0.0 ? (us - min_us_) / span : 0.0;
    angle_ = std::clamp(fraction, 0.0, 1.0) * travel_;
}

std::string Servo::describe() const {
    std::string s = "servo: " + round_to_string(angle_, 1) + " deg";
    if (pulses_ == 0) s += " (no pulse seen)";
    else s += " (" + round_to_string(last_pulse_us_, 0) + "us)";
    if (clock_hz_ == 0)
        s += ", clock rate not set (" + std::to_string(undecoded_) + " pulses undecoded)";
    return s;
}

// =============================================================== buzzer ====

void PiezoBuzzer::pin_changed(int pin, PinState state, uint64_t cycles) {
    (void)pin;
    bool high = driven_high(state);
    if (high == high_) return;
    ++edges_;

    if (high) {
        high_since_ = cycles;
        if (have_last_rise_ && cycles > last_rise_) {
            uint64_t period = cycles - last_rise_;
            period_total_ += period;
            span_cycles_ += period;
            ++periods_;
            // The high time banked since the previous rising edge belongs to
            // the period that just closed. It is only counted here, once that
            // period is complete, so the numerator and the denominator of the
            // duty always cover exactly the same span -- an incomplete leading
            // or trailing pulse counts toward neither.
            high_cycles_ += pending_high_;
            pending_high_ = 0;
            // The single most recent period, not an average. A tone that
            // changes should be reported as changed on the first period of the
            // new frequency; averaging would report a frequency the sketch
            // never played.
            last_period_ = period;
        }
        last_rise_ = cycles;
        have_last_rise_ = true;
    } else if (cycles > high_since_) {
        pending_high_ += cycles - high_since_;
    }
    high_ = high;
}

double PiezoBuzzer::frequency_hz() const {
    if (last_period_ == 0 || clock_hz_ == 0) return 0.0;
    return double(clock_hz_) / double(last_period_);
}

double PiezoBuzzer::mean_frequency_hz() const {
    if (periods_ == 0 || period_total_ == 0 || clock_hz_ == 0) return 0.0;
    double mean_period = double(period_total_) / double(periods_);
    return double(clock_hz_) / mean_period;
}

double PiezoBuzzer::duty() const {
    if (span_cycles_ == 0) return 0.0;
    return double(high_cycles_) / double(span_cycles_);
}

std::string PiezoBuzzer::describe() const {
    if (periods_ == 0) return "buzzer: silent";
    return "buzzer: " + round_to_string(frequency_hz(), 1) + "Hz";
}

// ======================================================= shift register ====

void ShiftRegister::set_pins(int data_pin, int clock_pin, int latch_pin) {
    data_pin_ = data_pin;
    clock_pin_ = clock_pin;
    latch_pin_ = latch_pin;
    error_.clear();
}

bool ShiftRegister::configured() const {
    if (data_pin_ < 0 || clock_pin_ < 0 || latch_pin_ < 0) return false;
    // Two of its three lines on one pin is a wiring mistake, not a
    // configuration: it would make every clock edge also a latch edge and the
    // reconstructed byte would be garbage that still looked like a byte.
    return data_pin_ != clock_pin_ && data_pin_ != latch_pin_ && clock_pin_ != latch_pin_;
}

void ShiftRegister::pin_changed(int pin, PinState state, uint64_t cycles) {
    (void)cycles;
    if (!configured()) {
        error_ = "shift register received an edge before its data, clock and "
                 "latch pins were assigned";
        return;
    }

    bool high = driven_high(state);
    if (pin == data_pin_) {
        // Held, not sampled here. The sketch sets data and then clocks, so the
        // data line does not move on the clock edge and the level has to
        // survive between edges.
        data_level_ = high;
        return;
    }
    if (pin == clock_pin_) {
        if (high && !clock_level_) {
            // Rising edge shifts one bit in at the bottom, so the first bit
            // clocked ends up highest -- which is what shiftOut(MSBFIRST)
            // produces and why a test can compare against a literal byte.
            shift_ = ((shift_ << 1) | (data_level_ ? 1u : 0u)) & mask();
            ++clocks_;
        }
        clock_level_ = high;
        return;
    }
    if (pin == latch_pin_) {
        if (high && !latch_level_) {
            output_ = shift_ & mask();
            ++latches_;
        }
        latch_level_ = high;
        return;
    }

    // An edge on a pin this part was never told about. Silently ignoring it
    // would hide a mis-wiring that produces a plausible wrong byte.
    error_ = "shift register saw an edge on pin " + std::to_string(pin) +
             ", which is not its data, clock or latch pin";
}

std::string ShiftRegister::describe() const {
    if (!configured()) return "shift_register: pins not assigned";
    std::string s = "shift_register: 0x";
    const char* hex = "0123456789ABCDEF";
    uint8_t nibbles = uint8_t((width_ + 3) / 4);
    for (int i = nibbles - 1; i >= 0; --i) s += hex[(output_ >> (i * 4)) & 0xF];
    s += " (" + std::to_string(latches_) + " latched)";
    if (!error_.empty()) s += " -- " + error_;
    return s;
}

// ============================================================= registry ====

std::vector<std::string> part_kinds() {
    // Alphabetical, so the help text and the error message read the same way
    // and neither has to be kept in sync with a construction order.
    return {"button", "buzzer", "led", "potentiometer", "servo", "shift_register"};
}

std::unique_ptr<Part> create_part(std::string_view kind, uint32_t clock_hz,
                                  std::string& error) {
    error.clear();
    if (kind == "button") return std::make_unique<Button>(clock_hz);
    if (kind == "buzzer") return std::make_unique<PiezoBuzzer>(clock_hz);
    if (kind == "led") return std::make_unique<Led>(clock_hz);
    if (kind == "potentiometer") return std::make_unique<Potentiometer>(clock_hz);
    if (kind == "servo") return std::make_unique<Servo>(clock_hz);
    if (kind == "shift_register") return std::make_unique<ShiftRegister>(clock_hz);

    // Name the thing that was asked for and list what there is. A user has just
    // mistyped a name on a command line, and "unknown part" on its own gives
    // them nothing to correct it with.
    error = "unknown part '" + std::string(kind) + "' -- available: ";
    std::vector<std::string> kinds = part_kinds();
    for (std::size_t i = 0; i < kinds.size(); ++i) {
        if (i) error += ", ";
        error += kinds[i];
    }
    return nullptr;
}

std::unique_ptr<Part> create_part(std::string_view kind, std::string& error) {
    // Clock unset: a kind named on a command line does not say which board it
    // is going onto. set_part_clock supplies it once the board is resolved, and
    // a part that never gets one reports zeroes and says so rather than
    // measuring against an invented rate.
    return create_part(kind, 0, error);
}

bool set_part_clock(Part* part, uint32_t clock_hz) {
    PartBase* base = dynamic_cast<PartBase*>(part);
    if (!base) return false;
    base->set_clock_hz(clock_hz);
    return true;
}

} // namespace ardio::emu
