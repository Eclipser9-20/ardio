// Tests for the virtual board: the piece that joins a core, the peripherals and
// the parts on the pins into something that runs a sketch.
//
// Every test here runs a real program. Some are assembled from AVR assembly by
// ardio's own assembler, so that the test can put the machine in a state a
// compiler would take a long way round to reach; the rest are compiled from a
// real .ino by ardio's own compiler, runtime and assembler, because a board
// that only runs hand-written test programs has not been shown to run a sketch.
//
// Nothing below asserts on a rendering of what happened. The observations are
// the ones a user of this machine has: a part hearing a pin change, a byte
// coming out of the USART, a value in the guest's own SRAM. A board that logged
// the right thing and drove the wrong pin would still be broken.
//
// Cycle counts are worked out by hand from the instruction set and the
// datasheet -- 256 timer ticks at a prescale of 1 is 256 clock cycles, one
// millisecond at 16 MHz is 16000 -- and written in as constants. Where a test
// cannot name an exact cycle without also restating the core's whole cycle
// table, it brackets the answer with the two numbers it can derive: the cycle
// the hardware event is due, and the cycle by which the handler must have
// finished.

#include "harness.h"
#include "runtime_source.h"

#include "ardio/avr/assembler.h"
#include "ardio/avr/device.h"
#include "ardio/board.h"
#include "ardio/build.h"
#include "ardio/emu/board.h"
#include "ardio/emu/machine.h"
#include "ardio/hex.h"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace ardio;
using namespace ardio::emu;

namespace {

// ------------------------------------------------------------- boards ----

const Board& uno() {
    static const Board* b = find_board_by_id("uno");
    static Board fallback;
    return b ? *b : fallback;
}

// 8 MHz on the same silicon, which is what makes it the right board to check
// that simulated time comes from the board's clock and not from a constant.
const Board& pro_mini() {
    static const Board* b = find_board_by_id("pro_mini");
    static Board fallback;
    return b ? *b : fallback;
}

// ---------------------------------------------------- ATmega328P registers --
//
// Written out rather than derived, so that a test says which register it means.
// These are data-space addresses.
constexpr uint16_t kPinB = 0x23, kDdrB = 0x24, kPortB = 0x25;
constexpr uint16_t kPinD = 0x29, kDdrD = 0x2A, kPortD = 0x2B;
constexpr uint16_t kTccr0b = 0x45;
constexpr uint16_t kTimsk0 = 0x6E;
constexpr uint16_t kAdcl = 0x78, kAdch = 0x79, kAdcsra = 0x7A, kAdmux = 0x7C;
constexpr uint16_t kRamStart = 0x0100;

// -------------------------------------------------------------- parts ----

// Remembers every pin change it is told about, with the cycle it happened on.
class RecordingPart final : public Part {
public:
    struct Change {
        int pin = 0;
        PinState state = PinState::Floating;
        uint64_t cycles = 0;
    };

    std::string kind() const override { return "recorder"; }
    void pin_changed(int pin, PinState state, uint64_t cycles) override {
        changes.push_back(Change{pin, state, cycles});
    }
    std::string describe() const override { return "recorder"; }

    size_t count(PinState state) const {
        size_t n = 0;
        for (const Change& c : changes) if (c.state == state) ++n;
        return n;
    }

    std::vector<Change> changes;
};

// Holds one pin at a fixed level, as a switch wired to a rail would.
class DrivingPart final : public Part {
public:
    DrivingPart(int pin, PinState level) : pin_(pin), level_(level) {}

    std::string kind() const override { return "driver"; }
    void pin_changed(int, PinState, uint64_t) override {}
    PinState drive(int pin) const override {
        return pin == pin_ ? level_ : PinState::Floating;
    }
    std::string describe() const override { return "driver"; }

private:
    int pin_ = 0;
    PinState level_ = PinState::Floating;
};

// ---------------------------------------------------------- programs -----

// Assembles a test program against the real ATmega328P description.
//
// The device prelude is appended exactly as the build does it, so a program
// here may use the AD_* names, and so the two flash tables land after the code
// rather than on top of the reset vector.
std::vector<uint8_t> assemble_program(const std::string& source) {
    AssembleResult result = assemble(source + "\n" + ardio::test::default_prelude());
    if (!result.ok) std::printf("    (assembler said: %s)\n", result.error.c_str());
    return result.code;
}

// A machine for `board` with `source` assembled into it, ready to run.
std::unique_ptr<Machine> machine_running(const Board& board, const std::string& source) {
    std::string error;
    std::unique_ptr<Machine> m = Machine::create(board, error);
    if (!m) {
        ::ardio_test::fail(__FILE__, __LINE__, "Machine::create failed: " + error);
        return nullptr;
    }
    if (!m->load(assemble_program(source), error)) {
        ::ardio_test::fail(__FILE__, __LINE__, "load failed: " + error);
        return nullptr;
    }
    return m;
}

// A byte of the guest's own SRAM, which is how the assembly programs below
// report what they saw.
uint8_t guest_byte(const Machine& m, uint16_t addr) {
    const State& s = m.state();
    if (!s.sram) return 0;
    return s.sram[addr - kRamStart];
}

// ------------------------------------------------------- real sketches ---

// Compiles a sketch through the whole of ardio -- sketch preprocessing, the C++
// front end, the assembly runtime and the assembler -- and returns the flash
// image. Empty means the build did not happen, and the caller skips: the
// runtime lives on disk and the test binary may be run from somewhere that
// cannot see it.
std::vector<uint8_t> build_flash(const std::string& ino_source, const std::string& stem) {
    std::string runtime_probe;
    if (!ardio::test::read_relative("runtime/core.S", runtime_probe)) return {};

    std::string dir = ARDIO_TEST_TMP;
    std::string path = dir + "/" + stem + ".ino";
    {
        std::ofstream out(path);
        if (!out) return {};
        out << ino_source;
    }

    BuildResult built = build_sketch(path, uno(), {}, dir);
    if (!built.ok) {
        std::printf("    (build said: %s)\n", built.error.c_str());
        return {};
    }

    std::ifstream in(built.hex_path);
    if (!in) return {};
    std::string hex((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::string error;
    std::optional<HexImage> image = parse_intel_hex(hex, error);
    if (!image) {
        std::printf("    (hex said: %s)\n", error.c_str());
        return {};
    }
    return image->data;
}

} // namespace

// ============================================================ creation =====

TEST(board_machine_refuses_a_board_with_no_device_description) {
    // The ESP8266 is a real board in ardio's database and is not an AVR at all,
    // so there is nothing for this emulator to be. Emulating something else
    // would produce a run that looked plausible from beginning to end.
    const Board* esp = find_board_by_id("esp8266");
    CHECK(esp != nullptr);
    if (!esp) return;

    std::string error;
    std::unique_ptr<Machine> m = Machine::create(*esp, error);
    CHECK(m == nullptr);
    CHECK(error.find("esp8266") != std::string::npos);
    CHECK(!error.empty());
}

TEST(board_machine_refuses_a_part_whose_flash_it_cannot_address) {
    // The 2560 has 256 KB of flash and State::pc is 16 bits. Running it anyway
    // would fetch from the wrong half of the image.
    const Board* mega = find_board_by_id("mega2560");
    CHECK(mega != nullptr);
    if (!mega) return;

    std::string error;
    std::unique_ptr<Machine> m = Machine::create(*mega, error);
    CHECK(m == nullptr);
    CHECK(error.find("atmega2560") != std::string::npos);
    CHECK(error.find("16 bits") != std::string::npos);
}

TEST(board_machine_creates_for_a_board_it_knows) {
    std::string error;
    std::unique_ptr<Machine> m = Machine::create(uno(), error);
    CHECK(m != nullptr);
    CHECK(error.empty());
    if (!m) return;
    CHECK_EQ(m->cycles(), uint64_t(0));
}

TEST(board_machine_rejects_an_image_larger_than_flash) {
    std::string error;
    std::unique_ptr<Machine> m = Machine::create(uno(), error);
    CHECK(m != nullptr);
    if (!m) return;

    // One byte past the 328P's 32 KB.
    std::vector<uint8_t> too_big(32769, 0x00);
    CHECK(!m->load(too_big, error));
    CHECK(error.find("32768") != std::string::npos);
    CHECK(error.find("32769") != std::string::npos);

    // And the boundary itself fits, so the check is a size limit rather than a
    // margin somebody guessed at.
    std::vector<uint8_t> exact(32768, 0x00);
    std::string ok_error = "not cleared";
    CHECK(m->load(exact, ok_error));
    CHECK(ok_error.empty());
}

TEST(board_machine_description_names_the_board_and_the_core_it_got) {
    std::string error;
    std::unique_ptr<Machine> m = Machine::create(uno(), error);
    CHECK(m != nullptr);
    if (!m) return;

    std::string text = m->description();
    CHECK(text.find(uno().name) != std::string::npos);
    CHECK(text.find("16 MHz") != std::string::npos);
    // The core describes itself, so this is the machine reporting what is
    // actually executing rather than what it hopes is.
    CHECK(text.find(make_core(*avr::find_device("atmega328p"))->description()) !=
          std::string::npos);
}

TEST(core_selection_returns_a_usable_core_either_way) {
    const avr::AvrDevice* device = avr::find_device("atmega328p");
    CHECK(device != nullptr);
    if (!device) return;

    std::unique_ptr<Core> best = make_core(*device);
    std::unique_ptr<Core> reference = make_reference_core(*device);
    CHECK(best != nullptr);
    CHECK(reference != nullptr);
    CHECK(!best->description().empty());
    // The fallback must say what it is, because the speed difference between
    // the two cores is the thing a user needs to be told about.
    CHECK(reference->description().find("reference") != std::string::npos);
}

// ================================================================ pins =====

TEST(a_pin_the_sketch_toggles_is_seen_by_the_part_wired_to_it) {
    // PB5 is digital pin 13. The program makes it an output and drives it
    // high, low, high.
    std::unique_ptr<Machine> m = machine_running(uno(), R"(
        ldi     r16, 0x20
        sts     0x24, r16       ; DDRB: PB5 an output, so it is driven low
        ldi     r17, 0x20
        sts     0x25, r17       ; PORTB: high
        ldi     r17, 0x00
        sts     0x25, r17       ; low
        ldi     r17, 0x20
        sts     0x25, r17       ; high
halt:   rjmp    halt
    )");
    if (!m) return;

    RecordingPart led;
    m->wire(13, &led);

    // Wiring reports the level the pin is already at, which is nothing: the
    // port is an input out of reset with its pull-up off.
    CHECK_EQ(led.changes.size(), size_t(1));
    CHECK(led.changes[0].state == PinState::Floating);

    StepResult result = m->run_for(1000);
    CHECK(result.outcome == RunOutcome::Halted);

    // Floating, then low (an output driving the zero already in PORTB), then
    // the three levels the program writes.
    CHECK_EQ(led.changes.size(), size_t(5));
    CHECK_EQ(led.count(PinState::High), size_t(2));
    CHECK_EQ(led.count(PinState::Low), size_t(2));
    if (led.changes.size() == 5) {
        CHECK(led.changes[1].state == PinState::Low);
        CHECK(led.changes[2].state == PinState::High);
        CHECK(led.changes[3].state == PinState::Low);
        CHECK(led.changes[4].state == PinState::High);
    }

    // Each change is dated at the cycle the store that caused it ran, so the
    // cycles strictly increase and none of the later ones is the reset cycle.
    for (size_t i = 1; i < led.changes.size(); ++i)
        CHECK(led.changes[i].cycles > led.changes[i - 1].cycles);
}

TEST(a_part_driving_a_pin_is_what_the_guest_reads_back) {
    // PD2 is digital pin 2. The program leaves port D an input with its
    // pull-ups off and stores what it reads, so the only thing that can decide
    // the level is the part.
    const char* program = R"(
        ldi     r16, 0x00
        sts     0x2A, r16       ; DDRD: all inputs
        sts     0x2B, r16       ; PORTD: no pull-ups
        lds     r17, 0x29       ; PIND
        sts     0x0100, r17
halt:   rjmp    halt
    )";

    std::unique_ptr<Machine> high = machine_running(uno(), program);
    if (!high) return;
    DrivingPart pressed(2, PinState::High);
    high->wire(2, &pressed);
    CHECK(high->run_for(1000).outcome == RunOutcome::Halted);
    CHECK_EQ(int(guest_byte(*high, kRamStart)), 0x04);

    std::unique_ptr<Machine> low = machine_running(uno(), program);
    if (!low) return;
    DrivingPart grounded(2, PinState::Low);
    low->wire(2, &grounded);
    CHECK(low->run_for(1000).outcome == RunOutcome::Halted);
    CHECK_EQ(int(guest_byte(*low, kRamStart)), 0x00);

    // And with nothing wired at all the pin reads low, which is what pins the
    // parts above changed proves they changed.
    std::unique_ptr<Machine> bare = machine_running(uno(), program);
    if (!bare) return;
    CHECK(bare->run_for(1000).outcome == RunOutcome::Halted);
    CHECK_EQ(int(guest_byte(*bare, kRamStart)), 0x00);
}

TEST(a_part_beats_the_internal_pullup_the_sketch_turned_on) {
    // The classic button: PORTD's bit is the pull-up switch, the part is a
    // switch to ground. A model that treated PORTx as the pin level would read
    // this back as high.
    const char* program = R"(
        ldi     r16, 0x00
        sts     0x2A, r16       ; DDRD: inputs
        ldi     r16, 0x04
        sts     0x2B, r16       ; PORTD: pull-up on PD2
        lds     r17, 0x29
        sts     0x0100, r17
halt:   rjmp    halt
    )";

    std::unique_ptr<Machine> released = machine_running(uno(), program);
    if (!released) return;
    CHECK(released->run_for(1000).outcome == RunOutcome::Halted);
    CHECK_EQ(int(guest_byte(*released, kRamStart)), 0x04);   // pulled up

    std::unique_ptr<Machine> held = machine_running(uno(), program);
    if (!held) return;
    DrivingPart button(2, PinState::Low);
    held->wire(2, &button);
    CHECK(held->run_for(1000).outcome == RunOutcome::Halted);
    CHECK_EQ(int(guest_byte(*held, kRamStart)), 0x00);
}

TEST(wiring_a_part_to_a_pin_the_board_lacks_is_inert) {
    // Machine::wire has no way to report an error, so the alternative to
    // ignoring this is crashing on it. The 328P's digital numbering stops at
    // 19.
    std::unique_ptr<Machine> m = machine_running(uno(), R"(
halt:   rjmp    halt
    )");
    if (!m) return;

    RecordingPart nowhere;
    m->wire(99, &nowhere);
    CHECK_EQ(nowhere.changes.size(), size_t(0));
    CHECK(m->run_for(100).outcome == RunOutcome::Halted);
    CHECK_EQ(nowhere.changes.size(), size_t(0));
}

// ============================================================== serial =====

TEST(serial_output_from_a_sketch_comes_out_of_take_serial_output) {
    std::vector<uint8_t> flash = build_flash(R"(#include <HardwareSerial.h>

void setup() {
    Serial.begin(9600);
    Serial.print("hi");
}

void loop() {
}
)", "emu_serial_out");
    if (flash.empty()) {
        std::printf("  skip serial sketch could not be built here\n");
        return;
    }

    std::string error;
    std::unique_ptr<Machine> m = Machine::create(uno(), error);
    CHECK(m != nullptr);
    if (!m) return;
    CHECK(m->load(flash, error));

    // A frame at 9600 baud on a 16 MHz part is 104 * 16 * 10 = 16640 cycles,
    // so two characters need about 2.1 ms of sketch time. Ten is comfortable
    // and still proves the bytes are not arriving instantly: nothing is
    // available before the first frame can have finished.
    StepResult early = m->run_for(16000);
    CHECK(early.outcome == RunOutcome::ReachedTime);
    CHECK(m->take_serial_output().empty());

    m->run_ms(10);
    CHECK_EQ(m->take_serial_output(), std::string("hi"));
    // Taking the output consumes it.
    CHECK(m->take_serial_output().empty());
}

TEST(serial_fed_from_outside_is_readable_by_the_guest) {
    std::vector<uint8_t> flash = build_flash(R"(#include <HardwareSerial.h>

void setup() {
    Serial.begin(9600);
}

void loop() {
    if (Serial.available()) {
        int c = Serial.read();
        Serial.write(c + 1);
    }
}
)", "emu_serial_in");
    if (flash.empty()) {
        std::printf("  skip serial sketch could not be built here\n");
        return;
    }

    std::string error;
    std::unique_ptr<Machine> m = Machine::create(uno(), error);
    CHECK(m != nullptr);
    if (!m) return;
    CHECK(m->load(flash, error));

    m->run_ms(2);                 // let setup() configure the USART
    m->take_serial_output();

    m->feed_serial("A");
    m->run_ms(10);
    // The sketch echoes the next character along, so this is the guest having
    // read the byte rather than the byte being handed back.
    CHECK_EQ(m->take_serial_output(), std::string("B"));

    m->feed_serial("m");
    m->run_ms(10);
    CHECK_EQ(m->take_serial_output(), std::string("n"));
}

// ================================================================= ADC =====

TEST(an_analog_voltage_set_from_outside_is_what_analog_read_returns) {
    // The whole analogRead sequence by hand: AVcc as the reference, channel 0,
    // the ADC on with a /128 prescaler, one conversion, then poll ADSC down and
    // store the ten bits.
    const char* program = R"(
        ldi     r16, 0x40
        sts     0x7C, r16       ; ADMUX: AVcc reference, channel 0
        ldi     r16, 0x87
        sts     0x7A, r16       ; ADCSRA: ADEN, prescaler /128
        ldi     r16, 0xC7
        sts     0x7A, r16       ; start a conversion
wait:   lds     r17, 0x7A
        sbrc    r17, 6          ; ADSC stays set while it converts
        rjmp    wait
        lds     r18, 0x78       ; ADCL
        lds     r19, 0x79       ; ADCH
        sts     0x0100, r18
        sts     0x0101, r19
halt:   rjmp    halt
    )";

    // Half the 5 V reference is half of full scale: 2.5 / 5 * 1024 = 512.
    std::unique_ptr<Machine> half = machine_running(uno(), program);
    if (!half) return;
    half->set_analog_volts(0, 2.5);
    CHECK(half->run_for(100000).outcome == RunOutcome::Halted);
    CHECK_EQ(int(guest_byte(*half, kRamStart)), 0x00);
    CHECK_EQ(int(guest_byte(*half, kRamStart + 1)), 0x02);

    // A0 answers to its digital number as well, and it must mean the same
    // input -- getting that wrong reads a plausible value off the wrong pin.
    std::unique_ptr<Machine> by_pin = machine_running(uno(), program);
    if (!by_pin) return;
    by_pin->set_analog_volts(14, 1.25);          // a quarter of full scale: 256
    CHECK(by_pin->run_for(100000).outcome == RunOutcome::Halted);
    CHECK_EQ(int(guest_byte(*by_pin, kRamStart)), 0x00);
    CHECK_EQ(int(guest_byte(*by_pin, kRamStart + 1)), 0x01);

    // Above the reference the converter pins at full scale rather than
    // wrapping, and an untouched channel reads zero.
    std::unique_ptr<Machine> over = machine_running(uno(), program);
    if (!over) return;
    over->set_analog_volts(0, 7.0);
    CHECK(over->run_for(100000).outcome == RunOutcome::Halted);
    CHECK_EQ(int(guest_byte(*over, kRamStart)), 0xFF);
    CHECK_EQ(int(guest_byte(*over, kRamStart + 1)), 0x03);

    std::unique_ptr<Machine> other = machine_running(uno(), program);
    if (!other) return;
    other->set_analog_volts(3, 5.0);             // a different channel entirely
    CHECK(other->run_for(100000).outcome == RunOutcome::Halted);
    CHECK_EQ(int(guest_byte(*other, kRamStart)), 0x00);
    CHECK_EQ(int(guest_byte(*other, kRamStart + 1)), 0x00);
}

TEST(the_adc_conversion_takes_the_time_the_datasheet_says) {
    // The first conversion after the ADC is enabled is 25 ADC clocks, and at a
    // /128 prescaler that is 3200 CPU cycles. The polling loop cannot fall
    // through before then, so the program cannot have halted inside a span
    // shorter than that -- which is the whole reason to emulate the delay.
    const char* program = R"(
        ldi     r16, 0x40
        sts     0x7C, r16
        ldi     r16, 0x87
        sts     0x7A, r16
        ldi     r16, 0xC7
        sts     0x7A, r16
wait:   lds     r17, 0x7A
        sbrc    r17, 6
        rjmp    wait
halt:   rjmp    halt
    )";

    std::unique_ptr<Machine> m = machine_running(uno(), program);
    if (!m) return;
    m->set_analog_volts(0, 5.0);

    StepResult early = m->run_for(2000);
    CHECK(early.outcome == RunOutcome::ReachedTime);   // still spinning on ADSC

    StepResult done = m->run_for(4000);
    CHECK(done.outcome == RunOutcome::Halted);
    CHECK(done.cycles >= 3200);
}

// ============================================================ timer 0 =====

TEST(a_timer_overflow_interrupt_fires_at_the_cycle_the_timer_overflows) {
    // Timer 0 with no prescaling overflows 256 counts after it starts, so the
    // flag is set 256 clock cycles after TCCR0B is written. The handler is
    // reached only if the run loop set its deadline from the timer's own next
    // event and then dispatched the vector, so this is the whole path.
    //
    // TIMER0_OVF is vector 16 on this part, and the 328P's vector slots are two
    // words each, so the entry point is word address 32.
    std::unique_ptr<Machine> m = machine_running(uno(), R"(
        .org    0
        jmp     start

        .org    32
        jmp     isr

start:  ldi     r16, 0x01
        sts     0x6E, r16       ; TIMSK0: TOIE0
        ldi     r16, 0x01
        sts     0x45, r16       ; TCCR0B: clk/1, the counter starts here
        sei
spin:   rjmp    spin

isr:    cli
        ldi     r17, 0x5A
        sts     0x0100, r17
stop:   rjmp    stop
    )");
    if (!m) return;

    // TCCR0B is written 7 cycles in -- jmp 3, ldi 1, sts 2, ldi 1 -- so the
    // overflow is due at cycle 263. Well before that, nothing has happened.
    StepResult early = m->run_for(200);
    CHECK(early.outcome == RunOutcome::ReachedTime);
    CHECK_EQ(int(guest_byte(*m, kRamStart)), 0x00);

    StepResult later = m->run_for(2000);
    CHECK(later.outcome == RunOutcome::Halted);
    CHECK_EQ(int(guest_byte(*m, kRamStart)), 0x5A);

    // The handler cannot have finished before the overflow was due, and it
    // costs at most the vector entry (4), the jmp in the table (3), cli (1),
    // ldi (1), sts (2) and the self-loop (2) -- thirteen cycles, plus the one
    // spin instruction in flight when the deadline arrived.
    CHECK(m->cycles() >= 263);
    CHECK(m->cycles() <= 263 + 16);
}

TEST(a_sketch_polling_the_overflow_flag_sees_it_without_any_interrupt) {
    // TIFR0 is polled at least as often as it is used as an interrupt source,
    // and a poll must see the flag on the cycle the hardware would set it. The
    // peripheral is only current because every access advances it first.
    std::unique_ptr<Machine> m = machine_running(uno(), R"(
        ldi     r16, 0x01
        sts     0x45, r16       ; TCCR0B: clk/1
wait:   lds     r17, 0x35       ; TIFR0
        sbrs    r17, 0          ; TOV0
        rjmp    wait
        ldi     r18, 0x01
        sts     0x0100, r18
halt:   rjmp    halt
    )");
    if (!m) return;

    StepResult early = m->run_for(200);
    CHECK(early.outcome == RunOutcome::ReachedTime);
    CHECK_EQ(int(guest_byte(*m, kRamStart)), 0x00);

    StepResult done = m->run_for(2000);
    CHECK(done.outcome == RunOutcome::Halted);
    CHECK_EQ(int(guest_byte(*m, kRamStart)), 0x01);
    CHECK(done.cycles >= 256);
}

// ======================================================= time and halt =====

TEST(run_ms_spends_the_boards_own_clock_rate) {
    // A loop that never ends and never traps, so the only thing that stops it
    // is the span running out.
    const char* spin = R"(
loop:   nop
        rjmp    loop
    )";

    std::unique_ptr<Machine> fast = machine_running(uno(), spin);
    if (!fast) return;
    StepResult a = fast->run_ms(1);
    CHECK(a.outcome == RunOutcome::ReachedTime);
    // 16 MHz is 16000 cycles per millisecond. The core can only stop between
    // instructions, and the longest here is two cycles.
    CHECK(a.cycles >= 16000);
    CHECK(a.cycles <= 16002);

    // Ten milliseconds is ten times as many, and elapsed cycles accumulate
    // across calls rather than restarting.
    StepResult b = fast->run_ms(10);
    CHECK(b.cycles >= 176000);
    CHECK(b.cycles <= 176002);

    // The same silicon on an 8 MHz board covers the same millisecond in half
    // the cycles, which is the whole point of the unit.
    std::unique_ptr<Machine> slow = machine_running(pro_mini(), spin);
    if (!slow) return;
    CHECK_EQ(pro_mini().f_cpu, 8000000);
    StepResult c = slow->run_ms(1);
    CHECK(c.outcome == RunOutcome::ReachedTime);
    CHECK(c.cycles >= 8000);
    CHECK(c.cycles <= 8002);
}

TEST(a_halted_sketch_reports_halted_instead_of_spinning) {
    // A self-targeting jump with interrupts off can never do anything again.
    std::unique_ptr<Machine> m = machine_running(uno(), R"(
halt:   rjmp    halt
    )");
    if (!m) return;

    StepResult first = m->run_ms(1000);
    CHECK(first.outcome == RunOutcome::Halted);
    // A full simulated second is 16 million cycles. It stopped after one jump,
    // so it did not spend them.
    CHECK(first.cycles < 100);

    // And it stays stopped without charging for the loop it is stuck in, which
    // is what makes a second call cheap rather than a second wait.
    uint64_t settled = m->cycles();
    StepResult again = m->run_ms(1000);
    CHECK(again.outcome == RunOutcome::Halted);
    CHECK_EQ(again.cycles, settled);
}

TEST(an_illegal_opcode_is_reported_as_a_fault_with_a_message) {
    std::string error;
    std::unique_ptr<Machine> m = Machine::create(uno(), error);
    CHECK(m != nullptr);
    if (!m) return;

    // Erased flash decodes to something, so the image is an opcode that is
    // genuinely not an instruction: 0x9401 is in the reserved corner of the
    // single-register group.
    std::vector<uint8_t> image = {0x01, 0x94};
    CHECK(m->load(image, error));

    StepResult result = m->run_for(1000);
    CHECK(result.outcome == RunOutcome::Fault);
    CHECK(!result.error.empty());
}

TEST(reset_puts_the_clock_and_the_peripherals_back) {
    std::unique_ptr<Machine> m = machine_running(uno(), R"(
        ldi     r16, 0x20
        sts     0x24, r16
        ldi     r17, 0x20
        sts     0x25, r17
halt:   rjmp    halt
    )");
    if (!m) return;

    CHECK(m->run_for(1000).outcome == RunOutcome::Halted);
    CHECK(m->cycles() > 0);

    m->reset();
    CHECK_EQ(m->cycles(), uint64_t(0));

    // The program runs again from the top, which it could not do if the halt
    // had been latched or the port had kept its direction.
    RecordingPart led;
    m->wire(13, &led);
    CHECK_EQ(led.changes.size(), size_t(1));
    CHECK(led.changes[0].state == PinState::Floating);

    StepResult result = m->run_for(1000);
    CHECK(result.outcome == RunOutcome::Halted);
    CHECK_EQ(led.count(PinState::High), size_t(1));
}

// ======================================================== a real sketch ====

TEST(a_compiled_blink_sketch_blinks_the_pin_it_says_it_does) {
    std::string source;
    if (!ardio::test::read_relative("examples/blink.ino", source)) {
        std::printf("  skip examples/blink.ino not found relative to the cwd\n");
        return;
    }

    std::vector<uint8_t> flash = build_flash(source, "emu_blink");
    if (flash.empty()) {
        std::printf("  skip blink could not be built here\n");
        return;
    }

    std::string error;
    std::unique_ptr<Machine> m = Machine::create(uno(), error);
    CHECK(m != nullptr);
    if (!m) return;
    CHECK(m->load(flash, error));

    RecordingPart led;
    m->wire(13, &led);

    // The sketch is on for 500 ms and off for 500 ms, so 1200 ms of sketch time
    // contains at least one full cycle of it.
    StepResult result = m->run_ms(1200);
    CHECK(result.outcome == RunOutcome::ReachedTime);
    CHECK(led.count(PinState::High) >= 1);
    CHECK(led.count(PinState::Low) >= 2);

    // And the half-second is a real half-second in sketch time: consecutive
    // level changes are about 8 million cycles apart at 16 MHz. Allow a wide
    // band, since the delay loop is calibrated in whole milliseconds and the
    // sketch spends real instructions either side of it.
    size_t edges = 0;
    for (size_t i = 1; i < led.changes.size(); ++i) {
        if (led.changes[i - 1].state == PinState::Floating) continue;
        uint64_t gap = led.changes[i].cycles - led.changes[i - 1].cycles;
        CHECK(gap > 7000000);
        CHECK(gap < 9000000);
        ++edges;
    }
    CHECK(edges >= 1);
}
