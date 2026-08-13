// Tests for the emulated peripherals.
//
// Every timing expectation below is a number computed by hand from the
// datasheet -- 256 timer ticks at a prescale of 64 is 16384 clock cycles, a
// 10-bit frame at a divisor of 103 is 16640 -- rather than a number read back
// off the implementation and pasted in. A test that asserts whatever the code
// currently does cannot fail, and the whole reason to emulate timing is that
// the timing is the part a sketch depends on.

#include "harness.h"

#include "ardio/avr/device.h"
#include "ardio/emu/peripherals.h"

#include <cstdint>
#include <string>

using ardio::emu::Adc;
using ardio::emu::GpioPort;
using ardio::emu::PinState;
using ardio::emu::Timer0;
using ardio::emu::Usart0;

namespace {

const ardio::avr::AvrDevice& mega328p() {
    const ardio::avr::AvrDevice* d = ardio::avr::find_device("atmega328p");
    static ardio::avr::AvrDevice fallback;
    return d ? *d : fallback;
}

// PORTB on the 328P: PINB 0x23, DDRB 0x24, PORTB 0x25.
constexpr uint16_t kPinB = 0x23;
constexpr uint16_t kDdrB = 0x24;
constexpr uint16_t kPortB = 0x25;

} // namespace

// =============================================================== GPIO ======

TEST(gpio_claims_exactly_its_three_registers) {
    GpioPort port(kPinB, 'B');
    CHECK(port.claims(kPinB));
    CHECK(port.claims(kDdrB));
    CHECK(port.claims(kPortB));
    CHECK(!port.claims(uint16_t(kPinB - 1)));
    CHECK(!port.claims(uint16_t(kPortB + 1)));
}

TEST(gpio_output_bit_drives_the_level_in_port) {
    GpioPort port(kPinB, 'B');
    port.write(kDdrB, 0x20);        // PB5 an output
    port.write(kPortB, 0x20);       // driven high

    CHECK(port.drive(5) == PinState::High);
    CHECK_EQ(int(port.read(kPinB) & 0x20), 0x20);

    port.write(kPortB, 0x00);
    CHECK(port.drive(5) == PinState::Low);
    CHECK_EQ(int(port.read(kPinB) & 0x20), 0);

    // A bit that was never made an output is not driven at all, whatever the
    // neighbouring bit is doing.
    CHECK(port.drive(4) == PinState::Floating);
}

TEST(gpio_input_bit_uses_port_as_the_pullup_switch) {
    GpioPort port(kPinB, 'B');
    port.write(kDdrB, 0x00);        // everything an input
    port.write(kPortB, 0x04);       // pull-up on PB2 only

    CHECK(port.pullup(2));
    CHECK(!port.pullup(3));
    // The pull-up must not read back as the MCU driving the pin, or a button
    // wired to ground would look like a short.
    CHECK(port.drive(2) == PinState::Floating);

    CHECK_EQ(int(port.read(kPinB)), 0x04);   // pulled up reads high
}

TEST(gpio_external_drive_beats_the_pullup) {
    GpioPort port(kPinB, 'B');
    port.write(kDdrB, 0x00);
    port.write(kPortB, 0xFF);       // pull-ups on everything

    CHECK_EQ(int(port.read(kPinB)), 0xFF);

    // A button between PB0 and ground.
    port.set_external(0, PinState::Low);
    CHECK_EQ(int(port.read(kPinB)), 0xFE);

    port.set_external(0, PinState::Floating);   // button released
    CHECK_EQ(int(port.read(kPinB)), 0xFF);
}

TEST(gpio_floating_input_without_pullup_reads_low) {
    GpioPort port(kPinB, 'B');
    port.write(kDdrB, 0x00);
    port.write(kPortB, 0x00);
    CHECK_EQ(int(port.read(kPinB)), 0x00);
}

TEST(gpio_writing_one_to_pin_toggles_port) {
    GpioPort port(kPinB, 'B');
    port.write(kDdrB, 0xFF);
    port.write(kPortB, 0x0F);

    port.write(kPinB, 0x03);        // toggle bits 0 and 1
    CHECK_EQ(int(port.read(kPortB)), 0x0C);
    CHECK(port.drive(0) == PinState::Low);
    CHECK(port.drive(2) == PinState::High);

    port.write(kPinB, 0x03);        // and back
    CHECK_EQ(int(port.read(kPortB)), 0x0F);

    // Zeroes in the written value leave their bits alone.
    port.write(kPinB, 0x00);
    CHECK_EQ(int(port.read(kPortB)), 0x0F);
}

TEST(gpio_pin_toggle_works_on_an_input_bit_too) {
    // The toggle acts on PORTx, so on an input it switches the pull-up. That
    // is a real and occasionally useful thing, and worth pinning down.
    GpioPort port(kPinB, 'B');
    port.write(kDdrB, 0x00);
    port.write(kPinB, 0x01);
    CHECK(port.pullup(0));
    CHECK_EQ(int(port.read(kPinB)), 0x01);
}

TEST(gpio_is_idle_forever) {
    GpioPort port(kPinB, 'B');
    port.advance(1000000);
    CHECK(port.next_event() == UINT64_MAX);
}

// ============================================================ timer 0 ======

namespace {

// A timer set up in normal mode with the given CS bits and nothing else.
Timer0 running_timer(uint8_t cs_bits) {
    Timer0 t(mega328p());
    t.write(t.tccr0a_addr(), 0x00);      // normal mode, TOP 0xFF
    t.write(t.tccr0b_addr(), cs_bits);
    return t;
}

} // namespace

TEST(timer_is_idle_while_the_clock_source_is_off) {
    Timer0 t(mega328p());
    CHECK(t.next_event() == UINT64_MAX);
    t.advance(1000000);
    CHECK_EQ(int(t.counter()), 0);
    CHECK_EQ(int(t.tifr()), 0);
}

TEST(timer_counts_at_the_prescaled_rate) {
    Timer0 t = running_timer(0x03);      // clk/64
    t.advance(64 * 7);
    CHECK_EQ(int(t.counter()), 7);
    t.advance(64 * 7 + 63);              // a partial tick does not count yet
    CHECK_EQ(int(t.counter()), 7);
    t.advance(64 * 8);
    CHECK_EQ(int(t.counter()), 8);
}

TEST(timer_overflow_arrives_on_the_computed_cycle) {
    // 256 ticks to wrap, 64 clocks per tick: 16384 cycles. This is the
    // Arduino core's own timer 0 setup, so it is also the number millis()
    // increments on.
    Timer0 t = running_timer(0x03);
    t.advance(16383);
    CHECK_EQ(int(t.tifr() & 0x01), 0);
    CHECK_EQ(int(t.counter()), 255);

    t.advance(16384);
    CHECK_EQ(int(t.tifr() & 0x01), 0x01);
    CHECK_EQ(int(t.counter()), 0);
}

TEST(timer_next_event_lands_exactly_on_the_overflow) {
    Timer0 t = running_timer(0x03);
    CHECK(t.next_event() == 16384u);

    // Partway through, the answer must still name the same absolute cycle
    // rather than sliding forward by whatever was consumed.
    t.advance(10000);
    CHECK(t.next_event() == 16384u);

    t.advance(16384);
    CHECK(t.next_event() == 32768u);
}

TEST(timer_next_event_accounts_for_the_prescaler) {
    Timer0 t = running_timer(0x05);      // clk/1024
    CHECK(t.next_event() == 256u * 1024u);

    Timer0 fast = running_timer(0x01);   // clk/1
    CHECK(fast.next_event() == 256u);
}

TEST(timer_catches_up_over_a_span_of_many_overflows) {
    Timer0 t = running_timer(0x03);
    // One jump covering three and a bit periods: advance is called at
    // deadlines, not per instruction, so it has to handle this.
    t.advance(16384 * 3 + 640);
    CHECK_EQ(int(t.tifr() & 0x01), 0x01);
    CHECK_EQ(int(t.counter()), 10);
}

TEST(timer_compare_match_sets_its_own_flag_on_its_own_cycle) {
    Timer0 t = running_timer(0x02);      // clk/8
    t.write(t.ocr0a_addr(), 99);
    t.write(t.tifr0_addr(), 0xFF);       // clear the flags set at position 0

    // The counter is already at 0, so reaching 99 takes 99 ticks of 8 clocks.
    CHECK(t.next_event() == 792u);
    t.advance(791);
    CHECK_EQ(int(t.tifr() & 0x02), 0);
    t.advance(792);
    CHECK_EQ(int(t.tifr() & 0x02), 0x02);
    CHECK_EQ(int(t.tifr() & 0x01), 0);   // the overflow is still 156 ticks off
}

TEST(timer_flags_are_cleared_by_writing_a_one) {
    Timer0 t = running_timer(0x03);
    t.advance(16384);
    CHECK_EQ(int(t.tifr() & 0x01), 0x01);

    t.write(t.tifr0_addr(), 0x00);       // the intuitive, useless store
    CHECK_EQ(int(t.tifr() & 0x01), 0x01);

    t.write(t.tifr0_addr(), 0x01);
    CHECK_EQ(int(t.tifr() & 0x01), 0);
}

TEST(timer_overflow_interrupt_needs_its_enable) {
    Timer0 t = running_timer(0x03);
    t.advance(16384);
    CHECK_EQ(int(t.pending_interrupt()), 0);   // TOIE0 is clear

    t.write(t.timsk0_addr(), 0x01);
    CHECK_EQ(int(t.pending_interrupt()), int(mega328p().timer0_ovf_vector));

    t.acknowledge_interrupt();
    CHECK_EQ(int(t.tifr() & 0x01), 0);
    CHECK_EQ(int(t.pending_interrupt()), 0);
}

TEST(timer_compare_outranks_overflow_when_both_are_pending) {
    Timer0 t = running_timer(0x03);
    t.write(t.ocr0a_addr(), 0);          // matches at the same tick as the wrap
    t.write(t.timsk0_addr(), 0x03);      // TOIE0 and OCIE0A
    t.advance(16384);

    CHECK_EQ(int(t.tifr() & 0x03), 0x03);
    // Lower vector number wins, which on this part is compare match A.
    CHECK_EQ(int(t.pending_interrupt()), int(mega328p().timer0_ovf_vector) - 2);
    t.acknowledge_interrupt();
    CHECK_EQ(int(t.pending_interrupt()), int(mega328p().timer0_ovf_vector));
}

TEST(timer_fast_pwm_with_ocr0a_as_top_shortens_the_period) {
    // Mode 7: WGM02 in TCCR0B, WGM01 and WGM00 in TCCR0A. TOP becomes OCR0A,
    // so the counter wraps early and millis() would run fast if this were
    // modelled as a fixed 256.
    Timer0 t(mega328p());
    t.write(t.tccr0a_addr(), 0x03);
    t.write(t.ocr0a_addr(), 63);
    t.write(t.tccr0b_addr(), 0x09);      // WGM02 | clk/1

    // The compare match at TOP comes first, one tick before the wrap, and the
    // timer must be woken for it: TIFR0 is polled as often as it is used as
    // an interrupt source.
    CHECK(t.next_event() == 63u);
    t.advance(63);
    CHECK_EQ(int(t.counter()), 63);
    CHECK_EQ(int(t.tifr() & 0x02), 0x02);   // OCF0A, at TOP
    CHECK_EQ(int(t.tifr() & 0x01), 0);      // but not the overflow yet
    CHECK(t.next_event() == 64u);           // 64 ticks to wrap past a TOP of 63
    t.advance(64);
    CHECK_EQ(int(t.counter()), 0);
    CHECK_EQ(int(t.tifr() & 0x01), 0x01);
}

TEST(timer_phase_correct_takes_twice_as_long_per_cycle) {
    // Mode 1: up to 0xFF then back down to 0, so a full cycle is 510 ticks
    // and not 256. Getting this wrong doubles every delay in a sketch that
    // uses analogWrite on a timer 0 pin.
    Timer0 t(mega328p());
    t.write(t.tccr0a_addr(), 0x01);
    t.write(t.tccr0b_addr(), 0x01);      // clk/1

    t.advance(255);
    CHECK_EQ(int(t.counter()), 255);
    t.advance(300);
    CHECK_EQ(int(t.counter()), 210);     // counting back down
    CHECK_EQ(int(t.tifr() & 0x01), 0);

    CHECK(t.next_event() == 510u);       // overflow is at BOTTOM in this mode
    t.advance(510);
    CHECK_EQ(int(t.counter()), 0);
    CHECK_EQ(int(t.tifr() & 0x01), 0x01);
}

TEST(timer_write_to_tcnt0_moves_the_counter) {
    Timer0 t = running_timer(0x01);      // clk/1
    t.write(t.tcnt0_addr(), 250);
    CHECK_EQ(int(t.read(t.tcnt0_addr())), 250);
    CHECK(t.next_event() == 6u);         // six ticks left to the wrap
    t.advance(6);
    CHECK_EQ(int(t.counter()), 0);
    CHECK_EQ(int(t.tifr() & 0x01), 0x01);
}

// ============================================================= USART0 ======

namespace {

// The USART configured the way serial_begin leaves it: 8N1, receiver and
// transmitter on, at the given divisor.
Usart0 open_usart(uint16_t ubrr) {
    Usart0 u(mega328p());
    u.write(u.ucsrb_addr(), 0x00);
    u.write(u.ubrrh_addr(), uint8_t(ubrr >> 8));
    u.write(u.ubrrl_addr(), uint8_t(ubrr & 0xFF));
    u.write(u.ucsrc_addr(), 0x06);
    u.write(u.ucsrb_addr(), 0x18);       // RXEN0 | TXEN0
    return u;
}

} // namespace

TEST(usart_frame_length_follows_the_divisor) {
    // 16 MHz, 9600 baud is a divisor of 103. A frame is a start bit, eight
    // data bits and a stop bit: ten bits of 16 * 104 clocks each.
    Usart0 u = open_usart(103);
    CHECK(u.frame_cycles() == 16640u);

    // 115200 baud on the same clock is a divisor of 8.
    Usart0 fast = open_usart(8);
    CHECK(fast.frame_cycles() == 1440u);
}

TEST(usart_double_speed_halves_the_frame) {
    Usart0 u = open_usart(103);
    u.write(u.ucsra_addr(), 0x02);       // U2X0
    CHECK(u.frame_cycles() == 8320u);
}

TEST(usart_two_stop_bits_lengthen_the_frame) {
    Usart0 u = open_usart(103);
    u.write(u.ucsrc_addr(), 0x0E);       // 8N2
    CHECK(u.frame_cycles() == 11u * 16u * 104u);
}

TEST(usart_transmitted_byte_appears_after_a_full_frame) {
    Usart0 u = open_usart(103);
    u.write(u.udr_addr(), 'A');

    CHECK(u.next_event() == 16640u);
    u.advance(16639);
    CHECK(u.output().empty());
    CHECK_EQ(int(u.ucsra() & 0x40), 0);          // TXC not yet

    u.advance(16640);
    CHECK(u.output() == std::string("A"));
    CHECK_EQ(int(u.ucsra() & 0x40), 0x40);       // TXC
    CHECK_EQ(int(u.ucsra() & 0x20), 0x20);       // UDRE
    CHECK(u.next_event() == UINT64_MAX);
}

TEST(usart_second_write_while_busy_clears_udre_and_queues) {
    Usart0 u = open_usart(103);
    u.write(u.udr_addr(), 'h');
    CHECK_EQ(int(u.ucsra() & 0x20), 0x20);       // the shift register took it
    u.write(u.udr_addr(), 'i');
    CHECK_EQ(int(u.ucsra() & 0x20), 0);          // now the buffer is full

    u.advance(16640);
    CHECK(u.output() == std::string("h"));
    CHECK_EQ(int(u.ucsra() & 0x20), 0x20);       // buffer handed on, empty again

    u.advance(16640 * 2);
    CHECK(u.output() == std::string("hi"));
}

TEST(usart_transmit_does_nothing_while_the_transmitter_is_off) {
    Usart0 u(mega328p());
    u.write(u.ubrrl_addr(), 103);
    u.write(u.udr_addr(), 'x');          // TXEN0 never set
    u.advance(100000);
    CHECK(u.output().empty());
    CHECK(u.next_event() == UINT64_MAX);
}

TEST(usart_received_byte_sets_rxc_after_a_frame) {
    Usart0 u = open_usart(103);
    u.feed('Z');
    CHECK(u.next_event() == 16640u);

    u.advance(16639);
    CHECK_EQ(int(u.ucsra() & 0x80), 0);
    u.advance(16640);
    CHECK_EQ(int(u.ucsra() & 0x80), 0x80);

    CHECK_EQ(int(u.read(u.udr_addr())), int('Z'));
    CHECK_EQ(int(u.ucsra() & 0x80), 0);          // reading UDR clears RXC
}

TEST(usart_receives_a_string_one_frame_at_a_time) {
    Usart0 u = open_usart(103);
    u.feed(std::string("ok"));

    u.advance(16640);
    CHECK_EQ(int(u.read(u.udr_addr())), int('o'));
    // The second byte only starts once the first has been shifted in, so it
    // is another whole frame away, not available immediately.
    CHECK(u.next_event() == 16640u * 2u);
    u.advance(16640 * 2);
    CHECK_EQ(int(u.ucsra() & 0x80), 0x80);
    CHECK_EQ(int(u.read(u.udr_addr())), int('k'));
}

TEST(usart_unread_byte_overruns_rather_than_being_lost_quietly) {
    Usart0 u = open_usart(103);
    u.feed(std::string("ab"));
    u.advance(16640 * 2);

    CHECK_EQ(int(u.ucsra() & 0x80), 0x80);
    CHECK_EQ(int(u.ucsra() & 0x08), 0x08);       // DOR0
    CHECK_EQ(int(u.read(u.udr_addr())), int('a'));
}

TEST(usart_receiver_off_ignores_the_line) {
    Usart0 u(mega328p());
    u.write(u.ubrrl_addr(), 103);
    u.write(u.ucsrb_addr(), 0x08);       // TXEN0 only
    u.feed('q');
    u.advance(100000);
    CHECK_EQ(int(u.ucsra() & 0x80), 0);
    CHECK(u.next_event() == UINT64_MAX);
}

TEST(usart_rx_interrupt_needs_its_enable) {
    Usart0 u = open_usart(103);
    u.feed('m');
    u.advance(16640);
    CHECK_EQ(int(u.pending_interrupt()), 0);

    u.write(u.ucsrb_addr(), 0x98);       // RXCIE0 | RXEN0 | TXEN0
    CHECK_EQ(int(u.pending_interrupt()), int(mega328p().timer0_ovf_vector) + 2);

    // The vector stays pending until the handler takes the byte, because
    // nothing else resolves the condition.
    u.acknowledge_interrupt();
    CHECK_EQ(int(u.pending_interrupt()), int(mega328p().timer0_ovf_vector) + 2);
    u.read(u.udr_addr());
    CHECK_EQ(int(u.pending_interrupt()), 0);
}

TEST(usart_idle_reports_no_event) {
    Usart0 u = open_usart(103);
    CHECK(u.next_event() == UINT64_MAX);
    u.advance(500000);
    CHECK(u.next_event() == UINT64_MAX);
}

// ================================================================ ADC ======

namespace {

Adc enabled_adc(uint8_t prescale_bits) {
    Adc a(mega328p());
    a.write(a.adcsra_addr(), uint8_t(0x80 | prescale_bits));
    return a;
}

} // namespace

TEST(adc_is_idle_until_a_conversion_is_started) {
    Adc a = enabled_adc(0x07);
    CHECK(a.next_event() == UINT64_MAX);
    a.advance(1000000);
    CHECK_EQ(int(a.read(a.adcsra_addr()) & 0x10), 0);   // ADIF clear
}

TEST(adc_first_conversion_takes_twenty_five_adc_clocks) {
    // Prescale 128 on a 16 MHz part is the Arduino core's setting: 125 kHz
    // ADC clock, and the first conversion after enabling costs 25 of them.
    Adc a = enabled_adc(0x07);
    a.set_channel_voltage(0, 2.5);
    a.write(a.admux_addr(), 0x40);       // AVCC reference, channel 0
    a.write(a.adcsra_addr(), 0xC7);      // ADEN | ADSC | prescale 128

    CHECK(a.next_event() == 25u * 128u);
    a.advance(25 * 128 - 1);
    CHECK_EQ(int(a.read(a.adcsra_addr()) & 0x40), 0x40);   // still converting
    CHECK_EQ(int(a.read(a.adcsra_addr()) & 0x10), 0);

    a.advance(25 * 128);
    CHECK_EQ(int(a.read(a.adcsra_addr()) & 0x40), 0);      // ADSC cleared
    CHECK_EQ(int(a.read(a.adcsra_addr()) & 0x10), 0x10);   // ADIF set
    CHECK(a.next_event() == UINT64_MAX);
}

TEST(adc_later_conversions_take_thirteen_adc_clocks) {
    Adc a = enabled_adc(0x07);
    a.write(a.admux_addr(), 0x40);
    a.write(a.adcsra_addr(), 0xC7);
    a.advance(25 * 128);

    a.write(a.adcsra_addr(), 0xD7);      // clear ADIF and start another
    CHECK(a.next_event() == 25u * 128u + 13u * 128u);
}

TEST(adc_prescaler_scales_the_conversion) {
    Adc a = enabled_adc(0x02);           // divide by 4
    a.write(a.adcsra_addr(), 0xC2);
    CHECK(a.next_event() == 25u * 4u);
}

TEST(adc_returns_the_value_for_the_injected_voltage) {
    Adc a = enabled_adc(0x07);
    a.set_channel_voltage(3, 2.5);       // a pot at half travel on A3
    a.write(a.admux_addr(), 0x43);       // AVCC reference, channel 3
    a.write(a.adcsra_addr(), 0xC7);
    a.advance(25 * 128);

    uint16_t result = uint16_t(a.read(a.adcl_addr()) | (a.read(a.adch_addr()) << 8));
    CHECK_EQ(int(result), 512);
}

TEST(adc_full_scale_and_zero_land_on_the_ends) {
    Adc a = enabled_adc(0x07);
    a.write(a.admux_addr(), 0x40);
    a.set_channel_voltage(0, 0.0);
    a.write(a.adcsra_addr(), 0xC7);
    a.advance(25 * 128);
    CHECK_EQ(int(a.read(a.adcl_addr()) | (a.read(a.adch_addr()) << 8)), 0);

    // A channel at or above the reference reads full scale rather than
    // wrapping to zero, which is what the converter saturates to.
    a.set_channel_voltage(0, 5.0);
    a.write(a.adcsra_addr(), 0xD7);
    a.advance(25 * 128 + 13 * 128);
    CHECK_EQ(int(a.read(a.adcl_addr()) | (a.read(a.adch_addr()) << 8)), 1023);
}

TEST(adc_internal_reference_changes_the_reading) {
    // The same 0.55 V on the same pin is half scale against the 1.1 V
    // internal reference and about a ninth of it against AVCC.
    Adc a = enabled_adc(0x07);
    a.set_channel_voltage(1, 0.55);

    a.write(a.admux_addr(), 0xC1);       // internal 1.1 V reference
    a.write(a.adcsra_addr(), 0xC7);
    a.advance(25 * 128);
    CHECK_EQ(int(a.read(a.adcl_addr()) | (a.read(a.adch_addr()) << 8)), 512);

    a.write(a.admux_addr(), 0x41);       // AVCC
    a.write(a.adcsra_addr(), 0xD7);
    a.advance(25 * 128 + 13 * 128);
    CHECK_EQ(int(a.read(a.adcl_addr()) | (a.read(a.adch_addr()) << 8)), 112);
}

TEST(adc_supply_voltage_is_a_property_of_the_board) {
    // The same sketch on a 3.3 V board reads a different number for the same
    // voltage, and that is a real and common source of confusion.
    Adc a = enabled_adc(0x07);
    a.set_supply_voltage(3.3);
    a.set_channel_voltage(0, 1.65);
    a.write(a.admux_addr(), 0x40);
    a.write(a.adcsra_addr(), 0xC7);
    a.advance(25 * 128);
    CHECK_EQ(int(a.read(a.adcl_addr()) | (a.read(a.adch_addr()) << 8)), 512);
}

TEST(adc_ground_channel_reads_zero_and_bandgap_reads_its_own_voltage) {
    Adc a = enabled_adc(0x07);
    a.set_channel_voltage(15, 4.0);      // nothing outside can drive it
    a.write(a.admux_addr(), 0x4F);       // AVCC reference, GND channel
    a.write(a.adcsra_addr(), 0xC7);
    a.advance(25 * 128);
    CHECK_EQ(int(a.read(a.adcl_addr()) | (a.read(a.adch_addr()) << 8)), 0);

    a.write(a.admux_addr(), 0x4E);       // the 1.1 V bandgap
    a.write(a.adcsra_addr(), 0xD7);
    a.advance(25 * 128 + 13 * 128);
    CHECK_EQ(int(a.read(a.adcl_addr()) | (a.read(a.adch_addr()) << 8)), 225);
}

TEST(adc_left_adjust_moves_the_result_to_the_top) {
    Adc a = enabled_adc(0x07);
    a.set_channel_voltage(0, 2.5);
    a.write(a.admux_addr(), 0x60);       // AVCC | ADLAR
    a.write(a.adcsra_addr(), 0xC7);
    a.advance(25 * 128);

    // 512 left-adjusted is 0x8000, so a sketch reading only ADCH gets 128.
    CHECK_EQ(int(a.read(a.adch_addr())), 0x80);
    CHECK_EQ(int(a.read(a.adcl_addr())), 0x00);
}

TEST(adc_interrupt_needs_its_enable_and_clears_on_acknowledge) {
    Adc a = enabled_adc(0x07);
    a.write(a.admux_addr(), 0x40);
    a.write(a.adcsra_addr(), 0xCF);      // ADEN | ADSC | ADIE | prescale 128
    a.advance(25 * 128);
    CHECK_EQ(int(a.pending_interrupt()), int(mega328p().timer0_ovf_vector) + 5);
    a.acknowledge_interrupt();
    CHECK_EQ(int(a.pending_interrupt()), 0);
}

TEST(adc_disabling_abandons_the_conversion) {
    Adc a = enabled_adc(0x07);
    a.write(a.adcsra_addr(), 0xC7);
    a.write(a.adcsra_addr(), 0x07);      // ADEN cleared mid-conversion
    CHECK(a.next_event() == UINT64_MAX);
    a.advance(1000000);
    CHECK_EQ(int(a.read(a.adcsra_addr()) & 0x10), 0);
}

// ============================================================ factory ======

TEST(factory_builds_a_port_for_every_port_the_pin_map_uses) {
    ardio::emu::PeripheralSet set = ardio::emu::make_standard_peripherals(mega328p());

    // The 328P's Arduino numbering covers PORTD, PORTB and PORTC and nothing
    // else, so three ports and no more.
    CHECK_EQ(int(set.ports.size()), 3);
    CHECK(set.port_for(0x29) != nullptr);   // PIND
    CHECK(set.port_for(0x23) != nullptr);   // PINB
    CHECK(set.port_for(0x26) != nullptr);   // PINC
    CHECK(set.port_for(0x20) == nullptr);   // PINA: not on this part
    CHECK_EQ(int(set.port_for(0x23)->label()), int('B'));
}

TEST(factory_builds_the_timer_usart_and_adc) {
    ardio::emu::PeripheralSet set = ardio::emu::make_standard_peripherals(mega328p());
    CHECK(set.timer0 != nullptr);
    CHECK(set.usart0 != nullptr);
    CHECK(set.adc != nullptr);
    CHECK_EQ(int(set.owned.size()), 6);
}

TEST(factory_peripherals_claim_disjoint_addresses) {
    // Two peripherals answering for the same address would make which one
    // responds depend on the order of the vector, which is exactly the kind
    // of bug that only shows up on the part that was added last.
    ardio::emu::PeripheralSet set = ardio::emu::make_standard_peripherals(mega328p());
    for (uint32_t addr = 0; addr <= 0xFFFF; ++addr) {
        int claims = 0;
        for (const auto& p : set.owned)
            if (p->claims(uint16_t(addr))) ++claims;
        CHECK(claims <= 1);
        if (claims > 1) break;
    }
}

TEST(factory_addresses_come_from_the_device_description) {
    const ardio::avr::AvrDevice& d = mega328p();
    ardio::emu::PeripheralSet set = ardio::emu::make_standard_peripherals(d);

    CHECK(set.usart0->claims(d.udr));
    CHECK(set.usart0->claims(d.ucsra));
    CHECK(set.timer0->claims(d.tcnt0));
    CHECK(set.timer0->claims(d.timsk0));
    CHECK(set.adc->claims(d.admux));
    CHECK(set.adc->claims(d.adcl));

    // And the derived ones sit where the datasheet says.
    CHECK_EQ(int(set.timer0->ocr0a_addr()), 0x47);
    CHECK_EQ(int(set.timer0->ocr0b_addr()), 0x48);
}

TEST(builtin_led_pin_resolves_to_a_port_bit) {
    // Pin 13 on this part is PB5, and the whole point of taking addresses
    // from the device is that this lookup works without naming that.
    const ardio::avr::AvrDevice& d = mega328p();
    ardio::emu::PeripheralSet set = ardio::emu::make_standard_peripherals(d);

    const ardio::avr::PinMapping& m = d.pins[d.led_builtin];
    GpioPort* port = set.port_for(m.pin_reg);
    CHECK(port != nullptr);
    if (!port) return;

    port->write(uint16_t(m.pin_reg + 1), uint8_t(1u << m.bit));   // DDR: output
    port->write(uint16_t(m.pin_reg + 2), uint8_t(1u << m.bit));   // PORT: high
    CHECK(port->drive(m.bit) == PinState::High);

    port->write(m.pin_reg, uint8_t(1u << m.bit));                 // toggle
    CHECK(port->drive(m.bit) == PinState::Low);
}

// ============================================ a part that is not the 328P ===
//
// The ATmega8 is in the device table precisely because it disagrees with every
// later part about where its peripherals live and what they contain. These
// check that the peripherals take the disagreement from the description rather
// than carrying 328P assumptions, which is the whole reason the table exists.

namespace {

const ardio::avr::AvrDevice& mega8() {
    const ardio::avr::AvrDevice* d = ardio::avr::find_device("atmega8");
    static ardio::avr::AvrDevice fallback;
    return d ? *d : fallback;
}

} // namespace

TEST(atmega8_timer_has_no_output_compare_and_claims_no_zero_address) {
    // Timer 0 on this part is a plain counter: one TCCR0 with the clock
    // select bits, no waveform register, no OCR0. The table says so by leaving
    // tccr0a at zero, and a peripheral that answered for address zero would be
    // shadowing r0 -- silently, and for every access the sketch makes to it.
    Timer0 t(mega8());
    CHECK(!t.claims(0));
    CHECK_EQ(int(t.ocr0a_addr()), 0);
    CHECK_EQ(int(t.ocr0b_addr()), 0);

    // And nothing derived lands on a register that does exist: TCCR0 is at the
    // address a 328P-shaped derivation would have called OCR0A.
    // These are data-space addresses. The datasheet lists TCNT0 and TCCR0 in
    // the high-I/O summary as $32 and $33, and data space is I/O plus 0x20.
    // They were briefly recorded here as the I/O values, which put TCNT0 on
    // PORTD (0x30 + 2) and TIFR on PORTB (0x36 + 2) once the ATmega8's real
    // port addresses landed -- the same class of mistake as the pin map's.
    CHECK_EQ(int(mega8().tccr0b), 0x53);
    CHECK_EQ(int(mega8().tcnt0), 0x52);
}

TEST(atmega8_timer_still_overflows_on_the_right_cycle) {
    Timer0 t(mega8());
    t.write(mega8().tccr0b, 0x03);       // clk/64, the only mode there is
    CHECK(t.next_event() == 16384u);

    t.advance(16383);
    CHECK_EQ(int(t.tifr() & 0x01), 0);
    t.advance(16384);
    CHECK_EQ(int(t.tifr() & 0x01), 0x01);

    // Bits 1 and 2 of this part's TIFR belong to timer 1, so timer 0 must
    // leave them alone rather than reporting compare matches it cannot have.
    CHECK_EQ(int(t.tifr() & 0x06), 0);

    t.write(mega8().timsk0, 0x01);
    CHECK_EQ(int(t.pending_interrupt()), 9);
    t.acknowledge_interrupt();
    CHECK_EQ(int(t.pending_interrupt()), 0);
}

TEST(atmega8_adc_does_not_answer_for_sfior) {
    // There is no ADCSRB on this part; the auto-trigger bits live in SFIOR,
    // which also carries the pull-up disable and the timer prescaler resets.
    // Claiming it would let the ADC swallow writes meant for those.
    Adc a(mega8());
    CHECK(!a.claims(0x50));
    CHECK(a.claims(mega8().admux));
    CHECK(a.claims(mega8().adcsra));

    // On a part that really has one, it is claimed.
    Adc modern(mega328p());
    CHECK(modern.claims(mega328p().adcsrb));
}

TEST(atmega8_usart_tells_ucsrc_from_ubrrh_by_the_ursel_bit) {
    // Both registers are one location here. URSEL, bit 7 of the value written,
    // is what says which of the two the write is for -- so a divisor high byte
    // and a frame-format byte go to the same address and must not be told
    // apart by the order the fields happen to be checked in.
    const ardio::avr::AvrDevice& d = mega8();
    CHECK_EQ(int(d.ucsrc), int(d.ubrrh));

    Usart0 u(mega8());
    u.write(d.ubrrh, 0x01);              // URSEL clear: the divisor high byte
    u.write(d.ubrrl, 0x00);
    CHECK_EQ(int(u.read(d.ubrrh)), 0x01);
    CHECK(u.frame_cycles() == 257u * 16u * 10u);

    u.write(d.ucsrc, 0x86);              // URSEL set: 8N1 into UCSRC
    CHECK_EQ(int(u.read(d.ubrrh)), 0x01);      // the divisor is untouched
    CHECK(u.frame_cycles() == 257u * 16u * 10u);

    u.write(d.ucsrc, 0x8E);              // URSEL set: 8N2
    CHECK(u.frame_cycles() == 257u * 16u * 11u);
}

TEST(atmega8_usart_transmits_at_its_own_addresses) {
    const ardio::avr::AvrDevice& d = mega8();
    Usart0 u(mega8());
    CHECK(u.claims(0x2B));               // UCSRA, down in low I/O space
    CHECK(u.claims(0x2C));               // UDR

    u.write(d.ubrrl, 51);                // 9600 baud at 8 MHz
    u.write(d.ucsrb, 0x18);
    u.write(d.udr, 'y');
    CHECK(u.next_event() == 52u * 16u * 10u);
    u.advance(52 * 16 * 10);
    CHECK(u.output() == std::string("y"));
}

TEST(atmega8_peripherals_claim_disjoint_addresses) {
    ardio::emu::PeripheralSet set = ardio::emu::make_standard_peripherals(mega8());
    CHECK(set.timer0 != nullptr);
    CHECK(set.usart0 != nullptr);
    CHECK(set.adc != nullptr);

    // Only the three register-block peripherals are checked here, not the
    // ports. The ATmega8's ports are at 0x30, 0x33 and 0x36 in data space, but
    // the pin map in the device table carries the 328P's 0x23, 0x26 and 0x29
    // -- which on this part are the ADC and the USART. That overlap is real
    // and it lives in the description, not in these peripherals: an emulated
    // digitalWrite on this part would land on ADCH. Asserting the ports are
    // disjoint would be asserting the table is right about something it is
    // not, so what is checked is what this file is answerable for.
    const ardio::emu::Peripheral* blocks[] = {set.timer0, set.usart0, set.adc};
    for (uint32_t addr = 0; addr <= 0xFFFF; ++addr) {
        int claims = 0;
        for (const ardio::emu::Peripheral* p : blocks)
            if (p->claims(uint16_t(addr))) ++claims;
        CHECK(claims <= 1);
        if (claims > 1) break;
    }
}
