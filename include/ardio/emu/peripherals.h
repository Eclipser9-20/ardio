#pragma once
#include "ardio/avr/device.h"
#include "ardio/emu/machine.h"

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

// The memory-mapped hardware behind an emulated sketch: the ports, timer 0,
// USART0 and the ADC.
//
// Nothing here names an address. Every register is taken from the AvrDevice,
// for the same reason the runtime assembly is: the ATmega8 keeps its USART in
// low I/O space and the 32U4 has no USART0 at all, so a hardcoded 0xC0 would
// be a per-part fork of the emulator the moment a second part is emulated.
// Where a register the device table does not carry is needed, it is derived
// from one that is by a relationship the datasheet guarantees, and the
// derivation says which relationship and where it does not hold.
//
// These model the hardware from the side runtime/core.S and runtime/serial.S
// talk to. The two descriptions should agree; where a comment here explains
// why a bit behaves as it does, the corresponding runtime file is explaining
// why it writes that bit.
namespace ardio::emu {

// One I/O port: PINx, DDRx = PINx+1, PORTx = PINx+2.
//
// The three registers are not three views of one value. DDRx picks direction
// per bit, and PORTx means two different things depending on it: the level to
// drive when the bit is an output, and whether the internal pull-up is engaged
// when it is an input. Modelling PORTx as "the pin level" is the simplification
// that makes a virtual button impossible, because the sketch's pull-up write
// would look identical to it driving the pin high against the button.
class GpioPort : public Peripheral {
public:
    // `pin_reg` is the data-space address of PINx; `label` is the port letter
    // for diagnostics ("B").
    GpioPort(uint16_t pin_reg, char label);

    bool claims(uint16_t addr) const override;
    uint8_t read(uint16_t addr) override;
    void write(uint16_t addr, uint8_t value) override;
    void advance(uint64_t cycles) override;

    uint16_t pin_reg() const { return pin_reg_; }
    char label() const { return label_; }

    // What the MCU is driving onto a bit, as a part wired to it would see.
    // An input is Floating even with its pull-up on: a pull-up is tens of
    // kilohms and loses to anything actually driving the line, so reporting it
    // as High would make a grounded button read as a contested pin.
    PinState drive(uint8_t bit) const;

    // Whether the internal pull-up is engaged on a bit, which is the state a
    // part needs to resolve a line nothing is driving.
    bool pullup(uint8_t bit) const;

    // What the outside world is driving onto a bit. Floating (the default)
    // means nothing is wired there, or what is wired is not driving.
    void set_external(uint8_t bit, PinState state);
    PinState external(uint8_t bit) const;

    uint8_t ddr() const { return ddr_; }
    uint8_t port() const { return port_; }

private:
    uint8_t resolve_pins() const;

    uint16_t pin_reg_ = 0;
    char label_ = '?';
    uint8_t ddr_ = 0;
    uint8_t port_ = 0;
    PinState external_[8] = {PinState::Floating, PinState::Floating,
                             PinState::Floating, PinState::Floating,
                             PinState::Floating, PinState::Floating,
                             PinState::Floating, PinState::Floating};
};

// Timer 0.
//
// This is the timer millis() and micros() are built on, so its accuracy is not
// a detail of PWM fidelity -- it decides whether a sketch that waits for a
// second waits for a second. It counts in prescaled ticks, sets the overflow
// and compare-match flags in TIFR0, and raises the corresponding interrupt
// when the matching enable in TIMSK0 is set.
//
// Modes covered: normal, fast PWM with TOP at 0xFF or at OCR0A, and
// phase-correct PWM. Phase-correct is included even though the Arduino core
// uses it only for analogWrite on pins 5 and 6, because getting it wrong would
// make timer 0 count at half speed with no visible symptom other than every
// delay being twice as long.
class Timer0 : public Peripheral {
public:
    explicit Timer0(const avr::AvrDevice& device);

    bool claims(uint16_t addr) const override;
    uint8_t read(uint16_t addr) override;
    void write(uint16_t addr, uint8_t value) override;
    void advance(uint64_t cycles) override;
    uint64_t next_event() const override;
    uint8_t pending_interrupt() const override;
    void acknowledge_interrupt() override;

    uint8_t counter() const { return uint8_t(count_from_position()); }
    uint8_t tifr() const { return tifr_; }

    // Addresses, exposed so a machine can wire the timer up and tests can poke
    // it without re-deriving OCR0A's address from TCNT0's.
    uint16_t tccr0a_addr() const { return tccr0a_; }
    uint16_t tccr0b_addr() const { return tccr0b_; }
    uint16_t tcnt0_addr() const { return tcnt0_; }
    uint16_t ocr0a_addr() const { return ocr0a_; }
    uint16_t ocr0b_addr() const { return ocr0b_; }
    uint16_t timsk0_addr() const { return timsk0_; }
    uint16_t tifr0_addr() const { return tifr0_; }

private:
    uint32_t prescaler() const;   // 0 when the timer is stopped
    uint32_t top() const;
    bool phase_correct() const;
    uint32_t period() const;      // positions in one full cycle of the counter
    uint32_t count_from_position() const;
    void catch_up(uint64_t cycles);

    uint16_t tccr0a_ = 0, tccr0b_ = 0, tcnt0_ = 0, ocr0a_ = 0, ocr0b_ = 0;
    uint16_t timsk0_ = 0, tifr0_ = 0;
    uint8_t vec_ovf_ = 0, vec_compa_ = 0, vec_compb_ = 0;

    // False on a part whose timer 0 is a plain counter with no output compare.
    bool has_compare_ = true;

    uint8_t tccr0a_v_ = 0, tccr0b_v_ = 0, ocr0a_v_ = 0, ocr0b_v_ = 0;
    uint8_t timsk0_v_ = 0, tifr_ = 0;

    // The counter is held as a position in the counting cycle rather than as
    // TCNT0 itself, because in phase-correct mode the same TCNT0 value occurs
    // twice per cycle and the direction is what tells the two apart.
    uint32_t position_ = 0;
    uint64_t now_ = 0;         // cycle count this peripheral has caught up to
    uint32_t tick_debt_ = 0;   // clock cycles accumulated toward the next tick
};

// USART0 in asynchronous mode.
//
// Bytes written by the sketch land in `output()` after the time they would
// really have taken on the wire, and bytes handed to feed() arrive in UDR the
// same way. The delay is the point: a sketch that writes faster than the baud
// rate allows blocks on UDRE in the real world, and a USART that completes
// instantly turns that blocking loop into a no-op, which is exactly the bug
// class an emulator is supposed to catch.
class Usart0 : public Peripheral {
public:
    explicit Usart0(const avr::AvrDevice& device);

    bool claims(uint16_t addr) const override;
    uint8_t read(uint16_t addr) override;
    void write(uint16_t addr, uint8_t value) override;
    void advance(uint64_t cycles) override;
    uint64_t next_event() const override;
    uint8_t pending_interrupt() const override;
    void acknowledge_interrupt() override;

    // Everything the sketch has transmitted, in order. The emulator prints it.
    const std::string& output() const { return output_; }
    void clear_output() { output_.clear(); }

    // Hand a byte to the receiver, as a terminal on the other end would. It
    // becomes readable one frame time later, not immediately.
    void feed(uint8_t byte);
    void feed(const std::string& bytes);

    // Clock cycles one frame occupies at the current UBRR and frame format.
    uint64_t frame_cycles() const;

    uint8_t ucsra() const { return ucsra_v_; }

    uint16_t ucsra_addr() const { return ucsra_; }
    uint16_t ucsrb_addr() const { return ucsrb_; }
    uint16_t ucsrc_addr() const { return ucsrc_; }
    uint16_t ubrrl_addr() const { return ubrrl_; }
    uint16_t ubrrh_addr() const { return ubrrh_; }
    uint16_t udr_addr() const { return udr_; }

private:
    uint32_t bits_per_frame() const;
    void start_next_rx();

    uint16_t ucsra_ = 0, ucsrb_ = 0, ucsrc_ = 0;
    uint16_t ubrrl_ = 0, ubrrh_ = 0, udr_ = 0;
    uint8_t vec_rx_ = 0, vec_udre_ = 0, vec_tx_ = 0;

    // True where UCSRC and UBRRH are one location behind the URSEL bit.
    bool shared_ucsrc_ubrrh_ = false;

    uint8_t ucsra_v_ = 0x20;   // UDRE is set out of reset: the buffer is empty
    uint8_t ucsrb_v_ = 0;
    uint8_t ucsrc_v_ = 0x06;   // 8N1, the reset value on these parts
    uint16_t ubrr_v_ = 0;

    uint8_t tx_buffer_ = 0;
    bool tx_buffered_ = false;
    bool tx_active_ = false;
    uint64_t tx_done_ = 0;
    uint8_t tx_shift_ = 0;

    std::deque<uint8_t> rx_wire_;   // bytes offered but not yet shifted in
    bool rx_active_ = false;
    uint64_t rx_done_ = 0;
    uint8_t rx_shift_ = 0;
    uint8_t rx_data_ = 0;

    std::string output_;
    uint64_t now_ = 0;
};

// The successive-approximation ADC.
//
// Voltages go in from outside, in volts, and come back as the 10-bit value the
// sketch reads -- so a virtual potentiometer is set to 2.5 V rather than to
// 512, and the reference-voltage arithmetic that trips people up in real
// sketches trips them up here too.
class Adc : public Peripheral {
public:
    explicit Adc(const avr::AvrDevice& device);

    bool claims(uint16_t addr) const override;
    uint8_t read(uint16_t addr) override;
    void write(uint16_t addr, uint8_t value) override;
    void advance(uint64_t cycles) override;
    uint64_t next_event() const override;
    uint8_t pending_interrupt() const override;
    void acknowledge_interrupt() override;

    // The voltage on an ADC channel. Channels the part reserves (the bandgap
    // and ground channels) ignore this, because nothing outside can drive them.
    void set_channel_voltage(uint8_t channel, double volts);
    double channel_voltage(uint8_t channel) const;

    // Supply and external reference, both in volts. A 3.3 V board is a
    // different board, and the same analogRead on it means a different voltage.
    void set_supply_voltage(double volts) { avcc_ = volts; }
    void set_aref_voltage(double volts) { aref_ = volts; }

    uint16_t admux_addr() const { return admux_; }
    uint16_t adcsra_addr() const { return adcsra_; }
    uint16_t adcl_addr() const { return adcl_; }
    uint16_t adch_addr() const { return adch_; }

private:
    uint32_t prescaler() const;
    double reference_voltage() const;
    uint16_t sample(uint8_t channel) const;

    uint16_t admux_ = 0, adcsra_ = 0, adcsrb_ = 0, adcl_ = 0, adch_ = 0;
    uint8_t vec_adc_ = 0;

    // False where the device's adcsrb field points at a register that is not
    // an ADCSRB, which this peripheral must not answer for.
    bool owns_adcsrb_ = false;

    uint8_t admux_v_ = 0, adcsra_v_ = 0, adcsrb_v_ = 0;
    uint16_t result_ = 0;

    bool converting_ = false;
    bool first_conversion_ = true;
    uint64_t convert_done_ = 0;
    uint8_t convert_channel_ = 0;

    double avcc_ = 5.0;
    double aref_ = 5.0;
    double channel_[16] = {};
    uint64_t now_ = 0;
};

// The standard set of peripherals for a part, owned together.
//
// The typed pointers are the reason this is a struct rather than a bare
// vector: the machine has to reach the USART to print its output and the
// ports to wire parts to, and recovering those from a vector of Peripheral*
// would mean a dynamic_cast per access.
struct PeripheralSet {
    std::vector<std::unique_ptr<Peripheral>> owned;

    std::vector<GpioPort*> ports;   // one per port the part's pins land on
    Timer0* timer0 = nullptr;
    Usart0* usart0 = nullptr;
    Adc* adc = nullptr;

    // The port carrying a given PINx address, or nullptr. This is the lookup a
    // pin number turns into, via AvrDevice::pins.
    GpioPort* port_for(uint16_t pin_reg) const;
};

// Builds the ports the device's pin map mentions, plus timer 0, USART0 and the
// ADC for any of those the device describes.
PeripheralSet make_standard_peripherals(const avr::AvrDevice& device);

} // namespace ardio::emu
