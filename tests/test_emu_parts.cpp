// Tests for the virtual breadboard's parts.
//
// Every cycle count below is computed by hand from a clock rate and a time --
// 1500 us at 16 MHz is 24000 cycles, and the same 1500 us at 8 MHz is 12000 --
// rather than read back out of the implementation. That is the whole point of
// testing a servo at two clock rates: the same pulse in microseconds is a
// different number of cycles on each board, and a part that had a rate baked
// into it would pass at one and fail at the other.
//
// Parts are driven directly with pin_changed. A part sees pins, so a test that
// gives it pins is testing exactly the interface a real board presents, and it
// does not need a machine to do it.

#include "harness.h"

#include "ardio/emu/parts.h"

#include <cstdint>
#include <memory>
#include <string>

using ardio::emu::Button;
using ardio::emu::Led;
using ardio::emu::Part;
using ardio::emu::PiezoBuzzer;
using ardio::emu::PinState;
using ardio::emu::Potentiometer;
using ardio::emu::Servo;
using ardio::emu::ShiftRegister;

namespace {

constexpr uint32_t k16MHz = 16000000;
constexpr uint32_t k8MHz = 8000000;

// Cycles for a duration in microseconds at a given clock. Written out rather
// than called on the part under test, so the test's arithmetic is independent
// of the implementation's.
uint64_t us_cycles(double us, uint32_t clock_hz) {
    return uint64_t(us * double(clock_hz) / 1e6);
}

} // namespace

// ================================================================== LED ====

TEST(led_starts_dark_and_counts_nothing) {
    Led led(k16MHz);
    CHECK(!led.on());
    CHECK_EQ(int(led.blinks()), 0);
    CHECK_EQ(int(led.lit_cycles(1000000)), 0);
}

TEST(led_counts_blinks_and_accumulates_lit_time) {
    Led led(k16MHz);
    // Four 100 ms flashes with 100 ms gaps, at 16 MHz: 1600000 cycles each.
    const uint64_t span = 1600000;
    uint64_t t = 0;
    for (int i = 0; i < 4; ++i) {
        led.pin_changed(13, PinState::High, t);
        t += span;
        led.pin_changed(13, PinState::Low, t);
        t += span;
    }
    CHECK_EQ(int(led.blinks()), 4);
    CHECK_EQ(int(led.transitions()), 8);
    CHECK(!led.on());
    CHECK_EQ(int(led.lit_cycles(t)), int(4 * span));
    // 4 * 100 ms is 400000 us.
    CHECK(led.lit_us(t) > 399999.0 && led.lit_us(t) < 400001.0);
}

TEST(led_still_on_counts_time_up_to_the_moment_asked_about) {
    Led led(k16MHz);
    led.pin_changed(13, PinState::High, 1000);
    CHECK(led.on());
    // The interval has not closed. Asking as of cycle 5000 must include the
    // 4000 cycles it has been lit so far, not stop at the last edge.
    CHECK_EQ(int(led.lit_cycles(5000)), 4000);
    CHECK_EQ(int(led.lit_cycles(9000)), 8000);
    led.pin_changed(13, PinState::Low, 9000);
    // Once closed, a later "as of" adds nothing.
    CHECK_EQ(int(led.lit_cycles(90000)), 8000);
}

TEST(led_treats_a_floating_pin_as_dark) {
    Led led(k16MHz);
    led.pin_changed(13, PinState::High, 0);
    CHECK(led.on());
    led.pin_changed(13, PinState::Floating, 100);
    CHECK(!led.on());
    CHECK_EQ(int(led.lit_cycles(200)), 100);
}

TEST(led_repeated_same_level_is_not_a_blink) {
    Led led(k16MHz);
    led.pin_changed(13, PinState::High, 0);
    led.pin_changed(13, PinState::High, 500);
    led.pin_changed(13, PinState::High, 900);
    CHECK_EQ(int(led.blinks()), 1);
    CHECK_EQ(int(led.transitions()), 1);
    CHECK_EQ(int(led.lit_cycles(1000)), 1000);
}

TEST(active_low_led_lights_when_the_pin_is_pulled_down) {
    Led led(k16MHz, /*active_low=*/true);
    led.pin_changed(13, PinState::High, 0);
    CHECK(!led.on());
    led.pin_changed(13, PinState::Low, 1000);
    CHECK(led.on());
    CHECK_EQ(int(led.blinks()), 1);
    led.pin_changed(13, PinState::High, 3000);
    CHECK_EQ(int(led.lit_cycles(4000)), 2000);
}

TEST(led_forward_voltage_and_colour_are_display_only) {
    Led led(k16MHz, false, "red");
    led.set_forward_voltage(2.1);
    CHECK(led.forward_voltage() > 2.09 && led.forward_voltage() < 2.11);
    CHECK(led.label() == "red");
    // Nothing about the label changes what the part measures.
    led.pin_changed(9, PinState::High, 0);
    led.pin_changed(9, PinState::Low, 100);
    CHECK_EQ(int(led.lit_cycles(100)), 100);
}

// =============================================================== button ====

TEST(button_released_drives_nothing_and_pressed_drives_low) {
    Button b(k16MHz);
    // Released, the switch is open: the sketch's pull-up carries the pin, so
    // the part must not drive it at all.
    CHECK(b.drive(2) == PinState::Floating);
    CHECK(!b.pressed());

    b.press(1000);
    b.advance(1000);
    CHECK(b.pressed());
    CHECK(b.drive(2) == PinState::Low);

    b.release(2000);
    b.advance(2000);
    CHECK(!b.pressed());
    CHECK(b.drive(2) == PinState::Floating);
}

TEST(button_script_applies_only_when_its_cycle_arrives) {
    Button b(k16MHz);
    b.press(5000);
    b.release(9000);

    b.advance(4999);
    CHECK(!b.pressed());
    b.advance(5000);
    CHECK(b.pressed());
    b.advance(8999);
    CHECK(b.pressed());
    b.advance(9000);
    CHECK(!b.pressed());
}

TEST(button_pin_edge_advances_the_script) {
    Button b(k16MHz);
    b.set_pin(2);
    b.press(4000);
    // No advance() call at all: the only thing that tells this part the time is
    // an edge on its own pin, which is what a board that never advances parts
    // would give it.
    b.pin_changed(2, PinState::High, 4000);
    CHECK(b.pressed());
    CHECK(b.drive(2) == PinState::Low);
    // And it does not answer for a pin it is not on.
    CHECK(b.drive(7) == PinState::Floating);
}

TEST(button_rejects_an_out_of_order_script) {
    Button b(k16MHz);
    CHECK(b.press(5000));
    CHECK(!b.release(4000));
    CHECK(!b.release(5000));
    CHECK(b.release(5001));
}

TEST(button_without_bounce_produces_exactly_one_edge_per_event) {
    Button b(k16MHz);
    b.press(10000);
    b.release(20000);
    CHECK_EQ(int(b.edges()), 2);

    // And the state is clean the whole way: sampling every 100 cycles across
    // the press finds no chatter.
    Button c(k16MHz);
    c.press(10000);
    int flips = 0;
    bool prev = c.pressed();
    for (uint64_t t = 0; t <= 20000; t += 100) {
        c.advance(t);
        if (c.pressed() != prev) { ++flips; prev = c.pressed(); }
    }
    CHECK_EQ(flips, 1);
}

TEST(button_bounce_produces_chatter_only_when_enabled) {
    // 1000 us of bounce at 16 MHz is 16000 cycles, with 4 chatter edges before
    // the settling edge -- so the mess is entirely inside [ 64000, 80000 ].
    Button b(k16MHz);
    b.set_bounce(true);
    b.set_bounce_profile(1000.0, 4);
    b.press(80000);
    CHECK(b.bouncing());
    CHECK_EQ(int(b.edges()), 5);

    // Before the bounce window opens, still released.
    b.advance(63000);
    CHECK(!b.pressed());

    // Across the window the state flips more than once. Stepping finely enough
    // to see each 3200-cycle segment.
    int flips = 0;
    bool prev = b.pressed();
    for (uint64_t t = 63000; t <= 80000; t += 200) {
        b.advance(t);
        if (b.pressed() != prev) { ++flips; prev = b.pressed(); }
    }
    CHECK(flips > 1);

    // However messy the way in, it settles pressed at the commanded cycle and
    // stays there -- enabling bounce must not move when the press lands.
    b.advance(80000);
    CHECK(b.pressed());
    b.advance(200000);
    CHECK(b.pressed());
}

TEST(button_bounce_off_by_default) {
    Button b(k16MHz);
    CHECK(!b.bouncing());
    b.set_bounce_profile(1000.0, 4);   // configured, but not turned on
    b.press(80000);
    CHECK_EQ(int(b.edges()), 1);
}

// ======================================================== potentiometer ====

TEST(potentiometer_reports_an_analog_input_not_a_pin_level) {
    Potentiometer pot(k16MHz, 0);
    CHECK_EQ(pot.analog_input(), 0);
    // It never drives a digital pin, whatever happens on one.
    pot.pin_changed(14, PinState::High, 1000);
    CHECK(pot.drive(14) == PinState::Floating);
}

TEST(potentiometer_position_divides_the_supply) {
    Potentiometer pot(k16MHz, 3, 5.0);
    pot.set_position(0.0);
    CHECK(pot.volts() < 0.0001);
    pot.set_position(1.0);
    CHECK(pot.volts() > 4.9999 && pot.volts() < 5.0001);
    pot.set_position(0.5);
    CHECK(pot.volts() > 2.4999 && pot.volts() < 2.5001);
}

TEST(potentiometer_clamps_at_its_stops) {
    Potentiometer pot(k16MHz, 0, 5.0);
    pot.set_position(-3.0);
    CHECK_EQ(int(pot.expected_reading(5.0)), 0);
    pot.set_position(9.0);
    CHECK_EQ(int(pot.expected_reading(5.0)), 1023);
}

TEST(potentiometer_predicts_the_ten_bit_reading) {
    Potentiometer pot(k16MHz, 0, 5.0);
    // Half of a 5 V supply against a 5 V reference is 512 counts exactly.
    pot.set_position(0.5);
    CHECK_EQ(int(pot.expected_reading(5.0)), 512);
    // Full scale pins at 1023 rather than wrapping to 1024.
    pot.set_position(1.0);
    CHECK_EQ(int(pot.expected_reading(5.0)), 1023);
    // 2.5 V against the internal 1.1 V reference is above the reference, so it
    // pins as well -- that is a real thing a sketch does by accident.
    pot.set_position(0.5);
    CHECK_EQ(int(pot.expected_reading(1.1)), 1023);
    // A quarter of 5 V against 5 V is 256.
    pot.set_position(0.25);
    CHECK_EQ(int(pot.expected_reading(5.0)), 256);
}

// ================================================================ servo ====

namespace {

// One servo frame: a pulse of `us` followed by the rest of a 20 ms period.
void servo_pulse(Servo& servo, double us, uint32_t clock, uint64_t& t) {
    servo.pin_changed(9, PinState::High, t);
    t += us_cycles(us, clock);
    servo.pin_changed(9, PinState::Low, t);
    t += us_cycles(20000.0 - us, clock);
}

bool near(double a, double b, double tol) { return a > b - tol && a < b + tol; }

} // namespace

TEST(servo_decodes_the_standard_pulse_widths_at_16mhz) {
    Servo servo(k16MHz);
    uint64_t t = 0;
    CHECK_EQ(int(servo.pulses()), 0);

    servo_pulse(servo, 1000.0, k16MHz, t);   // 16000 cycles high
    CHECK(near(servo.angle(), 0.0, 0.01));
    CHECK(near(servo.last_pulse_us(), 1000.0, 0.01));

    servo_pulse(servo, 1500.0, k16MHz, t);   // 24000 cycles high
    CHECK(near(servo.angle(), 90.0, 0.01));

    servo_pulse(servo, 2000.0, k16MHz, t);   // 32000 cycles high
    CHECK(near(servo.angle(), 180.0, 0.01));

    CHECK_EQ(int(servo.pulses()), 3);
    CHECK_EQ(int(servo.rejected()), 0);
}

TEST(servo_decodes_the_same_widths_at_8mhz) {
    // The same microseconds are half the cycles here. A part with 16 MHz baked
    // into it would read every one of these as twice its real width.
    Servo servo(k8MHz);
    uint64_t t = 0;

    servo_pulse(servo, 1000.0, k8MHz, t);    // 8000 cycles high
    CHECK(near(servo.angle(), 0.0, 0.01));
    servo_pulse(servo, 1500.0, k8MHz, t);    // 12000 cycles high
    CHECK(near(servo.angle(), 90.0, 0.01));
    servo_pulse(servo, 2000.0, k8MHz, t);    // 16000 cycles high
    CHECK(near(servo.angle(), 180.0, 0.01));
    CHECK(near(servo.last_pulse_us(), 2000.0, 0.01));
}

TEST(servo_measures_its_refresh_interval) {
    Servo servo(k16MHz);
    uint64_t t = 0;
    servo_pulse(servo, 1500.0, k16MHz, t);
    servo_pulse(servo, 1500.0, k16MHz, t);
    CHECK(near(servo.frame_us(), 20000.0, 1.0));
}

TEST(servo_rejects_a_width_nowhere_near_a_pulse) {
    Servo servo(k16MHz);
    // A 20 ms high level is not a command to travel anywhere.
    servo.pin_changed(9, PinState::High, 0);
    servo.pin_changed(9, PinState::Low, us_cycles(20000.0, k16MHz));
    CHECK_EQ(int(servo.pulses()), 0);
    CHECK_EQ(int(servo.rejected()), 1);
    CHECK(near(servo.angle(), 0.0, 0.01));

    // A real pulse afterwards still decodes.
    uint64_t t = us_cycles(40000.0, k16MHz);
    servo_pulse(servo, 1500.0, k16MHz, t);
    CHECK_EQ(int(servo.pulses()), 1);
    CHECK(near(servo.angle(), 90.0, 0.01));
}

TEST(servo_honours_a_non_standard_range) {
    // A 544-2400 us servo: 1472 us is its centre, not 1500.
    Servo servo(k16MHz, 544.0, 2400.0, 180.0);
    uint64_t t = 0;
    servo_pulse(servo, 1472.0, k16MHz, t);
    CHECK(near(servo.angle(), 90.0, 0.2));
    servo_pulse(servo, 544.0, k16MHz, t);
    CHECK(near(servo.angle(), 0.0, 0.2));
    servo_pulse(servo, 2400.0, k16MHz, t);
    CHECK(near(servo.angle(), 180.0, 0.2));
}

TEST(servo_without_a_clock_reports_nothing_rather_than_guessing) {
    Servo servo(0);
    uint64_t t = 0;
    servo_pulse(servo, 1500.0, k16MHz, t);
    CHECK_EQ(int(servo.pulses()), 0);
    CHECK_EQ(int(servo.undecoded()), 1);
    CHECK(near(servo.angle(), 0.0, 0.01));

    // Given the rate, it decodes normally from then on.
    servo.set_clock_hz(k16MHz);
    servo_pulse(servo, 2000.0, k16MHz, t);
    CHECK_EQ(int(servo.pulses()), 1);
    CHECK(near(servo.angle(), 180.0, 0.01));
}

// =============================================================== buzzer ====

namespace {

// A square wave of `hz` for `cycles_count` full periods, starting low.
void square_wave(PiezoBuzzer& buzzer, double hz, uint32_t clock, int periods,
                 uint64_t& t) {
    uint64_t period = uint64_t(double(clock) / hz);
    uint64_t half = period / 2;
    for (int i = 0; i < periods; ++i) {
        buzzer.pin_changed(8, PinState::High, t);
        buzzer.pin_changed(8, PinState::Low, t + half);
        t += period;
    }
}

} // namespace

TEST(buzzer_is_silent_until_it_has_seen_a_whole_period) {
    PiezoBuzzer buzzer(k16MHz);
    CHECK(!buzzer.audible());
    buzzer.pin_changed(8, PinState::High, 0);
    buzzer.pin_changed(8, PinState::Low, 8000);
    // One pulse is not a period: there is no interval between rising edges yet.
    CHECK_EQ(int(buzzer.periods()), 0);
    CHECK(buzzer.frequency_hz() < 0.0001);
}

TEST(buzzer_measures_a_known_square_wave) {
    // 440 Hz at 16 MHz is a period of 36363 cycles.
    PiezoBuzzer buzzer(k16MHz);
    uint64_t t = 0;
    square_wave(buzzer, 440.0, k16MHz, 10, t);

    CHECK_EQ(int(buzzer.periods()), 9);   // ten rising edges, nine intervals
    CHECK(near(buzzer.frequency_hz(), 440.0, 1.0));
    CHECK(near(buzzer.mean_frequency_hz(), 440.0, 1.0));
    CHECK(near(buzzer.duty(), 0.5, 0.01));
    CHECK(buzzer.audible());
}

TEST(buzzer_measures_the_same_tone_at_a_different_clock) {
    PiezoBuzzer buzzer(k8MHz);
    uint64_t t = 0;
    square_wave(buzzer, 1000.0, k8MHz, 5, t);
    CHECK(near(buzzer.frequency_hz(), 1000.0, 1.0));
}

TEST(buzzer_follows_a_tone_change_on_the_first_new_period) {
    PiezoBuzzer buzzer(k16MHz);
    uint64_t t = 0;
    square_wave(buzzer, 1000.0, k16MHz, 4, t);
    CHECK(near(buzzer.frequency_hz(), 1000.0, 1.0));
    square_wave(buzzer, 2000.0, k16MHz, 4, t);
    // The single most recent period is 2000 Hz, even though the mean over the
    // whole run sits somewhere in between.
    CHECK(near(buzzer.frequency_hz(), 2000.0, 2.0));
    CHECK(buzzer.mean_frequency_hz() > 1000.0);
    CHECK(buzzer.mean_frequency_hz() < 2000.0);
}

TEST(buzzer_duty_distinguishes_a_pwm_signal_from_a_tone) {
    PiezoBuzzer buzzer(k16MHz);
    // A quarter-duty wave: high for 4000 of every 16000 cycles.
    uint64_t t = 0;
    for (int i = 0; i < 8; ++i) {
        buzzer.pin_changed(8, PinState::High, t);
        buzzer.pin_changed(8, PinState::Low, t + 4000);
        t += 16000;
    }
    CHECK(near(buzzer.duty(), 0.25, 0.01));
    CHECK(near(buzzer.frequency_hz(), 1000.0, 0.1));
}

// ======================================================= shift register ====

namespace {

// Clocks one bit into a register wired data=11, clock=12, latch=8.
void shift_bit(ShiftRegister& sr, bool bit, uint64_t& t) {
    sr.pin_changed(11, bit ? PinState::High : PinState::Low, t++);
    sr.pin_changed(12, PinState::High, t++);
    sr.pin_changed(12, PinState::Low, t++);
}

void shift_byte_msb_first(ShiftRegister& sr, uint8_t byte, uint64_t& t) {
    for (int b = 7; b >= 0; --b) shift_bit(sr, (byte >> b) & 1, t);
}

} // namespace

TEST(shift_register_reconstructs_a_byte_shifted_in) {
    ShiftRegister sr(k16MHz, 11, 12, 8);
    uint64_t t = 0;
    shift_byte_msb_first(sr, 0xA5, t);

    CHECK_EQ(int(sr.clocks()), 8);
    CHECK_EQ(int(sr.shifted()), 0xA5);
    // Not latched yet: the outputs still show nothing.
    CHECK_EQ(int(sr.output()), 0);
    CHECK_EQ(int(sr.latches()), 0);

    sr.pin_changed(8, PinState::High, t++);
    CHECK_EQ(int(sr.output()), 0xA5);
    CHECK_EQ(int(sr.latches()), 1);
    CHECK(sr.error().empty());
}

TEST(shift_register_latches_only_on_a_rising_edge) {
    ShiftRegister sr(k16MHz, 11, 12, 8);
    uint64_t t = 0;
    shift_byte_msb_first(sr, 0x3C, t);
    sr.pin_changed(8, PinState::High, t++);
    CHECK_EQ(int(sr.output()), 0x3C);

    // Shift a second byte in with the latch held high the whole time. The
    // outputs must not follow it -- that is the entire reason the latch exists.
    shift_byte_msb_first(sr, 0xFF, t);
    CHECK_EQ(int(sr.output()), 0x3C);
    CHECK_EQ(int(sr.shifted()), 0xFF);
    CHECK_EQ(int(sr.latches()), 1);

    sr.pin_changed(8, PinState::Low, t++);
    sr.pin_changed(8, PinState::High, t++);
    CHECK_EQ(int(sr.output()), 0xFF);
    CHECK_EQ(int(sr.latches()), 2);
}

TEST(shift_register_holds_the_data_level_between_clock_edges) {
    // Data is set once and then clocked twice. A part that only looked at the
    // pin it was just told about would shift in a zero for the second edge.
    ShiftRegister sr(k16MHz, 11, 12, 8);
    uint64_t t = 0;
    sr.pin_changed(11, PinState::High, t++);
    sr.pin_changed(12, PinState::High, t++);
    sr.pin_changed(12, PinState::Low, t++);
    sr.pin_changed(12, PinState::High, t++);
    sr.pin_changed(12, PinState::Low, t++);
    CHECK_EQ(int(sr.shifted()), 0x03);
}

TEST(shift_register_ignores_a_level_that_does_not_change) {
    ShiftRegister sr(k16MHz, 11, 12, 8);
    uint64_t t = 0;
    sr.pin_changed(11, PinState::High, t++);
    sr.pin_changed(12, PinState::High, t++);
    sr.pin_changed(12, PinState::High, t++);   // still high: not a new edge
    sr.pin_changed(12, PinState::High, t++);
    CHECK_EQ(int(sr.clocks()), 1);
    CHECK_EQ(int(sr.shifted()), 0x01);
}

TEST(shift_register_reports_an_edge_on_a_pin_it_does_not_own) {
    ShiftRegister sr(k16MHz, 11, 12, 8);
    sr.pin_changed(4, PinState::High, 0);
    CHECK(!sr.error().empty());
    CHECK_EQ(int(sr.clocks()), 0);
}

TEST(shift_register_refuses_to_work_before_its_pins_are_assigned) {
    ShiftRegister sr(k16MHz);
    CHECK(!sr.configured());
    uint64_t t = 0;
    shift_byte_msb_first(sr, 0xFF, t);
    CHECK_EQ(int(sr.clocks()), 0);
    CHECK_EQ(int(sr.shifted()), 0);
    CHECK(!sr.error().empty());

    sr.set_pins(11, 12, 8);
    CHECK(sr.configured());
    CHECK(sr.error().empty());
    shift_byte_msb_first(sr, 0xFF, t);
    CHECK_EQ(int(sr.shifted()), 0xFF);
}

TEST(shift_register_rejects_two_functions_on_one_pin) {
    ShiftRegister sr(k16MHz, 11, 11, 8);
    CHECK(!sr.configured());
}

TEST(shift_register_supports_a_narrower_width) {
    ShiftRegister sr(k16MHz, 11, 12, 8, /*width_bits=*/4);
    uint64_t t = 0;
    shift_byte_msb_first(sr, 0xA5, t);   // eight bits into four
    // Only the last four clocked survive: 0x5.
    CHECK_EQ(int(sr.shifted()), 0x5);
}

// ============================================================= registry ====

TEST(registry_creates_every_kind_it_lists) {
    std::vector<std::string> kinds = ardio::emu::part_kinds();
    CHECK(kinds.size() >= 6);
    for (const std::string& kind : kinds) {
        std::string error = "untouched";
        std::unique_ptr<Part> part = ardio::emu::create_part(kind, error);
        CHECK(part != nullptr);
        CHECK(error.empty());
        if (part) CHECK(part->kind() == kind);
    }
}

TEST(registry_creates_the_named_parts) {
    std::string error;
    std::unique_ptr<Part> led = ardio::emu::create_part("led", k16MHz, error);
    CHECK(led != nullptr);
    CHECK(led && led->kind() == "led");

    std::unique_ptr<Part> button = ardio::emu::create_part("button", k16MHz, error);
    CHECK(button != nullptr);
    CHECK(button && button->kind() == "button");

    std::unique_ptr<Part> servo = ardio::emu::create_part("servo", k16MHz, error);
    CHECK(servo != nullptr);
    CHECK(servo && servo->kind() == "servo");
}

TEST(registry_reports_an_unknown_name_clearly) {
    std::string error;
    std::unique_ptr<Part> part = ardio::emu::create_part("lde", error);
    CHECK(part == nullptr);
    // The message must name what was asked for and list what there is, since
    // the user has just mistyped something on a command line.
    CHECK(error.find("lde") != std::string::npos);
    for (const std::string& kind : ardio::emu::part_kinds())
        CHECK(error.find(kind) != std::string::npos);
}

TEST(registry_part_works_after_its_clock_is_supplied) {
    std::string error;
    std::unique_ptr<Part> part = ardio::emu::create_part("servo", error);
    CHECK(part != nullptr);
    if (!part) return;

    CHECK(ardio::emu::set_part_clock(part.get(), k16MHz));
    Servo* servo = dynamic_cast<Servo*>(part.get());
    CHECK(servo != nullptr);
    if (!servo) return;

    uint64_t t = 0;
    servo_pulse(*servo, 1500.0, k16MHz, t);
    CHECK_EQ(int(servo->pulses()), 1);
    CHECK(near(servo->angle(), 90.0, 0.01));
}

TEST(registry_created_led_measures_against_the_clock_it_is_given) {
    std::string error;
    std::unique_ptr<Part> part = ardio::emu::create_part("led", error);
    CHECK(part != nullptr);
    if (!part) return;
    CHECK(ardio::emu::set_part_clock(part.get(), k8MHz));

    Led* led = dynamic_cast<Led*>(part.get());
    CHECK(led != nullptr);
    if (!led) return;
    led->pin_changed(13, PinState::High, 0);
    led->pin_changed(13, PinState::Low, 8000);
    // 8000 cycles at 8 MHz is 1000 us, not 500.
    CHECK(near(led->lit_us(8000), 1000.0, 0.01));
}

TEST(every_part_describes_itself) {
    for (const std::string& kind : ardio::emu::part_kinds()) {
        std::string error;
        std::unique_ptr<Part> part = ardio::emu::create_part(kind, k16MHz, error);
        CHECK(part != nullptr);
        if (part) CHECK(!part->describe().empty());
    }
}
