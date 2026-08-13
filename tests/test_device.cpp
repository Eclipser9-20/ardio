// Tests for the AVR device table and the assembly prelude generated from it.
//
// The register addresses below were read off the datasheet register summaries
// by hand rather than copied out of device.cpp, so that a typo in the table
// has to be made twice before it can reach a board. A wrong address is not a
// failure that shows up as a crash: the runtime writes to some other live
// register and the symptom appears somewhere unrelated, which is exactly the
// kind of bug worth paying for a second transcription to avoid.

#include "harness.h"

#include "ardio/avr/assembler.h"
#include "ardio/avr/device.h"

#include <algorithm>
#include <string>

using ardio::avr::AvrDevice;
using ardio::avr::device_prelude;
using ardio::avr::find_device;

namespace {

// Assembles source and reports the assembler's own message on failure, since
// "the prelude does not assemble" is useless without the line it choked on.
ardio::AssembleResult assemble_or_report(const std::string& source) {
    ardio::AssembleResult r = ardio::assemble(source);
    if (!r.ok) ::ardio_test::fail(__FILE__, __LINE__, "assemble failed: " + r.error);
    return r;
}

// Recovers the value the assembler gave a constant or label, by making it
// assemble a pair of ldi instructions that load the low and high bytes and
// then decoding them out of the image. This asks the real assembler what the
// symbol means rather than trusting the generator's own arithmetic.
//
// ldi encodes its immediate split in two nibbles: 1110 KKKK dddd KKKK.
bool symbol_value(const std::string& prelude, const std::string& name, long& out) {
    std::string src = prelude + "\n        ldi r16, lo8(" + name + ")\n" +
                      "        ldi r17, hi8(" + name + ")\n";
    ardio::AssembleResult r = ardio::assemble(src);
    if (!r.ok) return false;
    size_t n = r.code.size();
    if (n < 4) return false;
    auto imm = [&](size_t byte_index) {
        unsigned w = unsigned(r.code[byte_index]) | (unsigned(r.code[byte_index + 1]) << 8);
        return ((w >> 4) & 0xF0) | (w & 0x0F);
    };
    out = long(imm(n - 4)) | (long(imm(n - 2)) << 8);
    return true;
}

// The prelude places __ardio_pinmap and __ardio_analogmap at word addresses,
// so a table entry's byte offset in the image is twice the label's value.
bool table_bytes(const std::string& prelude, const std::string& label,
                 std::vector<uint8_t>& image, size_t& byte_offset) {
    ardio::AssembleResult r = ardio::assemble(prelude);
    if (!r.ok) return false;
    long word = 0;
    if (!symbol_value(prelude, label, word)) return false;
    image = r.code;
    byte_offset = size_t(word) * 2;
    return byte_offset <= image.size();
}

const AvrDevice& device(const char* name) {
    const AvrDevice* d = find_device(name);
    if (!d) {
        ::ardio_test::fail(__FILE__, __LINE__, std::string("no device named ") + name);
        static const AvrDevice empty;
        return empty;
    }
    return *d;
}

} // namespace

TEST(find_device_rejects_a_part_that_is_not_in_the_table) {
    CHECK(find_device("atmega328") == nullptr);      // no trailing p: a different part
    CHECK(find_device("attiny85") == nullptr);
    CHECK(find_device("") == nullptr);
    CHECK(find_device("ATMEGA328P") == nullptr);     // the match is exact, not folded
    CHECK(find_device("atmega328p") != nullptr);
}

TEST(atmega328p_memory_and_flash_geometry) {
    const AvrDevice& d = device("atmega328p");
    CHECK_EQ(int(d.flash_size), 32768);
    CHECK_EQ(int(d.ram_size), 2048);
    CHECK_EQ(int(d.ram_start), 0x0100);
    CHECK_EQ(int(d.ramend), 0x08FF);
    CHECK_EQ(int(d.page_size), 128);
    CHECK(!d.far_flash);
    // RAMEND must be the last byte of SRAM, not one past it.
    CHECK_EQ(int(d.ramend), int(d.ram_start) + int(d.ram_size) - 1);
}

TEST(atmega328p_peripheral_addresses) {
    const AvrDevice& d = device("atmega328p");
    CHECK_EQ(int(d.ucsra), 0xC0);
    CHECK_EQ(int(d.ucsrb), 0xC1);
    CHECK_EQ(int(d.ucsrc), 0xC2);
    CHECK_EQ(int(d.ubrrl), 0xC4);
    CHECK_EQ(int(d.ubrrh), 0xC5);
    CHECK_EQ(int(d.udr), 0xC6);

    CHECK_EQ(int(d.adcl), 0x78);
    CHECK_EQ(int(d.adch), 0x79);
    CHECK_EQ(int(d.adcsra), 0x7A);
    CHECK_EQ(int(d.adcsrb), 0x7B);
    CHECK_EQ(int(d.admux), 0x7C);

    CHECK_EQ(int(d.tccr0a), 0x44);
    CHECK_EQ(int(d.tccr0b), 0x45);
    CHECK_EQ(int(d.tcnt0), 0x46);
    CHECK_EQ(int(d.timsk0), 0x6E);
    CHECK_EQ(int(d.tifr0), 0x35);

    CHECK_EQ(int(d.twbr), 0xB8);
    CHECK_EQ(int(d.twsr), 0xB9);
    CHECK_EQ(int(d.twdr), 0xBB);
    CHECK_EQ(int(d.twcr), 0xBC);

    CHECK_EQ(int(d.led_builtin), 13);
}

TEST(atmega328p_pin_map_matches_the_nano_silkscreen) {
    const AvrDevice& d = device("atmega328p");
    CHECK_EQ(d.pins.size(), size_t(20));

    // Pins 0-7 are PORTD bits 0-7, and PIND is at 0x29.
    for (int p = 0; p <= 7; ++p) {
        CHECK_EQ(int(d.pins[size_t(p)].pin_reg), 0x29);
        CHECK_EQ(int(d.pins[size_t(p)].bit), p);
    }
    // Pins 8-13 are PORTB bits 0-5, and PINB is at 0x23.
    for (int p = 8; p <= 13; ++p) {
        CHECK_EQ(int(d.pins[size_t(p)].pin_reg), 0x23);
        CHECK_EQ(int(d.pins[size_t(p)].bit), p - 8);
    }
    // Pins 14-19 are A0-A5 on PORTC bits 0-5, and PINC is at 0x26.
    for (int p = 14; p <= 19; ++p) {
        CHECK_EQ(int(d.pins[size_t(p)].pin_reg), 0x26);
        CHECK_EQ(int(d.pins[size_t(p)].bit), p - 14);
    }

    // The on-board LED is pin 13, which is PB5 -- the pin the bootloader
    // blinks and the one every first sketch toggles.
    CHECK_EQ(int(d.pins[13].pin_reg), 0x23);
    CHECK_EQ(int(d.pins[13].bit), 5);
}

TEST(a_pin_number_the_part_does_not_have_reads_as_absent) {
    const AvrDevice& d = device("atmega328p");
    // A6 and A7 exist as ADC channels on the Nano but have no port pin, so
    // there is no digital pin 20 to ask about.
    CHECK(20u >= d.pins.size());
    // And an entry that is present but empty must still say so through a zero
    // register address, which is the signal callers act on.
    ardio::avr::PinMapping absent;
    CHECK_EQ(int(absent.pin_reg), 0);
    CHECK_EQ(int(absent.bit), 0);
}

TEST(atmega168_is_the_328p_with_half_the_memory) {
    const AvrDevice& a = device("atmega168");
    const AvrDevice& b = device("atmega328p");
    CHECK_EQ(int(a.flash_size), 16384);
    CHECK_EQ(int(a.ram_size), 1024);
    CHECK_EQ(int(a.ramend), 0x04FF);
    CHECK_EQ(int(a.page_size), 128);
    CHECK(!a.far_flash);
    CHECK_EQ(int(a.ucsra), int(b.ucsra));
    CHECK_EQ(int(a.admux), int(b.admux));
    CHECK_EQ(int(a.twcr), int(b.twcr));
    CHECK_EQ(a.pins.size(), b.pins.size());
}

TEST(atmega8_keeps_its_peripherals_in_low_io_space) {
    const AvrDevice& d = device("atmega8");
    CHECK_EQ(int(d.flash_size), 8192);
    CHECK_EQ(int(d.ram_size), 1024);
    CHECK_EQ(int(d.ram_start), 0x0060);
    CHECK_EQ(int(d.ramend), 0x045F);
    CHECK_EQ(int(d.page_size), 64);

    // The whole point of this row: nothing here is where the 328P puts it.
    CHECK_EQ(int(d.ucsra), 0x2B);
    CHECK_EQ(int(d.ucsrb), 0x2A);
    CHECK_EQ(int(d.ubrrl), 0x29);
    CHECK_EQ(int(d.udr), 0x2C);
    // UCSRC and UBRRH share one address, told apart by the URSEL bit.
    CHECK_EQ(int(d.ucsrc), 0x40);
    CHECK_EQ(int(d.ubrrh), 0x40);

    CHECK_EQ(int(d.adcl), 0x24);
    CHECK_EQ(int(d.adch), 0x25);
    CHECK_EQ(int(d.adcsra), 0x26);
    CHECK_EQ(int(d.admux), 0x27);

    CHECK_EQ(int(d.tcnt0), 0x32);
    CHECK_EQ(int(d.tccr0b), 0x33);
    CHECK_EQ(int(d.tifr0), 0x38);
    CHECK_EQ(int(d.timsk0), 0x39);
    // There is no waveform-mode register for timer 0 on this part.
    CHECK_EQ(int(d.tccr0a), 0);

    CHECK_EQ(int(d.twbr), 0x20);
    CHECK_EQ(int(d.twsr), 0x21);
    CHECK_EQ(int(d.twdr), 0x23);
    CHECK_EQ(int(d.twcr), 0x56);

    // The ports, by contrast, are exactly where the 328P has them, which is
    // why the old boards share the modern silkscreen.
    CHECK_EQ(d.pins.size(), size_t(20));
    CHECK_EQ(int(d.pins[0].pin_reg), 0x29);
    CHECK_EQ(int(d.pins[13].pin_reg), 0x23);
    CHECK_EQ(int(d.pins[13].bit), 5);
    CHECK_EQ(int(d.pins[14].pin_reg), 0x26);
    CHECK_EQ(d.analog.size(), size_t(6));
}

TEST(atmega32u4_uses_usart1_and_the_leonardo_numbering) {
    const AvrDevice& d = device("atmega32u4");
    CHECK_EQ(int(d.flash_size), 32768);
    CHECK_EQ(int(d.ram_size), 2560);
    CHECK_EQ(int(d.ramend), 0x0AFF);
    CHECK_EQ(int(d.page_size), 128);
    CHECK(!d.far_flash);

    // No USART0 exists on this part; these are USART1's registers.
    CHECK_EQ(int(d.ucsra), 0xC8);
    CHECK_EQ(int(d.ucsrb), 0xC9);
    CHECK_EQ(int(d.ucsrc), 0xCA);
    CHECK_EQ(int(d.ubrrl), 0xCC);
    CHECK_EQ(int(d.ubrrh), 0xCD);
    CHECK_EQ(int(d.udr), 0xCE);

    // ADC and timer 0 did not move.
    CHECK_EQ(int(d.admux), 0x7C);
    CHECK_EQ(int(d.adcsrb), 0x7B);
    CHECK_EQ(int(d.tccr0b), 0x45);

    // Spot checks against the Leonardo pinout. These are the entries a table
    // written from the port layout would get wrong.
    CHECK_EQ(int(d.pins[0].pin_reg), 0x29);   // PD2, the RX pin
    CHECK_EQ(int(d.pins[0].bit), 2);
    CHECK_EQ(int(d.pins[3].pin_reg), 0x29);   // PD0, not PD3
    CHECK_EQ(int(d.pins[3].bit), 0);
    CHECK_EQ(int(d.pins[5].pin_reg), 0x26);   // PC6
    CHECK_EQ(int(d.pins[5].bit), 6);
    CHECK_EQ(int(d.pins[7].pin_reg), 0x2C);   // PE6, the only PORTE pin
    CHECK_EQ(int(d.pins[7].bit), 6);
    CHECK_EQ(int(d.pins[13].pin_reg), 0x26);  // PC7, not PB5 as on the Uno
    CHECK_EQ(int(d.pins[13].bit), 7);
    CHECK_EQ(int(d.pins[18].pin_reg), 0x2F);  // A0 is PF7
    CHECK_EQ(int(d.pins[18].bit), 7);
    CHECK_EQ(int(d.led_builtin), 13);
}

TEST(atmega32u4_analog_channels_include_the_ones_above_seven) {
    const AvrDevice& d = device("atmega32u4");
    CHECK_EQ(d.analog.size(), size_t(12));
    // PORTF is wired to the header in reverse, so A0 is the highest channel.
    CHECK_EQ(int(d.analog[0].channel), 7);
    CHECK_EQ(int(d.analog[1].channel), 6);
    CHECK_EQ(int(d.analog[5].channel), 0);
    // A6-A11 are the channels that need MUX5 in ADCSRB. The channel number is
    // stored whole, so nothing about them is truncated to fit.
    CHECK_EQ(int(d.analog[6].channel), 8);
    CHECK_EQ(int(d.analog[7].channel), 10);
    CHECK_EQ(int(d.analog[11].channel), 9);
    for (const auto& a : d.analog) CHECK(a.exists);
}

TEST(atmega2560_is_marked_as_needing_far_flash_handling) {
    const AvrDevice& d = device("atmega2560");
    CHECK_EQ(int(d.flash_size), 262144);
    CHECK_EQ(int(d.ram_size), 8192);
    CHECK_EQ(int(d.ram_start), 0x0200);   // extended I/O runs to 0x01FF here
    CHECK_EQ(int(d.ramend), 0x21FF);
    CHECK_EQ(int(d.page_size), 256);
    CHECK(d.far_flash);
    CHECK_EQ(d.pins.size(), size_t(70));
    CHECK_EQ(d.analog.size(), size_t(16));

    CHECK_EQ(int(d.ucsra), 0xC0);
    CHECK_EQ(int(d.udr), 0xC6);
    CHECK_EQ(int(d.admux), 0x7C);
    CHECK_EQ(int(d.twcr), 0xBC);

    // Pins on ports A through G are representable and must be right.
    CHECK_EQ(int(d.pins[0].pin_reg), 0x2C);   // PE0
    CHECK_EQ(int(d.pins[0].bit), 0);
    CHECK_EQ(int(d.pins[13].pin_reg), 0x23);  // PB7, the LED
    CHECK_EQ(int(d.pins[13].bit), 7);
    CHECK_EQ(int(d.pins[22].pin_reg), 0x20);  // PA0
    CHECK_EQ(int(d.pins[22].bit), 0);
    CHECK_EQ(int(d.pins[54].pin_reg), 0x2F);  // PF0, A0
    CHECK_EQ(int(d.pins[54].bit), 0);
    CHECK_EQ(int(d.led_builtin), 13);
}

TEST(mega_pins_on_extended_io_ports_are_reported_as_absent) {
    // PORTH, PORTJ, PORTK and PORTL live at 0x100 and above, which does not
    // fit the byte PinMapping reserves for the address. Truncating would give
    // pin 6 the address 0x00 -- register r0 -- so those pins are declared not
    // to exist instead, which callers already ignore safely.
    const AvrDevice& d = device("atmega2560");
    const int extended[] = {6, 7, 8, 9, 14, 15, 16, 17, 42, 49, 62, 69};
    for (int p : extended) CHECK_EQ(int(d.pins[size_t(p)].pin_reg), 0);
}

TEST(atmega1280_shares_the_2560_pinout_with_less_flash) {
    const AvrDevice& a = device("atmega1280");
    const AvrDevice& b = device("atmega2560");
    CHECK_EQ(int(a.flash_size), 131072);
    CHECK_EQ(int(a.ram_size), 8192);
    CHECK_EQ(int(a.ramend), 0x21FF);
    CHECK_EQ(int(a.page_size), 256);
    CHECK(a.far_flash);
    CHECK_EQ(a.pins.size(), b.pins.size());
    CHECK_EQ(int(a.pins[54].pin_reg), int(b.pins[54].pin_reg));
}

TEST(atmega1284p_has_sixteen_kilobytes_of_ram_and_four_full_ports) {
    const AvrDevice& d = device("atmega1284p");
    CHECK_EQ(int(d.flash_size), 131072);
    CHECK_EQ(int(d.ram_size), 16384);
    CHECK_EQ(int(d.ram_start), 0x0100);
    CHECK_EQ(int(d.ramend), 0x40FF);
    CHECK_EQ(int(d.page_size), 256);
    CHECK(d.far_flash);

    CHECK_EQ(int(d.ucsra), 0xC0);
    CHECK_EQ(int(d.admux), 0x7C);
    CHECK_EQ(int(d.tifr0), 0x35);
    CHECK_EQ(int(d.twbr), 0xB8);

    // All four ports fit in low I/O space, so every one of the 32 pins is
    // representable -- none of them may come back as absent.
    CHECK_EQ(d.pins.size(), size_t(32));
    for (const auto& p : d.pins) CHECK(p.pin_reg != 0);
    CHECK_EQ(int(d.pins[0].pin_reg), 0x23);   // PB0
    CHECK_EQ(int(d.pins[8].pin_reg), 0x29);   // PD0
    CHECK_EQ(int(d.pins[16].pin_reg), 0x26);  // PC0
    CHECK_EQ(int(d.pins[24].pin_reg), 0x20);  // PA0
    CHECK_EQ(int(d.pins[31].bit), 7);
    CHECK_EQ(d.analog.size(), size_t(8));
}

TEST(a0_is_digital_pin_14_and_not_the_difference_of_the_two_counts) {
    const AvrDevice& d = device("atmega328p");
    CHECK_EQ(int(d.analog_pin_base), 14);
    // The arithmetic shortcut gives 12 here, because A6 and A7 are counted as
    // analog inputs and have no digital pin to subtract. Twelve would make
    // analogRead(14) sample channel 2 and hand back an entirely plausible
    // number from the wrong pad. Pinning the inequality is what stops the
    // shortcut being reintroduced later as a tidy-up.
    CHECK_EQ(d.pins.size() - d.analog.size(), size_t(12));
    CHECK(size_t(d.analog_pin_base) != d.pins.size() - d.analog.size());
}

TEST(every_parts_analog_pin_base_agrees_with_its_own_pin_table) {
    // A0's digital pin and the first analog entry name the same pad, so the
    // base has to land on a real pin, and the analog inputs that do have
    // digital pins have to continue consecutively from it -- that consecutive
    // run is the whole premise of folding a pin number by subtraction.
    for (const AvrDevice& d : ardio::avr::device_database()) {
        size_t base = d.analog_pin_base;
        if (base >= d.pins.size()) {
            ::ardio_test::fail(__FILE__, __LINE__,
                               d.name + ": analog_pin_base is not a digital pin");
            continue;
        }
        CHECK(d.pins[base].pin_reg != 0);

        // The run is shorter than the analog count wherever the part has
        // ADC-only pads: the 328P's A6 and A7 sit past the end of the pin
        // table altogether. The Leonardo leaves the run after A5, where
        // A6-A11 become aliases numbered 24 and up, and the Mega's A8-A15 are
        // on PORTK in extended I/O, which a byte-wide address cannot reach.
        size_t run = std::min(d.analog.size(), d.pins.size() - base);
        if (d.name == "atmega32u4") run = std::min(run, size_t(6));
        CHECK(run >= 6);
        bool extended_io = d.name == "atmega2560" || d.name == "atmega1280";
        for (size_t a = 0; a < run; ++a) {
            if (d.pins[base + a].pin_reg == 0 && !extended_io)
                ::ardio_test::fail(__FILE__, __LINE__,
                                   d.name + ": A" + std::to_string(a) +
                                       " has no digital pin behind it");
        }
    }

    CHECK_EQ(int(device("atmega168").analog_pin_base), 14);
    CHECK_EQ(int(device("atmega8").analog_pin_base), 14);
    CHECK_EQ(int(device("atmega32u4").analog_pin_base), 18);
    CHECK_EQ(int(device("atmega2560").analog_pin_base), 54);
    CHECK_EQ(int(device("atmega1280").analog_pin_base), 54);
    // Derived from this table's own numbering, which puts PORTA last.
    CHECK_EQ(int(device("atmega1284p").analog_pin_base), 24);
}

TEST(the_pin_at_the_analog_base_is_the_port_bit_a0_lives_on) {
    // Spot checks that each base lands on the pad A0 actually is, read out of
    // the pin table rather than restated as a second number.
    CHECK_EQ(int(device("atmega328p").pins[14].pin_reg), 0x26);   // PC0
    CHECK_EQ(int(device("atmega328p").pins[14].bit), 0);
    CHECK_EQ(int(device("atmega8").pins[14].pin_reg), 0x26);      // PC0
    CHECK_EQ(int(device("atmega32u4").pins[18].pin_reg), 0x2F);   // PF7
    CHECK_EQ(int(device("atmega32u4").pins[18].bit), 7);
    CHECK_EQ(int(device("atmega2560").pins[54].pin_reg), 0x2F);   // PF0
    CHECK_EQ(int(device("atmega2560").pins[54].bit), 0);
    CHECK_EQ(int(device("atmega1284p").pins[24].pin_reg), 0x20);  // PA0
    CHECK_EQ(int(device("atmega1284p").pins[24].bit), 0);
}

TEST(the_table_holds_every_part_ardio_claims_to_support) {
    const auto& table = ardio::avr::device_database();
    CHECK_EQ(table.size(), size_t(7));
    const char* names[] = {"atmega328p", "atmega168",  "atmega32u4", "atmega2560",
                           "atmega1280", "atmega1284p", "atmega8"};
    for (const char* n : names) CHECK(find_device(n) != nullptr);
    // Nothing in the table may be half-filled: a zero address would assemble
    // fine and then write to r0 on real hardware.
    for (const AvrDevice& d : table) {
        CHECK(!d.name.empty());
        CHECK(d.flash_size != 0);
        CHECK(d.page_size != 0);
        CHECK(d.ramend != 0);
        CHECK(d.ucsra != 0);
        CHECK(d.udr != 0);
        CHECK(d.admux != 0);
        CHECK(d.tcnt0 != 0);
        CHECK(d.twcr != 0);
        CHECK(!d.pins.empty());
        CHECK(!d.analog.empty());
    }
}

TEST(every_devices_prelude_assembles) {
    // The prelude is prepended to every runtime file, so a prelude the
    // assembler rejects does not fail one feature, it fails the whole build.
    for (const AvrDevice& d : ardio::avr::device_database()) {
        ardio::AssembleResult r = ardio::assemble(device_prelude(d, 16000000));
        if (!r.ok)
            ::ardio_test::fail(__FILE__, __LINE__,
                               d.name + " prelude does not assemble: " + r.error);
    }
}

TEST(the_prelude_defines_every_name_the_contract_promises) {
    // Referring to an undefined name is an assembly error, so building a
    // source that mentions all of them proves each one exists.
    const char* names[] = {
        "AD_RAMEND", "AD_RAMSTART", "AD_F_CPU",  "AD_NUM_PINS", "AD_NUM_ANALOG",
        "AD_ANALOG_PIN_BASE",
        "AD_SREG",   "AD_SPL",      "AD_SPH",
        "AD_UCSRA",  "AD_UCSRB",    "AD_UCSRC",  "AD_UBRRL",    "AD_UBRRH", "AD_UDR",
        "AD_ADMUX",  "AD_ADCSRA",   "AD_ADCSRB", "AD_ADCL",     "AD_ADCH",
        "AD_TCCR0A", "AD_TCCR0B",   "AD_TCNT0",  "AD_TIMSK0",   "AD_TIFR0",
        "AD_TWBR",   "AD_TWSR",     "AD_TWDR",   "AD_TWCR",     "AD_LED_BUILTIN",
    };
    for (const AvrDevice& d : ardio::avr::device_database()) {
        std::string src = device_prelude(d, 16000000);
        for (const char* n : names) src += std::string("        ldi r16, lo8(") + n + ")\n";
        src += "        ldi r16, lo8(__ardio_pinmap)\n";
        src += "        ldi r16, lo8(__ardio_analogmap)\n";
        ardio::AssembleResult r = ardio::assemble(src);
        if (!r.ok)
            ::ardio_test::fail(__FILE__, __LINE__, d.name + ": " + r.error);
    }
}

TEST(the_prelude_constants_carry_the_devices_values) {
    const AvrDevice& d = device("atmega328p");
    std::string prelude = device_prelude(d, 8000000);
    long v = 0;
    CHECK(symbol_value(prelude, "AD_RAMEND", v));
    CHECK_EQ(v, 0x08FFL);
    CHECK(symbol_value(prelude, "AD_RAMSTART", v));
    CHECK_EQ(v, 0x0100L);
    CHECK(symbol_value(prelude, "AD_NUM_PINS", v));
    CHECK_EQ(v, 20L);
    CHECK(symbol_value(prelude, "AD_NUM_ANALOG", v));
    CHECK_EQ(v, 8L);
    // The constant adc.S needs in place of the hardcoded 14 it used to carry.
    CHECK(symbol_value(prelude, "AD_ANALOG_PIN_BASE", v));
    CHECK_EQ(v, 14L);
    CHECK(symbol_value(prelude, "AD_UDR", v));
    CHECK_EQ(v, 0xC6L);
    CHECK(symbol_value(prelude, "AD_LED_BUILTIN", v));
    CHECK_EQ(v, 13L);

    // SREG and the stack pointer are named by their I/O addresses, not their
    // data-space ones, because in/out is how the runtime reaches them and
    // in/out cannot address anything above 0x3F.
    CHECK(symbol_value(prelude, "AD_SREG", v));
    CHECK_EQ(v, 0x3FL);
    CHECK(symbol_value(prelude, "AD_SPL", v));
    CHECK_EQ(v, 0x3DL);
    CHECK(symbol_value(prelude, "AD_SPH", v));
    CHECK_EQ(v, 0x3EL);
    // Which means they must survive being used as an out operand.
    ardio::AssembleResult r = assemble_or_report(prelude + "        out AD_SREG, r1\n"
                                                          "        out AD_SPL, r28\n"
                                                          "        out AD_SPH, r28\n");
    CHECK(r.ok);
    // lo8/hi8 only recover 16 bits, so the clock is checked through the two
    // bytes that fit: 8000000 is 0x7A1200, whose low word is 0x1200.
    CHECK(symbol_value(prelude, "AD_F_CPU", v));
    CHECK_EQ(v, 0x1200L);

    // f_cpu comes from the board, not the part, so the same silicon at a
    // different clock must produce a different constant.
    std::string faster = device_prelude(d, 16000000);
    long other = 0;
    CHECK(symbol_value(faster, "AD_F_CPU", other));
    CHECK(other != v);
}

TEST(every_preludes_analog_pin_base_matches_its_device) {
    // The prelude is the only form of the table adc.S ever sees, so the
    // constant it hands over has to be the one the device row carries.
    for (const AvrDevice& d : ardio::avr::device_database()) {
        std::string prelude = device_prelude(d, 16000000);
        long v = -1;
        if (!symbol_value(prelude, "AD_ANALOG_PIN_BASE", v)) {
            ::ardio_test::fail(__FILE__, __LINE__,
                               d.name + ": AD_ANALOG_PIN_BASE is not usable");
            continue;
        }
        CHECK_EQ(v, long(d.analog_pin_base));
    }
}

TEST(the_generated_pinmap_decodes_back_to_the_same_mappings) {
    const AvrDevice& d = device("atmega328p");
    std::string prelude = device_prelude(d, 16000000);
    std::vector<uint8_t> image;
    size_t at = 0;
    CHECK(table_bytes(prelude, "__ardio_pinmap", image, at));
    CHECK(at + 2 * d.pins.size() <= image.size());
    if (at + 2 * d.pins.size() > image.size()) return;

    for (size_t p = 0; p < d.pins.size(); ++p) {
        CHECK_EQ(int(image[at + 2 * p]), int(d.pins[p].pin_reg));
        CHECK_EQ(int(image[at + 2 * p + 1]), int(d.pins[p].bit));
    }
    // And the same numbers the Nano's silkscreen implies, read out of flash.
    CHECK_EQ(int(image[at + 2 * 0]), 0x29);       // pin 0 -> PIND
    CHECK_EQ(int(image[at + 2 * 8]), 0x23);       // pin 8 -> PINB
    CHECK_EQ(int(image[at + 2 * 14]), 0x26);      // pin 14 -> PINC
    CHECK_EQ(int(image[at + 2 * 13 + 1]), 5);     // pin 13 is bit 5
}

TEST(the_generated_pinmap_stores_absent_pins_as_two_zero_bytes) {
    const AvrDevice& d = device("atmega2560");
    std::string prelude = device_prelude(d, 16000000);
    std::vector<uint8_t> image;
    size_t at = 0;
    CHECK(table_bytes(prelude, "__ardio_pinmap", image, at));
    if (at + 2 * d.pins.size() > image.size()) return;
    // Pin 6 is on PORTH, which the byte-wide address cannot hold.
    CHECK_EQ(int(image[at + 2 * 6]), 0);
    CHECK_EQ(int(image[at + 2 * 6 + 1]), 0);
    // Pin 5 is PE3 and must be unaffected by its neighbour being absent.
    CHECK_EQ(int(image[at + 2 * 5]), 0x2C);
    CHECK_EQ(int(image[at + 2 * 5 + 1]), 3);
}

TEST(the_generated_analogmap_decodes_back_to_the_same_channels) {
    const AvrDevice& d = device("atmega32u4");
    std::string prelude = device_prelude(d, 16000000);
    std::vector<uint8_t> image;
    size_t at = 0;
    CHECK(table_bytes(prelude, "__ardio_analogmap", image, at));
    CHECK(at + d.analog.size() <= image.size());
    if (at + d.analog.size() > image.size()) return;

    for (size_t a = 0; a < d.analog.size(); ++a)
        CHECK_EQ(int(image[at + a]), int(d.analog[a].channel));
    CHECK_EQ(int(image[at + 0]), 7);    // A0 is ADC7
    CHECK_EQ(int(image[at + 6]), 8);    // A6 is ADC8, above the MUX5 line
    CHECK_EQ(int(image[at + 11]), 9);   // A11 is ADC9
}

TEST(the_prelude_leaves_the_reset_word_executable) {
    // The prelude carries data tables, and it is prepended to the runtime, so
    // word address 0 has to remain something the part can execute rather than
    // the first byte of the pin map. The generated jump over the tables is
    // what keeps that true.
    const AvrDevice& d = device("atmega328p");
    std::string prelude = device_prelude(d, 16000000);
    ardio::AssembleResult r = assemble_or_report(prelude);
    if (!r.ok) return;
    CHECK(r.code.size() >= 2);
    unsigned first = unsigned(r.code[0]) | (unsigned(r.code[1]) << 8);
    CHECK_EQ(int(first & 0xF000u), 0xC000);   // rjmp

    // It must land after both tables, at the end of the prelude.
    long end = 0;
    CHECK(symbol_value(prelude, "__ardio_prelude_end", end));
    long k = long(first & 0x0FFF);
    CHECK_EQ(k + 1, end);
}
