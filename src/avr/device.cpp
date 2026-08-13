// The table of AVR parts ardio knows how to build for, and the assembly
// prelude generated from a row of it.
//
// Every address below is the DATA-SPACE address from the part's datasheet
// register summary, not the I/O address. The two differ by 0x20 on all of
// these parts, and mixing them up is the classic way to write to the wrong
// register: the ATmega8's UCSRA is I/O 0x0B but data space 0x2B, and only the
// latter is meaningful to lds/sts. Using data-space addresses throughout means
// the runtime can reach any register with lds/sts and never has to care which
// ones happen to be low enough for in/out.
//
// The ATmega8 row is deliberately kept alongside the modern parts. It is the
// only one whose USART and ADC sit in low I/O space, and the only one with an
// undivided TCCR0 rather than the TCCR0A/TCCR0B pair, so if the abstraction
// ever quietly grows a 328P assumption the ATmega8 is what breaks first.

#include "ardio/avr/device.h"

#include <cstdio>

namespace ardio::avr {
namespace {

// Data-space addresses of PINx. A port occupies PINx, DDRx = PINx+1 and
// PORTx = PINx+2, and the ports run at a stride of three from PINA upwards, so
// these same numbers hold on the 328P, the 32U4 and the 2560 alike -- a part
// simply does not implement the ports it lacks.
constexpr uint16_t PIN_A = 0x20;
constexpr uint16_t PIN_B = 0x23;
constexpr uint16_t PIN_C = 0x26;
constexpr uint16_t PIN_D = 0x29;
constexpr uint16_t PIN_E = 0x2C;
constexpr uint16_t PIN_F = 0x2F;
constexpr uint16_t PIN_G = 0x32;

// The 2560 and 1280 run out of low I/O space after PORTG and put the rest of
// their ports in extended I/O, above 0xFF.
constexpr uint16_t PIN_H = 0x100;
constexpr uint16_t PIN_J = 0x103;
constexpr uint16_t PIN_K = 0x106;
constexpr uint16_t PIN_L = 0x109;

// PinMapping carries the PINx address in a single byte, which covers every
// port through PORTG but not PORTH..PORTL. Truncating 0x100 to 0x00 would not
// produce a broken pin, it would produce a pin that reads and writes r0 --
// silent, and very expensive to find. So a pin on an extended-I/O port is
// recorded as "does not exist" instead, which callers already handle by doing
// nothing at all. That loses 22 of the Mega's 70 pins; the real fix is to
// widen PinMapping::pin_reg to uint16_t, which is a change to a header this
// file is not allowed to make.
PinMapping pin(uint16_t port, uint8_t bit) {
    if (port > 0xFF) return {};
    return PinMapping{uint8_t(port), bit};
}

AnalogMapping ana(uint8_t channel) { return AnalogMapping{channel, true}; }

// Shared by every part whose peripherals sit at the addresses the ATmega328P
// established and the later megaAVRs kept: the 328P itself, the 168, the
// 32U4's ADC and timer, the 2560/1280 and the 1284P.
void modern_adc_and_timer0(AvrDevice& d) {
    d.admux  = 0x7C;
    d.adcsra = 0x7A;
    d.adcsrb = 0x7B;
    d.adcl   = 0x78;
    d.adch   = 0x79;

    d.tccr0a = 0x44;
    d.tccr0b = 0x45;
    d.tcnt0  = 0x46;
    d.timsk0 = 0x6E;
    d.tifr0  = 0x35;
}

// USART0 as laid out from the ATmega328P onwards. The 32U4 is the exception:
// it has no USART0 at all, only USART1, one register block higher.
void usart0_at_c0(AvrDevice& d) {
    d.ucsra = 0xC0;
    d.ucsrb = 0xC1;
    d.ucsrc = 0xC2;
    d.ubrrl = 0xC4;
    d.ubrrh = 0xC5;
    d.udr   = 0xC6;
}

void twi_at_b8(AvrDevice& d) {
    d.twbr = 0xB8;
    d.twsr = 0xB9;
    d.twdr = 0xBB;   // 0xBA is TWAR, which the runtime does not use
    d.twcr = 0xBC;
}

// ------------------------------------------------------------ the parts ---

// The Uno, Nano, Pro Mini and Duemilanove all carry this part, and all four
// use the same silkscreen numbering: 0-7 on PORTD, 8-13 on PORTB, 14-19 on
// PORTC. Only the Nano and the surface-mount Pro Mini break out A6 and A7,
// which are ADC-only inputs with no port pin behind them at all -- they are
// listed as analog channels and deliberately not as digital pins.
AvrDevice make_atmega328p() {
    AvrDevice d;
    d.name       = "atmega328p";
    d.flash_size = 32768;
    d.ram_size   = 2048;
    d.ram_start  = 0x0100;
    d.ramend     = 0x08FF;
    d.page_size  = 128;

    usart0_at_c0(d);
    modern_adc_and_timer0(d);
    twi_at_b8(d);
    d.timer0_ovf_vector = 16;

    d.pins = {
        pin(PIN_D, 0), pin(PIN_D, 1), pin(PIN_D, 2), pin(PIN_D, 3),
        pin(PIN_D, 4), pin(PIN_D, 5), pin(PIN_D, 6), pin(PIN_D, 7),
        pin(PIN_B, 0), pin(PIN_B, 1), pin(PIN_B, 2), pin(PIN_B, 3),
        pin(PIN_B, 4), pin(PIN_B, 5),
        pin(PIN_C, 0), pin(PIN_C, 1), pin(PIN_C, 2), pin(PIN_C, 3),
        pin(PIN_C, 4), pin(PIN_C, 5),
    };
    d.analog = {ana(0), ana(1), ana(2), ana(3), ana(4), ana(5), ana(6), ana(7)};
    // A0 is the pin numbered 14 above. It cannot be derived by subtracting the
    // analog count from the pin count, because A6 and A7 are counted as analog
    // inputs and have no digital pin to be subtracted.
    d.analog_pin_base = 14;
    d.led_builtin = 13;
    return d;
}

// Register-for-register the 328P with half the flash and half the RAM. The
// older Nano and Pro Mini shipped with it, which is why the pin numbering is
// identical rather than merely similar.
AvrDevice make_atmega168() {
    AvrDevice d  = make_atmega328p();
    d.name       = "atmega168";
    d.flash_size = 16384;
    d.ram_size   = 1024;
    d.ramend     = 0x04FF;
    return d;
}

// The Leonardo, Micro and Pro Micro.
//
// The digital numbering here is a board convention and nothing the silicon
// implies: it was chosen so that the Leonardo's header would line up with the
// Uno's, and the result is a mapping that jumps between five ports with no
// pattern to it (pin 3 is PD0 while pin 4 is PD4, pin 13 is PC7 while pin 5 is
// PC6). Reading it off the port layout instead of off the Leonardo pinout
// gives a table that looks plausible and is wrong on most entries.
//
// Pins 18 and up are the analog headers A0-A5, and the analog-only aliases A6
// to A11 are second names for digital pins that already exist lower down --
// A6 is pin 4, A8 is pin 8, and so on. They are given digital numbers 24-29
// by the core, so they appear twice in this table by design.
AvrDevice make_atmega32u4() {
    AvrDevice d;
    d.name       = "atmega32u4";
    d.flash_size = 32768;
    d.ram_size   = 2560;
    d.ram_start  = 0x0100;
    d.ramend     = 0x0AFF;
    d.page_size  = 128;

    // There is no USART0 on this part; the serial port on the header is
    // USART1, whose registers sit one block above where the 328P's are.
    d.ucsra = 0xC8;
    d.ucsrb = 0xC9;
    d.ucsrc = 0xCA;
    d.ubrrl = 0xCC;
    d.ubrrh = 0xCD;
    d.udr   = 0xCE;

    modern_adc_and_timer0(d);
    twi_at_b8(d);
    d.timer0_ovf_vector = 22;

    d.pins = {
        pin(PIN_D, 2), pin(PIN_D, 3), pin(PIN_D, 1), pin(PIN_D, 0),   //  0-3
        pin(PIN_D, 4), pin(PIN_C, 6), pin(PIN_D, 7), pin(PIN_E, 6),   //  4-7
        pin(PIN_B, 4), pin(PIN_B, 5), pin(PIN_B, 6), pin(PIN_B, 7),   //  8-11
        pin(PIN_D, 6), pin(PIN_C, 7),                                 // 12-13
        pin(PIN_B, 3), pin(PIN_B, 1), pin(PIN_B, 2), pin(PIN_B, 0),   // 14-17
        pin(PIN_F, 7), pin(PIN_F, 6), pin(PIN_F, 5), pin(PIN_F, 4),   // 18-21, A0-A3
        pin(PIN_F, 1), pin(PIN_F, 0),                                 // 22-23, A4-A5
        pin(PIN_D, 4), pin(PIN_D, 7), pin(PIN_B, 4), pin(PIN_B, 5),   // 24-27, A6-A9
        pin(PIN_B, 6), pin(PIN_D, 6),                                 // 28-29, A10-A11
    };

    // A0-A5 run down from ADC7 to ADC0 because PORTF is wired to the header in
    // reverse, and A6-A11 are the high channels on PORTD and PORTB. Channels
    // above 7 are selected by MUX5 in ADCSRB together with MUX4:0 in ADMUX,
    // where MUX5 = 1 and MUX4:0 = channel - 8; the channel number stored here
    // is the plain datasheet channel, so nothing is lost and the split into
    // two registers stays a decision for the runtime rather than the table.
    d.analog = {
        ana(7), ana(6), ana(5), ana(4), ana(1), ana(0),
        ana(8), ana(10), ana(11), ana(12), ana(13), ana(9),
    };
    // A0 is digital 18 on the Leonardo, the first of the six PORTF entries in
    // the table above. The six aliases A6-A11 sit at 24 and up and are not
    // contiguous with it, so folding a digital pin to an analog number is only
    // meaningful for the first six.
    d.analog_pin_base = 18;
    d.led_builtin = 13;
    return d;
}

// Both Megas share one pin numbering, so the table is built once and the two
// rows differ only in flash size.
std::vector<PinMapping> mega_pins() {
    return {
        pin(PIN_E, 0), pin(PIN_E, 1), pin(PIN_E, 4), pin(PIN_E, 5),   //  0-3
        pin(PIN_G, 5), pin(PIN_E, 3), pin(PIN_H, 3), pin(PIN_H, 4),   //  4-7
        pin(PIN_H, 5), pin(PIN_H, 6), pin(PIN_B, 4), pin(PIN_B, 5),   //  8-11
        pin(PIN_B, 6), pin(PIN_B, 7), pin(PIN_J, 1), pin(PIN_J, 0),   // 12-15
        pin(PIN_H, 1), pin(PIN_H, 0), pin(PIN_D, 3), pin(PIN_D, 2),   // 16-19
        pin(PIN_D, 1), pin(PIN_D, 0), pin(PIN_A, 0), pin(PIN_A, 1),   // 20-23
        pin(PIN_A, 2), pin(PIN_A, 3), pin(PIN_A, 4), pin(PIN_A, 5),   // 24-27
        pin(PIN_A, 6), pin(PIN_A, 7), pin(PIN_C, 7), pin(PIN_C, 6),   // 28-31
        pin(PIN_C, 5), pin(PIN_C, 4), pin(PIN_C, 3), pin(PIN_C, 2),   // 32-35
        pin(PIN_C, 1), pin(PIN_C, 0), pin(PIN_D, 7), pin(PIN_G, 2),   // 36-39
        pin(PIN_G, 1), pin(PIN_G, 0), pin(PIN_L, 7), pin(PIN_L, 6),   // 40-43
        pin(PIN_L, 5), pin(PIN_L, 4), pin(PIN_L, 3), pin(PIN_L, 2),   // 44-47
        pin(PIN_L, 1), pin(PIN_L, 0), pin(PIN_B, 3), pin(PIN_B, 2),   // 48-51
        pin(PIN_B, 1), pin(PIN_B, 0),                                 // 52-53
        pin(PIN_F, 0), pin(PIN_F, 1), pin(PIN_F, 2), pin(PIN_F, 3),   // 54-57, A0-A3
        pin(PIN_F, 4), pin(PIN_F, 5), pin(PIN_F, 6), pin(PIN_F, 7),   // 58-61, A4-A7
        pin(PIN_K, 0), pin(PIN_K, 1), pin(PIN_K, 2), pin(PIN_K, 3),   // 62-65, A8-A11
        pin(PIN_K, 4), pin(PIN_K, 5), pin(PIN_K, 6), pin(PIN_K, 7),   // 66-69, A12-A15
    };
}

AvrDevice make_atmega2560() {
    AvrDevice d;
    d.name       = "atmega2560";
    d.flash_size = 262144;
    d.ram_size   = 8192;
    // Extended I/O reaches to 0x01FF on this part, so SRAM starts a page later
    // than it does on the 328P.
    d.ram_start  = 0x0200;
    d.ramend     = 0x21FF;
    d.page_size  = 256;
    d.far_flash  = true;

    usart0_at_c0(d);
    modern_adc_and_timer0(d);
    twi_at_b8(d);
    d.timer0_ovf_vector = 23;

    d.pins   = mega_pins();
    d.analog = {
        ana(0), ana(1), ana(2),  ana(3),  ana(4),  ana(5),  ana(6),  ana(7),
        ana(8), ana(9), ana(10), ana(11), ana(12), ana(13), ana(14), ana(15),
    };
    // A0 is digital 54, where the PORTF block starts in the table above.
    d.analog_pin_base = 54;
    d.led_builtin = 13;
    return d;
}

// The original Mega. Same package, same pinout, same peripherals; a quarter of
// the flash, which is still past the 64 KB line, so far_flash stays set.
AvrDevice make_atmega1280() {
    AvrDevice d  = make_atmega2560();
    d.name       = "atmega1280";
    d.flash_size = 131072;
    return d;
}

// Not an official Arduino part, but the one megaAVR with 16 KB of SRAM, and
// the numbering below is the long-standing third-party convention for it:
// PORTB, PORTD, PORTC, PORTA taken eight pins at a time in that order. Like
// the Leonardo's, it is a board convention -- the silicon has no opinion about
// which port should be called "pin 0".
AvrDevice make_atmega1284p() {
    AvrDevice d;
    d.name       = "atmega1284p";
    d.flash_size = 131072;
    d.ram_size   = 16384;
    d.ram_start  = 0x0100;
    d.ramend     = 0x40FF;
    d.page_size  = 256;
    d.far_flash  = true;

    usart0_at_c0(d);
    modern_adc_and_timer0(d);
    twi_at_b8(d);
    d.timer0_ovf_vector = 18;

    d.pins = {
        pin(PIN_B, 0), pin(PIN_B, 1), pin(PIN_B, 2), pin(PIN_B, 3),
        pin(PIN_B, 4), pin(PIN_B, 5), pin(PIN_B, 6), pin(PIN_B, 7),
        pin(PIN_D, 0), pin(PIN_D, 1), pin(PIN_D, 2), pin(PIN_D, 3),
        pin(PIN_D, 4), pin(PIN_D, 5), pin(PIN_D, 6), pin(PIN_D, 7),
        pin(PIN_C, 0), pin(PIN_C, 1), pin(PIN_C, 2), pin(PIN_C, 3),
        pin(PIN_C, 4), pin(PIN_C, 5), pin(PIN_C, 6), pin(PIN_C, 7),
        pin(PIN_A, 0), pin(PIN_A, 1), pin(PIN_A, 2), pin(PIN_A, 3),
        pin(PIN_A, 4), pin(PIN_A, 5), pin(PIN_A, 6), pin(PIN_A, 7),
    };
    d.analog = {ana(0), ana(1), ana(2), ana(3), ana(4), ana(5), ana(6), ana(7)};
    // PORTA carries the ADC inputs and is the last block in the numbering
    // above, so A0 is digital 24. This follows from the pin table rather than
    // from any official pinout -- there is no official one for this part, and
    // third-party cores that number the ports in a different order arrive at a
    // different answer. The two must be changed together.
    d.analog_pin_base = 24;
    d.led_builtin = 13;
    return d;
}

// The part on the boards that predate the Duemilanove. Its peripherals are the
// reason this table exists at all: the USART and the ADC are in low I/O space
// at addresses that share nothing with the 328P's, and two registers the rest
// of the table takes for granted are simply not there.
AvrDevice make_atmega8() {
    AvrDevice d;
    d.name       = "atmega8";
    d.flash_size = 8192;
    d.ram_size   = 1024;
    // Only 32 general registers and 64 I/O registers precede SRAM here; there
    // is no extended I/O space, so RAM starts at 0x0060 rather than 0x0100.
    d.ram_start  = 0x0060;
    d.ramend     = 0x045F;
    d.page_size  = 64;

    // UCSRC and UBRRH are the same location, told apart by the URSEL bit in
    // the value written: URSEL set means the write lands in UCSRC. Both fields
    // therefore hold 0x40, and any code that writes the baud rate high byte
    // must leave URSEL clear.
    d.ucsra = 0x2B;
    d.ucsrb = 0x2A;
    d.ucsrc = 0x40;
    d.ubrrl = 0x29;
    d.ubrrh = 0x40;
    d.udr   = 0x2C;

    d.admux  = 0x27;
    d.adcsra = 0x26;
    // There is no ADCSRB on the ATmega8. The auto-trigger source bits that
    // later parts put there live in SFIOR instead, so SFIOR's address is what
    // this field carries -- it is the register that answers the same question,
    // and pointing the name at nothing would leave the runtime writing to
    // address zero.
    d.adcsrb = 0x50;
    d.adcl   = 0x24;
    d.adch   = 0x25;

    // Timer 0 here is a plain 8-bit counter with no output-compare unit, so
    // there is no TCCR0A/TCCR0B split: one TCCR0 holds the clock-select bits.
    // It is recorded as tccr0b because that is where CS02:0 live on every
    // other part in the table, and tccr0a is left at zero to say plainly that
    // the waveform-mode register does not exist.
    d.tccr0a = 0x0000;
    d.tccr0b = 0x33;
    d.tcnt0  = 0x32;
    // The interrupt mask and flag registers are shared by all three timers
    // rather than split per timer, hence TIMSK and TIFR under the 0-suffixed
    // names.
    d.timsk0 = 0x39;
    d.tifr0  = 0x38;
    d.timer0_ovf_vector = 9;

    d.twbr = 0x20;
    d.twsr = 0x21;
    d.twdr = 0x23;
    d.twcr = 0x56;

    // The ports are where the ATmega8 does agree with the 328P, which is why
    // the old boards and the new ones share a silkscreen.
    d.pins = {
        pin(PIN_D, 0), pin(PIN_D, 1), pin(PIN_D, 2), pin(PIN_D, 3),
        pin(PIN_D, 4), pin(PIN_D, 5), pin(PIN_D, 6), pin(PIN_D, 7),
        pin(PIN_B, 0), pin(PIN_B, 1), pin(PIN_B, 2), pin(PIN_B, 3),
        pin(PIN_B, 4), pin(PIN_B, 5),
        pin(PIN_C, 0), pin(PIN_C, 1), pin(PIN_C, 2), pin(PIN_C, 3),
        pin(PIN_C, 4), pin(PIN_C, 5),
    };
    // The 28-pin package brings out six ADC inputs; the two extra channels of
    // the surface-mount package have no header on any board that used this
    // part, so they are not offered.
    d.analog = {ana(0), ana(1), ana(2), ana(3), ana(4), ana(5)};
    // The same silkscreen as the 328P boards, so A0 is digital 14 here too --
    // and here it happens to equal pins.size() - analog.size(), which is a
    // coincidence of this part having no ADC-only pads rather than a rule.
    d.analog_pin_base = 14;
    d.led_builtin = 13;
    return d;
}

// ---------------------------------------------------------- rendering ---

std::string hex(long value, int digits) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "0x%0*lX", digits, value);
    return buf;
}

void equ(std::string& out, const char* name, const std::string& value) {
    out += ".equ ";
    out += name;
    out += ", ";
    out += value;
    out += "\n";
}

void equ_addr(std::string& out, const char* name, uint16_t address) {
    equ(out, name, hex(address, address > 0xFF ? 4 : 2));
}

} // namespace

const std::vector<AvrDevice>& device_database() {
    static const std::vector<AvrDevice> table = {
        make_atmega328p(), make_atmega168(),  make_atmega32u4(),
        make_atmega2560(), make_atmega1280(), make_atmega1284p(),
        make_atmega8(),
    };
    return table;
}

const AvrDevice* find_device(std::string_view mcu) {
    for (const AvrDevice& d : device_database())
        if (d.name == mcu) return &d;
    return nullptr;
}

std::string device_prelude(const AvrDevice& device, int f_cpu) {
    std::string out;
    out += "; Device description for ";
    out += device.name;
    out += ", generated from ardio's device table.\n";
    out += "; Every address is a data-space address, so lds and sts reach it directly.\n";
    out += ";\n";
    out += "; This block is prepended to the runtime, which means the two tables at the\n";
    out += "; end of it would otherwise land on top of the reset vector at word address 0.\n";
    out += "; The rjmp below occupies that word instead and steps over them, so the first\n";
    out += "; instruction of the runtime proper is still what the part executes on reset.\n";
    out += "\n";
    out += "        rjmp    __ardio_prelude_end\n";
    out += "\n";

    equ(out, "AD_RAMEND", hex(device.ramend, 4));
    equ(out, "AD_RAMSTART", hex(device.ram_start, 4));
    equ(out, "AD_F_CPU", std::to_string(f_cpu));
    equ(out, "AD_NUM_PINS", std::to_string(device.pins.size()));
    equ(out, "AD_NUM_ANALOG", std::to_string(device.analog.size()));
    equ(out, "AD_ANALOG_PIN_BASE", std::to_string(device.analog_pin_base));
    out += "\n";

    // These three are the exception to the data-space rule above, and it is a
    // deliberate one. SREG and the stack pointer are the only registers the
    // runtime reaches with in/out rather than lds/sts -- they are needed
    // before the stack exists, and in/out is one word and one cycle. But in
    // and out take an I/O address, which is the data-space address less 0x20,
    // so handing them 0x5F would be rejected as out of range at assembly
    // time. Their I/O addresses are identical on every AVR8 part, so there is
    // nothing device-specific to lose by naming them that way.
    equ(out, "AD_SREG", "0x3F");
    equ(out, "AD_SPL", "0x3D");
    equ(out, "AD_SPH", "0x3E");
    out += "\n";

    equ_addr(out, "AD_UCSRA", device.ucsra);
    equ_addr(out, "AD_UCSRB", device.ucsrb);
    equ_addr(out, "AD_UCSRC", device.ucsrc);
    equ_addr(out, "AD_UBRRL", device.ubrrl);
    equ_addr(out, "AD_UBRRH", device.ubrrh);
    equ_addr(out, "AD_UDR", device.udr);
    out += "\n";

    equ_addr(out, "AD_ADMUX", device.admux);
    equ_addr(out, "AD_ADCSRA", device.adcsra);
    equ_addr(out, "AD_ADCSRB", device.adcsrb);
    equ_addr(out, "AD_ADCL", device.adcl);
    equ_addr(out, "AD_ADCH", device.adch);
    out += "\n";

    equ_addr(out, "AD_TCCR0A", device.tccr0a);
    equ_addr(out, "AD_TCCR0B", device.tccr0b);
    equ_addr(out, "AD_TCNT0", device.tcnt0);
    equ_addr(out, "AD_TIMSK0", device.timsk0);
    equ_addr(out, "AD_TIFR0", device.tifr0);
    out += "\n";

    equ_addr(out, "AD_TWBR", device.twbr);
    equ_addr(out, "AD_TWSR", device.twsr);
    equ_addr(out, "AD_TWDR", device.twdr);
    equ_addr(out, "AD_TWCR", device.twcr);
    out += "\n";

    equ(out, "AD_LED_BUILTIN", std::to_string(device.led_builtin));
    out += "\n";

    // Two bytes per pin so that the runtime can index it by shifting the pin
    // number left once, and PINx first so that a single lpm gives the address
    // the other two port registers are reached from.
    out += "__ardio_pinmap:\n";
    for (size_t i = 0; i < device.pins.size(); ++i) {
        const PinMapping& p = device.pins[i];
        char line[96];
        std::snprintf(line, sizeof line, "        .byte 0x%02X, %u    ; pin %u\n",
                      unsigned(p.pin_reg), unsigned(p.bit), unsigned(i));
        out += line;
    }
    out += "\n";

    out += "__ardio_analogmap:\n";
    for (size_t i = 0; i < device.analog.size(); ++i) {
        const AnalogMapping& a = device.analog[i];
        char line[96];
        std::snprintf(line, sizeof line, "        .byte %u    ; A%u\n",
                      a.exists ? unsigned(a.channel) : 0xFFu, unsigned(i));
        out += line;
    }
    out += "\n";

    out += "__ardio_prelude_end:\n";
    return out;
}

} // namespace ardio::avr
