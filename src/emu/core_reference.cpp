// The portable reference execution core: a plain AVR interpreter.
//
// This file is the oracle. A native translator generates code for the same
// instruction set, and the two are required to end up in identical state after
// running the same program, so whatever this file says an instruction does IS
// what that instruction does as far as the rest of the emulator is concerned.
// That makes the usual engineering trade here run backwards: there is no
// pressure at all to be fast, and enormous pressure to be checkable by eye. So
// each flag is written out as its own explicit expression, in the same shape
// the instruction set summary states it, even where three instructions could
// obviously share a helper. A shared helper would hide exactly the kind of
// mistake this file exists to rule out -- if add's half-carry and sub's
// half-carry are computed by one function with a flag argument, nobody reading
// it can tell at a glance that both polarities are right.
//
// Two conventions cause most of the wrong answers in AVR emulators, so they are
// stated once here and relied on everywhere below:
//
//   * A word address counts 16-bit instruction words; a byte address counts
//     bytes of flash. The program counter, jump targets, call targets and
//     labels are WORD addresses. The Z pointer used by lpm is a BYTE address.
//     Confusing the two reads the wrong half of the wrong instruction and the
//     resulting bug looks like a data corruption rather than an addressing
//     mistake, which is why lpm is the one place below that multiplies nothing.
//
//   * Four instructions occupy two words: lds, sts, jmp and call. Every skip
//     instruction must step over such an instruction whole. Skipping one word
//     lands the program counter on what is really the second half of an
//     instruction -- an operand -- and executes it as an opcode, which usually
//     does something plausible enough to hide the cause for a long time.
//
// Flags live in State as separate bytes rather than a packed SREG, so SREG is
// assembled and taken apart only where the guest actually looks at it.

#include "ardio/emu/machine.h"

#include <cstdio>
#include <set>
#include <vector>

namespace ardio::emu {
namespace {

// SREG bit positions, in the order the architecture packs them.
constexpr int kBitC = 0, kBitZ = 1, kBitN = 2, kBitV = 3;
constexpr int kBitS = 4, kBitH = 5, kBitT = 6, kBitI = 7;

// Bit b of a byte, as a 0/1 int. Every flag expression below is written in
// terms of this so it reads the way the datasheet writes it.
inline int bit(uint8_t v, int b) { return (v >> b) & 1; }

// The complement of a bit, as 0 or 1 rather than as a bool.
//
// The flag formulas below are transcriptions of the boolean expressions the
// datasheet prints for H, V and C, which are written as products and sums of
// individual bits. Spelling those with ! and & works numerically, since bit()
// only ever yields 0 or 1, but it reads to the compiler like a mistaken bool
// operation and trips -Wlogical-not-parentheses. Naming the complement keeps
// the formulas looking like the datasheet without the noise.
inline int nbit(uint8_t v, int b) { return bit(v, b) ^ 1; }

std::string hex16(unsigned v) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "0x%04X", v & 0xFFFF);
    return buf;
}

class ReferenceCore final : public Core {
public:
    explicit ReferenceCore(const avr::AvrDevice& device) : dev_(device) {}

    RunResult run(State& state) override;

    void invalidate(uint32_t byte_start, uint32_t byte_end) override {
        // Nothing is cached, so self-modifying flash needs no help from us.
        // The method still exists because the translator has to implement it
        // and both cores answer the same interface.
        (void)byte_start;
        (void)byte_end;
    }

    void set_breakpoint(uint16_t word_addr) override { breakpoints_.insert(word_addr); }
    void clear_breakpoint(uint16_t word_addr) override { breakpoints_.erase(word_addr); }

    std::string description() const override {
        return "portable reference interpreter (decodes one instruction at a time; "
               "correct but slow, and the oracle every other core is checked against)";
    }

    void attach(Peripheral* p) {
        if (p) peripherals_.push_back(p);
    }

private:
    // ---- memory -------------------------------------------------------

    // Data space, which on an AVR is one flat address space with the register
    // file at the bottom, then I/O, then SRAM. Anything a peripheral claims is
    // routed to it, because a read there can have side effects.
    uint8_t read_data(State& s, uint16_t addr);
    void write_data(State& s, uint16_t addr, uint8_t value);

    uint8_t read_io(State& s, uint8_t io) { return read_data(s, uint16_t(io) + 0x20); }
    void write_io(State& s, uint8_t io, uint8_t v) { write_data(s, uint16_t(io) + 0x20, v); }

    uint8_t read_flash_byte(const State& s, uint32_t byte_addr) const {
        if (!s.flash || dev_.flash_size == 0) return 0xFF;
        return s.flash[byte_addr % dev_.flash_size];
    }

    uint16_t fetch(const State& s, uint16_t word_addr) const {
        uint32_t b = uint32_t(word_addr) * 2;
        return uint16_t(read_flash_byte(s, b) | (uint16_t(read_flash_byte(s, b + 1)) << 8));
    }

    // ---- stack --------------------------------------------------------
    //
    // The stack grows down and the pointer names the next FREE byte, so a push
    // writes at SP and then decrements. A return address is pushed low byte
    // first, which leaves its high byte at the lower address -- the order ret
    // reads them back in, and the order the runtime's own stack walking
    // assumes.
    void push_byte(State& s, uint8_t v) {
        write_data(s, s.sp, v);
        s.sp = uint16_t(s.sp - 1);
    }
    uint8_t pop_byte(State& s) {
        s.sp = uint16_t(s.sp + 1);
        return read_data(s, s.sp);
    }
    void push_pc(State& s, uint16_t word_addr) {
        push_byte(s, uint8_t(word_addr & 0xFF));
        push_byte(s, uint8_t(word_addr >> 8));
    }
    uint16_t pop_pc(State& s) {
        uint16_t hi = pop_byte(s);
        uint16_t lo = pop_byte(s);
        return uint16_t((hi << 8) | lo);
    }

    // ---- SREG ---------------------------------------------------------
    uint8_t sreg(const State& s) const {
        return uint8_t((s.flag_c ? 1 << kBitC : 0) | (s.flag_z ? 1 << kBitZ : 0) |
                       (s.flag_n ? 1 << kBitN : 0) | (s.flag_v ? 1 << kBitV : 0) |
                       (s.flag_s ? 1 << kBitS : 0) | (s.flag_h ? 1 << kBitH : 0) |
                       (s.flag_t ? 1 << kBitT : 0) | (s.flag_i ? 1 << kBitI : 0));
    }
    void set_sreg(State& s, uint8_t v) const {
        s.flag_c = uint8_t(bit(v, kBitC));
        s.flag_z = uint8_t(bit(v, kBitZ));
        s.flag_n = uint8_t(bit(v, kBitN));
        s.flag_v = uint8_t(bit(v, kBitV));
        s.flag_s = uint8_t(bit(v, kBitS));
        s.flag_h = uint8_t(bit(v, kBitH));
        s.flag_t = uint8_t(bit(v, kBitT));
        s.flag_i = uint8_t(bit(v, kBitI));
    }
    uint8_t sreg_bit(const State& s, int b) const { return uint8_t(bit(sreg(s), b)); }
    void set_sreg_bit(State& s, int b, int value) const {
        uint8_t v = sreg(s);
        v = uint8_t(value ? (v | (1 << b)) : (v & ~(1 << b)));
        set_sreg(s, v);
    }

    // ---- decoding helpers ---------------------------------------------

    // True if the instruction word starts one of the four two-word
    // instructions. Skips consult this; nothing else may assume a size.
    static bool is_two_word(uint16_t op) {
        if ((op & 0xFC0F) == 0x9000) return true;   // lds and sts
        if ((op & 0xFE0E) == 0x940C) return true;   // jmp and call
        return false;
    }

    // A self-targeting jump with interrupts off can never do anything again,
    // so the machine is finished rather than merely idle. Reporting that is
    // what lets `ardio emulate` exit instead of burning a core forever on a
    // sketch that trapped itself.
    static bool is_self_loop(uint16_t op, uint16_t pc_of_op, uint16_t second_word) {
        if (op == 0xCFFF) return true;              // rjmp .-2, i.e. to itself
        if ((op & 0xFE0E) == 0x940C && (op & 0x0002) == 0) {
            // jmp with the target in the second word. The high bits of a
            // 22-bit target are zero on every part ardio describes, so the
            // second word alone is the target.
            return second_word == pc_of_op;
        }
        return false;
    }

    // Pointer register pairs. X is r27:r26, Y is r29:r28, Z is r31:r30.
    static uint16_t read_pair(const State& s, int low) {
        return uint16_t(s.r[low] | (uint16_t(s.r[low + 1]) << 8));
    }
    static void write_pair(State& s, int low, uint16_t v) {
        s.r[low] = uint8_t(v & 0xFF);
        s.r[low + 1] = uint8_t(v >> 8);
    }

    // Executes one instruction. Returns true to keep going; on false, `out`
    // has been filled in with why we stopped.
    bool step(State& s, RunResult& out);

    // Pushes the return address and vectors, if a peripheral is asking and
    // the guest is willing to listen. Returns true if a vector was taken.
    bool dispatch_interrupt(State& s);

    // Held by value: a core outlives the call that built it, and the device
    // description a caller passes in may not.
    avr::AvrDevice dev_;
    std::vector<Peripheral*> peripherals_;
    std::set<uint16_t> breakpoints_;
};

// ------------------------------------------------------------- memory ---

uint8_t ReferenceCore::read_data(State& s, uint16_t addr) {
    // The register file is genuinely mapped into data space at 0, so a program
    // may reach r0-r31 through lds or a pointer. It is not a peripheral and
    // cannot be claimed by one.
    if (addr < 32) return s.r[addr];

    // The three registers whose I/O addresses are identical on every AVR8 part
    // are CPU state, not peripheral state, so they are answered here before
    // anything gets a chance to claim them.
    if (addr == 0x5F) return sreg(s);
    if (addr == 0x5D) return uint8_t(s.sp & 0xFF);
    if (addr == 0x5E) return uint8_t(s.sp >> 8);

    for (Peripheral* p : peripherals_)
        if (p->claims(addr)) return p->read(addr);

    if (s.sram && addr >= dev_.ram_start && addr <= dev_.ramend)
        return s.sram[addr - dev_.ram_start];

    // An address that is neither SRAM nor claimed is an I/O register this
    // machine does not model. Real silicon reads back zero from a reserved
    // location, and more to the point a defined answer keeps the reference and
    // the translator in agreement -- an emulator that returns whatever was on
    // the bus cannot be an oracle.
    return 0;
}

void ReferenceCore::write_data(State& s, uint16_t addr, uint8_t value) {
    if (addr < 32) { s.r[addr] = value; return; }

    if (addr == 0x5F) { set_sreg(s, value); return; }
    if (addr == 0x5D) { s.sp = uint16_t((s.sp & 0xFF00) | value); return; }
    if (addr == 0x5E) { s.sp = uint16_t((s.sp & 0x00FF) | (uint16_t(value) << 8)); return; }

    for (Peripheral* p : peripherals_)
        if (p->claims(addr)) { p->write(addr, value); return; }

    if (s.sram && addr >= dev_.ram_start && addr <= dev_.ramend) {
        s.sram[addr - dev_.ram_start] = value;
        return;
    }

    // Writing an unmodelled register is discarded rather than treated as an
    // error: a sketch configuring a peripheral the virtual board does not have
    // should still run, it just will not see anything happen.
}

// --------------------------------------------------------- interrupts ---

bool ReferenceCore::dispatch_interrupt(State& s) {
    if (!s.flag_i) return false;

    for (Peripheral* p : peripherals_) {
        uint8_t vector = p->pending_interrupt();
        if (vector == 0) continue;   // 0 is reset, which is never requested

        p->acknowledge_interrupt();

        // Entering an interrupt disables further interrupts until reti, which
        // is what stops a handler from being re-entered by its own source.
        s.flag_i = 0;
        push_pc(s, s.pc);

        // A vector NUMBER is not a word address. On the parts ardio describes
        // the table has one jmp per vector, so each slot is two words wide and
        // the entry point for vector n is at word address 2n. Parts small
        // enough to reach every handler with rjmp use one-word slots instead,
        // and the device description does not carry the distinction, so it is
        // inferred from flash size here.
        unsigned slot_words = dev_.flash_size > 8192 ? 2u : 1u;
        s.pc = uint16_t(vector * slot_words);

        // Four cycles: the vector jump plus the two-byte return address push.
        s.cycles += 4;
        return true;
    }
    return false;
}

// -------------------------------------------------------------- run ---

RunResult ReferenceCore::run(State& state) {
    RunResult out;
    bool first = true;

    for (;;) {
        if (state.cycles >= state.deadline) {
            out.reason = StopReason::Deadline;
            out.pc = state.pc;
            return out;
        }

        // A breakpoint is reported before the instruction at it runs, but not
        // on the very first instruction of a run -- otherwise resuming from a
        // breakpoint would stop on it again and never make progress.
        if (!first && breakpoints_.count(state.pc)) {
            out.reason = StopReason::Breakpoint;
            out.pc = state.pc;
            return out;
        }
        first = false;

        dispatch_interrupt(state);

        if (!step(state, out)) return out;
    }
}

// ------------------------------------------------------------- step ---

bool ReferenceCore::step(State& s, RunResult& out) {
    const uint16_t pc_of_op = s.pc;
    const uint16_t op = fetch(s, pc_of_op);
    const uint16_t second = fetch(s, uint16_t(pc_of_op + 1));

    if (!s.flag_i && is_self_loop(op, pc_of_op, second)) {
        // Charge the jump anyway so the cycle count reflects that it ran, then
        // report the machine as finished with the program counter still on the
        // loop, which is where a user would want to see it.
        s.cycles += (op == 0xCFFF) ? 2 : 3;
        out.reason = StopReason::Halted;
        out.pc = pc_of_op;
        return false;
    }

    s.pc = uint16_t(pc_of_op + 1);

    // Field extractions shared by whole families of encodings. Each is named
    // for the letter the instruction set summary uses.
    const int d5 = (op >> 4) & 0x1F;                       // Rd, any register
    const int r5 = ((op & 0x0200) >> 5) | (op & 0x0F);     // Rr, any register
    const int d4 = 16 + ((op >> 4) & 0x0F);                // Rd, r16-r31
    const uint8_t k8 = uint8_t(((op & 0x0F00) >> 4) | (op & 0x000F));
    const int b3 = op & 0x07;                              // bit number

    auto illegal = [&](void) {
        out.reason = StopReason::IllegalOpcode;
        out.pc = pc_of_op;
        out.error = "unknown opcode " + hex16(op) + " at word address " + hex16(pc_of_op);
        s.pc = pc_of_op;
        return false;
    };

    // Performs a skip: steps over the next instruction, whole, and charges the
    // extra cycle a two-word instruction costs to step over.
    auto do_skip = [&](void) {
        uint16_t next = fetch(s, s.pc);
        if (is_two_word(next)) { s.pc = uint16_t(s.pc + 2); s.cycles += 2; }
        else { s.pc = uint16_t(s.pc + 1); s.cycles += 1; }
    };

    switch (op >> 12) {

    // ---------------------------------------------------- 0000 ---------
    case 0x0: {
        if (op == 0x0000) {                       // nop
            s.cycles += 1;
            return true;
        }
        if ((op & 0xFF00) == 0x0100) {            // movw Rd+1:Rd, Rr+1:Rr
            int dd = ((op >> 4) & 0x0F) * 2;
            int rr = (op & 0x0F) * 2;
            s.r[dd] = s.r[rr];
            s.r[dd + 1] = s.r[rr + 1];
            s.cycles += 1;
            return true;
        }
        if ((op & 0xFF00) == 0x0200) {            // muls Rd, Rr  (both r16-r31)
            int dd = 16 + ((op >> 4) & 0x0F);
            int rr = 16 + (op & 0x0F);
            int16_t product = int16_t(int8_t(s.r[dd]) * int8_t(s.r[rr]));
            uint16_t res = uint16_t(product);
            s.r[0] = uint8_t(res & 0xFF);
            s.r[1] = uint8_t(res >> 8);
            s.flag_c = uint8_t(bit(uint8_t(res >> 8), 7));
            s.flag_z = uint8_t(res == 0);
            s.cycles += 2;
            return true;
        }
        if ((op & 0xFF00) == 0x0300) {            // mulsu / fmul / fmuls / fmulsu
            int dd = 16 + ((op >> 4) & 0x07);
            int rr = 16 + (op & 0x07);
            bool d_high = (op & 0x0080) != 0;
            bool r_high = (op & 0x0008) != 0;

            if (!d_high && !r_high) {             // mulsu: signed Rd, unsigned Rr
                int16_t product = int16_t(int8_t(s.r[dd]) * int16_t(s.r[rr]));
                uint16_t res = uint16_t(product);
                s.r[0] = uint8_t(res & 0xFF);
                s.r[1] = uint8_t(res >> 8);
                s.flag_c = uint8_t(bit(uint8_t(res >> 8), 7));
                s.flag_z = uint8_t(res == 0);
                s.cycles += 2;
                return true;
            }

            // The fractional forms multiply 1.7 fixed-point values, so the
            // product is shifted left by one to put the binary point back where
            // it belongs. The bit shifted out of the top is the carry, which is
            // why C is taken from the UNSHIFTED product's bit 15.
            uint16_t product;
            if (!d_high && r_high)                // fmul: both unsigned
                product = uint16_t(unsigned(s.r[dd]) * unsigned(s.r[rr]));
            else if (d_high && !r_high)           // fmuls: both signed
                product = uint16_t(int16_t(int8_t(s.r[dd]) * int8_t(s.r[rr])));
            else                                  // fmulsu: signed times unsigned
                product = uint16_t(int16_t(int8_t(s.r[dd]) * int16_t(s.r[rr])));

            s.flag_c = uint8_t(bit(uint8_t(product >> 8), 7));
            uint16_t res = uint16_t(product << 1);
            s.r[0] = uint8_t(res & 0xFF);
            s.r[1] = uint8_t(res >> 8);
            s.flag_z = uint8_t(res == 0);
            s.cycles += 2;
            return true;
        }
        if ((op & 0xFC00) == 0x0400) {            // cpc Rd, Rr
            uint8_t a = s.r[d5], b = s.r[r5], c = s.flag_c;
            uint8_t res = uint8_t(a - b - c);
            s.flag_h = uint8_t((nbit(a, 3) & bit(b, 3)) | (bit(b, 3) & bit(res, 3)) |
                               (bit(res, 3) & nbit(a, 3)));
            s.flag_v = uint8_t((bit(a, 7) & nbit(b, 7) & nbit(res, 7)) |
                               (nbit(a, 7) & bit(b, 7) & bit(res, 7)));
            s.flag_n = uint8_t(bit(res, 7));
            s.flag_c = uint8_t((nbit(a, 7) & bit(b, 7)) | (bit(b, 7) & bit(res, 7)) |
                               (bit(res, 7) & nbit(a, 7)));
            // Z is only ever cleared here, never set. That single asymmetry is
            // the whole point of cpc: a multi-byte compare walks from the low
            // byte up, and Z must end up set only if EVERY byte compared equal.
            if (res != 0) s.flag_z = 0;
            s.flag_s = uint8_t(s.flag_n ^ s.flag_v);
            s.cycles += 1;
            return true;
        }
        if ((op & 0xFC00) == 0x0800) {            // sbc Rd, Rr
            uint8_t a = s.r[d5], b = s.r[r5], c = s.flag_c;
            uint8_t res = uint8_t(a - b - c);
            s.flag_h = uint8_t((nbit(a, 3) & bit(b, 3)) | (bit(b, 3) & bit(res, 3)) |
                               (bit(res, 3) & nbit(a, 3)));
            s.flag_v = uint8_t((bit(a, 7) & nbit(b, 7) & nbit(res, 7)) |
                               (nbit(a, 7) & bit(b, 7) & bit(res, 7)));
            s.flag_n = uint8_t(bit(res, 7));
            s.flag_c = uint8_t((nbit(a, 7) & bit(b, 7)) | (bit(b, 7) & bit(res, 7)) |
                               (bit(res, 7) & nbit(a, 7)));
            if (res != 0) s.flag_z = 0;   // clear-only, exactly as for cpc
            s.flag_s = uint8_t(s.flag_n ^ s.flag_v);
            s.r[d5] = res;
            s.cycles += 1;
            return true;
        }
        if ((op & 0xFC00) == 0x0C00) {            // add Rd, Rr  (also lsl Rd)
            uint8_t a = s.r[d5], b = s.r[r5];
            uint8_t res = uint8_t(a + b);
            s.flag_h = uint8_t((bit(a, 3) & bit(b, 3)) | (bit(b, 3) & nbit(res, 3)) |
                               (nbit(res, 3) & bit(a, 3)));
            s.flag_v = uint8_t((bit(a, 7) & bit(b, 7) & nbit(res, 7)) |
                               (nbit(a, 7) & nbit(b, 7) & bit(res, 7)));
            s.flag_n = uint8_t(bit(res, 7));
            s.flag_z = uint8_t(res == 0);
            s.flag_c = uint8_t((bit(a, 7) & bit(b, 7)) | (bit(b, 7) & nbit(res, 7)) |
                               (nbit(res, 7) & bit(a, 7)));
            s.flag_s = uint8_t(s.flag_n ^ s.flag_v);
            s.r[d5] = res;
            s.cycles += 1;
            return true;
        }
        return illegal();
    }

    // ---------------------------------------------------- 0001 ---------
    case 0x1: {
        if ((op & 0xFC00) == 0x1000) {            // cpse Rd, Rr
            s.cycles += 1;
            if (s.r[d5] == s.r[r5]) do_skip();
            return true;
        }
        if ((op & 0xFC00) == 0x1400) {            // cp Rd, Rr
            uint8_t a = s.r[d5], b = s.r[r5];
            uint8_t res = uint8_t(a - b);
            s.flag_h = uint8_t((nbit(a, 3) & bit(b, 3)) | (bit(b, 3) & bit(res, 3)) |
                               (bit(res, 3) & nbit(a, 3)));
            s.flag_v = uint8_t((bit(a, 7) & nbit(b, 7) & nbit(res, 7)) |
                               (nbit(a, 7) & bit(b, 7) & bit(res, 7)));
            s.flag_n = uint8_t(bit(res, 7));
            s.flag_z = uint8_t(res == 0);
            s.flag_c = uint8_t((nbit(a, 7) & bit(b, 7)) | (bit(b, 7) & bit(res, 7)) |
                               (bit(res, 7) & nbit(a, 7)));
            s.flag_s = uint8_t(s.flag_n ^ s.flag_v);
            s.cycles += 1;
            return true;
        }
        if ((op & 0xFC00) == 0x1800) {            // sub Rd, Rr
            uint8_t a = s.r[d5], b = s.r[r5];
            uint8_t res = uint8_t(a - b);
            s.flag_h = uint8_t((nbit(a, 3) & bit(b, 3)) | (bit(b, 3) & bit(res, 3)) |
                               (bit(res, 3) & nbit(a, 3)));
            s.flag_v = uint8_t((bit(a, 7) & nbit(b, 7) & nbit(res, 7)) |
                               (nbit(a, 7) & bit(b, 7) & bit(res, 7)));
            s.flag_n = uint8_t(bit(res, 7));
            s.flag_z = uint8_t(res == 0);   // sub sets Z normally, unlike sbc
            s.flag_c = uint8_t((nbit(a, 7) & bit(b, 7)) | (bit(b, 7) & bit(res, 7)) |
                               (bit(res, 7) & nbit(a, 7)));
            s.flag_s = uint8_t(s.flag_n ^ s.flag_v);
            s.r[d5] = res;
            s.cycles += 1;
            return true;
        }
        if ((op & 0xFC00) == 0x1C00) {            // adc Rd, Rr  (also rol Rd)
            uint8_t a = s.r[d5], b = s.r[r5], c = s.flag_c;
            uint8_t res = uint8_t(a + b + c);
            s.flag_h = uint8_t((bit(a, 3) & bit(b, 3)) | (bit(b, 3) & nbit(res, 3)) |
                               (nbit(res, 3) & bit(a, 3)));
            s.flag_v = uint8_t((bit(a, 7) & bit(b, 7) & nbit(res, 7)) |
                               (nbit(a, 7) & nbit(b, 7) & bit(res, 7)));
            s.flag_n = uint8_t(bit(res, 7));
            s.flag_z = uint8_t(res == 0);   // adc sets Z normally; only the
                                            // subtract-with-borrow forms do not
            s.flag_c = uint8_t((bit(a, 7) & bit(b, 7)) | (bit(b, 7) & nbit(res, 7)) |
                               (nbit(res, 7) & bit(a, 7)));
            s.flag_s = uint8_t(s.flag_n ^ s.flag_v);
            s.r[d5] = res;
            s.cycles += 1;
            return true;
        }
        return illegal();
    }

    // ---------------------------------------------------- 0010 ---------
    case 0x2: {
        if ((op & 0xFC00) == 0x2000) {            // and Rd, Rr  (also tst Rd)
            uint8_t res = uint8_t(s.r[d5] & s.r[r5]);
            s.flag_v = 0;
            s.flag_n = uint8_t(bit(res, 7));
            s.flag_z = uint8_t(res == 0);
            s.flag_s = uint8_t(s.flag_n ^ s.flag_v);
            s.r[d5] = res;
            s.cycles += 1;
            return true;
        }
        if ((op & 0xFC00) == 0x2400) {            // eor Rd, Rr  (also clr Rd)
            uint8_t res = uint8_t(s.r[d5] ^ s.r[r5]);
            s.flag_v = 0;
            s.flag_n = uint8_t(bit(res, 7));
            s.flag_z = uint8_t(res == 0);
            s.flag_s = uint8_t(s.flag_n ^ s.flag_v);
            s.r[d5] = res;
            s.cycles += 1;
            return true;
        }
        if ((op & 0xFC00) == 0x2800) {            // or Rd, Rr
            uint8_t res = uint8_t(s.r[d5] | s.r[r5]);
            s.flag_v = 0;
            s.flag_n = uint8_t(bit(res, 7));
            s.flag_z = uint8_t(res == 0);
            s.flag_s = uint8_t(s.flag_n ^ s.flag_v);
            s.r[d5] = res;
            s.cycles += 1;
            return true;
        }
        if ((op & 0xFC00) == 0x2C00) {            // mov Rd, Rr
            s.r[d5] = s.r[r5];
            s.cycles += 1;
            return true;
        }
        return illegal();
    }

    // ---------------------------------------------------- 0011 ---------
    case 0x3: {                                   // cpi Rd, K
        uint8_t a = s.r[d4], b = k8;
        uint8_t res = uint8_t(a - b);
        s.flag_h = uint8_t((nbit(a, 3) & bit(b, 3)) | (bit(b, 3) & bit(res, 3)) |
                           (bit(res, 3) & nbit(a, 3)));
        s.flag_v = uint8_t((bit(a, 7) & nbit(b, 7) & nbit(res, 7)) |
                           (nbit(a, 7) & bit(b, 7) & bit(res, 7)));
        s.flag_n = uint8_t(bit(res, 7));
        s.flag_z = uint8_t(res == 0);
        s.flag_c = uint8_t((nbit(a, 7) & bit(b, 7)) | (bit(b, 7) & bit(res, 7)) |
                           (bit(res, 7) & nbit(a, 7)));
        s.flag_s = uint8_t(s.flag_n ^ s.flag_v);
        s.cycles += 1;
        return true;
    }

    // ---------------------------------------------------- 0100 ---------
    case 0x4: {                                   // sbci Rd, K
        uint8_t a = s.r[d4], b = k8, c = s.flag_c;
        uint8_t res = uint8_t(a - b - c);
        s.flag_h = uint8_t((nbit(a, 3) & bit(b, 3)) | (bit(b, 3) & bit(res, 3)) |
                           (bit(res, 3) & nbit(a, 3)));
        s.flag_v = uint8_t((bit(a, 7) & nbit(b, 7) & nbit(res, 7)) |
                           (nbit(a, 7) & bit(b, 7) & bit(res, 7)));
        s.flag_n = uint8_t(bit(res, 7));
        s.flag_c = uint8_t((nbit(a, 7) & bit(b, 7)) | (bit(b, 7) & bit(res, 7)) |
                           (bit(res, 7) & nbit(a, 7)));
        if (res != 0) s.flag_z = 0;   // clear-only, so a multi-byte compare works
        s.flag_s = uint8_t(s.flag_n ^ s.flag_v);
        s.r[d4] = res;
        s.cycles += 1;
        return true;
    }

    // ---------------------------------------------------- 0101 ---------
    case 0x5: {                                   // subi Rd, K
        uint8_t a = s.r[d4], b = k8;
        uint8_t res = uint8_t(a - b);
        s.flag_h = uint8_t((nbit(a, 3) & bit(b, 3)) | (bit(b, 3) & bit(res, 3)) |
                           (bit(res, 3) & nbit(a, 3)));
        s.flag_v = uint8_t((bit(a, 7) & nbit(b, 7) & nbit(res, 7)) |
                           (nbit(a, 7) & bit(b, 7) & bit(res, 7)));
        s.flag_n = uint8_t(bit(res, 7));
        s.flag_z = uint8_t(res == 0);   // subi sets Z normally, unlike sbci
        s.flag_c = uint8_t((nbit(a, 7) & bit(b, 7)) | (bit(b, 7) & bit(res, 7)) |
                           (bit(res, 7) & nbit(a, 7)));
        s.flag_s = uint8_t(s.flag_n ^ s.flag_v);
        s.r[d4] = res;
        s.cycles += 1;
        return true;
    }

    // ---------------------------------------------------- 0110 ---------
    case 0x6: {                                   // ori Rd, K  (also sbr)
        uint8_t res = uint8_t(s.r[d4] | k8);
        s.flag_v = 0;
        s.flag_n = uint8_t(bit(res, 7));
        s.flag_z = uint8_t(res == 0);
        s.flag_s = uint8_t(s.flag_n ^ s.flag_v);
        s.r[d4] = res;
        s.cycles += 1;
        return true;
    }

    // ---------------------------------------------------- 0111 ---------
    case 0x7: {                                   // andi Rd, K  (also cbr)
        uint8_t res = uint8_t(s.r[d4] & k8);
        s.flag_v = 0;
        s.flag_n = uint8_t(bit(res, 7));
        s.flag_z = uint8_t(res == 0);
        s.flag_s = uint8_t(s.flag_n ^ s.flag_v);
        s.r[d4] = res;
        s.cycles += 1;
        return true;
    }

    // ------------------------------------------- 1000 and 1010 ---------
    //
    // ldd/std with a six-bit displacement, which also covers plain `ld Rd, Y`
    // and `ld Rd, Z` -- those are simply the displacement form with q of zero.
    // The displacement bits are scattered across the word, which is why they
    // are reassembled by hand here.
    case 0x8:
    case 0xA: {
        int q = ((op & 0x2000) >> 8) | ((op & 0x0C00) >> 7) | (op & 0x0007);
        int base_low = (op & 0x0008) ? 28 : 30;   // Y is r29:r28, Z is r31:r30
        uint16_t addr = uint16_t(read_pair(s, base_low) + q);
        if (op & 0x0200) write_data(s, addr, s.r[d5]);   // std
        else s.r[d5] = read_data(s, addr);               // ldd
        s.cycles += 2;
        return true;
    }

    // ---------------------------------------------------- 1001 ---------
    case 0x9: {
        // lds and sts: the address is the whole second word, and the
        // instruction is two words long, so the program counter steps again.
        if ((op & 0xFE0F) == 0x9000) {            // lds Rd, k
            s.pc = uint16_t(s.pc + 1);
            s.r[d5] = read_data(s, second);
            s.cycles += 2;
            return true;
        }
        if ((op & 0xFE0F) == 0x9200) {            // sts k, Rr
            s.pc = uint16_t(s.pc + 1);
            write_data(s, second, s.r[d5]);
            s.cycles += 2;
            return true;
        }

        if ((op & 0xFE00) == 0x9000) {            // the ld/lpm/pop group
            switch (op & 0x000F) {
            case 0x1: {                           // ld Rd, Z+
                uint16_t z = read_pair(s, 30);
                s.r[d5] = read_data(s, z);
                write_pair(s, 30, uint16_t(z + 1));
                s.cycles += 2;
                return true;
            }
            case 0x2: {                           // ld Rd, -Z
                uint16_t z = uint16_t(read_pair(s, 30) - 1);
                write_pair(s, 30, z);
                s.r[d5] = read_data(s, z);
                s.cycles += 2;
                return true;
            }
            case 0x4: {                           // lpm Rd, Z
                // Z is a BYTE address into flash here, not a word address. Its
                // low bit selects which half of a word is read, which is how
                // the runtime's byte tables in flash are indexed at all.
                s.r[d5] = read_flash_byte(s, read_pair(s, 30));
                s.cycles += 3;
                return true;
            }
            case 0x5: {                           // lpm Rd, Z+
                uint16_t z = read_pair(s, 30);
                s.r[d5] = read_flash_byte(s, z);
                write_pair(s, 30, uint16_t(z + 1));
                s.cycles += 3;
                return true;
            }
            case 0x9: {                           // ld Rd, Y+
                uint16_t y = read_pair(s, 28);
                s.r[d5] = read_data(s, y);
                write_pair(s, 28, uint16_t(y + 1));
                s.cycles += 2;
                return true;
            }
            case 0xA: {                           // ld Rd, -Y
                uint16_t y = uint16_t(read_pair(s, 28) - 1);
                write_pair(s, 28, y);
                s.r[d5] = read_data(s, y);
                s.cycles += 2;
                return true;
            }
            case 0xC: {                           // ld Rd, X
                s.r[d5] = read_data(s, read_pair(s, 26));
                s.cycles += 2;
                return true;
            }
            case 0xD: {                           // ld Rd, X+
                uint16_t x = read_pair(s, 26);
                s.r[d5] = read_data(s, x);
                write_pair(s, 26, uint16_t(x + 1));
                s.cycles += 2;
                return true;
            }
            case 0xE: {                           // ld Rd, -X
                uint16_t x = uint16_t(read_pair(s, 26) - 1);
                write_pair(s, 26, x);
                s.r[d5] = read_data(s, x);
                s.cycles += 2;
                return true;
            }
            case 0xF: {                           // pop Rd
                s.r[d5] = pop_byte(s);
                s.cycles += 2;
                return true;
            }
            default:
                return illegal();
            }
        }

        if ((op & 0xFE00) == 0x9200) {            // the st/push group
            switch (op & 0x000F) {
            case 0x1: {                           // st Z+, Rr
                uint16_t z = read_pair(s, 30);
                write_data(s, z, s.r[d5]);
                write_pair(s, 30, uint16_t(z + 1));
                s.cycles += 2;
                return true;
            }
            case 0x2: {                           // st -Z, Rr
                uint16_t z = uint16_t(read_pair(s, 30) - 1);
                write_pair(s, 30, z);
                write_data(s, z, s.r[d5]);
                s.cycles += 2;
                return true;
            }
            case 0x9: {                           // st Y+, Rr
                uint16_t y = read_pair(s, 28);
                write_data(s, y, s.r[d5]);
                write_pair(s, 28, uint16_t(y + 1));
                s.cycles += 2;
                return true;
            }
            case 0xA: {                           // st -Y, Rr
                uint16_t y = uint16_t(read_pair(s, 28) - 1);
                write_pair(s, 28, y);
                write_data(s, y, s.r[d5]);
                s.cycles += 2;
                return true;
            }
            case 0xC: {                           // st X, Rr
                write_data(s, read_pair(s, 26), s.r[d5]);
                s.cycles += 2;
                return true;
            }
            case 0xD: {                           // st X+, Rr
                uint16_t x = read_pair(s, 26);
                write_data(s, x, s.r[d5]);
                write_pair(s, 26, uint16_t(x + 1));
                s.cycles += 2;
                return true;
            }
            case 0xE: {                           // st -X, Rr
                uint16_t x = uint16_t(read_pair(s, 26) - 1);
                write_pair(s, 26, x);
                write_data(s, x, s.r[d5]);
                s.cycles += 2;
                return true;
            }
            case 0xF: {                           // push Rr
                push_byte(s, s.r[d5]);
                s.cycles += 2;
                return true;
            }
            default:
                return illegal();
            }
        }

        // ---- 1001 010x: single-register operations and the control group ---
        if ((op & 0xFE00) == 0x9400) {
            // jmp and call carry a 22-bit target, but no part ardio describes
            // has more than 16 bits of word address, so the target is the
            // second word. Note again that this is a WORD address: the low bit
            // of a byte address would be lost here and the jump would land on
            // the wrong instruction half the time.
            if ((op & 0xFE0E) == 0x940C) {        // jmp k
                s.pc = second;
                s.cycles += 3;
                return true;
            }
            if ((op & 0xFE0E) == 0x940E) {        // call k
                push_pc(s, uint16_t(s.pc + 1));   // return past the second word
                s.pc = second;
                s.cycles += 4;
                return true;
            }

            switch (op & 0x000F) {
            case 0x0: {                           // com Rd
                uint8_t res = uint8_t(~s.r[d5]);
                s.flag_c = 1;                     // com always sets C, so that
                                                  // a following neg or sbc sees
                                                  // the borrow a one's-complement
                                                  // negation needs
                s.flag_v = 0;
                s.flag_n = uint8_t(bit(res, 7));
                s.flag_z = uint8_t(res == 0);
                s.flag_s = uint8_t(s.flag_n ^ s.flag_v);
                s.r[d5] = res;
                s.cycles += 1;
                return true;
            }
            case 0x1: {                           // neg Rd
                uint8_t a = s.r[d5];
                uint8_t res = uint8_t(0 - a);
                // These are the subtract rules with a minuend of zero, worked
                // through: the borrow out is set whenever the result is not
                // zero, and the only value that overflows is 0x80, whose
                // negation cannot be represented in eight signed bits.
                s.flag_h = uint8_t(bit(res, 3) | bit(a, 3));
                s.flag_v = uint8_t(res == 0x80);
                s.flag_n = uint8_t(bit(res, 7));
                s.flag_z = uint8_t(res == 0);
                s.flag_c = uint8_t(res != 0);
                s.flag_s = uint8_t(s.flag_n ^ s.flag_v);
                s.r[d5] = res;
                s.cycles += 1;
                return true;
            }
            case 0x2: {                           // swap Rd -- no flags at all
                uint8_t a = s.r[d5];
                s.r[d5] = uint8_t((a >> 4) | (a << 4));
                s.cycles += 1;
                return true;
            }
            case 0x3: {                           // inc Rd
                uint8_t a = s.r[d5];
                uint8_t res = uint8_t(a + 1);
                // C is deliberately untouched, which is what lets inc be used
                // inside a multi-byte add loop without destroying the carry.
                s.flag_v = uint8_t(a == 0x7F);
                s.flag_n = uint8_t(bit(res, 7));
                s.flag_z = uint8_t(res == 0);
                s.flag_s = uint8_t(s.flag_n ^ s.flag_v);
                s.r[d5] = res;
                s.cycles += 1;
                return true;
            }
            case 0x5: {                           // asr Rd
                uint8_t a = s.r[d5];
                uint8_t res = uint8_t((a >> 1) | (a & 0x80));   // sign preserved
                s.flag_c = uint8_t(bit(a, 0));
                s.flag_n = uint8_t(bit(res, 7));
                s.flag_z = uint8_t(res == 0);
                // V is the exclusive or of N and C after the shift, not the
                // usual signed-overflow rule. It exists so that brlt still
                // behaves after an arithmetic shift.
                s.flag_v = uint8_t(s.flag_n ^ s.flag_c);
                s.flag_s = uint8_t(s.flag_n ^ s.flag_v);
                s.r[d5] = res;
                s.cycles += 1;
                return true;
            }
            case 0x6: {                           // lsr Rd
                uint8_t a = s.r[d5];
                uint8_t res = uint8_t(a >> 1);
                s.flag_c = uint8_t(bit(a, 0));
                s.flag_n = 0;                     // bit 7 is always shifted in
                                                  // as zero, so N cannot be set
                s.flag_z = uint8_t(res == 0);
                s.flag_v = uint8_t(s.flag_n ^ s.flag_c);
                s.flag_s = uint8_t(s.flag_n ^ s.flag_v);
                s.r[d5] = res;
                s.cycles += 1;
                return true;
            }
            case 0x7: {                           // ror Rd
                uint8_t a = s.r[d5];
                uint8_t carry_in = s.flag_c;
                uint8_t res = uint8_t((a >> 1) | (carry_in << 7));
                s.flag_c = uint8_t(bit(a, 0));    // the bit shifted out
                s.flag_n = uint8_t(bit(res, 7));
                s.flag_z = uint8_t(res == 0);
                s.flag_v = uint8_t(s.flag_n ^ s.flag_c);
                s.flag_s = uint8_t(s.flag_n ^ s.flag_v);
                s.r[d5] = res;
                s.cycles += 1;
                return true;
            }
            case 0xA: {                           // dec Rd
                uint8_t a = s.r[d5];
                uint8_t res = uint8_t(a - 1);
                s.flag_v = uint8_t(a == 0x80);    // C untouched, as for inc
                s.flag_n = uint8_t(bit(res, 7));
                s.flag_z = uint8_t(res == 0);
                s.flag_s = uint8_t(s.flag_n ^ s.flag_v);
                s.r[d5] = res;
                s.cycles += 1;
                return true;
            }
            case 0x8: {
                // The whole SREG-bit and return group hides in one tail. bset
                // and bclr cover sec/clc/sei/cli and the rest; the assembler
                // spells them out as separate mnemonics but they encode here.
                if ((op & 0xFF8F) == 0x9408) {    // bset s
                    set_sreg_bit(s, (op >> 4) & 7, 1);
                    s.cycles += 1;
                    return true;
                }
                if ((op & 0xFF8F) == 0x9488) {    // bclr s
                    set_sreg_bit(s, (op >> 4) & 7, 0);
                    s.cycles += 1;
                    return true;
                }
                if (op == 0x9508) {               // ret
                    s.pc = pop_pc(s);
                    s.cycles += 4;
                    return true;
                }
                if (op == 0x9518) {               // reti
                    s.pc = pop_pc(s);
                    // reti sets I unconditionally rather than restoring a saved
                    // copy: the AVR does not stack SREG, which is why a handler
                    // that needs its flags has to push SREG itself.
                    s.flag_i = 1;
                    s.cycles += 4;
                    return true;
                }
                if (op == 0x9588) {               // sleep
                    // With no clock domains modelled there is nothing to power
                    // down, so this only costs its cycle. The machine still
                    // advances, which is what lets a timer wake it.
                    s.cycles += 1;
                    return true;
                }
                if (op == 0x9598) {               // break
                    s.cycles += 1;
                    out.reason = StopReason::Breakpoint;
                    out.pc = s.pc;
                    return false;
                }
                if (op == 0x95A8) {               // wdr
                    // No watchdog is modelled, so petting it is a nop that
                    // costs a cycle. It must still decode, because the runtime
                    // emits it in its delay loops.
                    s.cycles += 1;
                    return true;
                }
                if (op == 0x95C8) {               // lpm (implicit r0 from Z)
                    s.r[0] = read_flash_byte(s, read_pair(s, 30));
                    s.cycles += 3;
                    return true;
                }
                return illegal();
            }
            case 0x9: {
                if (op == 0x9409) {               // ijmp -- Z is a WORD address
                    s.pc = read_pair(s, 30);
                    s.cycles += 2;
                    return true;
                }
                if (op == 0x9509) {               // icall
                    push_pc(s, s.pc);
                    s.pc = read_pair(s, 30);
                    s.cycles += 3;
                    return true;
                }
                return illegal();
            }
            default:
                return illegal();
            }
        }

        // ---- 1001 011x: adiw and sbiw --------------------------------------
        if ((op & 0xFF00) == 0x9600 || (op & 0xFF00) == 0x9700) {
            // Only the four upper pairs can be reached: r25:r24, X, Y, Z.
            int low = 24 + ((op >> 4) & 0x03) * 2;
            unsigned k = ((op & 0x00C0) >> 2) | (op & 0x000F);
            uint16_t a = read_pair(s, low);
            uint16_t res;
            if ((op & 0x0100) == 0) {             // adiw
                res = uint16_t(a + k);
                // The flags are computed on the 16-bit result, from bit 15,
                // not from the high byte's arithmetic in isolation.
                s.flag_v = uint8_t(nbit(uint8_t(a >> 8), 7) & bit(uint8_t(res >> 8), 7));
                s.flag_c = uint8_t(nbit(uint8_t(res >> 8), 7) & bit(uint8_t(a >> 8), 7));
            } else {                              // sbiw
                res = uint16_t(a - k);
                s.flag_v = uint8_t(bit(uint8_t(a >> 8), 7) & nbit(uint8_t(res >> 8), 7));
                s.flag_c = uint8_t(bit(uint8_t(res >> 8), 7) & nbit(uint8_t(a >> 8), 7));
            }
            s.flag_n = uint8_t(bit(uint8_t(res >> 8), 7));
            s.flag_z = uint8_t(res == 0);
            s.flag_s = uint8_t(s.flag_n ^ s.flag_v);
            write_pair(s, low, res);
            s.cycles += 2;
            return true;
        }

        // ---- 1001 10xx: the bit instructions on low I/O space ---------------
        if ((op & 0xFF00) == 0x9800 || (op & 0xFF00) == 0x9900 ||
            (op & 0xFF00) == 0x9A00 || (op & 0xFF00) == 0x9B00) {
            uint8_t io = uint8_t((op >> 3) & 0x1F);
            if ((op & 0xFF00) == 0x9800) {        // cbi
                write_io(s, io, uint8_t(read_io(s, io) & ~(1 << b3)));
                s.cycles += 2;
                return true;
            }
            if ((op & 0xFF00) == 0x9A00) {        // sbi
                write_io(s, io, uint8_t(read_io(s, io) | (1 << b3)));
                s.cycles += 2;
                return true;
            }
            bool set = bit(read_io(s, io), b3) != 0;
            bool skip = ((op & 0xFF00) == 0x9900) ? !set : set;   // sbic : sbis
            s.cycles += 1;
            if (skip) do_skip();
            return true;
        }

        // ---- 1001 11rd: mul ------------------------------------------------
        if ((op & 0xFC00) == 0x9C00) {            // mul Rd, Rr, both unsigned
            uint16_t res = uint16_t(unsigned(s.r[d5]) * unsigned(s.r[r5]));
            // The result always lands in r1:r0, whichever registers were
            // multiplied. Code that keeps r1 as the zero register therefore
            // has to clear it again afterwards.
            s.r[0] = uint8_t(res & 0xFF);
            s.r[1] = uint8_t(res >> 8);
            s.flag_c = uint8_t(bit(uint8_t(res >> 8), 7));
            s.flag_z = uint8_t(res == 0);
            s.cycles += 2;
            return true;
        }

        return illegal();
    }

    // ---------------------------------------------------- 1011 ---------
    case 0xB: {                                   // in and out
        uint8_t a = uint8_t(((op & 0x0600) >> 5) | (op & 0x000F));
        if (op & 0x0800) write_io(s, a, s.r[d5]);   // out
        else s.r[d5] = read_io(s, a);               // in
        s.cycles += 1;
        return true;
    }

    // ---------------------------------------------------- 1100 ---------
    case 0xC: {                                   // rjmp k
        // The 12-bit displacement is signed and counts WORDS from the
        // instruction after this one.
        int16_t k = int16_t(op << 4) >> 4;
        s.pc = uint16_t(s.pc + k);
        s.cycles += 2;
        return true;
    }

    // ---------------------------------------------------- 1101 ---------
    case 0xD: {                                   // rcall k
        int16_t k = int16_t(op << 4) >> 4;
        push_pc(s, s.pc);
        s.pc = uint16_t(s.pc + k);
        s.cycles += 3;
        return true;
    }

    // ---------------------------------------------------- 1110 ---------
    case 0xE: {                                   // ldi Rd, K  (also ser Rd)
        s.r[d4] = k8;
        s.cycles += 1;
        return true;
    }

    // ---------------------------------------------------- 1111 ---------
    case 0xF: {
        if ((op & 0xF800) == 0xF000) {            // brbs s, k / brbc s, k
            int16_t k = int16_t(int16_t(op << 6) >> 9);   // seven signed bits
            bool set = sreg_bit(s, b3) != 0;
            bool taken = (op & 0x0400) ? !set : set;      // 0xF4xx is brbc
            if (taken) {
                s.pc = uint16_t(s.pc + k);
                s.cycles += 2;
            } else {
                s.cycles += 1;
            }
            return true;
        }
        if ((op & 0xFE08) == 0xF800) {            // bld Rd, b -- T into a bit
            if (s.flag_t) s.r[d5] = uint8_t(s.r[d5] | (1 << b3));
            else s.r[d5] = uint8_t(s.r[d5] & ~(1 << b3));
            s.cycles += 1;
            return true;
        }
        if ((op & 0xFE08) == 0xFA00) {            // bst Rd, b -- a bit into T
            s.flag_t = uint8_t(bit(s.r[d5], b3));
            s.cycles += 1;
            return true;
        }
        if ((op & 0xFE08) == 0xFC00) {            // sbrc Rr, b
            s.cycles += 1;
            if (bit(s.r[d5], b3) == 0) do_skip();
            return true;
        }
        if ((op & 0xFE08) == 0xFE00) {            // sbrs Rr, b
            s.cycles += 1;
            if (bit(s.r[d5], b3) != 0) do_skip();
            return true;
        }
        return illegal();
    }

    default:
        return illegal();
    }
}

} // namespace

std::unique_ptr<Core> make_reference_core(const avr::AvrDevice& device) {
    return std::make_unique<ReferenceCore>(device);
}

// Peripherals reach a core through this rather than through Core itself,
// because Core as declared has no way to be given one. See the note in the
// header review: the interface takes only a State, so the wiring between a core
// and the peripherals it must route memory through has nowhere else to live.
void reference_core_attach_peripheral(Core& core, Peripheral* peripheral) {
    static_cast<ReferenceCore&>(core).attach(peripheral);
}

} // namespace ardio::emu
