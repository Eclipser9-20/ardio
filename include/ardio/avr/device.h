#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// A description of one AVR part, in the terms the runtime needs.
//
// The runtime used to be written for a single chip: core.S, serial.S, adc.S and
// the rest carried ATmega328P register addresses as bare literals, and the pin
// mapper had the Nano's silkscreen numbering compiled into a chain of compares.
// That made "support another board" mean "fork every assembly file".
//
// Instead the runtime is now written against symbolic names, and a device
// description is turned into a block of .equ definitions (plus one generated
// table) that is prepended to the assembly before it reaches the assembler.
// Supporting a new AVR part is then a matter of adding a row to the table in
// device.cpp -- no assembly changes at all.
namespace ardio::avr {

// Where one Arduino digital pin physically lands.
//
// Only the address of PINx is stored. On every AVR the three port registers sit
// together as PINx, DDRx = PINx+1, PORTx = PINx+2, so the other two fall out as
// displacements off the same pointer -- which is exactly how the runtime
// reaches them.
// A byte holds every PINx address on the parts ardio can currently build for.
// It is deliberately not wider, because widening it alone would not help: the
// generated pin map stores two bytes per pin, so a 16-bit address would also
// change that table's format and the lookup in the runtime that reads it.
//
// The parts that need more are the ATmega2560 and ATmega1280, whose PORTH, J,
// K and L sit in extended I/O at 0x100 and above. Those pins are recorded as
// absent rather than truncated, since truncating 0x100 to 0x00 would aim the
// runtime at r0 and corrupt the register file on every digitalWrite. Nothing
// is lost in practice today: both parts have more than 64 KB of flash, so the
// code generator refuses them outright and the pin map is unreachable. Widen
// this field, the table and the lookup together as part of that same work.
struct PinMapping {
    uint8_t pin_reg = 0;  // data-space address of PINx (0 = pin does not exist)
    uint8_t bit = 0;      // bit number within that port, 0-7
};

// An analog input: which ADC channel the Ax label selects.
struct AnalogMapping {
    uint8_t channel = 0;
    bool exists = false;
};

struct AvrDevice {
    std::string name;        // "atmega328p"

    // --- memory -------------------------------------------------------
    uint32_t flash_size = 0; // bytes, total including any bootloader
    uint32_t ram_size = 0;   // bytes of internal SRAM
    uint16_t ram_start = 0;  // data-space address of the first SRAM byte
    uint16_t ramend = 0;     // data-space address of the last SRAM byte
    uint32_t page_size = 0;  // flash page, bytes

    // True when flash is larger than 64 KB, so a word address no longer fits
    // in the 16 bits that ijmp/icall and lpm's Z pointer carry. Those parts
    // need EIND/RAMPZ handling and elpm, which the code generator does not do
    // yet; the device table marks them so the build can refuse clearly rather
    // than emit silently wrong jumps.
    bool far_flash = false;

    // --- USART0 -------------------------------------------------------
    // Data-space addresses. Parts differ: the ATmega328P puts these at 0xC0
    // and up, the ATmega8 has them in low I/O space.
    uint16_t ucsra = 0, ucsrb = 0, ucsrc = 0;
    uint16_t ubrrl = 0, ubrrh = 0, udr = 0;

    // --- ADC ----------------------------------------------------------
    uint16_t admux = 0, adcsra = 0, adcsrb = 0, adcl = 0, adch = 0;

    // --- timer 0 (millis/micros and the tone/PWM base) -----------------
    uint16_t tccr0a = 0, tccr0b = 0, tcnt0 = 0, timsk0 = 0, tifr0 = 0;
    // Zero-based index into the vector table, so the reset vector is 0 and the
    // byte address of the handler is index * 4 on parts whose vectors are jmp,
    // or index * 2 where they are rjmp. The datasheets number vectors from 1,
    // so every value here is one less than the number printed there -- worth
    // knowing before checking one against a datasheet and concluding it is
    // wrong. Nothing consumes this yet; millis() is not wired up.
    uint8_t timer0_ovf_vector = 0;

    // --- two-wire -----------------------------------------------------
    uint16_t twbr = 0, twsr = 0, twdr = 0, twcr = 0;

    // --- pin numbering ------------------------------------------------
    // Indexed by Arduino digital pin number. A default-constructed entry
    // (pin_reg == 0) means that number is not a pin on this board.
    std::vector<PinMapping> pins;
    std::vector<AnalogMapping> analog;  // indexed by the number in "A<n>"

    uint8_t led_builtin = 0;  // digital pin number of the on-board LED

    // The digital pin number that A0 also answers to, so analogRead(14) and
    // analogRead(0) mean the same input on a board numbered like the Uno.
    //
    // This cannot be derived as pins.size() - analog.size(), which is the
    // tempting shortcut. On the ATmega328P that subtraction gives 12 rather
    // than 14, because the table lists eight analog inputs while only six of
    // them have a digital pin behind them: A6 and A7 are ADC-only pads with no
    // port bit at all. Using the subtraction would make analogRead(14) sample
    // channel 2 and return an entirely plausible reading from the wrong pin,
    // which is the kind of wrong that gets debugged with an oscilloscope.
    uint8_t analog_pin_base = 0;
};

// Looks a part up by the name used in Board::mcu. Returns nullptr if ardio has
// no description for it.
const AvrDevice* find_device(std::string_view mcu);

const std::vector<AvrDevice>& device_database();

// Renders the device as assembly the rest of the runtime is written against.
//
// The contract, which every runtime .S file may rely on and nothing else may
// define:
//
//   .equ constants
//     AD_RAMEND, AD_RAMSTART, AD_F_CPU, AD_NUM_PINS, AD_NUM_ANALOG
//     AD_ANALOG_PIN_BASE
//     AD_SREG, AD_SPL, AD_SPH                 (identical on all AVR8 parts)
//       These three are I/O-space addresses (0x3F, 0x3D, 0x3E), not data-space
//       ones like everything else here. The reset sequence reaches them with
//       out, which cannot encode anything above 0x3F, so a data-space address
//       would not assemble. They are the only exception to the rule.
//     AD_UCSRA, AD_UCSRB, AD_UCSRC, AD_UBRRL, AD_UBRRH, AD_UDR
//     AD_ADMUX, AD_ADCSRA, AD_ADCSRB, AD_ADCL, AD_ADCH
//     AD_TCCR0A, AD_TCCR0B, AD_TCNT0, AD_TIMSK0, AD_TIFR0
//     AD_TWBR, AD_TWSR, AD_TWDR, AD_TWCR
//     AD_LED_BUILTIN
//
//   __ardio_pinmap
//     Two bytes per digital pin, in flash: the data-space address of that
//     pin's PINx register, then its bit number. A pin that does not exist on
//     the part is stored as 0, 0 -- callers must treat a zero address as "no
//     such pin" and do nothing, which is what the old out-of-range branch did.
//     Read with lpm; the byte address of pin n's entry is
//     __ardio_pinmap * 2 + n * 2, since labels are word addresses.
//
//   __ardio_analogmap
//     One byte per analog input: the ADC channel it selects, or 0xFF for an
//     analog number the part does not have.
//
// The build APPENDS this after the runtime rather than prepending it. The two
// tables emit real bytes, and word address 0 is the reset vector, so putting
// the prelude first would land the pin map on the entry point. Appending is
// safe because the assembler resolves symbols in a second pass, so the .equ
// definitions are still visible to code that sits above them.
//
// f_cpu comes from the board rather than the part, because the same silicon is
// clocked differently on different boards (16 MHz Uno, 8 MHz Pro Mini 3V3).
std::string device_prelude(const AvrDevice& device, int f_cpu);

} // namespace ardio::avr
