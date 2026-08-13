// Differential tests for the native ARM64 translator.
//
// There is exactly one property worth asserting about a translator, and it is
// not that it produces the answer some human wrote into a test. It is that it
// produces the SAME answer as the reference interpreter, for every program, on
// every byte of state. The reference is the emulator's oracle; a translator
// that agrees with it everywhere is correct by the only definition the rest of
// the emulator uses, and one that disagrees anywhere is broken no matter how
// convincing its own output looks.
//
// So every test here assembles a program, runs it twice from byte-identical
// starting state -- once on make_reference_core, once on the translator -- and
// compares everything afterwards: all thirty-two registers, all eight flags,
// the stack pointer, the program counter, the cycle count, the stop reason, and
// the whole of SRAM. Nothing is spot-checked. A test that compared only the
// register an instruction writes would pass while the translator quietly
// clobbered a flag, and a clobbered flag turns into a branch taken the wrong
// way thousands of instructions later, in a sketch the user cannot debug on
// real hardware.
//
// The cycle count is part of that comparison on purpose. It is what every
// peripheral in the emulator is timed against, so a translator that charged a
// skip one cycle too few would run a sketch whose serial output arrived at the
// wrong moment -- a failure that looks like a hardware quirk rather than a bug.
//
// If the host is not ARM64 there is no translator to compare against, and
// make_arm64_core returns null. These tests then check that fact and stop,
// rather than failing for the crime of running on the wrong machine.

#include "harness.h"

#include "ardio/avr/assembler.h"
#include "ardio/avr/device.h"
#include "ardio/emu/machine.h"

#include <memory>
#include <string>
#include <vector>

namespace ardio::emu {
// The translator's factory. It is not on the Core interface because a caller
// wanting the best available core asks make_core; this is how the selector and
// these tests reach the native one specifically, and it returns null on a host
// with no translator.
std::unique_ptr<Core> make_arm64_core(const avr::AvrDevice& device);
} // namespace ardio::emu

using namespace ardio;
using namespace ardio::emu;

namespace {

const avr::AvrDevice& atmega328p() {
    static const avr::AvrDevice* d = avr::find_device("atmega328p");
    return *d;
}

// One machine: flash, SRAM, guest state and a core, kept together because
// State holds bare pointers into the first two.
struct Machine {
    std::vector<uint8_t> flash;
    std::vector<uint8_t> sram;
    State state;
    std::unique_ptr<Core> core;
    RunResult result;
};

// Everything the two cores must agree on, pulled out of a machine so that the
// comparison is one function and cannot accidentally omit a field.
struct Snapshot {
    uint8_t r[32] = {};
    uint8_t c = 0, z = 0, n = 0, v = 0, s = 0, h = 0, t = 0, i = 0;
    uint16_t pc = 0;
    uint16_t sp = 0;
    uint64_t cycles = 0;
    int reason = 0;
    std::vector<uint8_t> sram;
};

Snapshot snapshot(const Machine& m) {
    Snapshot out;
    for (int k = 0; k < 32; ++k) out.r[k] = m.state.r[k];
    out.c = m.state.flag_c;
    out.z = m.state.flag_z;
    out.n = m.state.flag_n;
    out.v = m.state.flag_v;
    out.s = m.state.flag_s;
    out.h = m.state.flag_h;
    out.t = m.state.flag_t;
    out.i = m.state.flag_i;
    out.pc = m.state.pc;
    out.sp = m.state.sp;
    out.cycles = m.state.cycles;
    out.reason = int(m.result.reason);
    out.sram = m.sram;
    return out;
}

void compare(const Snapshot& ref, const Snapshot& jit, const char* what) {
    auto bad = [&](const std::string& field, long long a, long long b) {
        ::ardio_test::fail(__FILE__, __LINE__,
                           std::string(what) + ": " + field + " -- reference=" +
                               std::to_string(a) + " translator=" + std::to_string(b));
    };

    for (int k = 0; k < 32; ++k)
        if (ref.r[k] != jit.r[k]) bad("r" + std::to_string(k), ref.r[k], jit.r[k]);

    if (ref.c != jit.c) bad("flag C", ref.c, jit.c);
    if (ref.z != jit.z) bad("flag Z", ref.z, jit.z);
    if (ref.n != jit.n) bad("flag N", ref.n, jit.n);
    if (ref.v != jit.v) bad("flag V", ref.v, jit.v);
    if (ref.s != jit.s) bad("flag S", ref.s, jit.s);
    if (ref.h != jit.h) bad("flag H", ref.h, jit.h);
    if (ref.t != jit.t) bad("flag T", ref.t, jit.t);
    if (ref.i != jit.i) bad("flag I", ref.i, jit.i);

    if (ref.pc != jit.pc) bad("pc", ref.pc, jit.pc);
    if (ref.sp != jit.sp) bad("sp", ref.sp, jit.sp);
    if (ref.cycles != jit.cycles) bad("cycles", (long long)ref.cycles, (long long)jit.cycles);
    if (ref.reason != jit.reason) bad("stop reason", ref.reason, jit.reason);

    if (ref.sram.size() != jit.sram.size()) {
        bad("sram size", (long long)ref.sram.size(), (long long)jit.sram.size());
        return;
    }
    for (size_t k = 0; k < ref.sram.size(); ++k)
        if (ref.sram[k] != jit.sram[k])
            bad("sram[" + std::to_string(k + atmega328p().ram_start) + "]",
                ref.sram[k], jit.sram[k]);
}

// Lays a freshly assembled program into a machine, without running it.
//
// Both machines in a pair are built by this same call from the same source, so
// their starting state is identical by construction rather than by two lists of
// assignments that could drift apart.
Machine build(const std::vector<uint8_t>& code, bool native) {
    const avr::AvrDevice& dev = atmega328p();
    Machine m;
    m.flash.assign(dev.flash_size, 0xFF);
    for (size_t k = 0; k < code.size() && k < m.flash.size(); ++k) m.flash[k] = code[k];
    m.sram.assign(dev.ram_size, 0);
    m.state.flash = m.flash.data();
    m.state.sram = m.sram.data();
    m.state.sp = dev.ramend;
    m.state.deadline = 2000000;
    m.core = native ? make_arm64_core(dev) : make_reference_core(dev);
    return m;
}

// True when this host has a translator at all. Every test asks first, so a
// build on an x86 machine reports the suite as passing rather than as broken.
bool have_translator() {
    static const bool yes = make_arm64_core(atmega328p()) != nullptr;
    return yes;
}

// Assembles, runs on both cores, and insists they agree on everything.
//
// The programs all end in a self-targeting rjmp with interrupts off, which both
// cores report as Halted. A program that ran off its own end would wander into
// erased flash and stop as an illegal opcode instead, so a test that lost its
// terminator fails rather than passing on nothing.
void differential(const std::string& source, const char* what) {
    if (!have_translator()) return;

    AssembleResult a = assemble(source);
    if (!a.ok) {
        ::ardio_test::fail(__FILE__, __LINE__,
                           std::string(what) + ": assembler said: " + a.error);
        return;
    }

    Machine ref = build(a.code, false);
    Machine jit = build(a.code, true);

    ref.result = ref.core->run(ref.state);
    jit.result = jit.core->run(jit.state);

    if (ref.result.reason != StopReason::Halted)
        ::ardio_test::fail(__FILE__, __LINE__,
                           std::string(what) + ": the reference did not halt (reason " +
                               std::to_string(int(ref.result.reason)) + ": " +
                               ref.result.error + ")");

    compare(snapshot(ref), snapshot(jit), what);
}

const char* kHalt = "\nend: rjmp end\n";

// Programs are written against a machine whose interrupts are off, which is
// what makes the terminating self-loop report as halted. Nothing here enables
// them, so no prologue is needed -- State starts with flag_i clear.

} // namespace

// ------------------------------------------------------- arithmetic ---

TEST(jit_add_covers_every_flag_case) {
    // Eight adds chosen so that between them C, Z, N, V, H and S are each set
    // and each cleared, including the half-carry on its own -- 0x08 + 0x08
    // carries out of bit 3 and nowhere else, which is the case a translator
    // borrowing the host's flags cannot see at all.
    differential(R"(
        ldi r16, 0x08
        ldi r17, 0x08
        add r16, r17        ; H only
        ldi r18, 0xFF
        ldi r19, 0x01
        add r18, r19        ; C, Z and H together
        ldi r20, 0x7F
        ldi r21, 0x01
        add r20, r21        ; V and N and H
        ldi r22, 0x80
        ldi r23, 0x80
        add r22, r23        ; C, Z, V
        ldi r24, 0x40
        ldi r25, 0x01
        add r24, r25        ; nothing set at all
    )" + std::string(kHalt), "add flag cases");
}

TEST(jit_adc_and_sbc_carry_chains) {
    differential(R"(
        ldi r16, 0x0F
        ldi r17, 0x01
        add r16, r17        ; leaves H set, C clear
        ldi r18, 0xFF
        ldi r19, 0x00
        adc r18, r19        ; adds nothing plus the carry
        sec
        ldi r20, 0xFF
        ldi r21, 0x00
        adc r20, r21        ; wraps to zero with C and H set
        clc
        ldi r22, 0x00
        ldi r23, 0x01
        sbc r22, r23        ; borrows out of the top
        sec
        ldi r24, 0x10
        ldi r25, 0x00
        sbc r24, r25        ; borrow out of bit 4 into the half-carry
    )" + std::string(kHalt), "adc and sbc");
}

TEST(jit_sub_and_cp_half_carry) {
    differential(R"(
        ldi r16, 0x10
        ldi r17, 0x01
        sub r16, r17        ; H set by the borrow from bit 4
        ldi r18, 0x80
        ldi r19, 0x01
        sub r18, r19        ; V set, sign flips
        ldi r20, 0x05
        ldi r21, 0x05
        cp  r20, r21        ; Z set, nothing written
        ldi r22, 0x00
        ldi r23, 0x80
        cp  r22, r23        ; C and V, the awkward signed case
        ldi r24, 0x7F
        ldi r25, 0xFF
        cp  r24, r25
    )" + std::string(kHalt), "sub and cp");
}

TEST(jit_immediate_arithmetic) {
    differential(R"(
        ldi r16, 0x10
        subi r16, 0x01      ; H
        ldi r17, 0x00
        subi r17, 0x01      ; C and N
        ldi r18, 0x42
        cpi r18, 0x42       ; Z
        ldi r19, 0x00
        sec
        sbci r19, 0x00      ; clear-only Z, and a borrow
        ldi r20, 0x0F
        andi r20, 0xF0
        ldi r21, 0x0F
        ori r21, 0xF0
    )" + std::string(kHalt), "immediate arithmetic");
}

TEST(jit_multi_byte_add) {
    // A 32-bit addition built the way a compiler builds one: add then adc three
    // times, with the carry threaded through. If the translator got the carry
    // out of any byte wrong the result diverges by exactly one somewhere.
    differential(R"(
        ldi r16, 0xFF
        ldi r17, 0xFF
        ldi r18, 0xFF
        ldi r19, 0x00
        ldi r20, 0x01
        ldi r21, 0x00
        ldi r22, 0x00
        ldi r23, 0x00
        add r16, r20
        adc r17, r21
        adc r18, r22
        adc r19, r23
    )" + std::string(kHalt), "multi-byte add");
}

TEST(jit_multi_byte_compare) {
    // cp then cpc up the byte order. Z must survive only if every byte matched,
    // which is the one asymmetry in cpc and the easiest thing to translate
    // wrongly, because it clears Z and never sets it.
    differential(R"(
        ldi r16, 0x34
        ldi r17, 0x12
        ldi r18, 0x34
        ldi r19, 0x12
        cp  r16, r18
        cpc r17, r19        ; equal: Z must still be set
        ldi r20, 0x35
        cp  r16, r20
        cpc r17, r19        ; differs in the low byte only: Z must be clear
    )" + std::string(kHalt), "multi-byte compare");
}

TEST(jit_inc_dec_neg_com) {
    differential(R"(
        ldi r16, 0x7F
        inc r16             ; V, N
        ldi r17, 0xFF
        inc r17             ; Z, C untouched
        ldi r18, 0x80
        dec r18             ; V
        ldi r19, 0x01
        dec r19             ; Z
        ldi r20, 0x80
        neg r20             ; V, the one value that cannot be negated
        ldi r21, 0x00
        neg r21             ; Z, C clear
        ldi r22, 0x01
        neg r22             ; C set, H set
        ldi r23, 0x0F
        com r23             ; C always set by com
    )" + std::string(kHalt), "inc dec neg com");
}

TEST(jit_shift_and_rotate_family) {
    differential(R"(
        ldi r16, 0x81
        lsr r16             ; C from bit 0, N forced clear, V follows C
        ldi r17, 0x81
        asr r17             ; sign preserved, C set
        ldi r18, 0x01
        asr r18             ; Z with C set
        clc
        ldi r19, 0x03
        ror r19             ; zero rotated in
        sec
        ldi r20, 0x00
        ror r20             ; carry rotated into bit 7
        ldi r21, 0x55
        lsl r21             ; lsl is add r21, r21
        ldi r22, 0x80
        lsl r22             ; C and Z
        sec
        ldi r23, 0x80
        rol r23             ; rol is adc r23, r23
        ldi r24, 0x12
        swap r24            ; no flags at all
    )" + std::string(kHalt), "shifts and rotates");
}

TEST(jit_logic_and_move) {
    differential(R"(
        ldi r16, 0xF0
        ldi r17, 0x0F
        and r16, r17        ; Z
        ldi r18, 0xF0
        or  r18, r17
        ldi r19, 0xAA
        ldi r20, 0xAA
        eor r19, r20        ; Z
        ldi r21, 0x80
        ldi r22, 0x00
        or  r21, r22        ; N
        mov r23, r21
        ldi r24, 0x34
        ldi r25, 0x12
        movw r30, r24
    )" + std::string(kHalt), "logic and move");
}

TEST(jit_adiw_and_sbiw) {
    differential(R"(
        ldi r24, 0xFF
        ldi r25, 0xFF
        adiw r24, 1         ; wraps: C and Z
        ldi r26, 0x00
        ldi r27, 0x00
        sbiw r26, 1         ; borrows: C and N
        ldi r28, 0x00
        ldi r29, 0x80
        sbiw r28, 1         ; V, sign flips the other way
        ldi r30, 0xFF
        ldi r31, 0x7F
        adiw r30, 1         ; V
    )" + std::string(kHalt), "adiw and sbiw");
}

// ------------------------------------------------------- branching ---

TEST(jit_every_branch_condition_taken_and_not) {
    // Each pair sets the flags one way, branches over an increment, then sets
    // them the other way and falls through, so both directions of each
    // condition are executed in one program.
    differential(R"(
        ldi r16, 0
        ldi r17, 0
        ldi r18, 0
        ldi r19, 0
        ldi r20, 0
        ldi r21, 0
        ldi r22, 0
        ldi r23, 0

        sec
        brcs a1
        inc r16
    a1: clc
        brcs a2
        inc r16
    a2: clc
        brcc a3
        inc r17
    a3: sec
        brcc a4
        inc r17

    a4: ldi r24, 0
        and r24, r24
        breq b1
        inc r18
    b1: ldi r24, 1
        and r24, r24
        breq b2
        inc r18
    b2: ldi r24, 1
        and r24, r24
        brne b3
        inc r19
    b3: ldi r24, 0
        and r24, r24
        brne b4
        inc r19

    b4: ldi r24, 0x80
        and r24, r24
        brmi c1
        inc r20
    c1: ldi r24, 0x01
        and r24, r24
        brmi c2
        inc r20
    c2: ldi r24, 0x01
        and r24, r24
        brpl c3
        inc r21
    c3: ldi r24, 0x80
        and r24, r24
        brpl c4
        inc r21

        ; The assembler spells only the common branch mnemonics, so the two
        ; conditions with no name of their own -- overflow and the T bit -- are
        ; written as the raw brbs/brbc encodings they assemble to. Each one
        ; jumps forward by a single word, which is exactly over the `inc` that
        ; follows it, so a branch taken leaves the register alone.
    c4: ldi r24, 0x7F
        ldi r25, 1
        add r24, r25
        .word 0xF00B        ; brbs 3, .+2  -- branch if V
        inc r22
        ldi r24, 0x01
        ldi r25, 1
        add r24, r25
        .word 0xF00B        ; brbs 3, .+2  -- V is clear, so not taken
        inc r22

        ldi r24, 0x00
        ldi r25, 0x01
        cp  r24, r25
        brlt e1
        inc r23
    e1: ldi r24, 0x01
        ldi r25, 0x00
        cp  r24, r25
        brge e2
        inc r23
    e2: set
        .word 0xF00E        ; brbs 6, .+2  -- branch if T
        inc r16
        clt
        .word 0xF00E        ; brbs 6, .+2  -- T is clear, so not taken
        inc r16
        .word 0xF40E        ; brbc 6, .+2  -- branch if T is clear
        inc r16
        nop
    )" + std::string(kHalt), "branch conditions");
}

TEST(jit_skips_including_over_two_word_instructions) {
    // The instruction being skipped is `lds`, which is two words. A skip that
    // stepped one word would land on the address operand and execute it as an
    // opcode, which is the classic way to get a plausible-looking wrong answer.
    differential(R"(
        ldi r16, 5
        ldi r17, 5
        cpse r16, r17
        lds r18, 0x0300     ; skipped whole, two words
        ldi r19, 1

        ldi r20, 6
        cpse r16, r20
        ldi r21, 0x77       ; not skipped
        ldi r22, 2

        ldi r23, 0x01
        sbrc r23, 0
        lds r24, 0x0301     ; skipped: bit 0 is set, so sbrc does not skip
        sbrs r23, 0
        lds r25, 0x0302     ; skipped whole
        sbrc r23, 1
        ldi r26, 0x33       ; skipped, one word
        ldi r27, 3
    )" + std::string(kHalt), "skips");
}

// ---------------------------------------------------- calls and jumps ---

TEST(jit_call_and_ret) {
    differential(R"(
        ldi r16, 0
        rcall bump
        rcall bump
        call  bump
        rjmp done
    bump:
        inc r16
        ret
    done:
        ldi r17, 0x5A
    )" + std::string(kHalt), "call and ret");
}

TEST(jit_indirect_jump_and_call) {
    differential(R"(
        ldi r30, lo8(target)
        ldi r31, hi8(target)
        icall
        ldi r16, 1
        ldi r30, lo8(after)
        ldi r31, hi8(after)
        ijmp
        ldi r17, 0x99       ; never reached
    after:
        ldi r18, 2
        rjmp end2
    target:
        ldi r19, 3
        ret
    end2:
        nop
    )" + std::string(kHalt), "ijmp and icall");
}

TEST(jit_absolute_jump) {
    differential(R"(
        ldi r16, 1
        jmp far
        ldi r17, 0x99       ; never reached
    far:
        ldi r18, 2
    )" + std::string(kHalt), "jmp");
}

// -------------------------------------------------------- memory ---

TEST(jit_loads_and_stores_every_mode) {
    differential(R"(
        ldi r26, 0x00
        ldi r27, 0x02       ; X = 0x0200
        ldi r28, 0x10
        ldi r29, 0x02       ; Y = 0x0210
        ldi r30, 0x20
        ldi r31, 0x02       ; Z = 0x0220

        ldi r16, 0x11
        st  X, r16
        ldi r16, 0x22
        st  X+, r16
        ldi r16, 0x33
        st  X+, r16
        ldi r16, 0x44
        st  -X, r16

        ldi r17, 0x55
        st  Y+, r17
        ldi r17, 0x66
        st  -Y, r17
        std Y+5, r17

        ldi r18, 0x77
        st  Z+, r18
        st  -Z, r18
        std Z+7, r18

        ld  r19, X
        ld  r20, X+
        ld  r21, -X
        ld  r22, Y+
        ld  r23, -Y
        ldd r24, Y+5
        ld  r25, Z+
        ld  r2,  -Z
        ldd r3,  Z+7

        sts 0x0240, r16
        lds r4, 0x0240
    )" + std::string(kHalt), "loads and stores");
}

TEST(jit_push_and_pop) {
    differential(R"(
        ldi r16, 0xAA
        ldi r17, 0xBB
        ldi r18, 0xCC
        push r16
        push r17
        push r18
        pop r19
        pop r20
        pop r21
    )" + std::string(kHalt), "push and pop");
}

TEST(jit_lpm_reads_flash_bytes) {
    // Z is a BYTE address into flash for lpm, and its low bit picks the half of
    // the word. Both halves are read, so a translator that treated it as a word
    // address diverges immediately.
    differential(R"(
        ldi r30, lo8(table*2)
        ldi r31, hi8(table*2)
        lpm r16, Z
        lpm r17, Z+
        lpm r18, Z+
        lpm r19, Z
        ldi r30, lo8(table*2)
        ldi r31, hi8(table*2)
        lpm                 ; implicit form, into r0
        rjmp over
    table:
        .byte 0xDE, 0xAD, 0xBE, 0xEF
    over:
        nop
    )" + std::string(kHalt), "lpm");
}

TEST(jit_in_and_out_reach_cpu_registers) {
    // SREG, SPL and SPH are answered by the CPU rather than by any peripheral,
    // in both cores, so reading and writing them through in and out checks that
    // the translator's data routing is the reference's routing.
    differential(R"(
        ldi r16, 0x2A
        out 0x3F, r16       ; SREG
        in  r17, 0x3F
        in  r18, 0x3D       ; SPL
        in  r19, 0x3E       ; SPH
        ldi r20, 0x00
        out 0x3F, r20       ; clear SREG again, interrupts included
    )" + std::string(kHalt), "in and out");
}

TEST(jit_bst_and_bld_move_bits_through_t) {
    differential(R"(
        ldi r16, 0x80
        bst r16, 7
        ldi r17, 0x00
        bld r17, 0
        bst r16, 0
        ldi r18, 0xFF
        bld r18, 3
    )" + std::string(kHalt), "bst and bld");
}

// -------------------------------------------------- the block cache ---

TEST(jit_loop_runs_the_same_through_the_block_cache) {
    // Two hundred iterations, so the loop body is translated once and then
    // executed from the cache. A cache that handed back the wrong block, or a
    // block that left a register in a host register at exit, shows up as a
    // divergence in the sum rather than in the count.
    differential(R"(
        ldi r16, 200
        ldi r17, 0
        ldi r18, 0
    loop:
        inc r17
        add r18, r17
        dec r16
        brne loop
    )" + std::string(kHalt), "loop through the cache");
}

TEST(jit_nested_loop_exercises_several_cached_blocks) {
    differential(R"(
        ldi r16, 0
        ldi r20, 12
    outer:
        ldi r21, 20
    inner:
        inc r16
        dec r21
        brne inner
        dec r20
        brne outer
    )" + std::string(kHalt), "nested loops");
}

TEST(jit_block_reached_by_two_different_paths) {
    // `shared` is entered by falling into it and by branching to it. Both must
    // reach the same cached block and leave the same state, which is what
    // proves the cache is keyed by where execution actually resumes.
    differential(R"(
        ldi r16, 0
        ldi r17, 0
        rjmp first
    shared:
        inc r16
        add r17, r16
        ret
    first:
        rcall shared
        ldi r18, 1
        rcall shared
        ldi r19, 2
        rcall shared
    )" + std::string(kHalt), "one block, two paths");
}

TEST(jit_deadline_stops_at_the_same_instruction) {
    // A short deadline lands in the middle of a block. Both cores must stop in
    // front of the same instruction with the same cycle count, which is the
    // rule that keeps peripheral timing right: a translator that ran a whole
    // block past the deadline would be several cycles ahead of the hardware.
    if (!have_translator()) return;

    AssembleResult a = assemble(R"(
        ldi r16, 1
        ldi r17, 2
        add r16, r17
        ldi r18, 3
        add r16, r18
        ldi r19, 4
        add r16, r19
        ldi r20, 5
    )" + std::string(kHalt));
    CHECK(a.ok);

    for (uint64_t deadline = 1; deadline <= 12; ++deadline) {
        Machine ref = build(a.code, false);
        Machine jit = build(a.code, true);
        ref.state.deadline = deadline;
        jit.state.deadline = deadline;
        ref.result = ref.core->run(ref.state);
        jit.result = jit.core->run(jit.state);
        compare(snapshot(ref), snapshot(jit), "deadline in mid-block");
    }
}

TEST(jit_resuming_after_a_deadline_matches) {
    // The same program run in many short spans must end where one long span
    // ends. This is how the board actually drives a core, so a block boundary
    // that lost state would break every real sketch and nothing else here.
    if (!have_translator()) return;

    AssembleResult a = assemble(R"(
        ldi r16, 30
        ldi r17, 0
    loop:
        inc r17
        push r17
        pop r18
        add r17, r18
        dec r16
        brne loop
    )" + std::string(kHalt));
    CHECK(a.ok);

    Machine ref = build(a.code, false);
    Machine jit = build(a.code, true);

    for (int span = 0; span < 400; ++span) {
        ref.state.deadline = ref.state.cycles + 3;
        jit.state.deadline = jit.state.cycles + 3;
        ref.result = ref.core->run(ref.state);
        jit.result = jit.core->run(jit.state);
        if (ref.result.reason == StopReason::Halted) break;
    }
    compare(snapshot(ref), snapshot(jit), "resumed in short spans");
}

// ------------------------------------------------------ invalidation ---

TEST(jit_invalidate_forces_a_retranslation) {
    // The translator caches by word address, so rewriting flash under a cached
    // block and calling invalidate must produce the NEW behaviour. Nothing in
    // the emulator can do this yet -- spm decodes as illegal in both cores --
    // but the cache has to be right before that path opens, because a stale
    // block after a bootloader self-update would run the old program with no
    // symptom other than the wrong answer.
    if (!have_translator()) return;

    AssembleResult first = assemble(R"(
        ldi r16, 0x11
    )" + std::string(kHalt));
    AssembleResult second = assemble(R"(
        ldi r16, 0x22
    )" + std::string(kHalt));
    CHECK(first.ok);
    CHECK(second.ok);

    Machine m = build(first.code, true);
    m.result = m.core->run(m.state);
    CHECK_EQ(int(m.result.reason), int(StopReason::Halted));
    CHECK_EQ(int(m.state.r[16]), 0x11);

    // Rewrite flash the way an spm would, tell the core, and run again from the
    // top with the state the first run left behind.
    for (size_t k = 0; k < second.code.size(); ++k) m.flash[k] = second.code[k];
    m.core->invalidate(0, uint32_t(second.code.size() - 1));

    m.state.pc = 0;
    m.state.deadline = m.state.cycles + 1000;
    m.result = m.core->run(m.state);
    CHECK_EQ(int(m.result.reason), int(StopReason::Halted));
    CHECK_EQ(int(m.state.r[16]), 0x22);
}

TEST(jit_stale_block_without_invalidate_is_the_only_difference) {
    // The complement of the test above: a cached block that was NOT invalidated
    // keeps running the old code. This is asserted so that the previous test
    // cannot pass by accident on a core that never cached anything at all --
    // which would make the invalidation test vacuous.
    if (!have_translator()) return;

    AssembleResult first = assemble(R"(
        ldi r16, 0x11
    )" + std::string(kHalt));
    AssembleResult second = assemble(R"(
        ldi r16, 0x22
    )" + std::string(kHalt));
    CHECK(first.ok);
    CHECK(second.ok);

    Machine m = build(first.code, true);
    m.result = m.core->run(m.state);
    CHECK_EQ(int(m.state.r[16]), 0x11);

    for (size_t k = 0; k < second.code.size(); ++k) m.flash[k] = second.code[k];
    m.state.pc = 0;
    m.state.deadline = m.state.cycles + 1000;
    m.result = m.core->run(m.state);
    CHECK_EQ(int(m.state.r[16]), 0x11);   // the stale block, as expected
}

// -------------------------------------------------------- fallback ---

TEST(jit_unimplemented_instructions_still_produce_the_right_state) {
    // mul, sbi, cbi, sbic, sbis, bset, bclr, sleep and wdr are not translated.
    // Each one ends the block in front of it and is executed by the reference
    // core instead, so the final state -- including the cycle count -- must be
    // exactly what the reference alone produces. If any of them were silently
    // skipped or approximated this is where it shows.
    differential(R"(
        ldi r16, 0x10
        ldi r17, 0x10
        mul r16, r17        ; not translated: r1:r0 and C, Z
        ldi r18, 0xF0
        ldi r19, 0x02
        mul r18, r19        ; not translated
        sei                 ; bset, not translated
        cli                 ; bclr, not translated
        sleep               ; not translated
        wdr                 ; not translated
        ldi r20, 1
        add r20, r20        ; translated again after the gap
    )" + std::string(kHalt), "unimplemented instructions fall back");
}

TEST(jit_fallback_in_the_middle_of_a_loop) {
    // The untranslated instruction is inside the loop body, so every iteration
    // ends a block, steps once on the reference and picks a new block up after
    // it. Cycle counts have to line up across all of that.
    differential(R"(
        ldi r16, 25
        ldi r17, 3
        ldi r20, 0
    loop:
        ldi r18, 5
        mul r17, r18        ; not translated
        add r20, r0
        dec r16
        brne loop
    )" + std::string(kHalt), "fallback inside a loop");
}

TEST(jit_illegal_opcode_reports_identically) {
    if (!have_translator()) return;

    // 0xFFFF is not an instruction. Erased flash is full of it, so this is
    // exactly what a program running off its own end hits, and both cores must
    // name the same word address rather than one of them stopping a block late.
    AssembleResult a = assemble(R"(
        ldi r16, 1
        .word 0xFFFF
    )");
    CHECK(a.ok);

    Machine ref = build(a.code, false);
    Machine jit = build(a.code, true);
    ref.result = ref.core->run(ref.state);
    jit.result = jit.core->run(jit.state);

    CHECK_EQ(int(ref.result.reason), int(StopReason::IllegalOpcode));
    compare(snapshot(ref), snapshot(jit), "illegal opcode");
    CHECK_EQ(int(ref.result.pc), int(jit.result.pc));
    CHECK(ref.result.error == jit.result.error);
}

TEST(jit_halts_on_a_self_loop_at_the_very_first_instruction) {
    // A block that would begin with an untranslatable instruction has nothing
    // in it, and the dispatcher has to single-step rather than loop translating
    // an empty block forever. A program that is nothing but the self-loop is
    // the smallest case of that.
    differential("end: rjmp end\n", "self-loop only");
}

// ------------------------------------------------- with peripherals ---

namespace {

// Four writable bytes, standing in for a port. What matters is not what it does
// but that an access in its range reaches it rather than SRAM, in both cores
// and in the same order.
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

// Raises one interrupt the first time it is asked, which is the smallest thing
// that makes a core vector.
class OneShot : public Peripheral {
public:
    bool claims(uint16_t) const override { return false; }
    uint8_t read(uint16_t) override { return 0; }
    void write(uint16_t, uint8_t) override {}
    void advance(uint64_t) override {}
    uint8_t pending_interrupt() const override { return armed ? vector : 0; }
    void acknowledge_interrupt() override { armed = false; }

    uint8_t vector = 1;
    bool armed = true;
};

} // namespace

TEST(jit_peripheral_accesses_match_the_reference) {
    // With a peripheral attached the translator ends a block after every access
    // to data space, so that a newly pending interrupt is noticed in the same
    // place the reference would notice it. That makes the blocks short and the
    // access order the thing under test: the two cores must touch the latch the
    // same number of times, in the same order, for the same total cycles.
    if (!have_translator()) return;

    AssembleResult a = assemble(R"(
        ldi r16, 0x5A
        sts 0x0080, r16
        lds r17, 0x0080
        ldi r26, 0x81
        ldi r27, 0x00
        ldi r18, 0x3C
        st  X+, r18
        ld  r19, -X
        ldi r20, 0x0F
        out 0x02, r20       ; data-space 0x22, inside the low latch
        in  r21, 0x02
    )" + std::string(kHalt));
    CHECK(a.ok);

    Machine ref = build(a.code, false);
    Machine jit = build(a.code, true);
    Latch ref_latch(0x0080), jit_latch(0x0080);
    Latch ref_io(0x0020), jit_io(0x0020);
    ref.core->attach(&ref_latch);
    ref.core->attach(&ref_io);
    jit.core->attach(&jit_latch);
    jit.core->attach(&jit_io);

    ref.result = ref.core->run(ref.state);
    jit.result = jit.core->run(jit.state);

    compare(snapshot(ref), snapshot(jit), "with a peripheral attached");
    CHECK_EQ(ref_latch.reads, jit_latch.reads);
    CHECK_EQ(ref_latch.writes, jit_latch.writes);
    for (int k = 0; k < 4; ++k) CHECK_EQ(int(ref_latch.regs[k]), int(jit_latch.regs[k]));
}

TEST(jit_interrupt_vectors_at_the_same_instruction) {
    // Vectoring costs four cycles and pushes a return address, and it happens
    // between two instructions -- which is a place a translated block does not
    // normally get to look. If the translator vectored a block late the return
    // address on the stack would be a different one, so this compares SRAM as
    // well as the registers.
    if (!have_translator()) return;

    AssembleResult a = assemble(R"(
        rjmp start          ; reset, vector 0
        nop                 ; on this part each vector slot is two words wide,
        rjmp handler        ; so vector 1 begins here, at word address 2
        nop
    start:
        ldi r16, 0
        sei
        inc r16
        inc r16
        inc r16
        rjmp end
    handler:
        ldi r17, 0x99
        reti
    )" + std::string(kHalt));
    CHECK(a.ok);

    Machine ref = build(a.code, false);
    Machine jit = build(a.code, true);
    OneShot ref_source, jit_source;
    ref.core->attach(&ref_source);
    jit.core->attach(&jit_source);

    ref.result = ref.core->run(ref.state);
    jit.result = jit.core->run(jit.state);

    compare(snapshot(ref), snapshot(jit), "interrupt dispatch");
    CHECK_EQ(int(ref.state.r[17]), 0x99);   // the handler really did run
}

TEST(jit_description_names_the_translator) {
    if (!have_translator()) return;
    std::unique_ptr<Core> core = make_arm64_core(atmega328p());
    CHECK(core->description().find("ARM64") != std::string::npos);
    CHECK(core->description() != make_reference_core(atmega328p())->description());
}

TEST(jit_absent_on_this_host_still_leaves_a_working_core) {
    // make_core must hand back something that runs whether or not this host has
    // a translator, which is the promise that keeps emulation available
    // everywhere.
    std::unique_ptr<Core> core = make_core(atmega328p());
    CHECK(core != nullptr);
    CHECK(!core->description().empty());
}
