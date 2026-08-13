// Tests for the portable reference execution core.
//
// The reference core is the emulator's oracle: a native translator for the same
// instruction set is checked against it, so anything this file lets through
// becomes wrong everywhere at once and stops being detectable by comparison.
// That shapes how these tests are written.
//
// Every test assembles a real program with ardio's own assembler, runs it on a
// real ATmega328P description, and asserts on the machine state afterwards.
// Nothing here inspects a disassembly, a log line or any other rendering of
// what happened -- a core that produced the right trace and the wrong register
// would still be broken, and a core that took an unexpected route to the right
// state is not.
//
// Expected values are worked out by hand from the instruction set summary and
// written into the test as constants, rather than being computed by a second
// implementation of the same rule. A test that recomputes the flag it is
// checking only proves the arithmetic was typed twice.
//
// Every program ends in a self-targeting rjmp with interrupts disabled, which
// the core reports as Halted. That is the terminator throughout: it means a
// test that runs off the end of its program stops with the wrong reason rather
// than wandering into erased flash.

#include "harness.h"

#include "ardio/avr/assembler.h"
#include "ardio/avr/device.h"
#include "ardio/emu/machine.h"

#include <memory>
#include <string>
#include <vector>

namespace ardio::emu {
// Peripherals reach a reference core through this. Core itself takes only a
// State, so there is no room on the interface for the wiring, and the tests
// that exercise peripheral routing and interrupt dispatch need it.
void reference_core_attach_peripheral(Core& core, Peripheral* peripheral);
} // namespace ardio::emu

using namespace ardio;
using namespace ardio::emu;

namespace {

// A whole virtual machine for one test: flash, SRAM, guest state and a core,
// kept together so the storage outlives the pointers State holds into it.
struct Machine {
    avr::AvrDevice device;
    std::vector<uint8_t> flash;
    std::vector<uint8_t> sram;
    State state;
    std::unique_ptr<Core> core;
    RunResult result;

    uint8_t reg(int i) const { return state.r[i]; }

    // Reads data space the way the guest would, for assertions about stores.
    uint8_t data(uint16_t addr) const {
        if (addr < device.ram_start || addr > device.ramend) return 0;
        return sram[addr - device.ram_start];
    }
};

const avr::AvrDevice& atmega328p() {
    static const avr::AvrDevice* d = avr::find_device("atmega328p");
    return *d;
}

// Assembles `src` and lays it into a fresh machine, without running it.
//
// The stack pointer starts at RAMEND, which is what the reset code in the
// runtime does and what every call in these programs assumes.
Machine build(const std::string& src) {
    Machine m;
    m.device = atmega328p();

    AssembleResult a = assemble(src);
    if (!a.ok) {
        std::printf("    (assembler said: %s)\n", a.error.c_str());
        // Leave the machine empty. Erased flash decodes as an illegal opcode,
        // so the test fails loudly rather than silently passing on nothing.
    }

    m.flash.assign(m.device.flash_size, 0xFF);
    for (size_t i = 0; i < a.code.size() && i < m.flash.size(); ++i)
        m.flash[i] = a.code[i];
    m.sram.assign(m.device.ram_size, 0);

    m.state.flash = m.flash.data();
    m.state.sram = m.sram.data();
    m.state.sp = m.device.ramend;
    m.state.deadline = 1000000;
    m.core = make_reference_core(m.device);
    return m;
}

// Builds, runs to completion and insists the program halted the way every
// program in this file is written to halt. A test whose program faulted would
// otherwise go on to assert about registers that were never reached.
Machine run(const std::string& src) {
    Machine m = build(src);
    m.result = m.core->run(m.state);
    if (m.result.reason != StopReason::Halted)
        std::printf("    (stopped with reason %d: %s)\n", int(m.result.reason),
                    m.result.error.c_str());
    return m;
}

// Asserts the whole visible flag set at once.
//
// Checking flags one at a time hides the mistakes that matter most here: an
// instruction that sets the flag under test correctly but clobbers a
// neighbouring one is exactly the bug that survives into the translator and
// then shows up as a sketch taking the wrong branch much later.
void check_flags(const Machine& m, int c, int z, int n, int v, int s, int h,
                 const char* what) {
    if (m.state.flag_c != c) ::ardio_test::fail(__FILE__, __LINE__, std::string(what) + ": C");
    if (m.state.flag_z != z) ::ardio_test::fail(__FILE__, __LINE__, std::string(what) + ": Z");
    if (m.state.flag_n != n) ::ardio_test::fail(__FILE__, __LINE__, std::string(what) + ": N");
    if (m.state.flag_v != v) ::ardio_test::fail(__FILE__, __LINE__, std::string(what) + ": V");
    if (m.state.flag_s != s) ::ardio_test::fail(__FILE__, __LINE__, std::string(what) + ": S");
    if (m.state.flag_h != h) ::ardio_test::fail(__FILE__, __LINE__, std::string(what) + ": H");
}

// A peripheral that is nothing but four writable bytes.
//
// It stands in for a port: the point is not what it does but that data-space
// accesses in its range reach it instead of SRAM, and that read-modify-write
// instructions like sbi go through it in both directions.
class Latch : public Peripheral {
public:
    explicit Latch(uint16_t base) : base_(base) {}

    bool claims(uint16_t addr) const override { return addr >= base_ && addr < base_ + 4; }
    uint8_t read(uint16_t addr) override { ++reads; return regs[addr - base_]; }
    void write(uint16_t addr, uint8_t value) override { ++writes; regs[addr - base_] = value; }
    void advance(uint64_t) override {}

    uint8_t regs[4] = {0, 0, 0, 0};
    int reads = 0;
    int writes = 0;

private:
    uint16_t base_;
};

// A peripheral that claims no memory at all and exists only to raise one
// interrupt, which is the smallest thing that exercises vectoring.
class OneShotInterrupt : public Peripheral {
public:
    explicit OneShotInterrupt(uint8_t vector) : vector_(vector) {}

    bool claims(uint16_t) const override { return false; }
    uint8_t read(uint16_t) override { return 0; }
    void write(uint16_t, uint8_t) override {}
    void advance(uint64_t) override {}

    uint8_t pending_interrupt() const override { return vector_; }
    void acknowledge_interrupt() override { vector_ = 0; ++acknowledged; }

    int acknowledged = 0;

private:
    uint8_t vector_;
};

} // namespace

// ============================================================= arithmetic ===

TEST(emu_add_sets_half_carry_across_the_nibble_boundary) {
    // 0x0F + 0x01 = 0x10. The carry out of bit 3 is what H reports, and it is
    // the flag the runtime's BCD-ish and nibble-splitting code depends on.
    Machine m = run("ldi r16, 0x0F\n"
                    "ldi r17, 0x01\n"
                    "add r16, r17\n"
                    "done: rjmp done\n");
    CHECK_EQ(int(m.reg(16)), 0x10);
    check_flags(m, 0, 0, 0, 0, 0, 1, "add 0x0F + 0x01");
}

TEST(emu_add_sets_signed_overflow_when_two_positives_go_negative) {
    // 0x50 + 0x50 = 0xA0. Both operands are positive as signed bytes and the
    // result is negative, which is precisely the V condition. N is set and V is
    // set, so S -- the sign the signed branches actually use -- is clear, and
    // brlt correctly does not treat 0xA0 as less than zero here.
    Machine m = run("ldi r16, 0x50\n"
                    "ldi r17, 0x50\n"
                    "add r16, r17\n"
                    "done: rjmp done\n");
    CHECK_EQ(int(m.reg(16)), 0xA0);
    check_flags(m, 0, 0, 1, 1, 0, 0, "add 0x50 + 0x50");
}

TEST(emu_add_wrapping_to_zero_sets_carry_zero_and_half_carry) {
    // 0xFF + 0x01 = 0x00 with a carry out of both bit 3 and bit 7.
    Machine m = run("ldi r16, 0xFF\n"
                    "ldi r17, 0x01\n"
                    "add r16, r17\n"
                    "done: rjmp done\n");
    CHECK_EQ(int(m.reg(16)), 0x00);
    check_flags(m, 1, 1, 0, 0, 0, 1, "add 0xFF + 0x01");
}

TEST(emu_sub_half_carry_is_a_borrow_not_a_carry) {
    // 0x10 - 0x01 = 0x0F. The low nibble borrows, so H is SET. This is the
    // polarity that gets written backwards when add and sub share a helper.
    Machine m = run("ldi r16, 0x10\n"
                    "ldi r17, 0x01\n"
                    "sub r16, r17\n"
                    "done: rjmp done\n");
    CHECK_EQ(int(m.reg(16)), 0x0F);
    check_flags(m, 0, 0, 0, 0, 0, 1, "sub 0x10 - 0x01");
}

TEST(emu_sub_sets_signed_overflow_at_the_negative_boundary) {
    // 0x80 - 0x01 = 0x7F: the most negative byte minus one cannot be
    // represented, so V is set. N is clear, so S is set and brlt still sees a
    // negative comparison.
    Machine m = run("ldi r16, 0x80\n"
                    "ldi r17, 0x01\n"
                    "sub r16, r17\n"
                    "done: rjmp done\n");
    CHECK_EQ(int(m.reg(16)), 0x7F);
    check_flags(m, 0, 0, 0, 1, 1, 1, "sub 0x80 - 0x01");
}

TEST(emu_sub_below_zero_sets_carry_as_a_borrow) {
    // 0x00 - 0x01 = 0xFF. C means "borrowed", which is why brlo is the unsigned
    // less-than branch and shares an encoding with brcs.
    Machine m = run("ldi r16, 0x00\n"
                    "ldi r17, 0x01\n"
                    "sub r16, r17\n"
                    "done: rjmp done\n");
    CHECK_EQ(int(m.reg(16)), 0xFF);
    check_flags(m, 1, 0, 1, 0, 1, 1, "sub 0x00 - 0x01");
}

TEST(emu_subi_sets_zero_but_sbci_only_ever_clears_it) {
    // The asymmetry that makes multi-byte compares work. subi computes Z from
    // its own result; sbci may only clear Z, so a zero high byte leaves an
    // earlier "not equal" verdict standing.
    Machine subi_case = run("clz\n"
                            "ldi r16, 0x07\n"
                            "subi r16, 0x07\n"
                            "done: rjmp done\n");
    CHECK_EQ(int(subi_case.reg(16)), 0x00);
    CHECK_EQ(int(subi_case.state.flag_z), 1);

    // Z starts clear, sbci produces zero, and Z must STAY clear.
    Machine sbci_zero = run("clz\n"
                            "clc\n"
                            "ldi r16, 0x07\n"
                            "sbci r16, 0x07\n"
                            "done: rjmp done\n");
    CHECK_EQ(int(sbci_zero.reg(16)), 0x00);
    CHECK_EQ(int(sbci_zero.state.flag_z), 0);

    // Z starts set, sbci produces a nonzero result, and Z must be cleared.
    Machine sbci_nonzero = run("sez\n"
                               "clc\n"
                               "ldi r16, 0x09\n"
                               "sbci r16, 0x07\n"
                               "done: rjmp done\n");
    CHECK_EQ(int(sbci_nonzero.reg(16)), 0x02);
    CHECK_EQ(int(sbci_nonzero.state.flag_z), 0);

    // Z starts set, sbci produces zero, and Z must be left alone.
    Machine sbci_keeps = run("sez\n"
                             "clc\n"
                             "ldi r16, 0x07\n"
                             "sbci r16, 0x07\n"
                             "done: rjmp done\n");
    CHECK_EQ(int(sbci_keeps.reg(16)), 0x00);
    CHECK_EQ(int(sbci_keeps.state.flag_z), 1);
}

TEST(emu_sbc_borrows_the_carry_and_only_clears_zero) {
    // 0x00 - 0x00 - 1 = 0xFF, and the borrow propagates out again.
    Machine m = run("sez\n"
                    "sec\n"
                    "ldi r16, 0x00\n"
                    "ldi r17, 0x00\n"
                    "sbc r16, r17\n"
                    "done: rjmp done\n");
    CHECK_EQ(int(m.reg(16)), 0xFF);
    CHECK_EQ(int(m.state.flag_c), 1);
    CHECK_EQ(int(m.state.flag_z), 0);   // cleared, because the result is nonzero
    CHECK_EQ(int(m.state.flag_h), 1);
    CHECK_EQ(int(m.state.flag_n), 1);
}

TEST(emu_neg_carry_is_set_for_every_value_except_zero) {
    Machine zero = run("ldi r16, 0x00\n"
                       "neg r16\n"
                       "done: rjmp done\n");
    CHECK_EQ(int(zero.reg(16)), 0x00);
    check_flags(zero, 0, 1, 0, 0, 0, 0, "neg 0x00");

    Machine one = run("ldi r16, 0x01\n"
                      "neg r16\n"
                      "done: rjmp done\n");
    CHECK_EQ(int(one.reg(16)), 0xFF);
    // H is the borrow out of bit 3, which negating any value with a nonzero low
    // nibble produces.
    check_flags(one, 1, 0, 1, 0, 1, 1, "neg 0x01");
}

TEST(emu_neg_overflows_only_on_the_most_negative_value) {
    // 0x80 negated is 0x80 again: the only byte whose negation does not fit, so
    // it is the only byte for which neg sets V.
    Machine m = run("ldi r16, 0x80\n"
                    "neg r16\n"
                    "done: rjmp done\n");
    CHECK_EQ(int(m.reg(16)), 0x80);
    // N and V are both set, so S is clear.
    check_flags(m, 1, 0, 1, 1, 0, 0, "neg 0x80");

    // 0x40 is the neighbouring case that must NOT overflow.
    Machine ok = run("ldi r16, 0x40\n"
                     "neg r16\n"
                     "done: rjmp done\n");
    CHECK_EQ(int(ok.reg(16)), 0xC0);
    check_flags(ok, 1, 0, 1, 0, 1, 0, "neg 0x40");
}

TEST(emu_com_always_sets_carry_and_never_overflows) {
    Machine m = run("clc\n"
                    "ldi r16, 0x0F\n"
                    "com r16\n"
                    "done: rjmp done\n");
    CHECK_EQ(int(m.reg(16)), 0xF0);
    CHECK_EQ(int(m.state.flag_c), 1);
    CHECK_EQ(int(m.state.flag_v), 0);
    CHECK_EQ(int(m.state.flag_n), 1);
    CHECK_EQ(int(m.state.flag_s), 1);
    CHECK_EQ(int(m.state.flag_z), 0);
}

TEST(emu_inc_and_dec_leave_carry_alone_and_overflow_at_the_sign_edge) {
    // inc overflows going from 0x7F, dec going from 0x80, and neither may touch
    // C -- that is what lets them appear inside a multi-byte add or subtract
    // loop without destroying the propagating carry.
    Machine inc = run("sec\n"
                      "ldi r16, 0x7F\n"
                      "inc r16\n"
                      "done: rjmp done\n");
    CHECK_EQ(int(inc.reg(16)), 0x80);
    CHECK_EQ(int(inc.state.flag_c), 1);   // untouched
    CHECK_EQ(int(inc.state.flag_v), 1);
    CHECK_EQ(int(inc.state.flag_n), 1);
    CHECK_EQ(int(inc.state.flag_s), 0);
    CHECK_EQ(int(inc.state.flag_z), 0);

    Machine wrap = run("clc\n"
                       "ldi r16, 0xFF\n"
                       "inc r16\n"
                       "done: rjmp done\n");
    CHECK_EQ(int(wrap.reg(16)), 0x00);
    CHECK_EQ(int(wrap.state.flag_c), 0);   // still untouched, even on wrap
    CHECK_EQ(int(wrap.state.flag_z), 1);
    CHECK_EQ(int(wrap.state.flag_v), 0);

    Machine dec = run("sec\n"
                      "ldi r16, 0x80\n"
                      "dec r16\n"
                      "done: rjmp done\n");
    CHECK_EQ(int(dec.reg(16)), 0x7F);
    CHECK_EQ(int(dec.state.flag_c), 1);
    CHECK_EQ(int(dec.state.flag_v), 1);
    CHECK_EQ(int(dec.state.flag_n), 0);
    CHECK_EQ(int(dec.state.flag_s), 1);
}

TEST(emu_logic_instructions_clear_overflow_and_derive_sign_from_it) {
    Machine m = run("sev\n"
                    "ldi r16, 0xF0\n"
                    "ldi r17, 0x8F\n"
                    "and r16, r17\n"
                    "done: rjmp done\n");
    CHECK_EQ(int(m.reg(16)), 0x80);
    CHECK_EQ(int(m.state.flag_v), 0);   // and always clears V
    CHECK_EQ(int(m.state.flag_n), 1);
    CHECK_EQ(int(m.state.flag_s), 1);   // S is N xor V, so it follows N here

    Machine cleared = run("ldi r16, 0x55\n"
                          "eor r16, r16\n"     // clr
                          "done: rjmp done\n");
    CHECK_EQ(int(cleared.reg(16)), 0x00);
    CHECK_EQ(int(cleared.state.flag_z), 1);
    CHECK_EQ(int(cleared.state.flag_n), 0);

    Machine ored = run("ldi r16, 0x0F\n"
                       "ori r16, 0xF0\n"
                       "done: rjmp done\n");
    CHECK_EQ(int(ored.reg(16)), 0xFF);
    CHECK_EQ(int(ored.state.flag_n), 1);
    CHECK_EQ(int(ored.state.flag_z), 0);

    Machine anded = run("ldi r16, 0xFF\n"
                        "andi r16, 0x0F\n"
                        "done: rjmp done\n");
    CHECK_EQ(int(anded.reg(16)), 0x0F);
    CHECK_EQ(int(anded.state.flag_n), 0);
}

TEST(emu_ser_loads_all_ones_and_tst_reports_the_sign) {
    // ser is ldi with an immediate of 0xFF and tst is and with the register
    // itself; both are real mnemonics that share encodings with something else,
    // so what is being checked is that the shared decode reaches the right arm.
    Machine m = run("ldi r16, 0xFF\n"
                    "tst r16\n"
                    "done: rjmp done\n");
    CHECK_EQ(int(m.reg(16)), 0xFF);
    CHECK_EQ(int(m.state.flag_n), 1);
    CHECK_EQ(int(m.state.flag_z), 0);
    CHECK_EQ(int(m.state.flag_v), 0);
}

// =========================================================== multi-byte ===

TEST(emu_multi_byte_add_propagates_the_carry_through_adc) {
    // 0x00FF + 0x0001 = 0x0100 held in r17:r16. This is the reason add computes
    // C at all, and the reason adc exists.
    Machine m = run("ldi r16, 0xFF\n"
                    "ldi r17, 0x00\n"
                    "ldi r18, 0x01\n"
                    "ldi r19, 0x00\n"
                    "add r16, r18\n"
                    "adc r17, r19\n"
                    "done: rjmp done\n");
    CHECK_EQ(int(m.reg(16)), 0x00);
    CHECK_EQ(int(m.reg(17)), 0x01);
    CHECK_EQ(int(m.state.flag_c), 0);
    CHECK_EQ(int(m.state.flag_z), 0);
}

TEST(emu_multi_byte_add_carries_all_the_way_out_of_the_top) {
    // 0xFFFF + 0x0001 = 0x0000 with C set. adc must set Z from its own result,
    // unlike the subtract-with-borrow forms, so Z here reports only that the
    // HIGH byte came out zero.
    Machine m = run("ldi r16, 0xFF\n"
                    "ldi r17, 0xFF\n"
                    "ldi r18, 0x01\n"
                    "ldi r19, 0x00\n"
                    "add r16, r18\n"
                    "adc r17, r19\n"
                    "done: rjmp done\n");
    CHECK_EQ(int(m.reg(16)), 0x00);
    CHECK_EQ(int(m.reg(17)), 0x00);
    CHECK_EQ(int(m.state.flag_c), 1);
    CHECK_EQ(int(m.state.flag_z), 1);
}

TEST(emu_multi_byte_compare_reports_equality_across_both_bytes) {
    // 0x1234 against 0x1234. cp sets Z on the low byte; cpc may only clear it,
    // so Z survives to report that the whole 16-bit value matched.
    Machine equal = run("ldi r16, 0x34\n"
                        "ldi r17, 0x12\n"
                        "ldi r18, 0x34\n"
                        "ldi r19, 0x12\n"
                        "cp  r16, r18\n"
                        "cpc r17, r19\n"
                        "done: rjmp done\n");
    CHECK_EQ(int(equal.state.flag_z), 1);
    CHECK_EQ(int(equal.state.flag_c), 0);

    // 0x1200 against 0x1100. The LOW bytes are equal, so cp sets Z; the high
    // bytes differ, so cpc must clear it again. A cpc that recomputed Z from
    // its own nonzero result would happen to be right here, which is why the
    // next case matters more.
    Machine high_differs = run("ldi r16, 0x00\n"
                               "ldi r17, 0x12\n"
                               "ldi r18, 0x00\n"
                               "ldi r19, 0x11\n"
                               "cp  r16, r18\n"
                               "cpc r17, r19\n"
                               "done: rjmp done\n");
    CHECK_EQ(int(high_differs.state.flag_z), 0);
    CHECK_EQ(int(high_differs.state.flag_c), 0);

    // 0x0034 against 0x0035. The LOW bytes differ and the high bytes are equal,
    // so cpc's own result is zero. Only the clear-only rule keeps Z clear here;
    // a cpc that set Z from its result would wrongly report the values equal.
    Machine low_differs = run("ldi r16, 0x34\n"
                              "ldi r17, 0x00\n"
                              "ldi r18, 0x35\n"
                              "ldi r19, 0x00\n"
                              "cp  r16, r18\n"
                              "cpc r17, r19\n"
                              "done: rjmp done\n");
    CHECK_EQ(int(low_differs.state.flag_z), 0);
    CHECK_EQ(int(low_differs.state.flag_c), 1);   // 0x0034 is below 0x0035
}

// ============================================================ word width ===

TEST(emu_adiw_computes_flags_on_the_whole_sixteen_bit_result) {
    // 0x7FFF + 1 = 0x8000: the 16-bit signed overflow, which is invisible if
    // the flags are taken from the high byte's own addition of zero and carry.
    Machine over = run("ldi r24, 0xFF\n"
                       "ldi r25, 0x7F\n"
                       "adiw r24, 1\n"
                       "done: rjmp done\n");
    CHECK_EQ(int(over.reg(24)), 0x00);
    CHECK_EQ(int(over.reg(25)), 0x80);
    CHECK_EQ(int(over.state.flag_v), 1);
    CHECK_EQ(int(over.state.flag_n), 1);
    CHECK_EQ(int(over.state.flag_s), 0);
    CHECK_EQ(int(over.state.flag_c), 0);
    CHECK_EQ(int(over.state.flag_z), 0);

    // 0xFFFF + 1 = 0x0000: carry out and a zero 16-bit result, so Z reports the
    // whole pair rather than either byte.
    Machine wrap = run("ldi r24, 0xFF\n"
                       "ldi r25, 0xFF\n"
                       "adiw r24, 1\n"
                       "done: rjmp done\n");
    CHECK_EQ(int(wrap.reg(24)), 0x00);
    CHECK_EQ(int(wrap.reg(25)), 0x00);
    CHECK_EQ(int(wrap.state.flag_c), 1);
    CHECK_EQ(int(wrap.state.flag_z), 1);
    CHECK_EQ(int(wrap.state.flag_v), 0);
    CHECK_EQ(int(wrap.state.flag_n), 0);

    // A plain increment of a pointer, the overwhelmingly common use, must leave
    // every flag quiet except the ones it genuinely means.
    Machine plain = run("ldi r30, 0x00\n"
                        "ldi r31, 0x01\n"
                        "adiw r30, 63\n"
                        "done: rjmp done\n");
    CHECK_EQ(int(plain.reg(30)), 63);
    CHECK_EQ(int(plain.reg(31)), 0x01);
    CHECK_EQ(int(plain.state.flag_c), 0);
    CHECK_EQ(int(plain.state.flag_z), 0);
    CHECK_EQ(int(plain.state.flag_n), 0);
    CHECK_EQ(int(plain.state.flag_v), 0);
}

TEST(emu_sbiw_borrows_and_overflows_on_the_sixteen_bit_result) {
    // 0x0000 - 1 = 0xFFFF: a borrow out of the 16-bit value.
    Machine borrow = run("ldi r24, 0x00\n"
                         "ldi r25, 0x00\n"
                         "sbiw r24, 1\n"
                         "done: rjmp done\n");
    CHECK_EQ(int(borrow.reg(24)), 0xFF);
    CHECK_EQ(int(borrow.reg(25)), 0xFF);
    CHECK_EQ(int(borrow.state.flag_c), 1);
    CHECK_EQ(int(borrow.state.flag_n), 1);
    CHECK_EQ(int(borrow.state.flag_v), 0);
    CHECK_EQ(int(borrow.state.flag_s), 1);
    CHECK_EQ(int(borrow.state.flag_z), 0);

    // 0x8000 - 1 = 0x7FFF: the 16-bit signed underflow.
    Machine over = run("ldi r24, 0x00\n"
                       "ldi r25, 0x80\n"
                       "sbiw r24, 1\n"
                       "done: rjmp done\n");
    CHECK_EQ(int(over.reg(24)), 0xFF);
    CHECK_EQ(int(over.reg(25)), 0x7F);
    CHECK_EQ(int(over.state.flag_v), 1);
    CHECK_EQ(int(over.state.flag_n), 0);
    CHECK_EQ(int(over.state.flag_s), 1);
    CHECK_EQ(int(over.state.flag_c), 0);

    // Counting a loop down to zero is what sbiw is really for, so Z must fire
    // exactly when the pair reaches zero.
    Machine to_zero = run("ldi r24, 0x01\n"
                          "ldi r25, 0x00\n"
                          "sbiw r24, 1\n"
                          "done: rjmp done\n");
    CHECK_EQ(int(to_zero.state.flag_z), 1);
    CHECK_EQ(int(to_zero.state.flag_c), 0);
}

// ================================================================ shifts ===

TEST(emu_lsr_shifts_the_low_bit_into_carry_and_can_never_set_n) {
    // 0x01 >> 1 = 0x00 with C set. N is always clear after lsr because a zero
    // is shifted into bit 7, and V is defined as N xor C so that brlt still
    // behaves on a shifted value -- which makes V equal to C here, and S equal
    // to V.
    Machine m = run("ldi r16, 0x01\n"
                    "lsr r16\n"
                    "done: rjmp done\n");
    CHECK_EQ(int(m.reg(16)), 0x00);
    check_flags(m, 1, 1, 0, 1, 1, 0, "lsr 0x01");

    Machine top = run("ldi r16, 0x80\n"
                      "lsr r16\n"
                      "done: rjmp done\n");
    CHECK_EQ(int(top.reg(16)), 0x40);
    check_flags(top, 0, 0, 0, 0, 0, 0, "lsr 0x80");
}

TEST(emu_asr_preserves_the_sign_bit_and_derives_v_from_n_and_c) {
    // 0x81 arithmetic-shifted right is 0xC0: bit 7 is duplicated rather than
    // zeroed, so this is a division by two that rounds towards negative
    // infinity. C takes the bit shifted out, and V is N xor C, which is 0 here.
    Machine m = run("ldi r16, 0x81\n"
                    "asr r16\n"
                    "done: rjmp done\n");
    CHECK_EQ(int(m.reg(16)), 0xC0);
    check_flags(m, 1, 0, 1, 0, 1, 0, "asr 0x81");

    Machine positive = run("ldi r16, 0x02\n"
                           "asr r16\n"
                           "done: rjmp done\n");
    CHECK_EQ(int(positive.reg(16)), 0x01);
    check_flags(positive, 0, 0, 0, 0, 0, 0, "asr 0x02");
}

TEST(emu_ror_rotates_carry_in_at_the_top_and_the_low_bit_out) {
    // With C set, 0x01 rotated right is 0x80 and C comes back out set. Getting
    // the two carries the wrong way round is how a multi-byte right shift ends
    // up losing a bit.
    Machine m = run("sec\n"
                    "ldi r16, 0x01\n"
                    "ror r16\n"
                    "done: rjmp done\n");
    CHECK_EQ(int(m.reg(16)), 0x80);
    // N is set from the rotated-in bit, C is the bit rotated out, V is N xor C.
    check_flags(m, 1, 0, 1, 0, 1, 0, "ror 0x01 with carry in");

    Machine no_carry = run("clc\n"
                           "ldi r16, 0x03\n"
                           "ror r16\n"
                           "done: rjmp done\n");
    CHECK_EQ(int(no_carry.reg(16)), 0x01);
    check_flags(no_carry, 1, 0, 0, 1, 1, 0, "ror 0x03 without carry in");
}

TEST(emu_multi_byte_right_shift_moves_a_bit_between_bytes) {
    // The reason ror's two carries have to be right: lsr on the high byte then
    // ror on the low byte divides a 16-bit value by two. 0x0100 >> 1 = 0x0080.
    Machine m = run("ldi r16, 0x00\n"
                    "ldi r17, 0x01\n"
                    "lsr r17\n"
                    "ror r16\n"
                    "done: rjmp done\n");
    CHECK_EQ(int(m.reg(17)), 0x00);
    CHECK_EQ(int(m.reg(16)), 0x80);
    CHECK_EQ(int(m.state.flag_c), 0);
}

TEST(emu_lsl_and_rol_are_the_add_encodings_and_shift_a_pair_left) {
    // lsl Rd is add Rd, Rd and rol Rd is adc Rd, Rd, so they must produce the
    // add flag set -- including H, which is the carry out of bit 3 and is the
    // one people forget a left shift touches.
    Machine m = run("clc\n"
                    "ldi r16, 0x80\n"
                    "ldi r17, 0x01\n"
                    "lsl r16\n"
                    "rol r17\n"
                    "done: rjmp done\n");
    CHECK_EQ(int(m.reg(16)), 0x00);
    CHECK_EQ(int(m.reg(17)), 0x03);
    CHECK_EQ(int(m.state.flag_c), 0);

    Machine flags = run("ldi r16, 0x08\n"
                        "lsl r16\n"
                        "done: rjmp done\n");
    CHECK_EQ(int(flags.reg(16)), 0x10);
    CHECK_EQ(int(flags.state.flag_h), 1);   // carry out of bit 3
    CHECK_EQ(int(flags.state.flag_c), 0);
}

TEST(emu_swap_exchanges_nibbles_and_touches_no_flag) {
    Machine m = run("sec\n"
                    "sez\n"
                    "ldi r16, 0x4A\n"
                    "swap r16\n"
                    "done: rjmp done\n");
    CHECK_EQ(int(m.reg(16)), 0xA4);
    CHECK_EQ(int(m.state.flag_c), 1);
    CHECK_EQ(int(m.state.flag_z), 1);
    CHECK_EQ(int(m.state.flag_n), 0);
}

// ============================================================== multiply ===

TEST(emu_mul_puts_the_product_in_r1_r0_whatever_it_multiplied) {
    // 0x10 * 0x10 = 0x0100. The destination is always r1:r0, which is why the
    // runtime has to clear r1 again afterwards to restore its zero register.
    Machine m = run("ldi r16, 0x10\n"
                    "ldi r17, 0x10\n"
                    "mul r16, r17\n"
                    "done: rjmp done\n");
    CHECK_EQ(int(m.reg(0)), 0x00);
    CHECK_EQ(int(m.reg(1)), 0x01);
    CHECK_EQ(int(m.state.flag_c), 0);
    CHECK_EQ(int(m.state.flag_z), 0);

    // C is bit 15 of the product, so a product that fills the pair sets it.
    Machine big = run("ldi r16, 0xFF\n"
                      "ldi r17, 0xFF\n"
                      "mul r16, r17\n"
                      "done: rjmp done\n");
    CHECK_EQ(int(big.reg(0)), 0x01);
    CHECK_EQ(int(big.reg(1)), 0xFE);
    CHECK_EQ(int(big.state.flag_c), 1);

    Machine zero = run("ldi r16, 0x00\n"
                       "ldi r17, 0x7F\n"
                       "mul r16, r17\n"
                       "done: rjmp done\n");
    CHECK_EQ(int(zero.state.flag_z), 1);
    CHECK_EQ(int(zero.state.flag_c), 0);
}

TEST(emu_muls_and_mulsu_treat_their_operands_with_the_right_signedness) {
    // The assembler has no mnemonic for these yet, so the encodings are emitted
    // directly: muls is 0000 0010 dddd rrrr and mulsu is 0000 0011 0ddd 0rrr,
    // both with register numbers biased by 16.
    //
    // muls r16, r17 with -1 and -1 gives +1: a purely unsigned multiply would
    // give 0xFE01 instead, so this distinguishes them.
    Machine signed_case = run("ldi r16, 0xFF\n"
                              "ldi r17, 0xFF\n"
                              ".word 0x0201\n"      // muls r16, r17
                              "done: rjmp done\n");
    CHECK_EQ(int(signed_case.reg(0)), 0x01);
    CHECK_EQ(int(signed_case.reg(1)), 0x00);
    CHECK_EQ(int(signed_case.state.flag_c), 0);
    CHECK_EQ(int(signed_case.state.flag_z), 0);

    // mulsu r16, r17 with -2 (signed) and 3 (unsigned) gives -6 = 0xFFFA. Note
    // the asymmetry: had r17 been read as signed it would be +3 as well and the
    // answer would coincide, so 0xFE is chosen for r17 in the next case.
    Machine mixed = run("ldi r16, 0xFE\n"
                        "ldi r17, 0x03\n"
                        ".word 0x0301\n"            // mulsu r16, r17
                        "done: rjmp done\n");
    CHECK_EQ(int(mixed.reg(0)), 0xFA);
    CHECK_EQ(int(mixed.reg(1)), 0xFF);
    CHECK_EQ(int(mixed.state.flag_c), 1);   // bit 15 of the product

    // -1 signed times 254 unsigned is -254 = 0xFF02. Read as signed on both
    // sides it would be +2, so this pins the unsigned operand down.
    Machine unsigned_operand = run("ldi r16, 0xFF\n"
                                   "ldi r17, 0xFE\n"
                                   ".word 0x0301\n"   // mulsu r16, r17
                                   "done: rjmp done\n");
    CHECK_EQ(int(unsigned_operand.reg(0)), 0x02);
    CHECK_EQ(int(unsigned_operand.reg(1)), 0xFF);
}

TEST(emu_fractional_multiplies_shift_the_product_left_by_one) {
    // fmul is 0000 0011 0ddd 1rrr. 0xFF * 0xFF = 0xFE01, and the fractional
    // form shifts that left by one to 0xFC02, with C taken from the bit that
    // fell off the top -- bit 15 of the UNSHIFTED product.
    Machine fmul = run("ldi r16, 0xFF\n"
                       "ldi r17, 0xFF\n"
                       ".word 0x0309\n"           // fmul r16, r17
                       "done: rjmp done\n");
    CHECK_EQ(int(fmul.reg(0)), 0x02);
    CHECK_EQ(int(fmul.reg(1)), 0xFC);
    CHECK_EQ(int(fmul.state.flag_c), 1);

    // 0x40 * 0x40 is 0.5 * 0.5 in 1.7 fixed point. The product is 0x1000 and
    // the shift makes it 0x2000, which is 0.25 in 1.15 -- the answer only comes
    // out right because of the shift.
    Machine half = run("ldi r16, 0x40\n"
                       "ldi r17, 0x40\n"
                       ".word 0x0309\n"           // fmul r16, r17
                       "done: rjmp done\n");
    CHECK_EQ(int(half.reg(0)), 0x00);
    CHECK_EQ(int(half.reg(1)), 0x20);
    CHECK_EQ(int(half.state.flag_c), 0);

    // fmuls is 0000 0011 1ddd 0rrr: -1 times -1 as 1.7 fractions gives 0x0001
    // shifted to 0x0002.
    Machine fmuls = run("ldi r16, 0xFF\n"
                        "ldi r17, 0xFF\n"
                        ".word 0x0381\n"          // fmuls r16, r17
                        "done: rjmp done\n");
    CHECK_EQ(int(fmuls.reg(0)), 0x02);
    CHECK_EQ(int(fmuls.reg(1)), 0x00);
    CHECK_EQ(int(fmuls.state.flag_c), 0);

    // fmulsu is 0000 0011 1ddd 1rrr: -1 signed times 254 unsigned is 0xFF02,
    // shifted left to 0xFE04.
    Machine fmulsu = run("ldi r16, 0xFF\n"
                         "ldi r17, 0xFE\n"
                         ".word 0x0389\n"         // fmulsu r16, r17
                         "done: rjmp done\n");
    CHECK_EQ(int(fmulsu.reg(0)), 0x04);
    CHECK_EQ(int(fmulsu.reg(1)), 0xFE);
    CHECK_EQ(int(fmulsu.state.flag_c), 1);
}

// ================================================================= moves ===

TEST(emu_mov_and_movw_copy_a_byte_and_a_pair) {
    Machine m = run("ldi r16, 0xAB\n"
                    "ldi r17, 0xCD\n"
                    "mov r18, r16\n"
                    "movw r20, r16\n"
                    "done: rjmp done\n");
    CHECK_EQ(int(m.reg(18)), 0xAB);
    CHECK_EQ(int(m.reg(20)), 0xAB);
    CHECK_EQ(int(m.reg(21)), 0xCD);
}

TEST(emu_lds_and_sts_reach_a_sixteen_bit_data_address) {
    // Both are two-word instructions whose second word is the address, so this
    // also proves the program counter stepped over that word rather than
    // executing it.
    Machine m = run("ldi r16, 0x5A\n"
                    "sts 0x0300, r16\n"
                    "lds r17, 0x0300\n"
                    "done: rjmp done\n");
    CHECK_EQ(int(m.data(0x0300)), 0x5A);
    CHECK_EQ(int(m.reg(17)), 0x5A);
}

TEST(emu_pointer_loads_and_stores_cover_every_addressing_mode) {
    // X plain, X post-increment and X pre-decrement, all against the same
    // address, so a mode that updated the pointer at the wrong moment would
    // read back the wrong byte.
    Machine m = run("ldi r26, 0x00\n"           // X = 0x0300
                    "ldi r27, 0x03\n"
                    "ldi r16, 0x11\n"
                    "st  X, r16\n"
                    "ldi r16, 0x22\n"
                    "st  X+, r16\n"             // stores at 0x0300, X becomes 0x0301
                    "ldi r16, 0x33\n"
                    "st  X, r16\n"              // stores at 0x0301
                    "ld  r18, -X\n"             // X back to 0x0300, reads 0x22
                    "ld  r19, X+\n"             // reads 0x0300, X to 0x0301
                    "ld  r20, X\n"              // reads 0x0301
                    "done: rjmp done\n");
    CHECK_EQ(int(m.data(0x0300)), 0x22);
    CHECK_EQ(int(m.data(0x0301)), 0x33);
    CHECK_EQ(int(m.reg(18)), 0x22);
    CHECK_EQ(int(m.reg(19)), 0x22);
    CHECK_EQ(int(m.reg(20)), 0x33);
    CHECK_EQ(int(m.reg(26)), 0x01);
    CHECK_EQ(int(m.reg(27)), 0x03);

    // Y and Z have the same three modes plus the displacement forms, and Y is
    // the frame pointer the code generator uses, so its post-increment and
    // pre-decrement have to be right for locals to survive.
    Machine yz = run("ldi r28, 0x10\n"          // Y = 0x0310
                     "ldi r29, 0x03\n"
                     "ldi r30, 0x20\n"          // Z = 0x0320
                     "ldi r31, 0x03\n"
                     "ldi r16, 0x44\n"
                     "st  Y+, r16\n"
                     "ldi r16, 0x55\n"
                     "st  Y, r16\n"
                     "ld  r17, -Y\n"
                     "ldi r16, 0x66\n"
                     "st  Z+, r16\n"
                     "ld  r18, -Z\n"
                     "done: rjmp done\n");
    CHECK_EQ(int(yz.data(0x0310)), 0x44);
    CHECK_EQ(int(yz.data(0x0311)), 0x55);
    CHECK_EQ(int(yz.reg(17)), 0x44);
    CHECK_EQ(int(yz.reg(28)), 0x10);
    CHECK_EQ(int(yz.data(0x0320)), 0x66);
    CHECK_EQ(int(yz.reg(18)), 0x66);
    CHECK_EQ(int(yz.reg(30)), 0x20);
}

TEST(emu_ldd_and_std_add_a_displacement_without_moving_the_pointer) {
    // The displacement bits are scattered across three fields of the opcode, so
    // a value with bits in all of them is used: 0x3F is the largest allowed.
    Machine m = run("ldi r28, 0x00\n"           // Y = 0x0300
                    "ldi r29, 0x03\n"
                    "ldi r16, 0x77\n"
                    "std Y+5, r16\n"
                    "ldi r16, 0x88\n"
                    "std Y+63, r16\n"
                    "ldd r17, Y+5\n"
                    "ldd r18, Y+63\n"
                    "done: rjmp done\n");
    CHECK_EQ(int(m.data(0x0305)), 0x77);
    CHECK_EQ(int(m.data(0x033F)), 0x88);
    CHECK_EQ(int(m.reg(17)), 0x77);
    CHECK_EQ(int(m.reg(18)), 0x88);
    CHECK_EQ(int(m.reg(28)), 0x00);   // the pointer itself is untouched
    CHECK_EQ(int(m.reg(29)), 0x03);
}

TEST(emu_push_and_pop_use_the_stack_in_the_right_direction) {
    Machine m = run("ldi r16, 0x01\n"
                    "ldi r17, 0x02\n"
                    "push r16\n"
                    "push r17\n"
                    "pop r18\n"                 // last in, first out
                    "pop r19\n"
                    "done: rjmp done\n");
    CHECK_EQ(int(m.reg(18)), 0x02);
    CHECK_EQ(int(m.reg(19)), 0x01);
    CHECK_EQ(int(m.state.sp), int(m.device.ramend));
    // The stack grows down, so the pushed bytes sit just below RAMEND.
    CHECK_EQ(int(m.data(uint16_t(m.device.ramend))), 0x01);
    CHECK_EQ(int(m.data(uint16_t(m.device.ramend - 1))), 0x02);
}

TEST(emu_in_and_out_reach_sreg_and_the_stack_pointer) {
    // SREG, SPL and SPH are the three registers at the same I/O address on
    // every AVR8 part, and the runtime's reset code reaches all three with
    // in/out, so they have to answer even with no peripheral attached.
    Machine m = run("sec\n"
                    "sen\n"
                    "in  r16, 0x3F\n"           // SREG
                    "in  r17, 0x3D\n"           // SPL
                    "in  r18, 0x3E\n"           // SPH
                    "done: rjmp done\n");
    CHECK_EQ(int(m.reg(16) & 0x01), 0x01);      // C
    CHECK_EQ(int(m.reg(16) & 0x04), 0x04);      // N
    CHECK_EQ(int(m.reg(17)), int(m.device.ramend & 0xFF));
    CHECK_EQ(int(m.reg(18)), int(m.device.ramend >> 8));

    // Writing SREG has to land in the individual flag bytes, since that is how
    // an interrupt handler restores the flags it saved.
    Machine restored = run("clc\n"
                           "ldi r16, 0x03\n"    // C and Z
                           "out 0x3F, r16\n"
                           "done: rjmp done\n");
    CHECK_EQ(int(restored.state.flag_c), 1);
    CHECK_EQ(int(restored.state.flag_z), 1);
    CHECK_EQ(int(restored.state.flag_i), 0);

    // Writing SPL and SPH has to move the actual stack pointer, which is what
    // the reset code does before the first call.
    Machine moved = run("ldi r16, 0x00\n"
                        "out 0x3D, r16\n"
                        "ldi r16, 0x04\n"
                        "out 0x3E, r16\n"
                        "done: rjmp done\n");
    CHECK_EQ(int(moved.state.sp), 0x0400);
}

TEST(emu_reads_of_unmodelled_io_are_defined_as_zero) {
    // An address that is neither SRAM nor claimed by any peripheral must read
    // back a fixed value, or the reference core cannot be an oracle: the
    // translator would be free to disagree and both would be "right".
    Machine m = run("ldi r16, 0xFF\n"
                    "lds r16, 0x0080\n"
                    "done: rjmp done\n");
    CHECK_EQ(int(m.reg(16)), 0x00);
}

// ================================================================== flash ===

TEST(emu_lpm_treats_z_as_a_byte_address_into_flash) {
    // The single most common AVR emulator bug. A label is a WORD address, so
    // the byte address of a table entry is the label doubled -- and the low bit
    // of Z then chooses which half of the word is read. If lpm were implemented
    // against word addresses, the +0 and +1 cases below would return the same
    // byte.
    Machine low = run("ldi r30, lo8(table * 2)\n"
                      "ldi r31, hi8(table * 2)\n"
                      "lpm\n"
                      "done: rjmp done\n"
                      "table: .byte 0xAA, 0xBB, 0xCC, 0xDD\n");
    CHECK_EQ(int(low.reg(0)), 0xAA);

    Machine high = run("ldi r30, lo8(table * 2 + 1)\n"
                       "ldi r31, hi8(table * 2 + 1)\n"
                       "lpm\n"
                       "done: rjmp done\n"
                       "table: .byte 0xAA, 0xBB, 0xCC, 0xDD\n");
    CHECK_EQ(int(high.reg(0)), 0xBB);

    Machine third = run("ldi r30, lo8(table * 2 + 2)\n"
                        "ldi r31, hi8(table * 2 + 2)\n"
                        "lpm\n"
                        "done: rjmp done\n"
                        "table: .byte 0xAA, 0xBB, 0xCC, 0xDD\n");
    CHECK_EQ(int(third.reg(0)), 0xCC);
}

TEST(emu_lpm_with_a_destination_and_post_increment_walks_a_table) {
    // lpm Rd, Z+ is 1001 000d dddd 0101, which the assembler does not spell
    // yet, so the two encodings are emitted directly. Walking the table is the
    // realistic use, and it also checks that Z advances by one BYTE and not by
    // one word.
    Machine m = run("ldi r30, lo8(table * 2)\n"
                    "ldi r31, hi8(table * 2)\n"
                    ".word 0x9105\n"           // lpm r16, Z+
                    ".word 0x9115\n"           // lpm r17, Z+
                    ".word 0x9124\n"           // lpm r18, Z   (no increment)
                    "done: rjmp done\n"
                    "table: .byte 0x10, 0x20, 0x30, 0x40\n");
    CHECK_EQ(int(m.reg(16)), 0x10);
    CHECK_EQ(int(m.reg(17)), 0x20);
    CHECK_EQ(int(m.reg(18)), 0x30);
}

// ================================================================ branches ===

TEST(emu_conditional_branches_cover_every_sreg_bit) {
    // brbs and brbc test any of the eight SREG bits, and the named branches are
    // just those two with a bit number baked in. Each pair below sets the flag
    // one way and checks that exactly one of the two branches is taken.
    const char* program =
        "clr r16\n"
        "sec\n"
        "brcs c_taken\n"
        "rjmp fail\n"
        "c_taken: ori r16, 0x01\n"
        "clc\n"
        "brcc c_clear\n"
        "rjmp fail\n"
        "c_clear: ori r16, 0x02\n"
        "sez\n"
        "breq z_taken\n"
        "rjmp fail\n"
        "z_taken: ori r16, 0x04\n"
        "clz\n"
        "brne z_clear\n"
        "rjmp fail\n"
        "z_clear: ori r16, 0x08\n"
        "sen\n"
        "brmi n_taken\n"
        "rjmp fail\n"
        "n_taken: ori r16, 0x10\n"
        "cln\n"
        "brpl n_clear\n"
        "rjmp fail\n"
        "n_clear: ori r16, 0x20\n"
        "ses\n"
        "brlt s_taken\n"
        "rjmp fail\n"
        "s_taken: ori r16, 0x40\n"
        "cls\n"
        "brge s_clear\n"
        "rjmp fail\n"
        "s_clear: ori r16, 0x80\n"
        "done: rjmp done\n"
        "fail: ldi r16, 0x00\n"
        "     rjmp fail\n";
    Machine m = run(program);
    CHECK_EQ(int(m.reg(16)), 0xFF);
    CHECK_EQ(int(m.result.reason == StopReason::Halted), 1);
}

TEST(emu_brbs_and_brbc_reach_the_half_carry_and_transfer_bits) {
    // Neither H nor T has a named branch in the assembler, so brbs/brbc are
    // emitted directly: 1111 00kk kkkk ksss with s of 5 for H and 6 for T, and
    // a displacement of +1 to skip the following instruction.
    //
    // seh; brbs 5, +1; ldi r16,0 -- with H set the ldi is jumped over.
    Machine h_set = run("ldi r16, 0x99\n"
                        "seh\n"
                        ".word 0xF00D\n"      // brbs 5, .+2
                        "ldi r16, 0x00\n"
                        "done: rjmp done\n");
    CHECK_EQ(int(h_set.reg(16)), 0x99);

    Machine h_clear = run("ldi r16, 0x99\n"
                          "clh\n"
                          ".word 0xF00D\n"    // brbs 5, .+2, not taken
                          "ldi r16, 0x00\n"
                          "done: rjmp done\n");
    CHECK_EQ(int(h_clear.reg(16)), 0x00);

    // set; brbc 6, +1 -- with T set the branch is NOT taken.
    Machine t_set = run("ldi r16, 0x99\n"
                        "set\n"
                        ".word 0xF40E\n"      // brbc 6, .+2
                        "ldi r16, 0x00\n"
                        "done: rjmp done\n");
    CHECK_EQ(int(t_set.reg(16)), 0x00);
}

TEST(emu_a_backward_branch_runs_a_loop_the_right_number_of_times) {
    // Ten iterations counted down with dec, which is the shape every delay loop
    // in the runtime has. A branch displacement computed with the wrong sign or
    // the wrong base would either not terminate or terminate immediately.
    Machine m = run("ldi r16, 10\n"
                    "clr r17\n"
                    "loop: inc r17\n"
                    "      dec r16\n"
                    "      brne loop\n"
                    "done: rjmp done\n");
    CHECK_EQ(int(m.reg(17)), 10);
    CHECK_EQ(int(m.reg(16)), 0);
}

TEST(emu_jmp_and_ijmp_take_word_addresses) {
    // jmp's target is a word address held in the second word. ijmp's is the Z
    // pointer, also a word address -- the one place Z does NOT mean bytes,
    // which is the mirror image of the lpm trap.
    Machine direct = run("jmp there\n"
                         "ldi r16, 0xFF\n"
                         "there: ldi r16, 0x42\n"
                         "done: rjmp done\n");
    CHECK_EQ(int(direct.reg(16)), 0x42);

    Machine indirect = run("ldi r30, lo8(there)\n"
                           "ldi r31, hi8(there)\n"
                           "ijmp\n"
                           "ldi r16, 0xFF\n"
                           "there: ldi r16, 0x42\n"
                           "done: rjmp done\n");
    CHECK_EQ(int(indirect.reg(16)), 0x42);
}

// ================================================= calls and returns ===

TEST(emu_call_and_ret_return_to_the_instruction_after_the_call) {
    // call is two words, so the return address is the call's address plus two.
    // Getting that wrong by one lands back on the call's own second word.
    Machine m = run("ldi r17, 0\n"
                    "call sub\n"
                    "ldi r17, 2\n"
                    "done: rjmp done\n"
                    "sub: ldi r16, 1\n"
                    "     ret\n");
    CHECK_EQ(int(m.reg(16)), 1);
    CHECK_EQ(int(m.reg(17)), 2);
    CHECK_EQ(int(m.state.sp), int(m.device.ramend));   // stack fully unwound
}

TEST(emu_rcall_and_icall_also_round_trip) {
    Machine relative = run("ldi r17, 0\n"
                           "rcall sub\n"
                           "ldi r17, 2\n"
                           "done: rjmp done\n"
                           "sub: ldi r16, 1\n"
                           "     ret\n");
    CHECK_EQ(int(relative.reg(16)), 1);
    CHECK_EQ(int(relative.reg(17)), 2);
    CHECK_EQ(int(relative.state.sp), int(relative.device.ramend));

    Machine indirect = run("ldi r30, lo8(sub)\n"
                           "ldi r31, hi8(sub)\n"
                           "icall\n"
                           "ldi r17, 2\n"
                           "done: rjmp done\n"
                           "sub: ldi r16, 1\n"
                           "     ret\n");
    CHECK_EQ(int(indirect.reg(16)), 1);
    CHECK_EQ(int(indirect.reg(17)), 2);
    CHECK_EQ(int(indirect.state.sp), int(indirect.device.ramend));
}

TEST(emu_nested_calls_unwind_in_the_right_order) {
    // Three levels deep with a marker written at each, so a return address
    // pushed in the wrong byte order would come back to the wrong level.
    Machine m = run("call a\n"
                    "ori r16, 0x01\n"
                    "done: rjmp done\n"
                    "a: call b\n"
                    "   ori r16, 0x02\n"
                    "   ret\n"
                    "b: call c\n"
                    "   ori r16, 0x04\n"
                    "   ret\n"
                    "c: ori r16, 0x08\n"
                    "   ret\n");
    CHECK_EQ(int(m.reg(16)), 0x0F);
    CHECK_EQ(int(m.state.sp), int(m.device.ramend));
}

TEST(emu_the_return_address_is_pushed_high_byte_first) {
    // The AVR stores a return address with its high byte at the LOWER of the
    // two addresses. Code that walks its own stack -- and any core that has to
    // agree with this one -- depends on the order, so it is asserted directly
    // rather than only through a successful ret.
    Machine m = build("call sub\n"
                      "done: rjmp done\n"
                      "sub: rjmp sub\n");
    m.result = m.core->run(m.state);
    CHECK_EQ(int(m.result.reason == StopReason::Halted), 1);
    // call sits at word 0 and is two words wide, so the return address is 2.
    CHECK_EQ(int(m.data(uint16_t(m.device.ramend))), 0x02);        // low byte
    CHECK_EQ(int(m.data(uint16_t(m.device.ramend - 1))), 0x00);    // high byte
    CHECK_EQ(int(m.state.sp), int(m.device.ramend - 2));
}

// ================================================================== skips ===

TEST(emu_a_skip_steps_over_a_two_word_instruction_whole) {
    // The four two-word instructions are lds, sts, jmp and call. A skip that
    // advanced by one word would land on the second half of one of them and
    // execute an operand as an opcode.
    //
    // Here the skipped instruction is a jmp to code that would set r16 to 0xFF.
    // Skipping it correctly leaves r16 at 0x42.
    Machine over_jmp = run("ldi r16, 0x00\n"
                           "sbrc r16, 0\n"      // bit 0 is clear, so skip
                           "jmp bad\n"
                           "ldi r16, 0x42\n"
                           "done: rjmp done\n"
                           "bad: ldi r16, 0xFF\n"
                           "     rjmp bad\n");
    CHECK_EQ(int(over_jmp.reg(16)), 0x42);

    // The same with sts, where a one-word skip would execute the address word
    // 0x0300 as an instruction and the store would not be observable either
    // way -- so the store's absence is what is checked.
    Machine over_sts = run("ldi r17, 0x01\n"
                           "sbrs r17, 0\n"      // bit 0 is set, so skip
                           "sts 0x0300, r17\n"
                           "ldi r16, 0x42\n"
                           "done: rjmp done\n");
    CHECK_EQ(int(over_sts.reg(16)), 0x42);
    CHECK_EQ(int(over_sts.data(0x0300)), 0x00);   // the store never happened

    // And with lds, whose second word would decode as something harmless and
    // therefore hide the bug entirely without this check on the destination.
    Machine over_lds = run("ldi r18, 0xEE\n"
                           "ldi r16, 0x00\n"
                           "cpse r16, r16\n"    // equal, so skip
                           "lds r18, 0x0300\n"
                           "done: rjmp done\n");
    CHECK_EQ(int(over_lds.reg(18)), 0xEE);       // the load never happened
}

TEST(emu_skips_that_do_not_fire_fall_through) {
    Machine m = run("ldi r16, 0x01\n"
                    "sbrc r16, 0\n"             // bit 0 is set, so no skip
                    "ldi r17, 0x55\n"
                    "sbrs r16, 1\n"             // bit 1 is clear, so no skip
                    "ldi r18, 0x66\n"
                    "cpse r16, r17\n"           // not equal, so no skip
                    "ldi r19, 0x77\n"
                    "done: rjmp done\n");
    CHECK_EQ(int(m.reg(17)), 0x55);
    CHECK_EQ(int(m.reg(18)), 0x66);
    CHECK_EQ(int(m.reg(19)), 0x77);
}

TEST(emu_sbic_and_sbis_test_a_bit_in_io_space_through_a_peripheral) {
    // sbi, cbi, sbic and sbis reach the low 32 I/O registers, which on a real
    // part are ports. Routing them through a peripheral proves both that a
    // read-modify-write goes out and comes back, and that the skip forms see
    // what was written.
    Latch latch(0x25);   // data address 0x25 is I/O register 0x05
    Machine m = build("sbi 0x05, 3\n"
                      "sbic 0x05, 3\n"          // bit is set, so no skip
                      "ldi r16, 0x01\n"
                      "sbis 0x05, 3\n"          // bit is set, so skip
                      "ldi r16, 0x00\n"
                      "cbi 0x05, 3\n"
                      "sbis 0x05, 3\n"          // now clear, so no skip
                      "ldi r17, 0x02\n"
                      "done: rjmp done\n");
    reference_core_attach_peripheral(*m.core, &latch);
    m.result = m.core->run(m.state);
    CHECK_EQ(int(m.result.reason == StopReason::Halted), 1);
    CHECK_EQ(int(m.reg(16)), 0x01);
    CHECK_EQ(int(m.reg(17)), 0x02);
    CHECK_EQ(int(latch.regs[0]), 0x00);   // set then cleared again
    CHECK(latch.writes >= 2);
}

TEST(emu_data_accesses_route_to_a_peripheral_instead_of_sram) {
    // A claimed address must never touch the SRAM array, because a peripheral
    // read can have side effects and a peripheral write can start something.
    Latch latch(0x0300);
    Machine m = build("ldi r16, 0x5A\n"
                      "sts 0x0300, r16\n"
                      "lds r17, 0x0300\n"
                      "ldi r26, 0x02\n"
                      "ldi r27, 0x03\n"
                      "ldi r18, 0x99\n"
                      "st  X, r18\n"
                      "done: rjmp done\n");
    reference_core_attach_peripheral(*m.core, &latch);
    m.result = m.core->run(m.state);
    CHECK_EQ(int(m.result.reason == StopReason::Halted), 1);
    CHECK_EQ(int(latch.regs[0]), 0x5A);
    CHECK_EQ(int(latch.regs[2]), 0x99);
    CHECK_EQ(int(m.reg(17)), 0x5A);
    CHECK_EQ(int(m.data(0x0300)), 0x00);   // SRAM was not written behind its back
    CHECK(latch.reads >= 1);
}

// =========================================================== bit handling ===

TEST(emu_bst_and_bld_move_a_bit_through_t) {
    Machine m = run("ldi r16, 0x80\n"
                    "bst r16, 7\n"              // T becomes 1
                    "ldi r17, 0x00\n"
                    "bld r17, 0\n"              // bit 0 of r17 becomes 1
                    "ldi r18, 0xFF\n"
                    "clt\n"
                    "bld r18, 4\n"              // bit 4 of r18 becomes 0
                    "done: rjmp done\n");
    CHECK_EQ(int(m.reg(17)), 0x01);
    CHECK_EQ(int(m.reg(18)), 0xEF);
    CHECK_EQ(int(m.state.flag_t), 0);
}

TEST(emu_bset_and_bclr_reach_every_sreg_bit_by_number) {
    // sei and cli are bset 7 and bclr 7, and the rest of the named flag
    // mnemonics are the same instruction with a different bit number. bset 5
    // and bclr 5 -- the half-carry -- have no named form the assembler knows,
    // so they are emitted directly as 1001 0100 0101 1000 and 1001 0100 1101
    // 1000.
    Machine m = run(".word 0x9458\n"            // bset 5 (seh)
                    "done: rjmp done\n");
    CHECK_EQ(int(m.state.flag_h), 1);

    Machine cleared = run("seh\n"
                          ".word 0x94D8\n"      // bclr 5 (clh)
                          "done: rjmp done\n");
    CHECK_EQ(int(cleared.state.flag_h), 0);

    // sei must set the interrupt flag; without it the terminating self-loop
    // would not be reported as a halt at all, so the halt itself is asserted.
    Machine interrupts = build("sei\n"
                               "done: rjmp done\n");
    interrupts.state.deadline = 200;
    interrupts.result = interrupts.core->run(interrupts.state);
    CHECK_EQ(int(interrupts.state.flag_i), 1);
    CHECK_EQ(int(interrupts.result.reason == StopReason::Deadline), 1);
}

// ============================================================== interrupts ===

TEST(emu_a_pending_vector_is_dispatched_and_reti_resumes_the_program) {
    // Vector 1 on a part with 32 KB of flash lives at word address 2, because
    // each slot in the table is a two-word jmp. The handler sets a flag the
    // main loop is waiting on, and the loop only exits if the return address
    // was pushed and restored correctly.
    OneShotInterrupt source(1);
    Machine m = build("      rjmp start\n"
                      "      .org 2\n"
                      "      rjmp handler\n"
                      "      .org 8\n"
                      "start: ldi r17, 0\n"
                      "      sei\n"
                      "wait: sbrs r17, 0\n"
                      "      rjmp wait\n"
                      "      cli\n"
                      "done: rjmp done\n"
                      "handler: ldi r17, 1\n"
                      "      ldi r18, 0x77\n"
                      "      reti\n");
    reference_core_attach_peripheral(*m.core, &source);
    m.state.deadline = 1000;
    m.result = m.core->run(m.state);

    CHECK_EQ(int(m.result.reason == StopReason::Halted), 1);
    CHECK_EQ(int(m.reg(17)), 1);
    CHECK_EQ(int(m.reg(18)), 0x77);
    CHECK_EQ(source.acknowledged, 1);
    CHECK_EQ(int(m.state.sp), int(m.device.ramend));   // return address consumed
    CHECK_EQ(int(m.state.flag_i), 0);                  // cli ran after the handler
}

TEST(emu_a_vector_is_not_taken_while_interrupts_are_disabled) {
    // The same source, but the program never enables interrupts. It must run to
    // its own halt untouched -- an emulator that vectors regardless would run
    // the handler and set r17.
    OneShotInterrupt source(1);
    Machine m = build("      rjmp start\n"
                      "      .org 2\n"
                      "      rjmp handler\n"
                      "      .org 8\n"
                      "start: ldi r17, 0\n"
                      "done: rjmp done\n"
                      "handler: ldi r17, 1\n"
                      "      reti\n");
    reference_core_attach_peripheral(*m.core, &source);
    m.result = m.core->run(m.state);

    CHECK_EQ(int(m.result.reason == StopReason::Halted), 1);
    CHECK_EQ(int(m.reg(17)), 0);
    CHECK_EQ(source.acknowledged, 0);
    CHECK_EQ(int(m.state.sp), int(m.device.ramend));
}

TEST(emu_entering_a_handler_disables_further_interrupts_until_reti) {
    // The AVR clears I on entry and reti sets it again. A handler that could be
    // re-entered by its own source would recurse until the stack ate the whole
    // of SRAM, so this is checked from inside the handler itself.
    OneShotInterrupt source(1);
    Machine m = build("      rjmp start\n"
                      "      .org 2\n"
                      "      rjmp handler\n"
                      "      .org 8\n"
                      "start: ldi r17, 0\n"
                      "      sei\n"
                      "wait: sbrs r17, 0\n"
                      "      rjmp wait\n"
                      "      cli\n"
                      "done: rjmp done\n"
                      // Inside the handler, SREG's I bit must read back as 0.
                      "handler: in r19, 0x3F\n"
                      "      andi r19, 0x80\n"
                      "      ldi r17, 1\n"
                      "      reti\n");
    reference_core_attach_peripheral(*m.core, &source);
    m.state.deadline = 1000;
    m.result = m.core->run(m.state);

    CHECK_EQ(int(m.result.reason == StopReason::Halted), 1);
    CHECK_EQ(int(m.reg(19)), 0x00);     // I was clear while the handler ran
    CHECK_EQ(int(m.state.flag_i), 0);   // and cli left it clear at the end
}

// ========================================================== cycle counts ===
//
// Peripherals are advanced against State::cycles, so the cycle count is the
// emulator's clock rather than a statistic. A timer fed by a count that drifts
// makes every delay in every sketch wrong, and wrong by an amount that depends
// on which instructions the code generator happened to pick.
//
// Each program below ends in the self-targeting rjmp that terminates every test
// here, which costs two cycles of its own; that two is included in the totals.

namespace {

// Runs a program and returns the cycles it consumed.
uint64_t cycles_of(const std::string& src) {
    Machine m = run(src);
    return m.state.cycles;
}

} // namespace

TEST(emu_single_cycle_instructions_cost_one_cycle_each) {
    // The terminating rjmp is two, so a program of n one-cycle instructions
    // totals n + 2.
    CHECK_EQ(int(cycles_of("done: rjmp done\n")), 2);
    CHECK_EQ(int(cycles_of("nop\ndone: rjmp done\n")), 3);
    CHECK_EQ(int(cycles_of("ldi r16, 1\ndone: rjmp done\n")), 3);
    CHECK_EQ(int(cycles_of("ldi r16, 1\nmov r17, r16\ndone: rjmp done\n")), 4);
    CHECK_EQ(int(cycles_of("ldi r16, 1\nadd r16, r16\ndone: rjmp done\n")), 4);
    CHECK_EQ(int(cycles_of("ldi r16, 1\nlsr r16\ndone: rjmp done\n")), 4);
    CHECK_EQ(int(cycles_of("ldi r16, 1\nswap r16\ndone: rjmp done\n")), 4);
    CHECK_EQ(int(cycles_of("in r16, 0x3F\ndone: rjmp done\n")), 3);
    CHECK_EQ(int(cycles_of("out 0x3F, r16\ndone: rjmp done\n")), 3);
    CHECK_EQ(int(cycles_of("sei\ncli\ndone: rjmp done\n")), 4);
    CHECK_EQ(int(cycles_of("nop\nwdr\ndone: rjmp done\n")), 4);
    CHECK_EQ(int(cycles_of("sleep\ndone: rjmp done\n")), 3);
}

TEST(emu_two_cycle_instructions_cost_two_cycles_each) {
    CHECK_EQ(int(cycles_of("adiw r24, 1\ndone: rjmp done\n")), 4);
    CHECK_EQ(int(cycles_of("sbiw r24, 1\ndone: rjmp done\n")), 4);
    CHECK_EQ(int(cycles_of("mul r16, r17\ndone: rjmp done\n")), 4);
    CHECK_EQ(int(cycles_of("push r16\ndone: rjmp done\n")), 4);
    CHECK_EQ(int(cycles_of("push r16\npop r17\ndone: rjmp done\n")), 6);
    CHECK_EQ(int(cycles_of("sbi 0x05, 0\ndone: rjmp done\n")), 4);
    CHECK_EQ(int(cycles_of("cbi 0x05, 0\ndone: rjmp done\n")), 4);
    // lds and sts are two words and two cycles.
    CHECK_EQ(int(cycles_of("sts 0x0300, r16\ndone: rjmp done\n")), 4);
    CHECK_EQ(int(cycles_of("lds r16, 0x0300\ndone: rjmp done\n")), 4);
    // Every pointer load and store, with or without a displacement.
    CHECK_EQ(int(cycles_of("ld r16, X\ndone: rjmp done\n")), 4);
    CHECK_EQ(int(cycles_of("ld r16, -Y\ndone: rjmp done\n")), 4);
    CHECK_EQ(int(cycles_of("ldd r16, Z+4\ndone: rjmp done\n")), 4);
    CHECK_EQ(int(cycles_of("st Z+, r16\ndone: rjmp done\n")), 4);
}

TEST(emu_flow_control_cycle_counts_match_the_instruction_summary) {
    // lpm takes three because it reaches out to the flash array.
    CHECK_EQ(int(cycles_of("lpm\ndone: rjmp done\n")), 5);
    // jmp is three, rjmp and ijmp two.
    CHECK_EQ(int(cycles_of("jmp there\nthere: nop\ndone: rjmp done\n")), 6);
    CHECK_EQ(int(cycles_of("ldi r30, lo8(there)\n"
                           "ldi r31, hi8(there)\n"
                           "ijmp\n"
                           "there: nop\n"
                           "done: rjmp done\n")), 7);
    // call is four and ret four; rcall and icall are three.
    CHECK_EQ(int(cycles_of("call sub\ndone: rjmp done\nsub: ret\n")), 10);
    CHECK_EQ(int(cycles_of("rcall sub\ndone: rjmp done\nsub: ret\n")), 9);
}

TEST(emu_a_branch_costs_one_more_cycle_when_it_is_taken) {
    // Not taken: sez is one, breq is one, nop is one, rjmp is two.
    CHECK_EQ(int(cycles_of("clz\nbreq target\ntarget: nop\ndone: rjmp done\n")), 5);
    // Taken: the same program with Z set, so breq costs two.
    CHECK_EQ(int(cycles_of("sez\nbreq target\ntarget: nop\ndone: rjmp done\n")), 6);
}

TEST(emu_a_skip_costs_more_when_it_steps_over_a_two_word_instruction) {
    // A skip is one cycle when it does not fire, two when it steps over a
    // one-word instruction, and three when it steps over a two-word one. That
    // last case is the one an emulator gets wrong at the same time as it gets
    // the address wrong.
    //
    // Not taken: ldi 1, sbrs 1, ldi 1, rjmp 2 = 5.
    CHECK_EQ(int(cycles_of("ldi r16, 0x01\n"
                           "sbrs r16, 1\n"
                           "ldi r17, 0\n"
                           "done: rjmp done\n")), 5);
    // Over one word: ldi 1, sbrs 2, rjmp 2 = 5, with the ldi skipped.
    CHECK_EQ(int(cycles_of("ldi r16, 0x01\n"
                           "sbrs r16, 0\n"
                           "ldi r17, 0\n"
                           "done: rjmp done\n")), 5);
    // Over two words: ldi 1, sbrs 3, rjmp 2 = 6, with the sts skipped.
    CHECK_EQ(int(cycles_of("ldi r16, 0x01\n"
                           "sbrs r16, 0\n"
                           "sts 0x0300, r16\n"
                           "done: rjmp done\n")), 6);
}

TEST(emu_a_delay_loop_costs_what_the_runtime_assumes_it_does) {
    // Four iterations of the classic three-cycle inner loop: dec is one and the
    // taken brne is two, with the final not-taken brne costing one. So the loop
    // is 4 * 3 - 1 = 11, plus the ldi and the terminating rjmp.
    CHECK_EQ(int(cycles_of("ldi r16, 4\n"
                           "loop: dec r16\n"
                           "      brne loop\n"
                           "done: rjmp done\n")), 1 + 11 + 2);
}

TEST(emu_an_interrupt_costs_four_cycles_to_vector) {
    // Vectoring is four cycles: the jump plus pushing the two-byte return
    // address. Timers depend on this, because the handler's own work is
    // measured from when it starts.
    OneShotInterrupt source(1);
    Machine m = build("      rjmp start\n"
                      "      .org 2\n"
                      "      rjmp handler\n"
                      "      .org 8\n"
                      "start: sei\n"          // 1 cycle
                      "spin: rjmp spin\n"     // 2 cycles, then the vector
                      "handler: cli\n"        // 1 cycle
                      "done: rjmp done\n");   // 2 cycles, and halts
    reference_core_attach_peripheral(*m.core, &source);
    m.state.deadline = 1000;
    m.result = m.core->run(m.state);
    CHECK_EQ(int(m.result.reason == StopReason::Halted), 1);
    // rjmp start 2, sei 1, vector 4, rjmp handler 2, cli 1, rjmp done 2.
    // The spin rjmp does not run: the vector is taken before it.
    CHECK_EQ(int(m.state.cycles), 12);
}

// ================================================ stopping and faulting ===

TEST(emu_the_deadline_stops_execution_part_way_through) {
    // The deadline is how a peripheral gets to run at all: the core returns,
    // the machine advances its timers, and the core is called again. So it has
    // to be honoured mid-program, not only at a convenient boundary.
    Machine m = build("nop\nnop\nnop\nnop\nnop\nnop\nnop\nnop\n"
                      "ldi r16, 0x42\n"
                      "done: rjmp done\n");
    m.state.deadline = 4;
    m.result = m.core->run(m.state);

    CHECK_EQ(int(m.result.reason == StopReason::Deadline), 1);
    CHECK_EQ(int(m.state.cycles), 4);
    CHECK_EQ(int(m.state.pc), 4);        // four nops in
    CHECK_EQ(int(m.reg(16)), 0x00);      // the ldi has not run yet

    // Raising the deadline and calling again must resume exactly where it left
    // off, which is the property the whole run/advance loop is built on.
    m.state.deadline = 1000;
    m.result = m.core->run(m.state);
    CHECK_EQ(int(m.result.reason == StopReason::Halted), 1);
    CHECK_EQ(int(m.reg(16)), 0x42);
}

TEST(emu_a_deadline_already_passed_stops_before_anything_runs) {
    Machine m = build("ldi r16, 0x42\n"
                      "done: rjmp done\n");
    m.state.deadline = 0;
    m.result = m.core->run(m.state);
    CHECK_EQ(int(m.result.reason == StopReason::Deadline), 1);
    CHECK_EQ(int(m.state.cycles), 0);
    CHECK_EQ(int(m.reg(16)), 0x00);
}

TEST(emu_a_self_loop_with_interrupts_off_is_reported_as_halted) {
    // A sketch that jumps to itself with interrupts disabled can never do
    // anything again, so the core says so rather than burning the deadline over
    // and over. The program counter is left ON the loop, which is where a user
    // looking at the stop point would expect it.
    Machine m = build("ldi r16, 0x42\n"
                      "done: rjmp done\n");
    m.result = m.core->run(m.state);
    CHECK_EQ(int(m.result.reason == StopReason::Halted), 1);
    CHECK_EQ(int(m.result.pc), 1);
    CHECK_EQ(int(m.state.pc), 1);
    CHECK_EQ(int(m.reg(16)), 0x42);

    // A jmp to itself is the same situation written differently, and has to be
    // recognised too -- the compiler emits it whenever the target is far away.
    Machine far = build("ldi r16, 0x42\n"
                        "stuck: jmp stuck\n");
    far.result = far.core->run(far.state);
    CHECK_EQ(int(far.result.reason == StopReason::Halted), 1);
    CHECK_EQ(int(far.result.pc), 1);
}

TEST(emu_a_self_loop_with_interrupts_on_is_not_a_halt) {
    // The same loop with interrupts enabled is an idle main loop waiting for a
    // handler, which is what half of all sketches look like. It must run until
    // the deadline instead of being declared dead.
    Machine m = build("sei\n"
                      "spin: rjmp spin\n");
    m.state.deadline = 100;
    m.result = m.core->run(m.state);
    CHECK_EQ(int(m.result.reason == StopReason::Deadline), 1);
    CHECK(m.state.cycles >= 100);
}

TEST(emu_an_undecodable_word_reports_where_it_was) {
    // 1001 0100 0000 1011 is not an instruction on this part. The message has
    // to name both the opcode and where it was found, because the alternative
    // is a user staring at a sketch with no idea which line produced it.
    Machine m = build("nop\n"
                      ".word 0x940B\n"
                      "done: rjmp done\n");
    m.result = m.core->run(m.state);
    CHECK_EQ(int(m.result.reason == StopReason::IllegalOpcode), 1);
    CHECK_EQ(int(m.result.pc), 1);
    CHECK_EQ(int(m.state.pc), 1);        // left on the faulting instruction
    CHECK(m.result.error.find("940B") != std::string::npos);
    CHECK(m.result.error.find("0001") != std::string::npos);
}

TEST(emu_erased_flash_does_not_decode) {
    // Running off the end of a program lands in erased flash, which is 0xFFFF
    // everywhere. That is not an instruction, so it faults rather than doing
    // something arbitrary -- the difference between a clear report and a sketch
    // that appears to hang.
    Machine m = build("nop\n");
    m.result = m.core->run(m.state);
    CHECK_EQ(int(m.result.reason == StopReason::IllegalOpcode), 1);
    CHECK_EQ(int(m.result.pc), 1);
}

TEST(emu_a_breakpoint_stops_before_the_instruction_at_it) {
    Machine m = build("ldi r16, 0x01\n"
                      "ldi r16, 0x02\n"
                      "ldi r16, 0x03\n"
                      "done: rjmp done\n");
    m.core->set_breakpoint(2);
    m.result = m.core->run(m.state);
    CHECK_EQ(int(m.result.reason == StopReason::Breakpoint), 1);
    CHECK_EQ(int(m.result.pc), 2);
    CHECK_EQ(int(m.reg(16)), 0x02);   // the instruction at the breakpoint has
                                      // not run yet

    // Resuming must make progress rather than stopping on the same breakpoint
    // again, which is the whole reason the check is skipped on the first
    // instruction of a run.
    m.result = m.core->run(m.state);
    CHECK_EQ(int(m.result.reason == StopReason::Halted), 1);
    CHECK_EQ(int(m.reg(16)), 0x03);

    // Clearing it lets a fresh run pass straight through.
    Machine again = build("ldi r16, 0x01\n"
                          "ldi r16, 0x02\n"
                          "done: rjmp done\n");
    again.core->set_breakpoint(1);
    again.core->clear_breakpoint(1);
    again.result = again.core->run(again.state);
    CHECK_EQ(int(again.result.reason == StopReason::Halted), 1);
}

TEST(emu_the_break_instruction_stops_the_core) {
    Machine m = build("ldi r16, 0x42\n"
                      "break\n"
                      "ldi r16, 0x00\n"
                      "done: rjmp done\n");
    m.result = m.core->run(m.state);
    CHECK_EQ(int(m.result.reason == StopReason::Breakpoint), 1);
    CHECK_EQ(int(m.reg(16)), 0x42);
}

// ================================================================ wiring ===

TEST(emu_the_reference_core_says_what_it_is) {
    // `ardio emulate --explain` prints this, and the difference between the
    // translator and this fallback is orders of magnitude in speed, so a user
    // has to be able to tell which one they got.
    auto core = make_reference_core(atmega328p());
    CHECK(!core->description().empty());
    CHECK(core->description().find("reference") != std::string::npos);
}

TEST(emu_invalidating_a_flash_range_is_harmless_here) {
    // Nothing is cached, so this can only be a no-op -- but it must not fault,
    // because the machine calls it after any spm regardless of which core is
    // running.
    Machine m = build("ldi r16, 0x42\n"
                      "done: rjmp done\n");
    m.core->invalidate(0, m.device.flash_size);
    m.result = m.core->run(m.state);
    CHECK_EQ(int(m.result.reason == StopReason::Halted), 1);
    CHECK_EQ(int(m.reg(16)), 0x42);
}

TEST(emu_the_register_file_is_visible_in_data_space) {
    // r0 to r31 are genuinely mapped at data addresses 0 to 31, so a store
    // through a pointer can land in a register. The runtime does not rely on
    // it, but the architecture guarantees it and a program is free to.
    Machine m = run("ldi r26, 0x05\n"           // X = 0x0005, which is r5
                    "ldi r27, 0x00\n"
                    "ldi r16, 0x37\n"
                    "st  X, r16\n"
                    "done: rjmp done\n");
    CHECK_EQ(int(m.reg(5)), 0x37);
}
