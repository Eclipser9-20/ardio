// Tests for the per-function frame decision.
//
// The code generator only builds a frame when the function turns out to need
// one. That is worth testing from two directions at once: the text has to lack
// the prologue in the cases where it is claimed to be unnecessary, and the
// function has to keep behaving identically -- same arguments in the same
// registers, same result in the same register -- when it does. So every shape
// below is both inspected and *run*, on a small interpreter for the subset of
// AVR the generator emits.

#include "harness.h"

#include "ardio/avr/assembler.h"
#include "ardio/avr/codegen.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace ardio;

namespace {

// ------------------------------------------------------------ tree building --

TypePtr int_type() { return make_type(TypeKind::Int); }
TypePtr char_type() { return make_type(TypeKind::Char); }
TypePtr void_type() { return make_type(TypeKind::Void); }

ExprPtr literal(long v) {
    auto e = std::make_unique<Expr>();
    e->kind = ExprKind::IntLiteral;
    e->int_value = v;
    e->type = int_type();
    return e;
}

ExprPtr identifier(const std::string& name, TypePtr t = int_type()) {
    auto e = std::make_unique<Expr>();
    e->kind = ExprKind::Identifier;
    e->name = name;
    e->type = std::move(t);
    return e;
}

ExprPtr binary(const std::string& op, ExprPtr a, ExprPtr b) {
    auto e = std::make_unique<Expr>();
    e->kind = ExprKind::Binary;
    e->op = op;
    e->lhs = std::move(a);
    e->rhs = std::move(b);
    e->type = int_type();
    return e;
}

ExprPtr assign(ExprPtr target, ExprPtr value) {
    auto e = std::make_unique<Expr>();
    e->kind = ExprKind::Assign;
    e->op = "=";
    e->lhs = std::move(target);
    e->rhs = std::move(value);
    e->type = int_type();
    return e;
}

ExprPtr index(ExprPtr base, ExprPtr subscript, TypePtr element) {
    auto e = std::make_unique<Expr>();
    e->kind = ExprKind::Index;
    e->lhs = std::move(base);
    e->rhs = std::move(subscript);
    e->type = std::move(element);
    return e;
}

ExprPtr call(const std::string& name, std::vector<ExprPtr> args) {
    auto e = std::make_unique<Expr>();
    e->kind = ExprKind::Call;
    e->name = name;
    e->args = std::move(args);
    e->type = int_type();
    return e;
}

StmtPtr ret(ExprPtr value) {
    auto s = std::make_unique<Stmt>();
    s->kind = StmtKind::Return;
    s->expr = std::move(value);
    return s;
}

StmtPtr expr_stmt(ExprPtr value) {
    auto s = std::make_unique<Stmt>();
    s->kind = StmtKind::Expression;
    s->expr = std::move(value);
    return s;
}

StmtPtr declare(const std::string& name, ExprPtr init, TypePtr t = int_type()) {
    auto s = std::make_unique<Stmt>();
    s->kind = StmtKind::VarDecl;
    s->var_name = name;
    s->var_type = std::move(t);
    s->var_init = std::move(init);
    return s;
}

StmtPtr block(std::vector<StmtPtr> stmts) {
    auto s = std::make_unique<Stmt>();
    s->kind = StmtKind::Block;
    s->body = std::move(stmts);
    return s;
}

// A whole function, ready for gen_function.
Function function(const std::string& name, std::vector<Param> params,
                  std::vector<StmtPtr> body, TypePtr result = int_type()) {
    Function f;
    f.name = name;
    f.return_type = std::move(result);
    f.params = std::move(params);
    f.body = block(std::move(body));
    return f;
}

bool has(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}

// True if the function saved or moved the frame pointer anywhere.
bool builds_a_frame(const std::string& text) {
    return has(text, "r28") || has(text, "r29") || has(text, "Y+");
}

// ------------------------------------------------------------- the machine --

// Enough of an ATmega to run what the generator writes. Anything outside that
// subset stops the run and is reported, so a wrong encoding can never be
// mistaken for a passing test.
struct Machine {
    uint8_t r[32] = {};
    uint8_t mem[0x0900] = {};
    uint16_t pc = 0;                    // word address
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
    uint8_t load(uint16_t a) const { return a < sizeof mem ? mem[a] : 0; }
    void store(uint16_t a, uint8_t v) { if (a < sizeof mem) mem[a] = v; }
    void push(uint8_t v) { store(sp--, v); }
    uint8_t pop() { return load(++sp); }
    bool S() const { return N != V; }

    void logic_flags(uint8_t res) { V = false; N = (res & 0x80) != 0; Z = res == 0; }
    void add_flags(uint8_t a, uint8_t b, unsigned res, bool plain) {
        C = res > 0xFF;
        uint8_t r8 = uint8_t(res);
        N = (r8 & 0x80) != 0;
        V = (((a ^ b) & 0x80) == 0) && (((a ^ r8) & 0x80) != 0);
        Z = plain ? r8 == 0 : (Z && r8 == 0);
    }
    void sub_flags(uint8_t a, uint8_t b, unsigned res, bool plain) {
        uint8_t r8 = uint8_t(res);
        N = (r8 & 0x80) != 0;
        V = (((a ^ b) & 0x80) != 0) && (((a ^ r8) & 0x80) != 0);
        Z = plain ? r8 == 0 : (Z && r8 == 0);
    }
};

bool step(Machine& m) {
    if (m.stopped) return false;
    uint16_t op = m.word(m.pc);
    uint16_t here = m.pc;
    m.pc++;

    auto rd5 = [&] { return unsigned((op >> 4) & 0x1F); };
    auto rr5 = [&] { return unsigned(((op >> 5) & 0x10) | (op & 0x0F)); };
    auto rd4 = [&] { return unsigned(16 + ((op >> 4) & 0x0F)); };
    auto k8  = [&] { return uint8_t(((op >> 4) & 0xF0) | (op & 0x0F)); };

    switch (op & 0xFC00) {
    case 0x0C00: {                                              // add
        unsigned res = unsigned(m.r[rd5()]) + m.r[rr5()];
        m.add_flags(m.r[rd5()], m.r[rr5()], res, true);
        m.r[rd5()] = uint8_t(res); return true;
    }
    case 0x1C00: {                                              // adc
        unsigned res = unsigned(m.r[rd5()]) + m.r[rr5()] + (m.C ? 1u : 0u);
        m.add_flags(m.r[rd5()], m.r[rr5()], res, false);
        m.r[rd5()] = uint8_t(res); return true;
    }
    case 0x1800: {                                              // sub
        unsigned res = unsigned(m.r[rd5()]) - m.r[rr5()];
        m.sub_flags(m.r[rd5()], m.r[rr5()], res, true);
        m.C = m.r[rd5()] < m.r[rr5()];
        m.r[rd5()] = uint8_t(res); return true;
    }
    case 0x0800: {                                              // sbc
        unsigned borrow = m.C ? 1u : 0u;
        unsigned res = unsigned(m.r[rd5()]) - m.r[rr5()] - borrow;
        bool carry = unsigned(m.r[rd5()]) < unsigned(m.r[rr5()]) + borrow;
        m.sub_flags(m.r[rd5()], m.r[rr5()], res, false);
        m.C = carry;
        m.r[rd5()] = uint8_t(res); return true;
    }
    case 0x2000: {                                              // and
        uint8_t v = uint8_t(m.r[rd5()] & m.r[rr5()]);
        m.logic_flags(v); m.r[rd5()] = v; return true;
    }
    case 0x2400: {                                              // eor
        uint8_t v = uint8_t(m.r[rd5()] ^ m.r[rr5()]);
        m.logic_flags(v); m.r[rd5()] = v; return true;
    }
    case 0x2800: {                                              // or
        uint8_t v = uint8_t(m.r[rd5()] | m.r[rr5()]);
        m.logic_flags(v); m.r[rd5()] = v; return true;
    }
    case 0x1400: {                                              // cp
        unsigned res = unsigned(m.r[rd5()]) - m.r[rr5()];
        m.sub_flags(m.r[rd5()], m.r[rr5()], res, true);
        m.C = m.r[rd5()] < m.r[rr5()]; return true;
    }
    case 0x0400: {                                              // cpc
        unsigned borrow = m.C ? 1u : 0u;
        unsigned res = unsigned(m.r[rd5()]) - m.r[rr5()] - borrow;
        bool carry = unsigned(m.r[rd5()]) < unsigned(m.r[rr5()]) + borrow;
        m.sub_flags(m.r[rd5()], m.r[rr5()], res, false);
        m.C = carry; return true;
    }
    case 0x2C00: m.r[rd5()] = m.r[rr5()]; return true;          // mov
    case 0x9C00: {                                              // mul
        unsigned p = unsigned(m.r[rd5()]) * m.r[rr5()];
        m.r[0] = uint8_t(p); m.r[1] = uint8_t(p >> 8);
        m.C = (p & 0x8000) != 0; m.Z = p == 0; return true;
    }
    default: break;
    }

    switch (op & 0xF000) {
    case 0x3000: {                                              // cpi
        unsigned res = unsigned(m.r[rd4()]) - k8();
        m.sub_flags(m.r[rd4()], k8(), res, true);
        m.C = m.r[rd4()] < k8(); return true;
    }
    case 0x4000: {                                              // sbci
        unsigned borrow = m.C ? 1u : 0u;
        unsigned res = unsigned(m.r[rd4()]) - k8() - borrow;
        bool carry = unsigned(m.r[rd4()]) < unsigned(k8()) + borrow;
        m.sub_flags(m.r[rd4()], k8(), res, false);
        m.C = carry; m.r[rd4()] = uint8_t(res); return true;
    }
    case 0x5000: {                                              // subi
        unsigned res = unsigned(m.r[rd4()]) - k8();
        m.sub_flags(m.r[rd4()], k8(), res, true);
        m.C = m.r[rd4()] < k8();
        m.r[rd4()] = uint8_t(res); return true;
    }
    case 0x6000: { uint8_t v = uint8_t(m.r[rd4()] | k8());      // ori
                   m.logic_flags(v); m.r[rd4()] = v; return true; }
    case 0x7000: { uint8_t v = uint8_t(m.r[rd4()] & k8());      // andi
                   m.logic_flags(v); m.r[rd4()] = v; return true; }
    case 0xE000: m.r[rd4()] = k8(); return true;                // ldi
    case 0xC000: {                                              // rjmp
        int16_t k = int16_t(op & 0x0FFF);
        if (k & 0x0800) k = int16_t(k | int16_t(0xF000));
        m.pc = uint16_t(here + 1 + k); return true;
    }
    default: break;
    }

    if ((op & 0xFF00) == 0x0100) {                              // movw
        unsigned d = ((op >> 4) & 0x0F) * 2, s = (op & 0x0F) * 2;
        m.r[d] = m.r[s]; m.r[d + 1] = m.r[s + 1]; return true;
    }
    if ((op & 0xFE00) == 0x9600) {                              // adiw / sbiw
        static const unsigned pair[4] = {24, 26, 28, 30};
        unsigned d = pair[(op >> 4) & 3];
        unsigned k = ((op >> 2) & 0x30) | (op & 0x0F);
        unsigned v = unsigned(m.r[d]) | (unsigned(m.r[d + 1]) << 8);
        unsigned res = (op & 0x0100) ? v - k : v + k;
        m.r[d] = uint8_t(res); m.r[d + 1] = uint8_t(res >> 8);
        m.Z = (res & 0xFFFF) == 0; m.N = (res & 0x8000) != 0;
        m.C = (op & 0x0100) ? v < k : res > 0xFFFF; m.V = false;
        return true;
    }
    if ((op & 0xF800) == 0xF000) {                              // brbs / brbc
        unsigned bit = op & 7;
        bool want_set = (op & 0x0400) == 0;
        bool flag = bit == 0 ? m.C : bit == 1 ? m.Z : bit == 2 ? m.N
                  : bit == 3 ? m.V : bit == 4 ? m.S() : m.T;
        if (flag == want_set) {
            int16_t k = int16_t((op >> 3) & 0x7F);
            if (k & 0x40) k = int16_t(k | int16_t(0xFF80));
            m.pc = uint16_t(here + 1 + k);
        }
        return true;
    }
    if ((op & 0xFC08) == 0xFC00) {                              // sbrc / sbrs
        bool bit = (m.r[(op >> 4) & 0x1F] >> (op & 7)) & 1;
        if (bit == ((op & 0x0200) != 0)) {
            uint16_t next = m.word(m.pc);
            bool two = (next & 0xFE0E) == 0x940C || (next & 0xFE0E) == 0x940E ||
                       (next & 0xFE0F) == 0x9000 || (next & 0xFE0F) == 0x9200;
            m.pc = uint16_t(m.pc + (two ? 2 : 1));
        }
        return true;
    }
    if ((op & 0xFE0F) == 0x900F) { m.r[rd5()] = m.pop(); return true; }   // pop
    if ((op & 0xFE0F) == 0x920F) { m.push(m.r[rd5()]); return true; }     // push
    if ((op & 0xFE0F) == 0x9000) {                                       // lds
        uint16_t a = m.word(m.pc); m.pc++;
        m.r[rd5()] = m.load(a); return true;
    }
    if ((op & 0xFE0F) == 0x9200) {                                       // sts
        uint16_t a = m.word(m.pc); m.pc++;
        m.store(a, m.r[rd5()]); return true;
    }
    if ((op & 0xFE0E) == 0x940E) {                                       // call
        uint16_t target = m.word(m.pc); m.pc++;
        m.push(uint8_t(m.pc & 0xFF)); m.push(uint8_t(m.pc >> 8));
        m.pc = target; return true;
    }
    if ((op & 0xFE0E) == 0x940C) {                                       // jmp
        m.pc = m.word(m.pc); return true;
    }
    if (op == 0x9508) {                                                  // ret
        uint8_t hi = m.pop(), lo = m.pop();
        uint16_t target = uint16_t((hi << 8) | lo);
        if (target == 0xFFFF) { m.stopped = true; return false; }        // sentinel
        m.pc = target; return true;
    }
    if ((op & 0xFE0F) == 0x9400) {                                       // com
        uint8_t v = uint8_t(~m.r[rd5()]);
        m.logic_flags(v); m.C = true; m.r[rd5()] = v; return true;
    }
    if ((op & 0xFE0F) == 0x9401) {                                       // neg
        uint8_t a = m.r[rd5()], v = uint8_t(0u - a);
        m.logic_flags(v); m.C = v != 0; m.V = v == 0x80;
        m.r[rd5()] = v; return true;
    }
    if ((op & 0xFE0F) == 0x9403) {                                       // inc
        uint8_t v = uint8_t(m.r[rd5()] + 1);
        m.V = v == 0x80; m.N = (v & 0x80) != 0; m.Z = v == 0;
        m.r[rd5()] = v; return true;
    }
    if ((op & 0xFE0F) == 0x940A) {                                       // dec
        uint8_t v = uint8_t(m.r[rd5()] - 1);
        m.V = v == 0x7F; m.N = (v & 0x80) != 0; m.Z = v == 0;
        m.r[rd5()] = v; return true;
    }
    if ((op & 0xFE0F) == 0x9405) {                                       // asr
        uint8_t a = m.r[rd5()], v = uint8_t((a >> 1) | (a & 0x80));
        m.C = (a & 1) != 0; m.N = (v & 0x80) != 0; m.Z = v == 0; m.V = m.N != m.C;
        m.r[rd5()] = v; return true;
    }
    if ((op & 0xFE0F) == 0x9406) {                                       // lsr
        uint8_t a = m.r[rd5()], v = uint8_t(a >> 1);
        m.C = (a & 1) != 0; m.N = false; m.Z = v == 0; m.V = m.C;
        m.r[rd5()] = v; return true;
    }
    if ((op & 0xFE0F) == 0x9407) {                                       // ror
        uint8_t a = m.r[rd5()], v = uint8_t((a >> 1) | (m.C ? 0x80 : 0));
        m.C = (a & 1) != 0; m.N = (v & 0x80) != 0; m.Z = v == 0; m.V = m.N != m.C;
        m.r[rd5()] = v; return true;
    }
    if ((op & 0xD000) == 0x8000) {                              // ldd / std
        unsigned q = (op & 7) | ((op >> 7) & 0x18) | ((op >> 8) & 0x20);
        unsigned base = (op & 8) ? 28u : 30u;                   // Y or Z
        uint16_t addr = uint16_t((m.r[base] | (m.r[base + 1] << 8)) + q);
        if (op & 0x0200) m.store(addr, m.r[rd5()]);
        else m.r[rd5()] = m.load(addr);
        return true;
    }
    if ((op & 0xFC00) == 0x9000) {                              // ld / st
        unsigned kind = op & 0x0F;
        unsigned base = (kind == 0x0C || kind == 0x0D || kind == 0x0E) ? 26u
                      : (kind == 0x08 || kind == 0x09 || kind == 0x0A) ? 28u : 30u;
        uint16_t p = uint16_t(m.r[base] | (m.r[base + 1] << 8));
        bool post = kind == 0x01 || kind == 0x09 || kind == 0x0D;
        bool pre  = kind == 0x02 || kind == 0x0A || kind == 0x0E;
        if (pre) --p;
        if (op & 0x0200) m.store(p, m.r[rd5()]);
        else m.r[rd5()] = m.load(p);
        uint16_t after = post ? uint16_t(p + 1) : p;
        m.r[base] = uint8_t(after); m.r[base + 1] = uint8_t(after >> 8);
        return true;
    }
    if ((op & 0xF000) == 0xB000) {                              // in / out
        unsigned a = (op & 0x0F) | ((op >> 5) & 0x30);
        auto sreg = [&]() -> uint8_t {
            return uint8_t((m.C ? 1 : 0) | (m.Z ? 2 : 0) | (m.N ? 4 : 0) |
                           (m.V ? 8 : 0) | (m.S() ? 0x10 : 0) | (m.T ? 0x40 : 0));
        };
        if (op & 0x0800) {                                      // out
            uint8_t v = m.r[rd5()];
            if (a == 0x3D) m.sp = uint16_t((m.sp & 0xFF00) | v);
            else if (a == 0x3E) m.sp = uint16_t((m.sp & 0x00FF) | (v << 8));
            else if (a == 0x3F) {
                m.C = v & 1; m.Z = v & 2; m.N = v & 4; m.V = v & 8; m.T = v & 0x40;
            }
        } else {                                                // in
            uint8_t v = 0;
            if (a == 0x3D) v = uint8_t(m.sp & 0xFF);
            else if (a == 0x3E) v = uint8_t(m.sp >> 8);
            else if (a == 0x3F) v = sreg();
            m.r[rd5()] = v;
        }
        return true;
    }
    if (op == 0x94F8 || op == 0x9478) return true;              // cli / sei
    if (op == 0x0000) return true;                              // nop

    char buf[64];
    std::snprintf(buf, sizeof buf, "unknown opcode 0x%04X at word %u",
                  unsigned(op), unsigned(here));
    m.error = buf;
    m.stopped = true;
    return false;
}

bool run(Machine& m, const std::vector<uint8_t>& image, std::string& error) {
    m.image = &image;
    m.push(0xFF);
    m.push(0xFF);                       // a return address that ends the run
    for (long steps = 0; steps < 200000; ++steps) {
        if (!step(m)) { error = m.error; return m.error.empty(); }
    }
    error = "the program did not stop";
    return false;
}

// ------------------------------------------------------------- the harness --

// Assembles `text` behind a stub that calls `entry`, runs it, and hands back
// the machine. Arguments are placed in registers before the call, exactly as a
// caller would, which is the point: whatever the callee did to its frame, it
// has to be reachable through the ABI alone.
bool call_function(const std::string& text, const std::string& entry,
                   const std::vector<std::pair<int, uint8_t>>& arguments,
                   Machine& machine, std::string& why) {
    std::string program = "call " + entry + "\nret\n" + text + "\n";
    AssembleResult r = assemble(program);
    if (!r.ok) { why = "assembler: " + r.error; return false; }
    machine.r[1] = 0;                   // the ABI's permanent zero
    for (const auto& argument : arguments) machine.r[argument.first] = argument.second;
    return run(machine, r.code, why);
}

uint16_t result16(const Machine& m) {
    return uint16_t(m.r[24] | (m.r[25] << 8));
}

// Runs one generated function with a 16-bit argument in r24:r25 (and an
// optional second in r22:r23) and reports the 16-bit return value.
bool call16(const std::string& text, const std::string& entry, int a, int b,
            uint16_t& result, std::string& why) {
    Machine m;
    std::vector<std::pair<int, uint8_t>> arguments = {
        {24, uint8_t(a & 0xFF)}, {25, uint8_t((a >> 8) & 0xFF)},
        {22, uint8_t(b & 0xFF)}, {23, uint8_t((b >> 8) & 0xFF)},
    };
    if (!call_function(text, entry, arguments, m, why)) return false;
    result = result16(m);
    return true;
}

} // namespace

// ------------------------------------------------------------------ tests --

TEST(frame_absent_when_a_function_has_no_locals_and_no_parameters) {
    CodeGen cg;
    std::vector<StmtPtr> body;
    body.push_back(ret(literal(7)));
    Function f = function("seven", {}, std::move(body));
    cg.gen_function(f);

    CHECK(cg.error.empty());
    CHECK(!builds_a_frame(cg.out));
    CHECK(!has(cg.out, "cli"));

    uint16_t value = 0;
    std::string why;
    CHECK(call16(cg.out, "seven", 0, 0, value, why));
    if (!why.empty()) std::printf("    %s\n", why.c_str());
    CHECK_EQ(value, 7u);
}

TEST(frame_absent_when_a_leaf_returns_its_parameter_unchanged) {
    CodeGen cg;
    std::vector<StmtPtr> body;
    body.push_back(ret(identifier("a")));
    Function f = function("echo", {{"a", int_type()}}, std::move(body));
    cg.gen_function(f);

    CHECK(cg.error.empty());
    CHECK(!builds_a_frame(cg.out));
    // The value never leaves the register it arrived in, so nothing is saved.
    CHECK(!has(cg.out, "push"));
    CHECK(!has(cg.out, "mov"));

    uint16_t value = 0;
    std::string why;
    CHECK(call16(cg.out, "echo", 0x1234, 0, value, why));
    if (!why.empty()) std::printf("    %s\n", why.c_str());
    CHECK_EQ(value, 0x1234u);
}

TEST(frame_absent_when_a_leaf_computes_with_its_parameter) {
    CodeGen cg;
    std::vector<StmtPtr> body;
    body.push_back(ret(binary("+", identifier("a"), literal(1))));
    Function f = function("increment", {{"a", int_type()}}, std::move(body));
    cg.gen_function(f);

    CHECK(cg.error.empty());
    CHECK(!builds_a_frame(cg.out));
    // r24:r25 is needed for the arithmetic, so the parameter is parked in a
    // call-saved register instead of being spilled to a frame.
    CHECK(has(cg.out, "push r2"));

    uint16_t value = 0;
    std::string why;
    CHECK(call16(cg.out, "increment", 1000, 0, value, why));
    if (!why.empty()) std::printf("    %s\n", why.c_str());
    CHECK_EQ(value, 1001u);
}

TEST(frame_absent_for_a_leaf_with_two_parameters) {
    CodeGen cg;
    std::vector<StmtPtr> body;
    body.push_back(ret(binary("-", identifier("a"), identifier("b"))));
    Function f = function("difference", {{"a", int_type()}, {"b", int_type()}},
                          std::move(body));
    cg.gen_function(f);

    CHECK(cg.error.empty());
    CHECK(!builds_a_frame(cg.out));

    uint16_t value = 0;
    std::string why;
    CHECK(call16(cg.out, "difference", 900, 250, value, why));
    if (!why.empty()) std::printf("    %s\n", why.c_str());
    CHECK_EQ(value, 650u);
}

TEST(frame_absent_when_an_unused_parameter_is_never_read) {
    CodeGen cg;
    std::vector<StmtPtr> body;
    body.push_back(ret(literal(3)));
    Function f = function("ignore", {{"a", int_type()}}, std::move(body));
    cg.gen_function(f);

    CHECK(cg.error.empty());
    CHECK(!builds_a_frame(cg.out));
    CHECK(!has(cg.out, "push"));

    uint16_t value = 0;
    std::string why;
    CHECK(call16(cg.out, "ignore", 0xBEEF, 0, value, why));
    if (!why.empty()) std::printf("    %s\n", why.c_str());
    CHECK_EQ(value, 3u);
}

TEST(frame_absent_for_a_char_parameter) {
    CodeGen cg;
    std::vector<StmtPtr> body;
    body.push_back(ret(binary("+", identifier("c", char_type()), literal(2))));
    Function f = function("bump", {{"c", char_type()}}, std::move(body));
    cg.gen_function(f);

    CHECK(cg.error.empty());
    CHECK(!builds_a_frame(cg.out));

    uint16_t value = 0;
    std::string why;
    CHECK(call16(cg.out, "bump", 40, 0, value, why));
    if (!why.empty()) std::printf("    %s\n", why.c_str());
    CHECK_EQ(value, 42u);
}

TEST(a_non_leaf_keeps_its_parameter_across_the_call_without_a_frame) {
    // twice(x) is a leaf; caller() calls it and then still needs its own
    // parameter, which has to survive a call that clobbers r18-r27.
    CodeGen cg;
    std::vector<StmtPtr> twice_body;
    twice_body.push_back(ret(binary("+", identifier("x"), identifier("x"))));
    Function twice = function("twice", {{"x", int_type()}}, std::move(twice_body));
    cg.gen_function(twice);

    std::vector<ExprPtr> args;
    args.push_back(literal(21));
    std::vector<StmtPtr> caller_body;
    caller_body.push_back(ret(binary("+", call("twice", std::move(args)),
                                     identifier("a"))));
    Function caller = function("plus_twice", {{"a", int_type()}},
                               std::move(caller_body));
    cg.gen_function(caller);

    CHECK(cg.error.empty());
    CHECK(!builds_a_frame(cg.out));
    // The parameter is held in a call-saved register, which the ABI says the
    // callee will hand back untouched.
    CHECK(has(cg.out, "push r2"));

    uint16_t value = 0;
    std::string why;
    CHECK(call16(cg.out, "plus_twice", 100, 0, value, why));
    if (!why.empty()) std::printf("    %s\n", why.c_str());
    CHECK_EQ(value, 142u);
}

TEST(a_written_parameter_stays_in_a_register) {
    CodeGen cg;
    std::vector<StmtPtr> body;
    body.push_back(expr_stmt(assign(identifier("a"),
                                    binary("+", identifier("a"), literal(5)))));
    body.push_back(ret(identifier("a")));
    Function f = function("bump_in_place", {{"a", int_type()}}, std::move(body));
    cg.gen_function(f);

    CHECK(cg.error.empty());
    // The assignment lands in the parked register instead of a frame slot.
    CHECK(!builds_a_frame(cg.out));
    CHECK(has(cg.out, "push r2"));

    uint16_t value = 0;
    std::string why;
    CHECK(call16(cg.out, "bump_in_place", 20, 0, value, why));
    if (!why.empty()) std::printf("    %s\n", why.c_str());
    CHECK_EQ(value, 25u);
}

TEST(a_plain_local_needs_no_frame_either) {
    CodeGen cg;
    std::vector<StmtPtr> body;
    body.push_back(declare("t", binary("+", identifier("a"), literal(1))));
    body.push_back(ret(binary("+", identifier("t"), identifier("t"))));
    Function f = function("double_next", {{"a", int_type()}}, std::move(body));
    cg.gen_function(f);

    CHECK(cg.error.empty());
    // A local that is only ever read and written whole is just another value
    // to park in a register.
    CHECK(!builds_a_frame(cg.out));

    uint16_t value = 0;
    std::string why;
    CHECK(call16(cg.out, "double_next", 10, 0, value, why));
    if (!why.empty()) std::printf("    %s\n", why.c_str());
    CHECK_EQ(value, 22u);
}

TEST(an_array_local_keeps_a_frame_opened_with_pushes) {
    // An array is addressed through Y, so it has to stay in memory -- which is
    // exactly the case a frame is for.
    TypePtr buffer = make_array(char_type(), 4);
    CodeGen cg;
    std::vector<StmtPtr> body;
    body.push_back(declare("buf", nullptr, buffer));
    body.push_back(expr_stmt(assign(index(identifier("buf", buffer), literal(1),
                                          char_type()),
                                    literal(9))));
    body.push_back(ret(index(identifier("buf", buffer), literal(1), char_type())));
    Function f = function("scratch", {}, std::move(body));
    cg.gen_function(f);

    CHECK(cg.error.empty());
    CHECK(builds_a_frame(cg.out));
    // Four bytes of frame, pushed open: the stack pointer is never written in
    // two halves, so interrupts stay enabled throughout.
    CHECK(has(cg.out, "push r1"));
    CHECK(!has(cg.out, "cli"));
    CHECK(!has(cg.out, "sbiw r28"));

    uint16_t value = 0;
    std::string why;
    CHECK(call16(cg.out, "scratch", 0, 0, value, why));
    if (!why.empty()) std::printf("    %s\n", why.c_str());
    CHECK_EQ(value, 9u);
}

TEST(a_large_frame_still_disables_interrupts_while_moving_the_stack_pointer) {
    TypePtr grid = make_array(int_type(), 5);
    CodeGen cg;
    std::vector<StmtPtr> body;
    body.push_back(declare("grid", nullptr, grid));
    body.push_back(expr_stmt(assign(index(identifier("grid", grid), literal(3),
                                          int_type()),
                                    literal(1234))));
    body.push_back(ret(index(identifier("grid", grid), literal(3), int_type())));
    Function f = function("wide_scratch", {}, std::move(body));
    cg.gen_function(f);

    CHECK(cg.error.empty());
    CHECK(builds_a_frame(cg.out));
    // Ten bytes is past the point where pushing is the shorter sequence, so the
    // stack pointer is written directly -- and that has to be done with
    // interrupts off, because SPH and SPL cannot be written together.
    CHECK(has(cg.out, "sbiw r28, 10"));
    CHECK(has(cg.out, "cli"));

    uint16_t value = 0;
    std::string why;
    CHECK(call16(cg.out, "wide_scratch", 0, 0, value, why));
    if (!why.empty()) std::printf("    %s\n", why.c_str());
    CHECK_EQ(value, 1234u);
}

TEST(a_frameless_function_leaves_the_stack_pointer_where_it_found_it) {
    CodeGen cg;
    std::vector<StmtPtr> body;
    body.push_back(ret(binary("*", identifier("a"), identifier("b"))));
    Function f = function("product", {{"a", int_type()}, {"b", int_type()}},
                          std::move(body));
    cg.gen_function(f);
    CHECK(cg.error.empty());

    Machine m;
    std::string why;
    std::vector<std::pair<int, uint8_t>> arguments = {
        {24, 12}, {25, 0}, {22, 12}, {23, 0}};
    CHECK(call_function(cg.out, "product", arguments, m, why));
    if (!why.empty()) std::printf("    %s\n", why.c_str());
    CHECK_EQ(result16(m), 144u);
    CHECK_EQ(m.sp, 0x08FFu);                  // the sentinel was popped, nothing else
    CHECK_EQ(m.r[1], uint8_t(0));             // the zero register survived
}

TEST(a_frameless_callee_is_callable_from_generated_code) {
    // The whole point of dropping the frame: callers cannot tell.
    CodeGen cg;
    std::vector<StmtPtr> inner_body;
    inner_body.push_back(ret(binary("+", identifier("x"), literal(4))));
    Function inner = function("plus_four", {{"x", int_type()}}, std::move(inner_body));
    cg.gen_function(inner);

    std::vector<ExprPtr> args;
    args.push_back(identifier("n"));
    std::vector<StmtPtr> outer_body;
    outer_body.push_back(declare("t", call("plus_four", std::move(args))));
    outer_body.push_back(ret(binary("*", identifier("t"), literal(3))));
    Function outer = function("thrice_plus_four", {{"n", int_type()}},
                              std::move(outer_body));
    cg.gen_function(outer);
    CHECK(cg.error.empty());

    uint16_t value = 0;
    std::string why;
    CHECK(call16(cg.out, "thrice_plus_four", 6, 0, value, why));
    if (!why.empty()) std::printf("    %s\n", why.c_str());
    CHECK_EQ(value, 30u);
}

TEST(nested_calls_each_restore_the_registers_they_parked_in) {
    // Both functions want the same call-saved register. The inner one has to
    // hand it back exactly as it found it, or the outer one loses its value.
    CodeGen cg;
    std::vector<StmtPtr> inner_body;
    inner_body.push_back(ret(binary("+", identifier("x"), literal(1))));
    Function inner = function("successor", {{"x", int_type()}}, std::move(inner_body));
    cg.gen_function(inner);

    std::vector<ExprPtr> args;
    args.push_back(literal(5));
    std::vector<StmtPtr> outer_body;
    outer_body.push_back(ret(binary("+", call("successor", std::move(args)),
                                    identifier("a"))));
    Function outer = function("six_more", {{"a", int_type()}}, std::move(outer_body));
    cg.gen_function(outer);

    CHECK(cg.error.empty());
    CHECK(!builds_a_frame(cg.out));
    CHECK(has(cg.out, "push r2"));

    uint16_t value = 0;
    std::string why;
    CHECK(call16(cg.out, "six_more", 700, 0, value, why));
    if (!why.empty()) std::printf("    %s\n", why.c_str());
    CHECK_EQ(value, 706u);
}

TEST(a_parameter_written_before_a_call_keeps_the_new_value_after_it) {
    CodeGen cg;
    std::vector<StmtPtr> inner_body;
    inner_body.push_back(ret(literal(0)));
    Function inner = function("nothing", {}, std::move(inner_body));
    cg.gen_function(inner);

    std::vector<StmtPtr> body;
    body.push_back(expr_stmt(assign(identifier("n"),
                                    binary("+", identifier("n"), literal(2)))));
    body.push_back(expr_stmt(call("nothing", {})));
    body.push_back(ret(identifier("n")));
    Function f = function("bump_then_call", {{"n", int_type()}}, std::move(body));
    cg.gen_function(f);

    CHECK(cg.error.empty());
    CHECK(!builds_a_frame(cg.out));

    uint16_t value = 0;
    std::string why;
    CHECK(call16(cg.out, "bump_then_call", 40, 0, value, why));
    if (!why.empty()) std::printf("    %s\n", why.c_str());
    CHECK_EQ(value, 42u);
}

TEST(a_void_function_with_only_globals_needs_no_frame) {
    CodeGen cg;
    std::vector<StmtPtr> body;
    body.push_back(expr_stmt(assign(identifier("counter"), literal(9))));
    Function f = function("set_counter", {}, std::move(body), void_type());
    cg.gen_function(f);

    CHECK(cg.error.empty());
    CHECK(!builds_a_frame(cg.out));

    Machine m;
    std::string why;
    CHECK(call_function(cg.out, "set_counter", {}, m, why));
    if (!why.empty()) std::printf("    %s\n", why.c_str());
    int address = cg.global_address("counter");
    CHECK(address > 0);
    CHECK_EQ(m.load(uint16_t(address)), uint8_t(9));
}

TEST(a_loop_over_a_parameter_loses_its_frame_too) {
    // Control flow means the parameter is read on several paths; the rewrite
    // has to hold up across all of them.
    CodeGen cg;
    std::vector<StmtPtr> loop_body;
    loop_body.push_back(expr_stmt(assign(identifier("total"),
                                         binary("+", identifier("total"),
                                                identifier("n")))));
    loop_body.push_back(expr_stmt(assign(identifier("i"),
                                         binary("+", identifier("i"), literal(1)))));

    auto loop = std::make_unique<Stmt>();
    loop->kind = StmtKind::While;
    loop->expr = binary("<", identifier("i"), literal(3));
    loop->then_branch = block(std::move(loop_body));

    std::vector<StmtPtr> body;
    body.push_back(declare("total", literal(0)));
    body.push_back(declare("i", literal(0)));
    body.push_back(std::move(loop));
    body.push_back(ret(identifier("total")));
    Function f = function("triple", {{"n", int_type()}}, std::move(body));
    cg.gen_function(f);
    CHECK(cg.error.empty());

    // Three values live at once -- the parameter, the running total and the
    // counter -- and all three end up in registers.
    CHECK(!builds_a_frame(cg.out));

    uint16_t value = 0;
    std::string why;
    CHECK(call16(cg.out, "triple", 7, 0, value, why));
    if (!why.empty()) std::printf("    %s\n", why.c_str());
    CHECK_EQ(value, 21u);
}
