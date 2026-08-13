// The native execution core for ARM64 hosts: an AVR-to-ARM64 translator.
//
// The reference core decodes one instruction per executed instruction, forever.
// This one decodes each instruction once, emits ARM64 for it, caches the result
// keyed by word address, and thereafter jumps straight into machine code. A
// loop body is decoded on its first iteration and never again.
//
// The rule this file is written under is that the reference core is right by
// definition. Anywhere the two could disagree, this file is wrong, and the
// differential tests exist to find that out. That principle decides most of the
// design questions below, and it decides them all in the same direction:
//
//   * The instruction set is covered in part, not in whole. An instruction that
//     is not translated ends the block before it, and the dispatcher hands that
//     one instruction to a privately owned reference core to execute. Nothing
//     is approximated and nothing is skipped -- the worst that an unimplemented
//     instruction costs is speed.
//
//   * Flags are computed from the datasheet's boolean formulas, bit by bit, in
//     emitted ARM64, rather than borrowed from the host's NZCV. ARM64 has no
//     half-carry at all, so H would have to be synthesised regardless, and the
//     sign, overflow and carry rules do not line up either: an AVR subtract
//     sets C as a BORROW, which is the complement of ARM64's C, and cpc, sbc
//     and sbci only ever CLEAR Z. Borrowing the host's flags for the cases that
//     do happen to match, and synthesising the rest, would leave a reader
//     unable to tell by inspection which was which. So every flag is emitted as
//     the same expression the reference computes, and the transcription is what
//     gets checked rather than an equivalence argument.
//
//   * Blocks end at every control transfer. There is no chaining between blocks
//     and no attempt to keep guest registers live across a block boundary. What
//     is kept in host registers is the state base pointer and the cycle count,
//     which is enough to remove a load and a store per instruction without
//     introducing any question about where a value is when a block exits.
//
// Two host registers hold the whole calling convention. x19 points at the guest
// State, so every guest register, flag and pointer is reachable at a constant
// displacement; x20 points at this core, for the calls back out to memory.
// x21 accumulates the cycle count for the block and is written back once, at
// exit. x22 holds a pointer value across a call back out, where one is needed.
//
// Cycle counts, deadlines and interrupts need care, because the reference
// checks all three between every pair of instructions and translated code
// checks them only between blocks. Three rules restore the equivalence, and
// they are the least obvious thing in this file:
//
//   * Every instruction's cycle cost is known when it is translated, so a
//     block's total cost is known too. The dispatcher enters a block only when
//     the whole of it fits before the deadline, and single-steps otherwise. A
//     block therefore never runs an instruction that the reference would have
//     stopped in front of.
//
//   * An interrupt can only become pending because a peripheral was touched,
//     and a peripheral can only be touched by a data-space access. So when any
//     peripheral is attached, a block ends after any instruction that reaches
//     data space, which puts the dispatcher's interrupt check exactly where the
//     reference's is. With no peripherals attached no interrupt can ever be
//     raised, and blocks run long.
//
//   * Breakpoints are checked before every instruction, which a block cannot
//     do. While any breakpoint is set the whole run is delegated to the
//     reference core. Single-stepping under a debugger is not a speed problem.

#include "ardio/emu/machine.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <vector>

#if defined(__aarch64__) || defined(_M_ARM64)
#define ARDIO_EMU_ARM64 1
#include <pthread.h>
#include <sys/mman.h>
#endif

namespace ardio::emu {

#if ARDIO_EMU_ARM64

namespace {

// ------------------------------------------------------ State layout ---
//
// Translated code reaches guest state at constant displacements off x19, so
// these offsets are compiled into the emitted instructions and cannot be
// recovered from a cached block afterwards. They are asserted rather than
// assumed: a field moving without kStateLayout being bumped would otherwise
// turn into a silent read of the wrong guest register.
constexpr int kOffR = 0;
constexpr int kOffC = 32, kOffZ = 33, kOffN = 34, kOffV = 35;
constexpr int kOffS = 36, kOffH = 37, kOffT = 38, kOffI = 39;
constexpr int kOffPc = 40, kOffSp = 42, kOffCycles = 48;

static_assert(offsetof(State, r) == kOffR, "State::r moved; bump kStateLayout");
static_assert(offsetof(State, flag_c) == kOffC, "flags moved; bump kStateLayout");
static_assert(offsetof(State, flag_z) == kOffZ, "flags moved; bump kStateLayout");
static_assert(offsetof(State, flag_n) == kOffN, "flags moved; bump kStateLayout");
static_assert(offsetof(State, flag_v) == kOffV, "flags moved; bump kStateLayout");
static_assert(offsetof(State, flag_s) == kOffS, "flags moved; bump kStateLayout");
static_assert(offsetof(State, flag_h) == kOffH, "flags moved; bump kStateLayout");
static_assert(offsetof(State, flag_t) == kOffT, "flags moved; bump kStateLayout");
static_assert(offsetof(State, flag_i) == kOffI, "flags moved; bump kStateLayout");
static_assert(offsetof(State, pc) == kOffPc, "State::pc moved; bump kStateLayout");
static_assert(offsetof(State, sp) == kOffSp, "State::sp moved; bump kStateLayout");
static_assert(offsetof(State, cycles) == kOffCycles, "State::cycles moved; bump kStateLayout");
static_assert(kStateLayout == 1,
              "kStateLayout changed: re-check every offset above and every "
              "emitted sequence that hardcodes one, then update this assert");

// SREG bit number to the offset of the byte holding that flag. brbs and brbc
// name a bit position, and the flags are stored unpacked, so the branch
// emitter turns one into the other through this rather than materialising a
// whole SREG byte.
constexpr int kFlagOffsetForBit[8] = {kOffC, kOffZ, kOffN, kOffV,
                                      kOffS, kOffH, kOffT, kOffI};

// ------------------------------------------------------ the assembler ---

// Host registers with a fixed meaning inside a translated block. Everything
// else, w0 through w17, is scratch between AVR instructions and is destroyed
// by any call back out.
constexpr int kState = 19;    // State*
constexpr int kCore = 20;     // JitCore*
constexpr int kCycles = 21;   // accumulated cycles, written back at exit
constexpr int kKeep = 22;     // survives a call back out
constexpr int kZR = 31;

// ARM64 condition codes, in the architecture's own numbering.
enum Cond { kEQ = 0, kNE = 1 };

// A tiny ARM64 encoder. It emits only the forms this translator needs, and
// each is written as the bit fields the architecture manual names rather than
// as a magic constant, so a mis-encoding is visible without a disassembler.
class A64 {
public:
    std::vector<uint32_t> out;

    void raw(uint32_t w) { out.push_back(w); }

    // ---- loads and stores off a base register ----
    // The unsigned-offset forms scale their immediate by the access size, so
    // a halfword offset is halved and a doubleword offset divided by eight.
    // Passing a byte offset that does not divide evenly would silently address
    // the wrong field, so each is checked.
    void ldrb(int rt, int rn, int off) { mem(0x39400000u, rt, rn, unsigned(off)); }
    void strb(int rt, int rn, int off) { mem(0x39000000u, rt, rn, unsigned(off)); }
    void ldrh(int rt, int rn, int off) { mem(0x79400000u, rt, rn, unsigned(off) / 2); }
    void strh(int rt, int rn, int off) { mem(0x79000000u, rt, rn, unsigned(off) / 2); }
    void ldr64(int rt, int rn, int off) { mem(0xF9400000u, rt, rn, unsigned(off) / 8); }
    void str64(int rt, int rn, int off) { mem(0xF9000000u, rt, rn, unsigned(off) / 8); }

    // ---- constants ----
    void movz32(int rd, unsigned imm16, int shift) {
        raw(0x52800000u | (unsigned(shift / 16) << 21) | ((imm16 & 0xFFFFu) << 5) | unsigned(rd));
    }
    void movk32(int rd, unsigned imm16, int shift) {
        raw(0x72800000u | (unsigned(shift / 16) << 21) | ((imm16 & 0xFFFFu) << 5) | unsigned(rd));
    }
    void movi32(int rd, uint32_t v) {
        movz32(rd, v & 0xFFFFu, 0);
        if (v >> 16) movk32(rd, (v >> 16) & 0xFFFFu, 16);
    }
    void movi64(int rd, uint64_t v) {
        raw(0xD2800000u | ((unsigned(v & 0xFFFFu)) << 5) | unsigned(rd));
        for (int sh = 16; sh < 64; sh += 16) {
            unsigned part = unsigned((v >> sh) & 0xFFFFu);
            if (part) raw(0xF2800000u | (unsigned(sh / 16) << 21) | (part << 5) | unsigned(rd));
        }
    }

    // ---- moves ----
    void mov32(int rd, int rm) { raw(0x2A0003E0u | (unsigned(rm) << 16) | unsigned(rd)); }
    void mov64(int rd, int rm) { raw(0xAA0003E0u | (unsigned(rm) << 16) | unsigned(rd)); }

    // ---- arithmetic and logic, 32-bit ----
    void add(int rd, int rn, int rm) { rrr(0x0B000000u, rd, rn, rm); }
    void sub(int rd, int rn, int rm) { rrr(0x4B000000u, rd, rn, rm); }
    void andr(int rd, int rn, int rm) { rrr(0x0A000000u, rd, rn, rm); }
    void orrr(int rd, int rn, int rm) { rrr(0x2A000000u, rd, rn, rm); }
    void eorr(int rd, int rn, int rm) { rrr(0x4A000000u, rd, rn, rm); }
    void mvn(int rd, int rm) { raw(0x2A2003E0u | (unsigned(rm) << 16) | unsigned(rd)); }

    void addi(int rd, int rn, unsigned imm12) {
        raw(0x11000000u | ((imm12 & 0xFFFu) << 10) | (unsigned(rn) << 5) | unsigned(rd));
    }
    void subi(int rd, int rn, unsigned imm12) {
        raw(0x51000000u | ((imm12 & 0xFFFu) << 10) | (unsigned(rn) << 5) | unsigned(rd));
    }
    void addi64(int rd, int rn, unsigned imm12) {
        raw(0x91000000u | ((imm12 & 0xFFFu) << 10) | (unsigned(rn) << 5) | unsigned(rd));
    }

    // Compare against zero. Nothing between this and the conditional that
    // consumes it may set NZCV, which in practice means only stores and bit
    // extractions may come between.
    void cmpz(int rn) { raw(0x7100001Fu | (unsigned(rn) << 5)); }

    // ---- bitfields ----
    void ubfx(int rd, int rn, int lsb, int width) {
        ubfm(rd, rn, lsb, lsb + width - 1);
    }
    void uxtb(int rd, int rn) { ubfm(rd, rn, 0, 7); }
    void uxth(int rd, int rn) { ubfm(rd, rn, 0, 15); }
    void lsri(int rd, int rn, int sh) { ubfm(rd, rn, sh, 31); }
    void lsli(int rd, int rn, int sh) { ubfm(rd, rn, (32 - sh) & 31, 31 - sh); }

    // ---- conditionals ----
    void cset(int rd, Cond c) {
        raw(0x1A9F07E0u | ((unsigned(c) ^ 1u) << 12) | unsigned(rd));
    }
    void csel32(int rd, int rn, int rm, Cond c) {
        raw(0x1A800000u | (unsigned(rm) << 16) | (unsigned(c) << 12) |
            (unsigned(rn) << 5) | unsigned(rd));
    }
    void csel64(int rd, int rn, int rm, Cond c) {
        raw(0x9A800000u | (unsigned(rm) << 16) | (unsigned(c) << 12) |
            (unsigned(rn) << 5) | unsigned(rd));
    }

    // ---- calls and frames ----
    void blr(int rn) { raw(0xD63F0000u | (unsigned(rn) << 5)); }
    void ret() { raw(0xD65F03C0u); }

    void stp_pre(int rt, int rt2, int imm) {
        raw(0xA9800000u | ((unsigned(imm / 8) & 0x7Fu) << 15) | (unsigned(rt2) << 10) |
            (31u << 5) | unsigned(rt));
    }
    void stp_off(int rt, int rt2, int imm) {
        raw(0xA9000000u | ((unsigned(imm / 8) & 0x7Fu) << 15) | (unsigned(rt2) << 10) |
            (31u << 5) | unsigned(rt));
    }
    void ldp_off(int rt, int rt2, int imm) {
        raw(0xA9400000u | ((unsigned(imm / 8) & 0x7Fu) << 15) | (unsigned(rt2) << 10) |
            (31u << 5) | unsigned(rt));
    }
    void ldp_post(int rt, int rt2, int imm) {
        raw(0xA8C00000u | ((unsigned(imm / 8) & 0x7Fu) << 15) | (unsigned(rt2) << 10) |
            (31u << 5) | unsigned(rt));
    }
    void mov_sp_to(int rd) { raw(0x910003E0u | unsigned(rd)); }

private:
    void mem(uint32_t base, int rt, int rn, unsigned imm12) {
        raw(base | ((imm12 & 0xFFFu) << 10) | (unsigned(rn) << 5) | unsigned(rt));
    }
    void rrr(uint32_t base, int rd, int rn, int rm) {
        raw(base | (unsigned(rm) << 16) | (unsigned(rn) << 5) | unsigned(rd));
    }
    void ubfm(int rd, int rn, int immr, int imms) {
        raw(0x53000000u | (unsigned(immr) << 16) | (unsigned(imms) << 10) |
            (unsigned(rn) << 5) | unsigned(rd));
    }
};

// ------------------------------------------------------ code memory ---

// A slab of W^X executable memory.
//
// Apple Silicon will not map a page both writable and executable at the same
// time. MAP_JIT gets a region that can be either, and
// pthread_jit_write_protect_np flips the calling thread between the two views.
// Writes therefore have to be bracketed, and the bracket has to be closed
// before anything jumps into the region.
//
// Cache maintenance is the other half and is easy to leave out, because
// leaving it out mostly works: the data written goes through the data cache,
// while the core fetches instructions through a separate instruction cache
// that has no idea the memory changed. Whether a stale line is still present
// depends on timing and on what else the process has been doing, so an
// omission shows up as a block that runs correctly a thousand times and then
// executes garbage. __builtin___clear_cache issues the flush and invalidate
// the architecture requires, and it is called on exactly the range written.
class CodeSlab {
public:
    static constexpr size_t kSize = 4u << 20;

    bool open() {
        void* p = mmap(nullptr, kSize, PROT_READ | PROT_WRITE | PROT_EXEC,
                       MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
        if (p == MAP_FAILED) return false;
        base_ = static_cast<uint8_t*>(p);
        used_ = 0;
        return true;
    }

    ~CodeSlab() {
        if (base_) munmap(base_, kSize);
    }

    bool valid() const { return base_ != nullptr; }

    // Room for `words` instructions, or false if the slab is full. A caller
    // that gets false is expected to reset the whole slab, which is safe only
    // because translation happens between blocks and never while translated
    // code is on the stack.
    bool has_room(size_t words) const { return used_ + words * 4 <= kSize; }
    void reset() { used_ = 0; }

    // Copies a finished block in and returns its entry point.
    void* commit(const std::vector<uint32_t>& code) {
        uint8_t* dst = base_ + used_;
        pthread_jit_write_protect_np(0);
        std::memcpy(dst, code.data(), code.size() * 4);
        pthread_jit_write_protect_np(1);
        __builtin___clear_cache(reinterpret_cast<char*>(dst),
                                reinterpret_cast<char*>(dst + code.size() * 4));
        used_ += code.size() * 4;
        return dst;
    }

private:
    uint8_t* base_ = nullptr;
    size_t used_ = 0;
};

// ------------------------------------------------------ the core ---

class JitCore;

// The calls a translated block makes back out into C++. Data-space accesses
// cannot be inlined, because a peripheral read can have side effects, and the
// register file and SREG are mapped into the same address space; the stack
// helpers exist so that push and pop go through exactly the same routing.
extern "C" {
uint32_t ardio_jit_read_data(JitCore* core, State* s, uint32_t addr);
void ardio_jit_write_data(JitCore* core, State* s, uint32_t addr, uint32_t value);
uint32_t ardio_jit_read_flash(JitCore* core, State* s, uint32_t byte_addr);
void ardio_jit_push_pc(JitCore* core, State* s, uint32_t word_addr);
uint32_t ardio_jit_pop_pc(JitCore* core, State* s);
}

using BlockFn = void (*)(State*, JitCore*);

struct Block {
    BlockFn code = nullptr;
    uint16_t start = 0;       // first word address covered
    uint16_t end = 0;         // last word address covered, inclusive
    unsigned max_cycles = 0;  // most this block can charge, for the deadline test
    uint32_t layout = 0;      // the State layout it was compiled against
};

class JitCore final : public Core {
public:
    explicit JitCore(const avr::AvrDevice& device)
        : dev_(device), ref_(make_reference_core(device)) {
        slab_.open();
    }

    bool usable() const { return slab_.valid() && ref_ != nullptr; }

    void attach(Peripheral* p) override {
        if (!p) return;
        peripherals_.push_back(p);
        ref_->attach(p);
        // The rule that ends a block after every data-space access only takes
        // effect once something can raise an interrupt, and blocks translated
        // before that would not carry it. Dropping them is cheaper than
        // reasoning about which ones were affected.
        flush();
    }

    RunResult run(State& state) override;

    void invalidate(uint32_t byte_start, uint32_t byte_end) override;

    void set_breakpoint(uint16_t word_addr) override {
        breakpoints_.insert(word_addr);
        ref_->set_breakpoint(word_addr);
    }
    void clear_breakpoint(uint16_t word_addr) override {
        breakpoints_.erase(word_addr);
        ref_->clear_breakpoint(word_addr);
    }

    std::string description() const override {
        return "native ARM64 translator (compiles AVR basic blocks to host machine "
               "code and caches them; falls back to the portable interpreter for "
               "instructions it does not translate)";
    }

    // Reached from translated code through the extern "C" trampolines.
    uint8_t read_data(State& s, uint16_t addr);
    void write_data(State& s, uint16_t addr, uint8_t value);
    uint8_t read_flash_byte(const State& s, uint32_t byte_addr) const {
        if (!s.flash || dev_.flash_size == 0) return 0xFF;
        return s.flash[byte_addr % dev_.flash_size];
    }

private:
    uint8_t sreg(const State& s) const;
    void set_sreg(State& s, uint8_t v) const;

    uint16_t fetch(const State& s, uint16_t word_addr) const {
        uint32_t b = uint32_t(word_addr) * 2;
        return uint16_t(read_flash_byte(s, b) | (uint16_t(read_flash_byte(s, b + 1)) << 8));
    }

    void flush() {
        blocks_.clear();
        slab_.reset();
    }

    // Executes exactly one guest instruction on the reference core, which is
    // how everything this translator does not cover gets run. The reference
    // stops as soon as the cycle count reaches the deadline, so lending it a
    // deadline one cycle ahead makes it run a single instruction and return.
    // Interrupt dispatch happens inside, in the same place the reference would
    // have done it, which is why the fallback is used for the interrupt case
    // too rather than being duplicated here.
    RunResult single_step(State& s);

    const Block* lookup(State& s, uint16_t word_addr);
    bool translate(State& s, uint16_t word_addr, Block& out);

    avr::AvrDevice dev_;
    std::unique_ptr<Core> ref_;
    std::vector<Peripheral*> peripherals_;
    std::set<uint16_t> breakpoints_;
    std::map<uint16_t, Block> blocks_;
    CodeSlab slab_;

    // Blocks are translated from a particular flash image. A caller is free to
    // point State at a different one between runs, and the cached code would
    // then be for the wrong program, so the image is remembered and the cache
    // dropped if it changes.
    const uint8_t* translated_flash_ = nullptr;
};

// ----------------------------------------------------- data routing ---
//
// These are transcriptions of the reference core's own routing, including the
// parts that look like details: the register file really is mapped at address
// zero, SREG and the two stack pointer bytes are answered by the CPU rather
// than by any peripheral, and an address belonging to nothing reads back zero.
// A translator that returned bus noise there could not be compared against
// anything.

uint8_t JitCore::sreg(const State& s) const {
    return uint8_t((s.flag_c ? 1 << 0 : 0) | (s.flag_z ? 1 << 1 : 0) |
                   (s.flag_n ? 1 << 2 : 0) | (s.flag_v ? 1 << 3 : 0) |
                   (s.flag_s ? 1 << 4 : 0) | (s.flag_h ? 1 << 5 : 0) |
                   (s.flag_t ? 1 << 6 : 0) | (s.flag_i ? 1 << 7 : 0));
}

void JitCore::set_sreg(State& s, uint8_t v) const {
    s.flag_c = uint8_t((v >> 0) & 1);
    s.flag_z = uint8_t((v >> 1) & 1);
    s.flag_n = uint8_t((v >> 2) & 1);
    s.flag_v = uint8_t((v >> 3) & 1);
    s.flag_s = uint8_t((v >> 4) & 1);
    s.flag_h = uint8_t((v >> 5) & 1);
    s.flag_t = uint8_t((v >> 6) & 1);
    s.flag_i = uint8_t((v >> 7) & 1);
}

uint8_t JitCore::read_data(State& s, uint16_t addr) {
    if (addr < 32) return s.r[addr];
    if (addr == 0x5F) return sreg(s);
    if (addr == 0x5D) return uint8_t(s.sp & 0xFF);
    if (addr == 0x5E) return uint8_t(s.sp >> 8);
    for (Peripheral* p : peripherals_)
        if (p->claims(addr)) return p->read(addr);
    if (s.sram && addr >= dev_.ram_start && addr <= dev_.ramend)
        return s.sram[addr - dev_.ram_start];
    return 0;
}

void JitCore::write_data(State& s, uint16_t addr, uint8_t value) {
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
}

extern "C" {

uint32_t ardio_jit_read_data(JitCore* core, State* s, uint32_t addr) {
    return core->read_data(*s, uint16_t(addr));
}

void ardio_jit_write_data(JitCore* core, State* s, uint32_t addr, uint32_t value) {
    core->write_data(*s, uint16_t(addr), uint8_t(value));
}

uint32_t ardio_jit_read_flash(JitCore* core, State* s, uint32_t byte_addr) {
    return core->read_flash_byte(*s, byte_addr);
}

// The stack grows down and SP names the next free byte, and the return address
// goes on low byte first so that its high byte ends up at the lower address.
// Both halves are re-read from State between the two accesses because a write
// can land on SP itself.
void ardio_jit_push_pc(JitCore* core, State* s, uint32_t word_addr) {
    core->write_data(*s, s->sp, uint8_t(word_addr & 0xFF));
    s->sp = uint16_t(s->sp - 1);
    core->write_data(*s, s->sp, uint8_t((word_addr >> 8) & 0xFF));
    s->sp = uint16_t(s->sp - 1);
}

uint32_t ardio_jit_pop_pc(JitCore* core, State* s) {
    s->sp = uint16_t(s->sp + 1);
    uint16_t hi = core->read_data(*s, s->sp);
    s->sp = uint16_t(s->sp + 1);
    uint16_t lo = core->read_data(*s, s->sp);
    return uint16_t((hi << 8) | lo);
}

} // extern "C"

// ------------------------------------------------------ the translator ---

// True if the instruction word starts one of the four two-word instructions.
// Skips consult this, and so does block discovery, which has to advance by the
// right number of words or it would decode an operand as an opcode.
bool is_two_word(uint16_t op) {
    if ((op & 0xFC0F) == 0x9000) return true;   // lds and sts
    if ((op & 0xFE0E) == 0x940C) return true;   // jmp and call
    return false;
}

// A self-targeting jump is never translated. With interrupts off the reference
// reports the machine as halted rather than executing it, and that decision
// depends on a flag whose value is not known at translation time, so the
// instruction is left to the reference core entirely.
bool is_self_loop_shape(uint16_t op, uint16_t pc_of_op, uint16_t second) {
    if (op == 0xCFFF) return true;
    if ((op & 0xFE0E) == 0x940C && (op & 0x0002) == 0) return second == pc_of_op;
    return false;
}

// One AVR instruction's translation, for the block builder's benefit.
struct Emit {
    enum Kind { Straight, Terminator, Unsupported } kind = Unsupported;
    unsigned max_cycles = 0;
    unsigned words = 1;
    bool touches_data = false;   // may reach a peripheral, so may raise an interrupt
};

// The translator proper.
//
// It is a free function rather than a member so that the state it carries is
// only what it needs: the assembler being appended to, and the flash it is
// reading. Nothing here mutates the guest.
class Translator {
public:
    Translator(A64& a, const uint8_t* flash, uint32_t flash_size)
        : a_(a), flash_(flash), flash_size_(flash_size) {}

    Emit one(uint16_t pc_of_op);

private:
    // ---- guest state access ----
    void ld_r(int w, int idx) { a_.ldrb(w, kState, kOffR + idx); }
    void st_r(int idx, int w) { a_.strb(w, kState, kOffR + idx); }
    void ld_flag(int w, int off) { a_.ldrb(w, kState, off); }
    void st_flag(int off, int w) { a_.strb(w, kState, off); }
    void ld_pair(int w, int low) { a_.ldrh(w, kState, kOffR + low); }
    void st_pair(int low, int w) { a_.strh(w, kState, kOffR + low); }

    void set_pc(uint16_t v) {
        a_.movi32(0, v);
        a_.strh(0, kState, kOffPc);
    }
    void add_cycles(unsigned n) {
        if (n) a_.addi64(kCycles, kCycles, n);
    }

    // Calls one of the trampolines. Arguments must already be in w2 upwards;
    // x0 and x1 are filled in here because they are always the same two.
    void call(void* fn) {
        a_.mov64(0, kCore);
        a_.mov64(1, kState);
        a_.movi64(8, uint64_t(reinterpret_cast<uintptr_t>(fn)));
        a_.blr(8);
    }

    uint16_t fetch(uint16_t word_addr) const {
        uint32_t b = uint32_t(word_addr) * 2;
        if (!flash_ || flash_size_ == 0) return 0xFFFF;
        return uint16_t(flash_[b % flash_size_] |
                        (uint16_t(flash_[(b + 1) % flash_size_]) << 8));
    }

    // ---- flag emitters ----
    void flags_add(bool z_normal);
    void flags_sub(bool z_normal);
    void flags_logic(int res);
    void store_z(int res, bool z_normal);

    // ---- instruction families ----
    Emit alu_add(int d, int b_reg, int k, bool have_k, bool with_carry);
    Emit alu_sub(int d, int b_reg, int k, bool have_k, bool with_carry, bool z_normal,
                 bool store_result);
    Emit skip_after(uint16_t pc_of_op, unsigned base_cycles, Cond taken);

    A64& a_;
    const uint8_t* flash_;
    uint32_t flash_size_;
};

// res is in w2 and is already narrowed to eight bits; NZCV must be free.
void Translator::store_z(int res, bool z_normal) {
    a_.cmpz(res);
    if (z_normal) {
        a_.cset(8, kEQ);
    } else {
        // cpc, sbc and sbci clear Z and never set it, which is the whole
        // mechanism behind a multi-byte compare: Z survives from the low byte
        // only if every byte so far compared equal.
        ld_flag(8, kOffZ);
        a_.csel32(8, 8, kZR, kEQ);
    }
    st_flag(kOffZ, 8);
}

// Operands in w0 and w1, result in w2, all already eight bits wide.
//
// Every line is one term of the boolean expression the instruction set summary
// prints. The half-carry is the reason this is not done with the host's flags:
// ARM64 has no carry out of bit 3 to read, so it is built out of the three
// bit-3 products, and getting its polarity backwards is the single most likely
// way for this file to be quietly wrong.
void Translator::flags_add(bool z_normal) {
    a_.mvn(3, 2);    // ~res
    a_.mvn(9, 0);    // ~a
    a_.mvn(10, 1);   // ~b

    // H = a3.b3 + b3.~res3 + ~res3.a3
    a_.ubfx(4, 0, 3, 1);
    a_.ubfx(5, 1, 3, 1);
    a_.ubfx(6, 3, 3, 1);
    a_.andr(7, 4, 5);
    a_.andr(16, 5, 6);
    a_.orrr(7, 7, 16);
    a_.andr(16, 6, 4);
    a_.orrr(7, 7, 16);
    st_flag(kOffH, 7);

    a_.ubfx(4, 0, 7, 1);    // a7
    a_.ubfx(5, 1, 7, 1);    // b7
    a_.ubfx(6, 2, 7, 1);    // res7
    a_.ubfx(11, 9, 7, 1);   // ~a7
    a_.ubfx(12, 10, 7, 1);  // ~b7
    a_.ubfx(13, 3, 7, 1);   // ~res7

    // V = a7.b7.~res7 + ~a7.~b7.res7
    a_.andr(7, 4, 5);
    a_.andr(7, 7, 13);
    a_.andr(16, 11, 12);
    a_.andr(16, 16, 6);
    a_.orrr(14, 7, 16);
    st_flag(kOffV, 14);

    // N = res7
    st_flag(kOffN, 6);

    // C = a7.b7 + b7.~res7 + ~res7.a7
    a_.andr(7, 4, 5);
    a_.andr(16, 5, 13);
    a_.orrr(7, 7, 16);
    a_.andr(16, 13, 4);
    a_.orrr(7, 7, 16);
    st_flag(kOffC, 7);

    store_z(2, z_normal);

    a_.eorr(17, 6, 14);   // S = N xor V
    st_flag(kOffS, 17);
}

// The subtract polarities, written out separately rather than shared with the
// add. They are not the same expression with a sign flipped: the half-carry
// and the carry both take the complement of the MINUEND's bit and the plain
// bit of the subtrahend and result, because AVR's C after a subtract is a
// borrow rather than a carry.
void Translator::flags_sub(bool z_normal) {
    a_.mvn(3, 2);    // ~res
    a_.mvn(9, 0);    // ~a
    a_.mvn(10, 1);   // ~b

    // H = ~a3.b3 + b3.res3 + res3.~a3
    a_.ubfx(11, 9, 3, 1);   // ~a3
    a_.ubfx(5, 1, 3, 1);    // b3
    a_.ubfx(6, 2, 3, 1);    // res3
    a_.andr(7, 11, 5);
    a_.andr(16, 5, 6);
    a_.orrr(7, 7, 16);
    a_.andr(16, 6, 11);
    a_.orrr(7, 7, 16);
    st_flag(kOffH, 7);

    a_.ubfx(4, 0, 7, 1);    // a7
    a_.ubfx(5, 1, 7, 1);    // b7
    a_.ubfx(6, 2, 7, 1);    // res7
    a_.ubfx(11, 9, 7, 1);   // ~a7
    a_.ubfx(12, 10, 7, 1);  // ~b7
    a_.ubfx(13, 3, 7, 1);   // ~res7

    // V = a7.~b7.~res7 + ~a7.b7.res7
    a_.andr(7, 4, 12);
    a_.andr(7, 7, 13);
    a_.andr(16, 11, 5);
    a_.andr(16, 16, 6);
    a_.orrr(14, 7, 16);
    st_flag(kOffV, 14);

    st_flag(kOffN, 6);

    // C = ~a7.b7 + b7.res7 + res7.~a7
    a_.andr(7, 11, 5);
    a_.andr(16, 5, 6);
    a_.orrr(7, 7, 16);
    a_.andr(16, 6, 11);
    a_.orrr(7, 7, 16);
    st_flag(kOffC, 7);

    store_z(2, z_normal);

    a_.eorr(17, 6, 14);
    st_flag(kOffS, 17);
}

// The and/or/eor shape: V is cleared, N and Z come from the result, S follows.
// Result in w2.
void Translator::flags_logic(int res) {
    a_.movi32(7, 0);
    st_flag(kOffV, 7);
    a_.ubfx(6, res, 7, 1);
    st_flag(kOffN, 6);
    store_z(res, true);
    st_flag(kOffS, 6);   // S = N xor 0
}

Emit Translator::alu_add(int d, int b_reg, int k, bool have_k, bool with_carry) {
    ld_r(0, d);
    if (have_k) a_.movi32(1, unsigned(k));
    else ld_r(1, b_reg);
    a_.add(2, 0, 1);
    if (with_carry) {
        ld_flag(15, kOffC);
        a_.add(2, 2, 15);
    }
    a_.uxtb(2, 2);
    // The result is stored before the flags so that the flag emitter is free to
    // use every scratch register; nothing it computes depends on the store.
    st_r(d, 2);
    flags_add(true);
    Emit e;
    e.kind = Emit::Straight;
    e.max_cycles = 1;
    return e;
}

Emit Translator::alu_sub(int d, int b_reg, int k, bool have_k, bool with_carry,
                         bool z_normal, bool store_result) {
    ld_r(0, d);
    if (have_k) a_.movi32(1, unsigned(k));
    else ld_r(1, b_reg);
    a_.sub(2, 0, 1);
    if (with_carry) {
        ld_flag(15, kOffC);
        a_.sub(2, 2, 15);
    }
    a_.uxtb(2, 2);
    if (store_result) st_r(d, 2);
    flags_sub(z_normal);
    Emit e;
    e.kind = Emit::Straight;
    e.max_cycles = 1;
    return e;
}

// The tail shared by cpse, sbrc and sbrs: NZCV already reflects the test, and
// `taken` is the condition under which the next instruction is stepped over.
//
// Both destinations are known here, because the size of the instruction being
// skipped is known from its opcode, so the whole thing becomes a pair of
// conditional selects with no branch. Skipping one word where two were needed
// would land the program counter on an operand, which is why the size comes
// from is_two_word rather than from an assumption.
Emit Translator::skip_after(uint16_t pc_of_op, unsigned base_cycles, Cond taken) {
    uint16_t next = uint16_t(pc_of_op + 1);
    uint16_t skipped = fetch(next);
    unsigned skip_words = is_two_word(skipped) ? 2u : 1u;
    uint16_t target = uint16_t(next + skip_words);

    a_.movi32(0, target);
    a_.movi32(1, next);
    a_.csel32(0, 0, 1, taken);
    a_.strh(0, kState, kOffPc);

    a_.addi64(kCycles, kCycles, base_cycles);
    a_.addi64(2, kCycles, skip_words);
    a_.csel64(kCycles, 2, kCycles, taken);

    Emit e;
    e.kind = Emit::Terminator;
    e.max_cycles = base_cycles + skip_words;
    return e;
}

Emit Translator::one(uint16_t pc_of_op) {
    const uint16_t op = fetch(pc_of_op);
    const uint16_t second = fetch(uint16_t(pc_of_op + 1));
    const uint16_t next = uint16_t(pc_of_op + 1);

    Emit unsupported;   // kind defaults to Unsupported

    if (is_self_loop_shape(op, pc_of_op, second)) return unsupported;

    const int d5 = (op >> 4) & 0x1F;
    const int r5 = ((op & 0x0200) >> 5) | (op & 0x0F);
    const int d4 = 16 + ((op >> 4) & 0x0F);
    const unsigned k8 = unsigned(((op & 0x0F00) >> 4) | (op & 0x000F));
    const int b3 = op & 0x07;

    auto straight = [](unsigned cycles) {
        Emit e;
        e.kind = Emit::Straight;
        e.max_cycles = cycles;
        return e;
    };

    switch (op >> 12) {

    case 0x0:
        if (op == 0x0000) return straight(1);                 // nop
        if ((op & 0xFF00) == 0x0100) {                        // movw
            int dd = ((op >> 4) & 0x0F) * 2;
            int rr = (op & 0x0F) * 2;
            ld_r(0, rr);
            ld_r(1, rr + 1);
            st_r(dd, 0);
            st_r(dd + 1, 1);
            return straight(1);
        }
        if ((op & 0xFC00) == 0x0400)                          // cpc
            return alu_sub(d5, r5, 0, false, true, false, false);
        if ((op & 0xFC00) == 0x0800)                          // sbc
            return alu_sub(d5, r5, 0, false, true, false, true);
        if ((op & 0xFC00) == 0x0C00)                          // add
            return alu_add(d5, r5, 0, false, false);
        return unsupported;                                   // the multiply group

    case 0x1:
        if ((op & 0xFC00) == 0x1000) {                        // cpse
            ld_r(0, d5);
            ld_r(1, r5);
            a_.sub(2, 0, 1);
            a_.uxtb(2, 2);
            a_.cmpz(2);
            return skip_after(pc_of_op, 1, kEQ);
        }
        if ((op & 0xFC00) == 0x1400)                          // cp
            return alu_sub(d5, r5, 0, false, false, true, false);
        if ((op & 0xFC00) == 0x1800)                          // sub
            return alu_sub(d5, r5, 0, false, false, true, true);
        if ((op & 0xFC00) == 0x1C00)                          // adc
            return alu_add(d5, r5, 0, false, true);
        return unsupported;

    case 0x2: {
        if ((op & 0xFC00) == 0x2C00) {                        // mov
            ld_r(0, r5);
            st_r(d5, 0);
            return straight(1);
        }
        ld_r(0, d5);
        ld_r(1, r5);
        if ((op & 0xFC00) == 0x2000) a_.andr(2, 0, 1);        // and
        else if ((op & 0xFC00) == 0x2400) a_.eorr(2, 0, 1);   // eor
        else if ((op & 0xFC00) == 0x2800) a_.orrr(2, 0, 1);   // or
        else return unsupported;
        a_.uxtb(2, 2);
        st_r(d5, 2);
        flags_logic(2);
        return straight(1);
    }

    case 0x3:                                                 // cpi
        return alu_sub(d4, 0, int(k8), true, false, true, false);
    case 0x4:                                                 // sbci
        return alu_sub(d4, 0, int(k8), true, true, false, true);
    case 0x5:                                                 // subi
        return alu_sub(d4, 0, int(k8), true, false, true, true);

    case 0x6:                                                 // ori
    case 0x7: {                                               // andi
        ld_r(0, d4);
        a_.movi32(1, k8);
        if ((op >> 12) == 0x6) a_.orrr(2, 0, 1);
        else a_.andr(2, 0, 1);
        a_.uxtb(2, 2);
        st_r(d4, 2);
        flags_logic(2);
        return straight(1);
    }

    case 0x8:
    case 0xA: {                                               // ldd and std
        int q = ((op & 0x2000) >> 8) | ((op & 0x0C00) >> 7) | (op & 0x0007);
        int base_low = (op & 0x0008) ? 28 : 30;
        ld_pair(2, base_low);
        if (q) a_.addi(2, 2, unsigned(q));
        a_.uxth(2, 2);
        if (op & 0x0200) {                                    // std
            ld_r(3, d5);
            call(reinterpret_cast<void*>(&ardio_jit_write_data));
        } else {                                              // ldd
            call(reinterpret_cast<void*>(&ardio_jit_read_data));
            st_r(d5, 0);
        }
        Emit e = straight(2);
        e.touches_data = true;
        return e;
    }

    case 0x9: {
        if ((op & 0xFE0F) == 0x9000) {                        // lds
            a_.movi32(2, second);
            call(reinterpret_cast<void*>(&ardio_jit_read_data));
            st_r(d5, 0);
            Emit e = straight(2);
            e.words = 2;
            e.touches_data = true;
            return e;
        }
        if ((op & 0xFE0F) == 0x9200) {                        // sts
            a_.movi32(2, second);
            ld_r(3, d5);
            call(reinterpret_cast<void*>(&ardio_jit_write_data));
            Emit e = straight(2);
            e.words = 2;
            e.touches_data = true;
            return e;
        }

        if ((op & 0xFE00) == 0x9000) {                        // ld, lpm, pop
            int low = 0;
            int mode = 0;   // 0 plain, 1 post-increment, 2 pre-decrement
            bool flash = false;
            switch (op & 0x000F) {
            case 0x1: low = 30; mode = 1; break;
            case 0x2: low = 30; mode = 2; break;
            case 0x4: low = 30; mode = 0; flash = true; break;
            case 0x5: low = 30; mode = 1; flash = true; break;
            case 0x9: low = 28; mode = 1; break;
            case 0xA: low = 28; mode = 2; break;
            case 0xC: low = 26; mode = 0; break;
            case 0xD: low = 26; mode = 1; break;
            case 0xE: low = 26; mode = 2; break;
            case 0xF: {                                       // pop
                // SP is incremented first and the read uses the new value, and
                // it is re-read from State rather than kept, because a store
                // to SP's own address would otherwise be lost.
                a_.ldrh(2, kState, kOffSp);
                a_.addi(2, 2, 1);
                a_.uxth(2, 2);
                a_.strh(2, kState, kOffSp);
                call(reinterpret_cast<void*>(&ardio_jit_read_data));
                st_r(d5, 0);
                Emit e = straight(2);
                e.touches_data = true;
                return e;
            }
            default:
                return unsupported;
            }

            // The pointer is held in w22 across the call because the address
            // the write-back is computed from is the one used for the access,
            // and the register file is itself addressable, so re-reading the
            // pair afterwards could see a value the access had just changed.
            ld_pair(kKeep, low);
            if (mode == 2) {
                a_.subi(kKeep, kKeep, 1);
                a_.uxth(kKeep, kKeep);
                st_pair(low, kKeep);
            }
            a_.mov32(2, kKeep);
            call(reinterpret_cast<void*>(flash ? &ardio_jit_read_flash
                                               : &ardio_jit_read_data));
            st_r(d5, 0);
            if (mode == 1) {
                a_.addi(kKeep, kKeep, 1);
                a_.uxth(kKeep, kKeep);
                st_pair(low, kKeep);
            }
            Emit e = straight(flash ? 3 : 2);
            e.touches_data = !flash;
            return e;
        }

        if ((op & 0xFE00) == 0x9200) {                        // st and push
            int low = 0;
            int mode = 0;
            switch (op & 0x000F) {
            case 0x1: low = 30; mode = 1; break;
            case 0x2: low = 30; mode = 2; break;
            case 0x9: low = 28; mode = 1; break;
            case 0xA: low = 28; mode = 2; break;
            case 0xC: low = 26; mode = 0; break;
            case 0xD: low = 26; mode = 1; break;
            case 0xE: low = 26; mode = 2; break;
            case 0xF: {                                       // push
                // The write happens at the current SP and the decrement is
                // applied to whatever SP holds afterwards, matching the
                // reference, which re-reads it for the same reason.
                a_.ldrh(2, kState, kOffSp);
                ld_r(3, d5);
                call(reinterpret_cast<void*>(&ardio_jit_write_data));
                a_.ldrh(0, kState, kOffSp);
                a_.subi(0, 0, 1);
                a_.uxth(0, 0);
                a_.strh(0, kState, kOffSp);
                Emit e = straight(2);
                e.touches_data = true;
                return e;
            }
            default:
                return unsupported;
            }

            ld_pair(kKeep, low);
            if (mode == 2) {
                a_.subi(kKeep, kKeep, 1);
                a_.uxth(kKeep, kKeep);
                st_pair(low, kKeep);
            }
            a_.mov32(2, kKeep);
            ld_r(3, d5);
            call(reinterpret_cast<void*>(&ardio_jit_write_data));
            if (mode == 1) {
                a_.addi(kKeep, kKeep, 1);
                a_.uxth(kKeep, kKeep);
                st_pair(low, kKeep);
            }
            Emit e = straight(2);
            e.touches_data = true;
            return e;
        }

        if ((op & 0xFE00) == 0x9400) {
            if ((op & 0xFE0E) == 0x940C) {                    // jmp
                set_pc(second);
                add_cycles(3);
                Emit e;
                e.kind = Emit::Terminator;
                e.max_cycles = 3;
                e.words = 2;
                return e;
            }
            if ((op & 0xFE0E) == 0x940E) {                    // call
                a_.movi32(2, unsigned(uint16_t(pc_of_op + 2)));
                call(reinterpret_cast<void*>(&ardio_jit_push_pc));
                set_pc(second);
                add_cycles(4);
                Emit e;
                e.kind = Emit::Terminator;
                e.max_cycles = 4;
                e.words = 2;
                e.touches_data = true;
                return e;
            }

            switch (op & 0x000F) {
            case 0x0: {                                       // com
                ld_r(0, d5);
                a_.mvn(2, 0);
                a_.uxtb(2, 2);
                st_r(d5, 2);
                a_.movi32(7, 1);
                st_flag(kOffC, 7);   // com always sets C
                flags_logic(2);
                return straight(1);
            }
            case 0x1: {                                       // neg
                ld_r(0, d5);
                a_.sub(2, kZR, 0);
                a_.uxtb(2, 2);
                st_r(d5, 2);
                // The subtract rules with a minuend of zero, worked through the
                // same way the reference works them through: the borrow is set
                // for any non-zero result, and 0x80 is the one value whose
                // negation does not fit.
                a_.ubfx(4, 0, 3, 1);
                a_.ubfx(5, 2, 3, 1);
                a_.orrr(6, 4, 5);
                st_flag(kOffH, 6);
                a_.movi32(7, 0x80);
                a_.sub(8, 2, 7);
                a_.cmpz(8);
                a_.cset(14, kEQ);
                st_flag(kOffV, 14);
                a_.ubfx(6, 2, 7, 1);
                st_flag(kOffN, 6);
                a_.cmpz(2);
                a_.cset(8, kEQ);
                st_flag(kOffZ, 8);
                a_.cset(9, kNE);
                st_flag(kOffC, 9);
                a_.eorr(17, 6, 14);
                st_flag(kOffS, 17);
                return straight(1);
            }
            case 0x2: {                                       // swap, no flags
                ld_r(0, d5);
                a_.lsri(1, 0, 4);
                a_.lsli(2, 0, 4);
                a_.orrr(2, 2, 1);
                a_.uxtb(2, 2);
                st_r(d5, 2);
                return straight(1);
            }
            case 0x3: {                                       // inc
                ld_r(0, d5);
                a_.addi(2, 0, 1);
                a_.uxtb(2, 2);
                st_r(d5, 2);
                // C is deliberately left alone, which is what lets inc sit
                // inside a multi-byte add without destroying the carry.
                a_.movi32(7, 0x7F);
                a_.sub(8, 0, 7);
                a_.cmpz(8);
                a_.cset(14, kEQ);
                st_flag(kOffV, 14);
                a_.ubfx(6, 2, 7, 1);
                st_flag(kOffN, 6);
                store_z(2, true);
                a_.eorr(17, 6, 14);
                st_flag(kOffS, 17);
                return straight(1);
            }
            case 0xA: {                                       // dec
                ld_r(0, d5);
                a_.subi(2, 0, 1);
                a_.uxtb(2, 2);
                st_r(d5, 2);
                a_.movi32(7, 0x80);
                a_.sub(8, 0, 7);
                a_.cmpz(8);
                a_.cset(14, kEQ);
                st_flag(kOffV, 14);
                a_.ubfx(6, 2, 7, 1);
                st_flag(kOffN, 6);
                store_z(2, true);
                a_.eorr(17, 6, 14);
                st_flag(kOffS, 17);
                return straight(1);
            }
            case 0x5:                                         // asr
            case 0x6:                                         // lsr
            case 0x7: {                                       // ror
                int form = op & 0x000F;
                ld_r(0, d5);
                a_.lsri(2, 0, 1);
                if (form == 0x5) {                            // sign preserved
                    a_.movi32(7, 0x80);
                    a_.andr(7, 0, 7);
                    a_.orrr(2, 2, 7);
                } else if (form == 0x7) {                     // carry shifted in
                    ld_flag(7, kOffC);
                    a_.lsli(7, 7, 7);
                    a_.orrr(2, 2, 7);
                }
                a_.uxtb(2, 2);
                st_r(d5, 2);
                a_.ubfx(15, 0, 0, 1);   // C is the bit shifted out
                st_flag(kOffC, 15);
                if (form == 0x6) a_.movi32(6, 0);   // lsr shifts zero into bit 7
                else a_.ubfx(6, 2, 7, 1);
                st_flag(kOffN, 6);
                // V is N xor C after the shift rather than the usual signed
                // overflow rule, so that brlt still means something here.
                a_.eorr(14, 6, 15);
                st_flag(kOffV, 14);
                store_z(2, true);
                a_.eorr(17, 6, 14);
                st_flag(kOffS, 17);
                return straight(1);
            }
            case 0x8: {
                if (op == 0x9508) {                           // ret
                    call(reinterpret_cast<void*>(&ardio_jit_pop_pc));
                    a_.strh(0, kState, kOffPc);
                    add_cycles(4);
                    Emit e;
                    e.kind = Emit::Terminator;
                    e.max_cycles = 4;
                    e.touches_data = true;
                    return e;
                }
                // bset, bclr, reti, sleep, break, wdr and the implicit lpm all
                // encode here. They are left to the reference core: three of
                // them change the interrupt enable or stop the run, and none is
                // hot enough to be worth the risk.
                return unsupported;
            }
            case 0x9: {
                if (op == 0x9409) {                           // ijmp
                    ld_pair(0, 30);   // Z is a WORD address here
                    a_.strh(0, kState, kOffPc);
                    add_cycles(2);
                    Emit e;
                    e.kind = Emit::Terminator;
                    e.max_cycles = 2;
                    return e;
                }
                if (op == 0x9509) {                           // icall
                    a_.movi32(2, next);
                    call(reinterpret_cast<void*>(&ardio_jit_push_pc));
                    ld_pair(0, 30);
                    a_.strh(0, kState, kOffPc);
                    add_cycles(3);
                    Emit e;
                    e.kind = Emit::Terminator;
                    e.max_cycles = 3;
                    e.touches_data = true;
                    return e;
                }
                return unsupported;
            }
            default:
                return unsupported;
            }
        }

        if ((op & 0xFF00) == 0x9600 || (op & 0xFF00) == 0x9700) {   // adiw, sbiw
            int low = 24 + ((op >> 4) & 0x03) * 2;
            unsigned k = ((op & 0x00C0) >> 2) | (op & 0x000F);
            bool is_add = (op & 0x0100) == 0;
            ld_pair(0, low);
            if (is_add) a_.addi(1, 0, k);
            else a_.subi(1, 0, k);
            a_.uxth(1, 1);
            st_pair(low, 1);
            // The flags come from bit 15 of the sixteen-bit values, not from
            // the high byte considered on its own.
            a_.ubfx(4, 0, 15, 1);
            a_.ubfx(6, 1, 15, 1);
            a_.mvn(9, 0);
            a_.mvn(10, 1);
            a_.ubfx(11, 9, 15, 1);
            a_.ubfx(13, 10, 15, 1);
            if (is_add) {
                a_.andr(14, 11, 6);   // V = ~a15 . res15
                a_.andr(7, 13, 4);    // C = ~res15 . a15
            } else {
                a_.andr(14, 4, 13);   // V = a15 . ~res15
                a_.andr(7, 6, 11);    // C = res15 . ~a15
            }
            st_flag(kOffV, 14);
            st_flag(kOffC, 7);
            st_flag(kOffN, 6);
            store_z(1, true);
            a_.eorr(17, 6, 14);
            st_flag(kOffS, 17);
            return straight(2);
        }

        // cbi, sbi, sbic, sbis and mul all encode below here, and all are left
        // to the reference core.
        return unsupported;
    }

    case 0xB: {                                               // in and out
        unsigned io = unsigned(((op & 0x0600) >> 5) | (op & 0x000F));
        a_.movi32(2, io + 0x20);
        if (op & 0x0800) {                                    // out
            ld_r(3, d5);
            call(reinterpret_cast<void*>(&ardio_jit_write_data));
        } else {                                              // in
            call(reinterpret_cast<void*>(&ardio_jit_read_data));
            st_r(d5, 0);
        }
        Emit e = straight(1);
        e.touches_data = true;
        return e;
    }

    case 0xC: {                                               // rjmp
        int16_t k = int16_t(int16_t(op << 4) >> 4);   // signed, counted in words
        set_pc(uint16_t(next + k));
        add_cycles(2);
        Emit e;
        e.kind = Emit::Terminator;
        e.max_cycles = 2;
        return e;
    }

    case 0xD: {                                               // rcall
        int16_t k = int16_t(int16_t(op << 4) >> 4);
        a_.movi32(2, next);
        call(reinterpret_cast<void*>(&ardio_jit_push_pc));
        set_pc(uint16_t(next + k));
        add_cycles(3);
        Emit e;
        e.kind = Emit::Terminator;
        e.max_cycles = 3;
        e.touches_data = true;
        return e;
    }

    case 0xE:                                                 // ldi
        a_.movi32(0, k8);
        st_r(d4, 0);
        return straight(1);

    case 0xF: {
        if ((op & 0xF800) == 0xF000) {                        // brbs and brbc
            int16_t k = int16_t(int16_t(op << 6) >> 9);   // seven signed bits
            uint16_t target = uint16_t(next + k);
            // The flags are stored unpacked, so the bit the branch names is
            // read directly rather than assembled into an SREG byte first.
            ld_flag(0, kFlagOffsetForBit[b3]);
            a_.cmpz(0);
            Cond taken = (op & 0x0400) ? kEQ : kNE;   // 0xF4xx is brbc
            a_.movi32(1, target);
            a_.movi32(2, next);
            a_.csel32(1, 1, 2, taken);
            a_.strh(1, kState, kOffPc);
            a_.addi64(kCycles, kCycles, 1);
            a_.addi64(3, kCycles, 1);
            a_.csel64(kCycles, 3, kCycles, taken);
            Emit e;
            e.kind = Emit::Terminator;
            e.max_cycles = 2;
            return e;
        }
        if ((op & 0xFE08) == 0xF800) {                        // bld
            ld_r(0, d5);
            a_.movi32(1, 1u << b3);
            a_.orrr(2, 0, 1);          // the bit set
            a_.mvn(3, 1);
            a_.andr(3, 0, 3);          // the bit cleared
            ld_flag(4, kOffT);
            a_.cmpz(4);
            a_.csel32(2, 3, 2, kEQ);
            a_.uxtb(2, 2);
            st_r(d5, 2);
            return straight(1);
        }
        if ((op & 0xFE08) == 0xFA00) {                        // bst
            ld_r(0, d5);
            a_.ubfx(1, 0, b3, 1);
            st_flag(kOffT, 1);
            return straight(1);
        }
        if ((op & 0xFE08) == 0xFC00) {                        // sbrc
            ld_r(0, d5);
            a_.ubfx(1, 0, b3, 1);
            a_.cmpz(1);
            return skip_after(pc_of_op, 1, kEQ);
        }
        if ((op & 0xFE08) == 0xFE00) {                        // sbrs
            ld_r(0, d5);
            a_.ubfx(1, 0, b3, 1);
            a_.cmpz(1);
            return skip_after(pc_of_op, 1, kNE);
        }
        return unsupported;
    }

    default:
        return unsupported;
    }
}

// ------------------------------------------------------ block building ---

constexpr unsigned kMaxInsnsPerBlock = 64;

bool JitCore::translate(State& s, uint16_t word_addr, Block& out) {
    A64 a;

    // Prologue. x19 through x22 are callee-saved under the host ABI and are
    // used to hold guest state across the calls back out, so they are saved
    // here and restored at every exit.
    a.stp_pre(29, 30, -64);
    a.mov_sp_to(29);
    a.stp_off(19, 20, 16);
    a.stp_off(21, 22, 32);
    a.mov64(kState, 0);
    a.mov64(kCore, 1);
    a.ldr64(kCycles, kState, kOffCycles);

    Translator t(a, s.flash, dev_.flash_size);

    const bool has_peripherals = !peripherals_.empty();
    uint16_t pc = word_addr;
    unsigned count = 0;
    unsigned max_cycles = 0;
    bool terminated = false;

    while (count < kMaxInsnsPerBlock) {
        size_t before = a.out.size();
        Emit e = t.one(pc);
        if (e.kind == Emit::Unsupported) {
            // Discard whatever the attempt emitted. The block ends in front of
            // this instruction and the dispatcher hands it to the reference
            // core, so nothing partial may be left behind.
            a.out.resize(before);
            break;
        }

        // A terminator charges its own cycles, because its cost can depend on
        // whether a branch was taken. Everything else has a fixed cost that is
        // known here, so it is added to the running total in one instruction.
        if (e.kind == Emit::Straight && e.max_cycles) a.addi64(kCycles, kCycles, e.max_cycles);

        max_cycles += e.max_cycles;
        ++count;
        pc = uint16_t(pc + e.words);

        if (e.kind == Emit::Terminator) {
            terminated = true;
            break;
        }
        // With a peripheral attached, an access to data space can leave an
        // interrupt pending, and the reference would notice before the next
        // instruction. Ending the block here puts the dispatcher's check in
        // the same place.
        if (has_peripherals && e.touches_data) break;
    }

    if (count == 0) return false;

    if (!terminated) {
        // The block ran out of instructions rather than ending at a control
        // transfer, so the program counter has to be written explicitly.
        a.movi32(0, pc);
        a.strh(0, kState, kOffPc);
    }

    a.str64(kCycles, kState, kOffCycles);
    a.ldp_off(21, 22, 32);
    a.ldp_off(19, 20, 16);
    a.ldp_post(29, 30, 64);
    a.ret();

    if (!slab_.has_room(a.out.size())) {
        // Nothing is executing while we translate, so dropping every cached
        // block and starting the slab over is safe. It costs a re-translation
        // of whatever is hot, which for a 4 MiB slab is a rare event.
        flush();
        if (!slab_.has_room(a.out.size())) return false;
    }

    out.code = reinterpret_cast<BlockFn>(slab_.commit(a.out));
    out.start = word_addr;
    out.end = uint16_t(pc - 1);
    out.max_cycles = max_cycles;
    out.layout = kStateLayout;
    return true;
}

const Block* JitCore::lookup(State& s, uint16_t word_addr) {
    if (translated_flash_ != s.flash) {
        flush();
        translated_flash_ = s.flash;
    }

    auto it = blocks_.find(word_addr);
    if (it != blocks_.end()) {
        // A cached block carries the State layout it was compiled against, so
        // one that outlived a layout change can be dropped rather than run
        // against a struct whose fields have moved.
        if (it->second.layout == kStateLayout) return &it->second;
        blocks_.erase(it);
    }

    Block b;
    if (!translate(s, word_addr, b)) return nullptr;
    return &blocks_.emplace(word_addr, b).first->second;
}

void JitCore::invalidate(uint32_t byte_start, uint32_t byte_end) {
    if (byte_end < byte_start) return;
    // A write to a single byte of flash changes the word containing it, so the
    // range is widened to whole words before anything is compared. Rounding
    // the start up instead would miss a block whose last instruction shares a
    // word with the first byte written.
    uint32_t first_word = byte_start / 2;
    uint32_t last_word = byte_end / 2;

    for (auto it = blocks_.begin(); it != blocks_.end();) {
        const Block& b = it->second;
        if (uint32_t(b.end) >= first_word && uint32_t(b.start) <= last_word)
            it = blocks_.erase(it);
        else
            ++it;
    }
    // The code those blocks occupied is not reclaimed. The slab is a bump
    // allocator and is reset wholesale when it fills, which is the only point
    // at which no translated code can be on the stack.
}

RunResult JitCore::single_step(State& s) {
    uint64_t real_deadline = s.deadline;
    s.deadline = s.cycles + 1;
    RunResult r = ref_->run(s);
    s.deadline = real_deadline;
    return r;
}

RunResult JitCore::run(State& state) {
    // A breakpoint has to be tested before every instruction, which a
    // translated block cannot do, so while any is set the reference core runs
    // the whole thing. It also owns the rule that a breakpoint at the very
    // first instruction of a run does not fire, which is what makes resuming
    // from one make progress.
    if (!breakpoints_.empty()) return ref_->run(state);

    RunResult out;

    for (;;) {
        if (state.cycles >= state.deadline) {
            out.reason = StopReason::Deadline;
            out.pc = state.pc;
            return out;
        }

        // An interrupt is dispatched by handing the next instruction to the
        // reference core, which vectors first and then executes, exactly as it
        // would have on its own.
        bool pending = false;
        if (state.flag_i) {
            for (Peripheral* p : peripherals_)
                if (p->pending_interrupt() != 0) { pending = true; break; }
        }

        const Block* b = pending ? nullptr : lookup(state, state.pc);

        // A block is entered only when the whole of it fits before the
        // deadline. The reference stops in front of the first instruction at
        // which the count has reached the deadline, so a block that could run
        // past it must not be entered at all; single-stepping up to the
        // boundary reproduces the reference's stopping point exactly.
        if (!b || state.cycles + b->max_cycles > state.deadline) {
            RunResult r = single_step(state);
            if (r.reason != StopReason::Deadline) return r;
            continue;
        }

        b->code(&state, this);
    }
}

} // namespace

std::unique_ptr<Core> make_arm64_core(const avr::AvrDevice& device) {
    auto core = std::make_unique<JitCore>(device);
    // The slab is opened here rather than in the constructor so that a host
    // that refuses a MAP_JIT mapping -- a sandbox, or a hardened runtime
    // without the entitlement -- produces a null core and lets the caller fall
    // back to the interpreter, instead of a core that faults on first use.
    if (!core->usable()) return nullptr;
    return core;
}

#else   // not ARM64

// There is no translator for this host. Returning nothing is how the selector
// is told to use the portable core, so emulation still works everywhere.
std::unique_ptr<Core> make_arm64_core(const avr::AvrDevice& device) {
    (void)device;
    return nullptr;
}

#endif

} // namespace ardio::emu
