// Read-only tables held in flash.
//
// A `const` global array that the program only ever subscripts does not need
// to be in SRAM at all: its bytes are already in the flash image, and LPM can
// read them from there. Moving one saves the SRAM it used to occupy and the
// startup code that used to copy it there, which on a chip with 2 KB of SRAM
// and 32 KB of flash is the trade worth making.
//
// The interesting detail is an addressing one. LPM takes a BYTE address in Z,
// while every label the assembler hands out is a WORD address, because the
// instruction stream is 16 bits wide. So a table's byte address is twice its
// label, and getting that conversion wrong produces a table that is subtly,
// quietly wrong -- reading every other byte, or reading the code that came
// before it -- rather than one that fails to build. A test that only checked
// for the presence of an LPM would not notice.
//
// So these tests assemble the generated code and run it on an interpreter that
// decodes the machine words the assembler produced, then read the results back
// out of simulated SRAM. That covers the conversion end to end: the byte the
// program reads has to be the byte the initialiser put there.

#include "harness.h"
#include "ardio/avr/assembler.h"
#include "ardio/avr/compiler.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace flashsim {

// An ATmega-shaped machine covering the instructions ardio's code generator
// emits, plus LPM, which is what these tests are about. Anything else stops
// the run and is reported, so an untested encoding is never mistaken for a
// passing test.
struct Cpu {
    uint8_t r[32] = {};
    uint8_t mem[0x0900] = {};
    uint16_t pc = 0;                 // word address
    uint16_t sp = 0x08FF;
    bool C = false, Z = false, N = false, V = false, T = false;
    bool stopped = false;
    std::string error;

    const std::vector<uint8_t>* image = nullptr;

    uint16_t word(uint16_t w) const {
        size_t i = size_t(w) * 2;
        if (i + 1 >= image->size()) return 0xFFFF;
        return uint16_t((*image)[i] | ((*image)[i + 1] << 8));
    }
    // Flash, addressed in bytes -- what LPM sees through Z.
    uint8_t flash_byte(uint16_t a) const {
        return a < image->size() ? (*image)[a] : 0xFF;
    }
    uint8_t  load(uint16_t a) const { return a < sizeof(mem) ? mem[a] : 0; }
    void     store(uint16_t a, uint8_t v) { if (a < sizeof(mem)) mem[a] = v; }
    void     push(uint8_t v) { store(sp--, v); }
    uint8_t  pop() { return load(++sp); }

    bool S() const { return N != V; }

    void logic_flags(uint8_t res) {
        V = false; N = (res & 0x80) != 0; Z = res == 0;
    }
    void add_flags(uint8_t a, uint8_t b, unsigned res, bool carry_in_zero) {
        C = res > 0xFF;
        uint8_t r8 = uint8_t(res);
        N = (r8 & 0x80) != 0;
        V = (((a ^ b) & 0x80) == 0) && (((a ^ r8) & 0x80) != 0);
        if (carry_in_zero) Z = r8 == 0; else Z = Z && r8 == 0;
    }
    void sub_flags(uint8_t a, uint8_t b, unsigned res, bool plain) {
        C = res > 0xFF;
        uint8_t r8 = uint8_t(res);
        N = (r8 & 0x80) != 0;
        V = (((a ^ b) & 0x80) != 0) && (((a ^ r8) & 0x80) != 0);
        if (plain) Z = r8 == 0; else Z = Z && r8 == 0;
    }
};

// Decodes and runs one instruction. Returns false once the machine has
// stopped, either at the entry point's halt loop or on an unknown opcode.
bool step(Cpu& c) {
    if (c.stopped) return false;
    uint16_t op = c.word(c.pc);
    uint16_t here = c.pc;

    // `.Lhalt: rjmp .Lhalt`, the spin the generator ends main() with. Reaching
    // it means the program ran to completion.
    if (op == 0xCFFF) { c.stopped = true; return false; }
    c.pc++;

    auto rd5 = [&] { return (op >> 4) & 0x1F; };
    auto rr5 = [&] { return unsigned(((op >> 5) & 0x10) | (op & 0x0F)); };
    auto rd4 = [&] { return 16 + ((op >> 4) & 0x0F); };
    auto k8  = [&] { return uint8_t(((op >> 4) & 0xF0) | (op & 0x0F)); };

    // Two-operand arithmetic and logic.
    switch (op & 0xFC00) {
    case 0x0C00: { // add
        unsigned res = c.r[rd5()] + c.r[rr5()];
        c.add_flags(c.r[rd5()], c.r[rr5()], res, true);
        c.r[rd5()] = uint8_t(res); return true;
    }
    case 0x1C00: { // adc
        unsigned res = c.r[rd5()] + c.r[rr5()] + (c.C ? 1 : 0);
        c.add_flags(c.r[rd5()], c.r[rr5()], res, false);
        c.r[rd5()] = uint8_t(res); return true;
    }
    case 0x1800: { // sub
        unsigned res = unsigned(c.r[rd5()]) - c.r[rr5()];
        c.sub_flags(c.r[rd5()], c.r[rr5()], res, true);
        c.C = c.r[rd5()] < c.r[rr5()];
        c.r[rd5()] = uint8_t(res); return true;
    }
    case 0x0800: { // sbc
        unsigned borrow = c.C ? 1u : 0u;
        unsigned res = unsigned(c.r[rd5()]) - c.r[rr5()] - borrow;
        bool new_c = unsigned(c.r[rd5()]) < unsigned(c.r[rr5()]) + borrow;
        c.sub_flags(c.r[rd5()], c.r[rr5()], res, false);
        c.C = new_c;
        c.r[rd5()] = uint8_t(res); return true;
    }
    case 0x2000: { uint8_t v = uint8_t(c.r[rd5()] & c.r[rr5()]);   // and
                   c.logic_flags(v); c.r[rd5()] = v; return true; }
    case 0x2400: { uint8_t v = uint8_t(c.r[rd5()] ^ c.r[rr5()]);   // eor
                   c.logic_flags(v); c.r[rd5()] = v; return true; }
    case 0x2800: { uint8_t v = uint8_t(c.r[rd5()] | c.r[rr5()]);   // or
                   c.logic_flags(v); c.r[rd5()] = v; return true; }
    case 0x1400: { // cp
        unsigned res = unsigned(c.r[rd5()]) - c.r[rr5()];
        c.sub_flags(c.r[rd5()], c.r[rr5()], res, true);
        c.C = c.r[rd5()] < c.r[rr5()];
        return true;
    }
    case 0x0400: { // cpc
        unsigned borrow = c.C ? 1u : 0u;
        unsigned res = unsigned(c.r[rd5()]) - c.r[rr5()] - borrow;
        bool new_c = unsigned(c.r[rd5()]) < unsigned(c.r[rr5()]) + borrow;
        c.sub_flags(c.r[rd5()], c.r[rr5()], res, false);
        c.C = new_c;
        return true;
    }
    case 0x2C00: c.r[rd5()] = c.r[rr5()]; return true;             // mov
    case 0x9C00: {                                                 // mul
        unsigned p = unsigned(c.r[rd5()]) * c.r[rr5()];
        c.r[0] = uint8_t(p); c.r[1] = uint8_t(p >> 8);
        c.C = (p & 0x8000) != 0; c.Z = p == 0; return true;
    }
    default: break;
    }

    // Register-immediate.
    switch (op & 0xF000) {
    case 0x3000: { // cpi
        unsigned res = unsigned(c.r[rd4()]) - k8();
        c.sub_flags(c.r[rd4()], k8(), res, true);
        c.C = c.r[rd4()] < k8();
        return true;
    }
    case 0x4000: { // sbci
        unsigned borrow = c.C ? 1u : 0u;
        unsigned res = unsigned(c.r[rd4()]) - k8() - borrow;
        bool new_c = unsigned(c.r[rd4()]) < unsigned(k8()) + borrow;
        c.sub_flags(c.r[rd4()], k8(), res, false);
        c.C = new_c;
        c.r[rd4()] = uint8_t(res); return true;
    }
    case 0x5000: { // subi
        unsigned res = unsigned(c.r[rd4()]) - k8();
        c.sub_flags(c.r[rd4()], k8(), res, true);
        c.C = c.r[rd4()] < k8();
        c.r[rd4()] = uint8_t(res); return true;
    }
    case 0x6000: { uint8_t v = uint8_t(c.r[rd4()] | k8());         // ori
                   c.logic_flags(v); c.r[rd4()] = v; return true; }
    case 0x7000: { uint8_t v = uint8_t(c.r[rd4()] & k8());         // andi
                   c.logic_flags(v); c.r[rd4()] = v; return true; }
    case 0xE000: c.r[rd4()] = k8(); return true;                   // ldi
    case 0xC000: {                                                 // rjmp
        int16_t k = int16_t(op & 0x0FFF);
        if (k & 0x0800) k = int16_t(k | int16_t(0xF000));
        c.pc = uint16_t(here + 1 + k); return true;
    }
    default: break;
    }

    if ((op & 0xFF00) == 0x0100) {                                 // movw
        unsigned d = ((op >> 4) & 0x0F) * 2, r = (op & 0x0F) * 2;
        c.r[d] = c.r[r]; c.r[d + 1] = c.r[r + 1]; return true;
    }
    if ((op & 0xFE00) == 0x9600) {                                 // adiw / sbiw
        static const unsigned pair[4] = {24, 26, 28, 30};
        unsigned d = pair[(op >> 4) & 3];
        unsigned k = ((op >> 2) & 0x30) | (op & 0x0F);
        unsigned v = unsigned(c.r[d]) | (unsigned(c.r[d + 1]) << 8);
        unsigned res = (op & 0x0100) ? v - k : v + k;
        c.r[d] = uint8_t(res); c.r[d + 1] = uint8_t(res >> 8);
        c.Z = (res & 0xFFFF) == 0; c.N = (res & 0x8000) != 0;
        c.C = (op & 0x0100) ? v < k : res > 0xFFFF; c.V = false;
        return true;
    }
    if ((op & 0xFC00) == 0xF400 || (op & 0xFC00) == 0xF000) {      // brbs / brbc
        unsigned bit = op & 7;
        bool set = (op & 0x0400) == 0;
        bool flag = bit == 0 ? c.C : bit == 1 ? c.Z : bit == 2 ? c.N
                  : bit == 3 ? c.V : bit == 4 ? c.S() : false;
        if (flag == set) {
            int16_t k = int16_t((op >> 3) & 0x7F);
            if (k & 0x40) k = int16_t(k | int16_t(0xFF80));
            c.pc = uint16_t(here + 1 + k);
        }
        return true;
    }
    if ((op & 0xFC08) == 0xFC00) {                                 // sbrc / sbrs
        unsigned r = (op >> 4) & 0x1F, b = op & 7;
        bool bit = (c.r[r] >> b) & 1;
        bool skip_when_set = (op & 0x0200) != 0;
        if (bit == skip_when_set) {
            uint16_t next = c.word(c.pc);
            bool two = (next & 0xFE0E) == 0x940C || (next & 0xFE0E) == 0x940E ||
                       (next & 0xFE0F) == 0x9000 || (next & 0xFE0F) == 0x9200;
            c.pc = uint16_t(c.pc + (two ? 2 : 1));
        }
        return true;
    }
    if (op == 0x95C8) {                                            // lpm: R0 <- (Z)
        c.r[0] = c.flash_byte(uint16_t(c.r[30] | (c.r[31] << 8)));
        return true;
    }
    if ((op & 0xFE0F) == 0x900F) { c.r[rd5()] = c.pop(); return true; }   // pop
    if ((op & 0xFE0F) == 0x920F) { c.push(c.r[rd5()]); return true; }     // push
    if ((op & 0xFE0F) == 0x9000) {                                       // lds
        uint16_t a = c.word(c.pc); c.pc++;
        c.r[rd5()] = c.load(a); return true;
    }
    if ((op & 0xFE0F) == 0x9200) {                                       // sts
        uint16_t a = c.word(c.pc); c.pc++;
        c.store(a, c.r[rd5()]); return true;
    }
    if ((op & 0xFE0E) == 0x940C) {                                       // jmp
        c.pc = c.word(c.pc); return true;
    }
    if ((op & 0xFE0E) == 0x940E) {                                       // call
        uint16_t target = c.word(c.pc); c.pc++;
        c.push(uint8_t(c.pc & 0xFF)); c.push(uint8_t(c.pc >> 8));
        c.pc = target; return true;
    }
    if (op == 0x9508) {                                                  // ret
        uint8_t hi = c.pop(), lo = c.pop();
        uint16_t target = uint16_t((hi << 8) | lo);
        if (target == 0xFFFF) { c.stopped = true; return false; }        // sentinel
        c.pc = target; return true;
    }
    if ((op & 0xFE0F) == 0x9400) {                                 // com
        uint8_t v = uint8_t(~c.r[rd5()]);
        c.logic_flags(v); c.C = true; c.r[rd5()] = v; return true;
    }
    if ((op & 0xFE0F) == 0x9401) {                                 // neg
        uint8_t a = c.r[rd5()], v = uint8_t(0u - a);
        c.logic_flags(v); c.C = v != 0; c.V = v == 0x80;
        c.r[rd5()] = v; return true;
    }
    if ((op & 0xFE0F) == 0x9403) {                                 // inc
        uint8_t v = uint8_t(c.r[rd5()] + 1);
        c.V = v == 0x80; c.N = (v & 0x80) != 0; c.Z = v == 0;
        c.r[rd5()] = v; return true;
    }
    if ((op & 0xFE0F) == 0x940A) {                                 // dec
        uint8_t v = uint8_t(c.r[rd5()] - 1);
        c.V = v == 0x7F; c.N = (v & 0x80) != 0; c.Z = v == 0;
        c.r[rd5()] = v; return true;
    }
    if ((op & 0xFE0F) == 0x9405) {                                 // asr
        uint8_t a = c.r[rd5()], v = uint8_t((a >> 1) | (a & 0x80));
        c.C = (a & 1) != 0; c.N = (v & 0x80) != 0; c.Z = v == 0; c.V = c.N != c.C;
        c.r[rd5()] = v; return true;
    }
    if ((op & 0xFE0F) == 0x9406) {                                 // lsr
        uint8_t a = c.r[rd5()], v = uint8_t(a >> 1);
        c.C = (a & 1) != 0; c.N = false; c.Z = v == 0; c.V = c.C;
        c.r[rd5()] = v; return true;
    }
    if ((op & 0xFE0F) == 0x9407) {                                 // ror
        uint8_t a = c.r[rd5()], v = uint8_t((a >> 1) | (c.C ? 0x80 : 0));
        c.C = (a & 1) != 0; c.N = (v & 0x80) != 0; c.Z = v == 0; c.V = c.N != c.C;
        c.r[rd5()] = v; return true;
    }
    if ((op & 0xD000) == 0x8000) {                                 // ld/st with q
        unsigned q = (op & 7) | ((op >> 7) & 0x18) | ((op >> 8) & 0x20);
        unsigned base = (op & 8) ? 28u : 30u;                      // Y or Z
        uint16_t addr = uint16_t((c.r[base] | (c.r[base + 1] << 8)) + q);
        if (op & 0x0200) c.store(addr, c.r[rd5()]);
        else c.r[rd5()] = c.load(addr);
        return true;
    }
    if ((op & 0xFC00) == 0x9000) {                                 // ld/st, X/Y/Z+-
        unsigned kind = op & 0x0F;
        unsigned base = (kind == 0x0C || kind == 0x0D || kind == 0x0E) ? 26u
                      : (kind == 0x08 || kind == 0x09 || kind == 0x0A) ? 28u : 30u;
        uint16_t p = uint16_t(c.r[base] | (c.r[base + 1] << 8));
        bool post = kind == 0x01 || kind == 0x09 || kind == 0x0D;
        bool pre  = kind == 0x02 || kind == 0x0A || kind == 0x0E;
        if (pre) --p;
        if (op & 0x0200) c.store(p, c.r[rd5()]);
        else c.r[rd5()] = c.load(p);
        uint16_t after = post ? uint16_t(p + 1) : p;
        c.r[base] = uint8_t(after); c.r[base + 1] = uint8_t(after >> 8);
        return true;
    }
    if ((op & 0xF000) == 0xB000) {                                 // in / out
        unsigned a = (op & 0x0F) | ((op >> 5) & 0x30);
        auto sreg = [&]() -> uint8_t {
            return uint8_t((c.C ? 1 : 0) | (c.Z ? 2 : 0) | (c.N ? 4 : 0) |
                           (c.V ? 8 : 0) | (c.S() ? 0x10 : 0) | (c.T ? 0x40 : 0));
        };
        if (op & 0x0800) {                                         // out
            uint8_t v = c.r[rd5()];
            if (a == 0x3D) c.sp = uint16_t((c.sp & 0xFF00) | v);
            else if (a == 0x3E) c.sp = uint16_t((c.sp & 0x00FF) | (v << 8));
            else if (a == 0x3F) {
                c.C = v & 1; c.Z = v & 2; c.N = v & 4; c.V = v & 8; c.T = v & 0x40;
            }
        } else {                                                   // in
            uint8_t v = 0;
            if (a == 0x3D) v = uint8_t(c.sp & 0xFF);
            else if (a == 0x3E) v = uint8_t(c.sp >> 8);
            else if (a == 0x3F) v = sreg();
            c.r[rd5()] = v;
        }
        return true;
    }
    if (op == 0x94F8 || op == 0x9478) return true;                 // cli / sei
    if (op == 0x0000) return true;                                 // nop

    char buf[64];
    std::snprintf(buf, sizeof buf, "unknown opcode 0x%04X at word %u",
                  unsigned(op), unsigned(here));
    c.error = buf;
    c.stopped = true;
    return false;
}

// Runs a whole image from word 0: the reset preamble, the startup stores, then
// main(), stopping at the halt loop main() returns into.
bool run(Cpu& c, const std::vector<uint8_t>& image, std::string& error) {
    c.image = &image;
    c.push(0xFF);
    c.push(0xFF);
    for (long steps = 0; steps < 2000000; ++steps) {
        if (!step(c)) {
            error = c.error;
            return c.error.empty();
        }
    }
    error = "the program did not stop";
    return false;
}

} // namespace flashsim

namespace {

using ardio::AssembleResult;
using ardio::CompileResult;

// SRAM is laid out upwards from here, so the first global that still needs
// SRAM lands exactly at this address.
constexpr int kSramStart = 0x0100;

// Compiles, assembles and runs a program, leaving the machine to be inspected.
// Returns false with an explanation rather than asserting, so each test can
// report in its own terms.
bool run_program(const std::string& source, flashsim::Cpu& cpu, std::string& why,
                 std::string* assembly = nullptr) {
    CompileResult compiled = ardio::compile_avr(source);
    if (!compiled.ok) { why = "compiler: " + compiled.error; return false; }
    if (assembly) *assembly = compiled.assembly;

    AssembleResult assembled = ardio::assemble(compiled.assembly);
    if (!assembled.ok) { why = "assembler: " + assembled.error; return false; }

    return flashsim::run(cpu, assembled.code, why);
}

int byte_at(const flashsim::Cpu& c, int address) {
    return int(c.mem[size_t(address)]);
}

int word_at(const flashsim::Cpu& c, int address) {
    return int(c.mem[size_t(address)]) | (int(c.mem[size_t(address) + 1]) << 8);
}

// A 16-bit value read back as signed, which is how the source declared it.
int signed_word_at(const flashsim::Cpu& c, int address) {
    return int(int16_t(word_at(c, address)));
}

// The highest SRAM address the code ever names, as a count of bytes in use.
// Every global is reached by an absolute LDS or STS, so scanning those covers
// the whole static allocation -- which is the number that has to fall when a
// table moves to flash.
int sram_bytes_used(const std::string& assembly) {
    int high = kSramStart - 1;
    size_t i = 0;
    while (i < assembly.size()) {
        size_t end = assembly.find('\n', i);
        if (end == std::string::npos) end = assembly.size();
        std::string line = assembly.substr(i, end - i);
        i = end + 1;

        size_t p = line.find_first_not_of(" \t");
        if (p == std::string::npos) continue;
        std::string rest = line.substr(p);

        std::string operands;
        if (rest.compare(0, 4, "sts ") == 0) {
            operands = rest.substr(4);
            size_t comma = operands.find(',');
            if (comma != std::string::npos) operands = operands.substr(0, comma);
        } else if (rest.compare(0, 4, "lds ") == 0) {
            size_t comma = rest.find(',');
            if (comma == std::string::npos) continue;
            operands = rest.substr(comma + 1);
        } else {
            continue;
        }

        size_t digit = operands.find_first_of("0123456789");
        if (digit == std::string::npos) continue;
        if (operands.find_first_not_of(" \t", 0) != digit) continue;  // not a literal
        int address = std::atoi(operands.c_str() + digit);
        if (address > high) high = address;
    }
    return high - kSramStart + 1;
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

} // namespace

// ---------------------------------------------------------------------------
// Values. Each of these reads a table element by element and checks that what
// came back is what the initialiser wrote -- which is only true if the word
// label was doubled into a byte address exactly once.
// ---------------------------------------------------------------------------

TEST(flash_byte_table_reads_back_every_element) {
    // `copy` is written to, so it stays in SRAM at the bottom of the heap;
    // `table` is only read, so it moves to flash.
    const std::string source =
        "const char table[6] = {7, 11, 13, 250, 0, 42};\n"
        "char copy[6];\n"
        "int main() {\n"
        "    for (int i = 0; i < 6; i = i + 1) copy[i] = table[i];\n"
        "    return 0;\n"
        "}\n";

    flashsim::Cpu cpu;
    std::string why, assembly;
    if (!run_program(source, cpu, why, &assembly)) {
        std::printf("    %s\n", why.c_str());
        CHECK(false);
        return;
    }

    CHECK(contains(assembly, "__flash_table:"));
    CHECK(contains(assembly, ".byte"));

    const int expected[6] = {7, 11, 13, 250, 0, 42};
    for (int i = 0; i < 6; ++i) CHECK_EQ(byte_at(cpu, kSramStart + i), expected[i]);
}

TEST(flash_word_table_reads_back_every_element) {
    // Two-byte elements: the index is scaled by two on top of the label
    // doubling, and the two are independent. A table read at half stride would
    // still return element zero correctly and nothing else.
    const std::string source =
        "const int table[5] = {1000, -2, 4660, 32767, -32768};\n"
        "int copy[5];\n"
        "int main() {\n"
        "    for (int i = 0; i < 5; i = i + 1) copy[i] = table[i];\n"
        "    return 0;\n"
        "}\n";

    flashsim::Cpu cpu;
    std::string why, assembly;
    if (!run_program(source, cpu, why, &assembly)) {
        std::printf("    %s\n", why.c_str());
        CHECK(false);
        return;
    }

    CHECK(contains(assembly, "__flash_table:"));

    const int expected[5] = {1000, -2, 4660, 32767, -32768};
    for (int i = 0; i < 5; ++i)
        CHECK_EQ(signed_word_at(cpu, kSramStart + 2 * i), expected[i]);
}

TEST(flash_two_dimensional_table_reads_the_declared_element) {
    // A row of `int grid[3][2]` is four bytes wide, so the outer subscript
    // scales by the row and the inner by the element. Row-major order has to
    // survive the move to flash unchanged.
    const std::string source =
        "const int grid[3][2] = {{11, 12}, {21, 22}, {31, 32}};\n"
        "int copy[6];\n"
        "int main() {\n"
        "    for (int r = 0; r < 3; r = r + 1)\n"
        "        for (int c = 0; c < 2; c = c + 1)\n"
        "            copy[r * 2 + c] = grid[r][c];\n"
        "    return 0;\n"
        "}\n";

    flashsim::Cpu cpu;
    std::string why, assembly;
    if (!run_program(source, cpu, why, &assembly)) {
        std::printf("    %s\n", why.c_str());
        CHECK(false);
        return;
    }

    CHECK(contains(assembly, "__flash_grid:"));

    const int expected[6] = {11, 12, 21, 22, 31, 32};
    for (int i = 0; i < 6; ++i)
        CHECK_EQ(word_at(cpu, kSramStart + 2 * i), expected[i]);
}

TEST(flash_string_table_reads_back_its_characters) {
    const std::string source =
        "const char message[] = \"Flash!\";\n"
        "char copy[7];\n"
        "int main() {\n"
        "    for (int i = 0; i < 7; i = i + 1) copy[i] = message[i];\n"
        "    return 0;\n"
        "}\n";

    flashsim::Cpu cpu;
    std::string why, assembly;
    if (!run_program(source, cpu, why, &assembly)) {
        std::printf("    %s\n", why.c_str());
        CHECK(false);
        return;
    }

    CHECK(contains(assembly, "__flash_message:"));

    std::string read_back;
    for (int i = 0; i < 6; ++i) read_back.push_back(char(byte_at(cpu, kSramStart + i)));
    CHECK(read_back == "Flash!");
    CHECK_EQ(byte_at(cpu, kSramStart + 6), 0);          // the terminator too
}

TEST(flash_table_element_reached_by_a_computed_index) {
    // A constant index could be folded into the address at compile time; a
    // computed one goes through the full scale-and-add path, which is where an
    // address that is off by a factor of two shows up.
    const std::string source =
        "const char table[8] = {100, 101, 102, 103, 104, 105, 106, 107};\n"
        "char picked;\n"
        "int main() {\n"
        "    int i = 0;\n"
        "    while (i < 5) i = i + 1;\n"
        "    picked = table[i + 2];\n"
        "    return 0;\n"
        "}\n";

    flashsim::Cpu cpu;
    std::string why;
    if (!run_program(source, cpu, why)) {
        std::printf("    %s\n", why.c_str());
        CHECK(false);
        return;
    }
    CHECK_EQ(byte_at(cpu, kSramStart), 107);
}

// ---------------------------------------------------------------------------
// The addressing detail, asserted directly as well as through the values.
// ---------------------------------------------------------------------------

TEST(flash_table_address_is_its_label_doubled) {
    // The label names a word; LPM wants a byte. The generated code has to say
    // so in the operand rather than relying on the assembler to guess.
    const std::string source =
        "const char table[3] = {1, 2, 3};\n"
        "char first;\n"
        "int main() { first = table[0]; return 0; }\n";

    CompileResult compiled = ardio::compile_avr(source);
    CHECK(compiled.ok);
    if (!compiled.ok) return;

    CHECK(contains(compiled.assembly, "lo8(__flash_table * 2)"));
    CHECK(contains(compiled.assembly, "hi8(__flash_table * 2)"));
    CHECK(contains(compiled.assembly, "lpm"));
}

TEST(flash_table_bytes_land_after_the_last_instruction) {
    // Data before the code would be executed on reset. The table has to follow
    // everything the generator emitted.
    const std::string source =
        "const char table[3] = {1, 2, 3};\n"
        "char first;\n"
        "int main() { first = table[0]; return 0; }\n";

    CompileResult compiled = ardio::compile_avr(source);
    CHECK(compiled.ok);
    if (!compiled.ok) return;

    const size_t table = compiled.assembly.find("__flash_table:");
    const size_t entry = compiled.assembly.find("main:");
    CHECK(table != std::string::npos);
    CHECK(entry != std::string::npos);
    CHECK(table > entry);
}

// ---------------------------------------------------------------------------
// SRAM actually drops. This is the whole point of the exercise, so it is
// measured rather than assumed.
// ---------------------------------------------------------------------------

TEST(flash_placement_frees_the_sram_the_table_used_to_hold) {
    // The same program twice, differing only in the `const`. Without it the
    // table keeps its SRAM; with it the SRAM the table used to occupy is gone.
    const std::string body =
        " char table[40] = {1};\n"
        "char copy;\n"
        "int main() { copy = table[7]; return 0; }\n";

    CompileResult mutable_version = ardio::compile_avr(body);
    CompileResult const_version = ardio::compile_avr("const" + body);
    CHECK(mutable_version.ok);
    CHECK(const_version.ok);
    if (!mutable_version.ok || !const_version.ok) return;

    const int before = sram_bytes_used(mutable_version.assembly);
    const int after = sram_bytes_used(const_version.assembly);
    CHECK_EQ(before, 41);       // 40 bytes of table plus the one-byte copy
    CHECK_EQ(after, 1);         // just copy
    CHECK(after < before);
}

TEST(flash_placement_removes_the_startup_stores_as_well) {
    // The SRAM saving comes with a flash saving: the run of ldi/sts that used
    // to write the table out at startup goes away with it.
    const std::string body =
        " char table[40] = {1, 2, 3, 4, 5};\n"
        "char copy;\n"
        "int main() { copy = table[7]; return 0; }\n";

    CompileResult mutable_version = ardio::compile_avr(body);
    CompileResult const_version = ardio::compile_avr("const" + body);
    CHECK(mutable_version.ok);
    CHECK(const_version.ok);
    if (!mutable_version.ok || !const_version.ok) return;

    auto count_sts = [](const std::string& text) {
        int n = 0;
        for (size_t p = text.find("sts "); p != std::string::npos;
             p = text.find("sts ", p + 1))
            ++n;
        return n;
    };
    CHECK(count_sts(mutable_version.assembly) > 40);
    CHECK(count_sts(const_version.assembly) <= 1);
}

// ---------------------------------------------------------------------------
// Refusals. A table that cannot move keeps the existing behaviour exactly --
// no diagnostic, no dropped store, no changed answer.
// ---------------------------------------------------------------------------

TEST(a_table_that_is_written_to_stays_in_sram) {
    // Flash cannot be written at run time. Placing this table there and then
    // discarding the store would be a miscompile that no amount of reading
    // would reveal, so the store is what has to be detected.
    const std::string source =
        "const char table[4] = {1, 2, 3, 4};\n"
        "char got;\n"
        "int main() {\n"
        "    table[2] = 99;\n"
        "    got = table[2];\n"
        "    return 0;\n"
        "}\n";

    flashsim::Cpu cpu;
    std::string why, assembly;
    if (!run_program(source, cpu, why, &assembly)) {
        std::printf("    %s\n", why.c_str());
        CHECK(false);
        return;
    }

    CHECK(!contains(assembly, "__flash_table"));
    CHECK(!contains(assembly, "lpm"));
    // The store happened, and the read saw it. The table is at the bottom of
    // SRAM and `got` follows it.
    CHECK_EQ(byte_at(cpu, kSramStart + 2), 99);
    CHECK_EQ(byte_at(cpu, kSramStart + 4), 99);
}

TEST(a_table_written_through_a_computed_index_stays_in_sram) {
    // The same refusal when the subscript is not a constant, which is the case
    // an analysis that only looked at literal indices would miss.
    const std::string source =
        "const char table[4] = {5, 6, 7, 8};\n"
        "char got;\n"
        "int main() {\n"
        "    for (int i = 0; i < 4; i = i + 1) table[i] = i + 20;\n"
        "    got = table[3];\n"
        "    return 0;\n"
        "}\n";

    flashsim::Cpu cpu;
    std::string why, assembly;
    if (!run_program(source, cpu, why, &assembly)) {
        std::printf("    %s\n", why.c_str());
        CHECK(false);
        return;
    }

    CHECK(!contains(assembly, "__flash_table"));
    CHECK_EQ(byte_at(cpu, kSramStart + 3), 23);
    CHECK_EQ(byte_at(cpu, kSramStart + 4), 23);
}

TEST(a_table_written_to_through_a_compound_assignment_stays_in_sram) {
    const std::string source =
        "const char table[3] = {1, 2, 3};\n"
        "char got;\n"
        "int main() {\n"
        "    table[1] += 10;\n"
        "    got = table[1];\n"
        "    return 0;\n"
        "}\n";

    flashsim::Cpu cpu;
    std::string why, assembly;
    if (!run_program(source, cpu, why, &assembly)) {
        std::printf("    %s\n", why.c_str());
        CHECK(false);
        return;
    }

    CHECK(!contains(assembly, "__flash_table"));
    CHECK_EQ(byte_at(cpu, kSramStart + 1), 12);
    CHECK_EQ(byte_at(cpu, kSramStart + 3), 12);
}

TEST(a_table_whose_address_is_taken_stays_in_sram) {
    // A flash table has no data address: handing one to a pointer that is then
    // read with LD would read whatever happens to sit at that SRAM address.
    const std::string source =
        "const char table[3] = {4, 5, 6};\n"
        "char got;\n"
        "int main() {\n"
        "    char* p = table;\n"
        "    got = p[2];\n"
        "    return 0;\n"
        "}\n";

    flashsim::Cpu cpu;
    std::string why, assembly;
    if (!run_program(source, cpu, why, &assembly)) {
        std::printf("    %s\n", why.c_str());
        CHECK(false);
        return;
    }

    CHECK(!contains(assembly, "__flash_table"));
    CHECK_EQ(byte_at(cpu, kSramStart + 3), 6);
}

TEST(a_table_passed_to_a_function_stays_in_sram) {
    const std::string source =
        "const char table[3] = {1, 2, 3};\n"
        "char got;\n"
        "char third(char* p) { return p[2]; }\n"
        "int main() { got = third(table); return 0; }\n";

    flashsim::Cpu cpu;
    std::string why, assembly;
    if (!run_program(source, cpu, why, &assembly)) {
        std::printf("    %s\n", why.c_str());
        CHECK(false);
        return;
    }

    CHECK(!contains(assembly, "__flash_table"));
    CHECK_EQ(byte_at(cpu, kSramStart + 3), 3);
}

TEST(a_partly_subscripted_table_stays_in_sram) {
    // `grid[1]` is a row, not an element: it decays to an address, and a flash
    // address is not one a plain LD can follow.
    const std::string source =
        "const int grid[2][2] = {{1, 2}, {3, 4}};\n"
        "int got;\n"
        "int second(int* row) { return row[1]; }\n"
        "int main() { got = second(grid[1]); return 0; }\n";

    flashsim::Cpu cpu;
    std::string why, assembly;
    if (!run_program(source, cpu, why, &assembly)) {
        std::printf("    %s\n", why.c_str());
        CHECK(false);
        return;
    }

    CHECK(!contains(assembly, "__flash_grid"));
    CHECK_EQ(word_at(cpu, kSramStart + 8), 4);
}

TEST(a_table_without_const_stays_in_sram) {
    // Const-ness is the programmer's own statement that the bytes are fixed.
    // Without it a table that merely happens to go unwritten keeps the SRAM it
    // was declared to occupy.
    const std::string source =
        "char table[3] = {1, 2, 3};\n"
        "char got;\n"
        "int main() { got = table[1]; return 0; }\n";

    CompileResult compiled = ardio::compile_avr(source);
    CHECK(compiled.ok);
    if (!compiled.ok) return;
    CHECK(!contains(compiled.assembly, "__flash_table"));
}

TEST(a_const_local_array_is_untouched) {
    // Only globals are considered: a local lives in the frame, and the token
    // scan that recovers `const` must not mistake one for a global.
    const std::string source =
        "char got;\n"
        "int main() {\n"
        "    const char table[3] = {1, 2, 3};\n"
        "    got = table[2];\n"
        "    return 0;\n"
        "}\n";

    flashsim::Cpu cpu;
    std::string why, assembly;
    if (!run_program(source, cpu, why, &assembly)) {
        std::printf("    %s\n", why.c_str());
        CHECK(false);
        return;
    }

    CHECK(!contains(assembly, "__flash_table"));
    CHECK_EQ(byte_at(cpu, kSramStart), 3);
}

TEST(a_const_scalar_is_unaffected) {
    // Only arrays move; a scalar was already folded into its uses.
    const std::string source =
        "const int limit = 7;\n"
        "int got;\n"
        "int main() { got = limit + 1; return 0; }\n";

    flashsim::Cpu cpu;
    std::string why, assembly;
    if (!run_program(source, cpu, why, &assembly)) {
        std::printf("    %s\n", why.c_str());
        CHECK(false);
        return;
    }

    CHECK(!contains(assembly, "__flash_limit"));
    CHECK_EQ(word_at(cpu, kSramStart + 2), 8);
}

// ---------------------------------------------------------------------------
// Several tables at once, which is the shape a real sketch has: a stroke font
// beside the index that says where each glyph starts in it.
// ---------------------------------------------------------------------------

TEST(two_flash_tables_keep_their_own_bytes) {
    const std::string source =
        "const char strokes[10] = {90, 91, 92, 93, 94, 95, 96, 97, 98, 99};\n"
        "const int starts[4] = {0, 3, 7, 10};\n"
        "char picked;\n"
        "int span;\n"
        "int main() {\n"
        "    picked = strokes[starts[1]];\n"
        "    span = starts[2] - starts[1];\n"
        "    return 0;\n"
        "}\n";

    flashsim::Cpu cpu;
    std::string why, assembly;
    if (!run_program(source, cpu, why, &assembly)) {
        std::printf("    %s\n", why.c_str());
        CHECK(false);
        return;
    }

    CHECK(contains(assembly, "__flash_strokes:"));
    CHECK(contains(assembly, "__flash_starts:"));

    // A one-byte table followed by a two-byte one: the second table's label is
    // only word-aligned because the assembler pads the first, and that padding
    // is exactly what a doubled label has to survive.
    CHECK_EQ(byte_at(cpu, kSramStart), 93);            // strokes[starts[1]]
    CHECK_EQ(word_at(cpu, kSramStart + 1), 4);         // starts[2] - starts[1]

    // Neither table costs SRAM any more: only `picked` and `span` do.
    CHECK_EQ(sram_bytes_used(assembly), 3);
}

TEST(an_odd_length_flash_table_does_not_shift_the_next_one) {
    // Nine bytes is an odd run, so the assembler pads before the next label.
    // If the padding were counted into the second table's byte address, every
    // element of it would come back shifted.
    const std::string source =
        "const char odd[9] = {1, 2, 3, 4, 5, 6, 7, 8, 9};\n"
        "const char after[3] = {77, 78, 79};\n"
        "char a;\n"
        "char b;\n"
        "int main() { a = odd[8]; b = after[1]; return 0; }\n";

    flashsim::Cpu cpu;
    std::string why, assembly;
    if (!run_program(source, cpu, why, &assembly)) {
        std::printf("    %s\n", why.c_str());
        CHECK(false);
        return;
    }

    CHECK_EQ(byte_at(cpu, kSramStart), 9);
    CHECK_EQ(byte_at(cpu, kSramStart + 1), 78);
}
