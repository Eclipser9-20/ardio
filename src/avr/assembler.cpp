// A two-pass assembler for the AVR instruction set.
//
// Pass 1 records where each label lands, because a branch may refer to a label
// defined further down. Pass 2 encodes, resolving labels to PC-relative
// offsets or absolute word addresses.
//
// Most AVR instructions are one 16-bit word, but CALL, JMP, LDS and STS are
// two, so pass 1 asks instruction_words() for each mnemonic rather than
// assuming a fixed size.
//
// Directives complicate that: .byte, .ascii and .space emit an arbitrary
// number of BYTES, so neither pass can count in whole words. Both passes
// therefore track the position in bytes and round up to a word boundary
// wherever a word has to start (before an instruction, at a label, at .org
// and at the end of the image), padding the gap with 0xFF -- the erased
// value of AVR flash.

#include "ardio/avr/assembler.h"

#include <cctype>
#include <cstdlib>
#include <map>

namespace ardio {
namespace {

// ---------------------------------------------------------------- lexing ---

std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
        s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.remove_suffix(1);
    return s;
}

// A comment runs to the end of the line, but ';' and '#' are ordinary
// characters inside the string of a .ascii/.asciz directive, so quoted runs
// are skipped over rather than searched.
std::string_view strip_comment(std::string_view s) {
    char quote = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (quote) {
            if (c == '\\' && i + 1 < s.size()) ++i;
            else if (c == quote) quote = 0;
        } else if (c == '"' || c == '\'') {
            quote = c;
        } else if (c == ';' || c == '#') {
            return s.substr(0, i);
        }
    }
    return s;
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = char(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

struct Line {
    std::string label;
    std::string mnemonic;
    std::vector<std::string> operands;
    size_t number = 0;
};

// Finds the next character `want` that is not inside a quoted string or a
// parenthesised group. Returns npos if there is none.
size_t find_top_level(std::string_view s, char want, size_t from = 0) {
    char quote = 0;
    int depth = 0;
    for (size_t i = from; i < s.size(); ++i) {
        char c = s[i];
        if (quote) {
            if (c == '\\' && i + 1 < s.size()) ++i;
            else if (c == quote) quote = 0;
            continue;
        }
        if (c == '"' || c == '\'') { quote = c; continue; }
        if (c == '(') { ++depth; continue; }
        if (c == ')') { if (depth > 0) --depth; continue; }
        if (c == want && depth == 0) return i;
    }
    return std::string_view::npos;
}

std::vector<std::string> split_operands(std::string_view s) {
    std::vector<std::string> out;
    while (!s.empty()) {
        size_t comma = find_top_level(s, ',');
        std::string_view piece = comma == std::string_view::npos ? s : s.substr(0, comma);
        piece = trim(piece);
        if (!piece.empty()) out.emplace_back(piece);
        if (comma == std::string_view::npos) break;
        s.remove_prefix(comma + 1);
    }
    return out;
}

std::vector<Line> split_lines(std::string_view src) {
    std::vector<Line> lines;
    size_t i = 0, n = 0;
    while (i <= src.size()) {
        size_t end = src.find('\n', i);
        if (end == std::string_view::npos) end = src.size();
        std::string_view raw = trim(strip_comment(src.substr(i, end - i)));
        ++n;
        bool last = end == src.size();
        i = end + 1;
        if (raw.empty()) { if (last) break; continue; }

        Line line;
        line.number = n;

        size_t colon = find_top_level(raw, ':');
        if (colon != std::string_view::npos) {
            line.label = std::string(trim(raw.substr(0, colon)));
            raw = trim(raw.substr(colon + 1));
        }

        if (!raw.empty()) {
            size_t sp = raw.find_first_of(" \t");
            if (sp == std::string_view::npos) {
                line.mnemonic = lower(raw);
            } else {
                line.mnemonic = lower(raw.substr(0, sp));
                line.operands = split_operands(trim(raw.substr(sp)));
            }
        }

        if (!line.label.empty() || !line.mnemonic.empty()) lines.push_back(line);
        if (last) break;
    }
    return lines;
}

// CALL, JMP, LDS and STS carry a 16-bit operand in a second word.
int instruction_words(const std::string& m) {
    return (m == "call" || m == "jmp" || m == "lds" || m == "sts") ? 2 : 1;
}

// Pointer-register addressing modes: ld/st X, X+, -X, Y, Y+, -Y, Z, Z+, -Z.
struct PointerMode {
    char reg = 0;      // 'X', 'Y' or 'Z'
    int mode = 0;      // 0 = plain, 1 = post-increment, 2 = pre-decrement
};

bool parse_pointer(const std::string& s, PointerMode& out) {
    if (s.empty()) return false;
    if (s[0] == '-') {
        if (s.size() != 2) return false;
        char r = char(std::toupper(static_cast<unsigned char>(s[1])));
        if (r != 'X' && r != 'Y' && r != 'Z') return false;
        out = {r, 2};
        return true;
    }
    char r = char(std::toupper(static_cast<unsigned char>(s[0])));
    if (r != 'X' && r != 'Y' && r != 'Z') return false;
    if (s.size() == 1) { out = {r, 0}; return true; }
    if (s.size() == 2 && s[1] == '+') { out = {r, 1}; return true; }
    return false;
}

// Encodes ld/st for the pointer modes. Returns false for combinations the
// architecture does not provide (X has no displacement form, and plain
// Y/Z here are handled by ldd/std with q=0).
bool encode_pointer_access(bool load, int reg, const PointerMode& p, uint16_t& out) {
    uint16_t base = load ? 0x9000 : 0x9200;
    uint16_t rbits = uint16_t(reg << 4);
    if (p.reg == 'X') {
        uint16_t tail = p.mode == 0 ? 0x000C : p.mode == 1 ? 0x000D : 0x000E;
        out = uint16_t(base | rbits | tail);
        return true;
    }
    if (p.reg == 'Y') {
        if (p.mode == 0) { out = uint16_t((load ? 0x8008 : 0x8208) | rbits); return true; }
        uint16_t tail = p.mode == 1 ? 0x0009 : 0x000A;
        out = uint16_t(base | rbits | tail);
        return true;
    }
    // Z
    if (p.mode == 0) { out = uint16_t((load ? 0x8000 : 0x8200) | rbits); return true; }
    uint16_t tail = p.mode == 1 ? 0x0001 : 0x0002;
    out = uint16_t(base | rbits | tail);
    return true;
}

// Single-bit SREG and register-bit instructions.
bool encode_bit_op(const std::string& m, uint16_t& out) {
    if (m == "sec") { out = 0x9408; return true; }
    if (m == "clc") { out = 0x9488; return true; }
    if (m == "sen") { out = 0x9428; return true; }
    if (m == "cln") { out = 0x94A8; return true; }
    if (m == "sez") { out = 0x9418; return true; }
    if (m == "clz") { out = 0x9498; return true; }
    if (m == "ses") { out = 0x9448; return true; }
    if (m == "cls") { out = 0x94C8; return true; }
    if (m == "sev") { out = 0x9438; return true; }
    if (m == "clv") { out = 0x94B8; return true; }
    if (m == "set") { out = 0x9468; return true; }
    if (m == "clt") { out = 0x94E8; return true; }
    if (m == "seh") { out = 0x9458; return true; }
    if (m == "clh") { out = 0x94D8; return true; }
    if (m == "lpm") { out = 0x95C8; return true; }   // implicit R0 <- Z
    if (m == "ijmp"){ out = 0x9409; return true; }
    if (m == "icall"){out = 0x9509; return true; }
    if (m == "break"){out = 0x9598; return true; }
    return false;
}

// -------------------------------------------------------------- operands ---

struct Ctx {
    std::string error;
    size_t line = 0;
    void fail(const std::string& msg) {
        if (error.empty()) error = "line " + std::to_string(line) + ": " + msg;
    }
    bool failed() const { return !error.empty(); }
};

int parse_register(const std::string& s) {
    if (s.size() < 2 || (s[0] != 'r' && s[0] != 'R')) return -1;
    for (size_t i = 1; i < s.size(); ++i)
        if (!std::isdigit(static_cast<unsigned char>(s[i]))) return -1;
    int n = std::atoi(s.c_str() + 1);
    return (n >= 0 && n <= 31) ? n : -1;
}

bool parse_number(const std::string& s, long& out) {
    if (s.empty()) return false;
    size_t start = 0;
    bool neg = false;
    if (s[0] == '-') { neg = true; start = 1; }
    if (start >= s.size()) return false;

    std::string body = s.substr(start);
    char* end = nullptr;
    long v = 0;
    if (body.size() > 2 && body[0] == '0' && (body[1] == 'x' || body[1] == 'X'))
        v = std::strtol(body.c_str() + 2, &end, 16);
    else if (body.size() > 2 && body[0] == '0' && (body[1] == 'b' || body[1] == 'B'))
        v = std::strtol(body.c_str() + 2, &end, 2);
    else
        v = std::strtol(body.c_str(), &end, 10);

    if (!end || *end != '\0') return false;
    out = neg ? -v : v;
    return true;
}

// ----------------------------------------------------------- expressions ---

// Named constants (.equ/.set) and labels share one table. Label values are
// word addresses, matching what call/jmp/rjmp expect.
using Symbols = std::map<std::string, long>;

bool ident_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == '$';
}

// Decodes the escape after a backslash. Unknown escapes stand for themselves,
// which is what a reader expects from "\%" or "\ ".
char decode_escape(char c) {
    switch (c) {
        case 'n': return '\n';
        case 't': return '\t';
        case 'r': return '\r';
        case '0': return '\0';
        case 'a': return '\a';
        case 'b': return '\b';
        case 'f': return '\f';
        case 'v': return '\v';
        default:  return c;
    }
}

// Unquotes a "..." operand. Returns false if the operand is not a string.
bool parse_string(const std::string& s, std::string& out) {
    if (s.size() < 2 || s.front() != '"' || s.back() != '"') return false;
    out.clear();
    for (size_t i = 1; i + 1 < s.size(); ++i) {
        if (s[i] == '\\' && i + 2 < s.size()) out.push_back(decode_escape(s[++i]));
        else out.push_back(s[i]);
    }
    return true;
}

// A small recursive-descent evaluator over constants, labels and literals.
// Precedence, loosest first: | ^ ; + - ; * / % & << >> ; unary - ~ ! ; atoms.
// lo8()/hi8() take the low and high byte of a value, which is how a 16-bit
// address is loaded into a register pair with two ldi instructions.
class ExprParser {
public:
    ExprParser(const std::string& text, const Symbols& syms) : t_(text), syms_(syms) {}

    bool run(long& out) {
        long v = 0;
        if (!parse_or(v)) return false;
        skip();
        if (i_ != t_.size()) return fail("unexpected '" + t_.substr(i_) + "'");
        out = v;
        return true;
    }

    const std::string& error() const { return error_; }

private:
    const std::string& t_;
    const Symbols& syms_;
    size_t i_ = 0;
    std::string error_;

    bool fail(const std::string& m) {
        if (error_.empty()) error_ = m;
        return false;
    }
    void skip() {
        while (i_ < t_.size() && std::isspace(static_cast<unsigned char>(t_[i_]))) ++i_;
    }
    bool eat(char c) {
        skip();
        if (i_ < t_.size() && t_[i_] == c) { ++i_; return true; }
        return false;
    }
    bool eat2(char a, char b) {
        skip();
        if (i_ + 1 < t_.size() && t_[i_] == a && t_[i_ + 1] == b) { i_ += 2; return true; }
        return false;
    }

    bool parse_or(long& out) {
        if (!parse_add(out)) return false;
        for (;;) {
            skip();
            if (i_ < t_.size() && t_[i_] == '|' && !(i_ + 1 < t_.size() && t_[i_ + 1] == '|')) {
                ++i_;
                long r = 0;
                if (!parse_add(r)) return false;
                out |= r;
            } else if (i_ < t_.size() && t_[i_] == '^') {
                ++i_;
                long r = 0;
                if (!parse_add(r)) return false;
                out ^= r;
            } else {
                return true;
            }
        }
    }

    bool parse_add(long& out) {
        if (!parse_mul(out)) return false;
        for (;;) {
            skip();
            if (i_ < t_.size() && (t_[i_] == '+' || t_[i_] == '-')) {
                char op = t_[i_++];
                long r = 0;
                if (!parse_mul(r)) return false;
                out = op == '+' ? out + r : out - r;
            } else {
                return true;
            }
        }
    }

    bool parse_mul(long& out) {
        if (!parse_unary(out)) return false;
        for (;;) {
            long r = 0;
            skip();
            if (eat2('<', '<')) {
                if (!parse_unary(r)) return false;
                out = long(static_cast<unsigned long>(out) << (r & 63));
            } else if (eat2('>', '>')) {
                if (!parse_unary(r)) return false;
                out >>= (r & 63);
            } else if (i_ < t_.size() && (t_[i_] == '*' || t_[i_] == '/' ||
                                          t_[i_] == '%' || t_[i_] == '&')) {
                char op = t_[i_++];
                if (!parse_unary(r)) return false;
                if ((op == '/' || op == '%') && r == 0) return fail("division by zero");
                out = op == '*' ? out * r
                    : op == '/' ? out / r
                    : op == '%' ? out % r
                                : (out & r);
            } else {
                return true;
            }
        }
    }

    bool parse_unary(long& out) {
        skip();
        if (i_ < t_.size() && (t_[i_] == '-' || t_[i_] == '~' || t_[i_] == '+' ||
                               t_[i_] == '!')) {
            char op = t_[i_++];
            if (!parse_unary(out)) return false;
            if (op == '-') out = -out;
            else if (op == '~') out = ~out;
            else if (op == '!') out = !out;
            return true;
        }
        return parse_atom(out);
    }

    bool parse_number_literal(long& out) {
        size_t start = i_;
        int base = 10;
        if (t_.compare(i_, 2, "0x") == 0 || t_.compare(i_, 2, "0X") == 0) {
            base = 16;
            i_ += 2;
        } else if (t_.compare(i_, 2, "0b") == 0 || t_.compare(i_, 2, "0B") == 0) {
            base = 2;
            i_ += 2;
        }
        size_t digits = i_;
        while (i_ < t_.size() && std::isalnum(static_cast<unsigned char>(t_[i_]))) ++i_;
        if (i_ == digits) return fail("expected a number");
        std::string body = t_.substr(digits, i_ - digits);
        char* end = nullptr;
        long v = std::strtol(body.c_str(), &end, base);
        if (!end || *end != '\0') return fail("bad number '" + t_.substr(start, i_ - start) + "'");
        out = v;
        return true;
    }

    bool parse_atom(long& out) {
        skip();
        if (i_ >= t_.size()) return fail("expected a value");
        char c = t_[i_];

        if (c == '(') {
            ++i_;
            if (!parse_or(out)) return false;
            if (!eat(')')) return fail("expected ')'");
            return true;
        }
        if (c == '\'') {                       // character literal, e.g. 'A'
            ++i_;
            if (i_ >= t_.size()) return fail("unterminated character literal");
            char v = t_[i_++];
            if (v == '\\' && i_ < t_.size()) v = decode_escape(t_[i_++]);
            if (i_ >= t_.size() || t_[i_] != '\'') return fail("unterminated character literal");
            ++i_;
            out = long(static_cast<unsigned char>(v));
            return true;
        }
        if (std::isdigit(static_cast<unsigned char>(c))) return parse_number_literal(out);

        if (ident_char(c) && !std::isdigit(static_cast<unsigned char>(c))) {
            size_t start = i_;
            while (i_ < t_.size() && ident_char(t_[i_])) ++i_;
            std::string name = t_.substr(start, i_ - start);
            size_t save = i_;
            if (eat('(')) {
                std::string fn = lower(name);
                if (fn != "lo8" && fn != "hi8" && fn != "lo" && fn != "hi") {
                    i_ = save;
                    return fail("unknown function '" + name + "'");
                }
                long v = 0;
                if (!parse_or(v)) return false;
                if (!eat(')')) return fail("expected ')'");
                out = (fn == "lo8" || fn == "lo") ? (v & 0xFF) : ((v >> 8) & 0xFF);
                return true;
            }
            auto it = syms_.find(name);
            if (it == syms_.end()) return fail("undefined label '" + name + "'");
            out = it->second;
            return true;
        }
        return fail("unexpected '" + t_.substr(i_) + "'");
    }
};

// Parses "Y+6" / "Z+0" into the displacement. Returns false if not that form.
bool parse_displacement(const std::string& s, char& base, long& q) {
    if (s.size() < 1) return false;
    char b = char(std::toupper(static_cast<unsigned char>(s[0])));
    if (b != 'Y' && b != 'Z') return false;
    base = b;
    if (s.size() == 1) { q = 0; return true; }
    if (s[1] != '+') return false;
    return parse_number(s.substr(2), q);
}

int need_register(Ctx& ctx, const std::vector<std::string>& ops, size_t idx,
                  const char* what) {
    if (idx >= ops.size()) { ctx.fail(std::string("missing ") + what); return 0; }
    int r = parse_register(ops[idx]);
    if (r < 0) { ctx.fail("expected a register (r0-r31), got '" + ops[idx] + "'"); return 0; }
    return r;
}

// Evaluates an expression operand, reporting through ctx on failure.
bool eval(Ctx& ctx, const Symbols& syms, const std::string& text, long& out) {
    ExprParser p(text, syms);
    if (p.run(out)) return true;
    ctx.fail(p.error().empty() ? ("bad expression '" + text + "'") : p.error());
    return false;
}

long need_number(Ctx& ctx, const Symbols& syms, const std::vector<std::string>& ops,
                 size_t idx, const char* what) {
    if (idx >= ops.size()) { ctx.fail(std::string("missing ") + what); return 0; }
    long v = 0;
    if (!eval(ctx, syms, ops[idx], v)) return 0;
    return v;
}

void check_range(Ctx& ctx, long v, long lo, long hi, const char* what) {
    if (v < lo || v > hi)
        ctx.fail(std::string(what) + " " + std::to_string(v) + " is out of range (" +
                 std::to_string(lo) + " to " + std::to_string(hi) + ")");
}

// -------------------------------------------------------------- encoding ---

bool encode_two_reg(const std::string& m, int d, int r, uint16_t& out) {
    auto pack = [&](uint16_t base) {
        return uint16_t(base | ((r & 0x10) << 5) | (d << 4) | (r & 0x0F));
    };
    if (m == "mov") { out = pack(0x2C00); return true; }
    if (m == "add" || m == "lsl") { out = pack(0x0C00); return true; }
    if (m == "adc" || m == "rol") { out = pack(0x1C00); return true; }
    if (m == "sub") { out = pack(0x1800); return true; }
    if (m == "sbc") { out = pack(0x0800); return true; }
    if (m == "and" || m == "tst") { out = pack(0x2000); return true; }
    if (m == "or")  { out = pack(0x2800); return true; }
    if (m == "eor" || m == "clr") { out = pack(0x2400); return true; }
    if (m == "cp")  { out = pack(0x1400); return true; }
    if (m == "cpc") { out = pack(0x0400); return true; }
    if (m == "mul") { out = pack(0x9C00); return true; }
    return false;
}

bool encode_one_reg(const std::string& m, int d, uint16_t& out) {
    if (m == "inc")  { out = uint16_t(0x9403 | (d << 4)); return true; }
    if (m == "dec")  { out = uint16_t(0x940A | (d << 4)); return true; }
    if (m == "com")  { out = uint16_t(0x9400 | (d << 4)); return true; }
    if (m == "neg")  { out = uint16_t(0x9401 | (d << 4)); return true; }
    if (m == "asr")  { out = uint16_t(0x9405 | (d << 4)); return true; }
    if (m == "lsr")  { out = uint16_t(0x9406 | (d << 4)); return true; }
    if (m == "ror")  { out = uint16_t(0x9407 | (d << 4)); return true; }
    if (m == "swap") { out = uint16_t(0x9402 | (d << 4)); return true; }
    if (m == "push") { out = uint16_t(0x920F | (d << 4)); return true; }
    if (m == "pop")  { out = uint16_t(0x900F | (d << 4)); return true; }
    return false;
}

bool encode_no_operand(const std::string& m, uint16_t& out) {
    if (m == "nop")   { out = 0x0000; return true; }
    if (m == "ret")   { out = 0x9508; return true; }
    if (m == "reti")  { out = 0x9518; return true; }
    if (m == "sei")   { out = 0x9478; return true; }
    if (m == "cli")   { out = 0x94F8; return true; }
    if (m == "sleep") { out = 0x9588; return true; }
    if (m == "wdr")   { out = 0x95A8; return true; }
    return false;
}

bool branch_base(const std::string& m, uint16_t& base) {
    if (m == "brne") { base = 0xF401; return true; }
    if (m == "breq") { base = 0xF001; return true; }
    if (m == "brcs" || m == "brlo") { base = 0xF000; return true; }
    if (m == "brcc" || m == "brsh") { base = 0xF400; return true; }
    if (m == "brmi") { base = 0xF002; return true; }
    if (m == "brpl") { base = 0xF402; return true; }
    if (m == "brge") { base = 0xF404; return true; }
    if (m == "brlt") { base = 0xF004; return true; }
    return false;
}

// Immediate forms restricted to r16-r31.
bool immediate_base(const std::string& m, uint16_t& base) {
    if (m == "ldi")  { base = 0xE000; return true; }
    if (m == "subi") { base = 0x5000; return true; }
    if (m == "sbci") { base = 0x4000; return true; }
    if (m == "andi") { base = 0x7000; return true; }
    if (m == "ori")  { base = 0x6000; return true; }
    if (m == "cpi")  { base = 0x3000; return true; }
    return false;
}

// ------------------------------------------------------------ directives ---

// Holds the output image and the current position, counted in BYTES so that
// data directives can be sized exactly. Pass 1 runs with emit = false: nothing
// is stored, but the position advances identically, so both passes agree on
// every address.
struct Emitter {
    std::vector<uint8_t> bytes;
    long pc = 0;
    bool emit = false;

    void put(uint8_t b) {
        if (emit) bytes.push_back(b);
        ++pc;
    }
    void put_word(uint16_t w) {
        put(uint8_t(w & 0xFF));
        put(uint8_t((w >> 8) & 0xFF));
    }
    // Rounds up to the next word, padding with the erased flash value.
    void align_word() {
        if (pc & 1) put(0xFF);
    }
    long word_pc() const { return pc / 2; }
};

bool is_directive(const std::string& m) { return m.size() > 1 && m[0] == '.'; }

// Executes one directive. Errors are reported through ctx.
bool run_directive(const Line& line, Ctx& ctx, Emitter& e, Symbols& syms) {
    const std::string& d = line.mnemonic;
    const std::vector<std::string>& ops = line.operands;

    if (d == ".equ" || d == ".set") {
        if (ops.size() != 2) { ctx.fail(d + " needs a name and a value"); return false; }
        if (ops[0].empty() || std::isdigit(static_cast<unsigned char>(ops[0][0]))) {
            ctx.fail("'" + ops[0] + "' is not a usable constant name");
            return false;
        }
        long v = 0;
        if (!eval(ctx, syms, ops[1], v)) return false;
        syms[ops[0]] = v;
        return true;
    }

    // There is no linker yet, so visibility directives are accepted and
    // ignored; source written for one stays portable.
    if (d == ".global" || d == ".globl" || d == ".extern") {
        if (ops.empty()) { ctx.fail(d + " needs a name"); return false; }
        return true;
    }

    if (d == ".org") {
        if (ops.size() != 1) { ctx.fail(".org needs one address"); return false; }
        long addr = 0;
        if (!eval(ctx, syms, ops[0], addr)) return false;
        if (addr < 0) { ctx.fail(".org address cannot be negative"); return false; }
        long target = addr * 2;
        if (target < e.pc) {
            ctx.fail(".org cannot move backwards, from word " +
                     std::to_string(e.word_pc()) + " to " + std::to_string(addr));
            return false;
        }
        while (e.pc < target) e.put(0xFF);
        return true;
    }

    if (d == ".byte" || d == ".db") {
        if (ops.empty()) { ctx.fail(d + " needs at least one value"); return false; }
        for (const std::string& op : ops) {
            std::string text;
            if (parse_string(op, text)) {            // "abc" is shorthand for its bytes
                for (char c : text) e.put(uint8_t(c));
                continue;
            }
            long v = 0;
            if (!eval(ctx, syms, op, v)) return false;
            check_range(ctx, v, -128, 255, "byte value");
            if (ctx.failed()) return false;
            e.put(uint8_t(v & 0xFF));
        }
        return true;
    }

    if (d == ".word" || d == ".dw") {
        if (ops.empty()) { ctx.fail(d + " needs at least one value"); return false; }
        e.align_word();
        for (const std::string& op : ops) {
            long v = 0;
            if (!eval(ctx, syms, op, v)) return false;
            check_range(ctx, v, -32768, 65535, "word value");
            if (ctx.failed()) return false;
            e.put_word(uint16_t(v & 0xFFFF));
        }
        return true;
    }

    if (d == ".ascii" || d == ".asciz" || d == ".string") {
        if (ops.empty()) { ctx.fail(d + " needs a quoted string"); return false; }
        for (const std::string& op : ops) {
            std::string text;
            if (!parse_string(op, text)) {
                ctx.fail(d + " expects a quoted string, got '" + op + "'");
                return false;
            }
            for (char c : text) e.put(uint8_t(c));
            if (d != ".ascii") e.put(0);
        }
        return true;
    }

    if (d == ".space" || d == ".skip") {
        if (ops.empty() || ops.size() > 2) {
            ctx.fail(d + " needs a byte count and an optional fill value");
            return false;
        }
        long n = 0;
        if (!eval(ctx, syms, ops[0], n)) return false;
        if (n < 0) { ctx.fail(d + " count cannot be negative"); return false; }
        long fill = 0;
        if (ops.size() == 2) {
            if (!eval(ctx, syms, ops[1], fill)) return false;
            check_range(ctx, fill, -128, 255, "fill value");
            if (ctx.failed()) return false;
        }
        for (long k = 0; k < n; ++k) e.put(uint8_t(fill & 0xFF));
        return true;
    }

    ctx.fail("unknown directive '" + d + "'");
    return false;
}

} // namespace

AssembleResult assemble(std::string_view source) {
    AssembleResult result;
    std::vector<Line> lines = split_lines(source);

    // ---- pass 1: symbol addresses ------------------------------------------
    //
    // Sizing happens in bytes, because .byte/.ascii/.space runs are not
    // word-sized. A label always names a word address, so the position is
    // rounded up to a word before one is recorded -- that is what keeps a
    // label following an odd-length .byte run pointing at a real instruction.
    Symbols syms;
    {
        Ctx ctx;
        Emitter e;                       // e.emit stays false: sizing only
        std::map<std::string, long> labels;
        for (const Line& line : lines) {
            ctx.line = line.number;
            if (!line.label.empty()) {
                e.align_word();
                if (labels.count(line.label)) {
                    result.error = "line " + std::to_string(line.number) +
                                   ": duplicate label '" + line.label + "'";
                    return result;
                }
                labels[line.label] = e.word_pc();
                syms[line.label] = e.word_pc();
            }
            if (line.mnemonic.empty()) continue;
            if (is_directive(line.mnemonic)) {
                if (!run_directive(line, ctx, e, syms)) { result.error = ctx.error; return result; }
            } else {
                e.align_word();
                e.pc += 2 * instruction_words(line.mnemonic);
            }
        }
    }

    // ---- pass 2: encode ----------------------------------------------------
    Ctx ctx;
    Emitter img;
    img.emit = true;
    long pc = 0;                          // current word address

    // Resolves a jump or branch target. A bare number is an offset when the
    // instruction is PC-relative; anything else (label, constant, expression)
    // is an address.
    auto resolve_target = [&](const std::string& op, bool relative, long& out_addr) {
        long n = 0;
        if (relative && parse_number(op, n)) { out_addr = pc + n; return true; }
        return eval(ctx, syms, op, out_addr);
    };

    for (const Line& line : lines) {
        ctx.line = line.number;
        if (!line.label.empty()) img.align_word();
        if (line.mnemonic.empty()) continue;
        if (is_directive(line.mnemonic)) {
            if (!run_directive(line, ctx, img, syms)) { result.error = ctx.error; return result; }
            continue;
        }
        img.align_word();
        pc = img.word_pc();
        const std::string& m = line.mnemonic;
        const auto& ops = line.operands;
        uint16_t word = 0, extra = 0;
        bool encoded = false, two_words = false;

        uint16_t base = 0;
        PointerMode pmode;
        if (encode_no_operand(m, word) || encode_bit_op(m, word)) {
            encoded = true;
        } else if (m == "ld" || m == "st") {
            // "ld Rd, X+" / "st -Y, Rr" -- the register and pointer swap sides.
            int r = 0;
            bool form_ok = true;
            if (m == "ld") {
                r = need_register(ctx, ops, 0, "destination register");
                if (ops.size() < 2 || !parse_pointer(ops[1], pmode)) form_ok = false;
            } else {
                if (ops.empty() || !parse_pointer(ops[0], pmode)) form_ok = false;
                r = need_register(ctx, ops, 1, "source register");
            }
            if (!form_ok) ctx.fail("expected X, X+, -X, Y, Y+, -Y, Z, Z+ or -Z");
            if (!ctx.failed() && encode_pointer_access(m == "ld", r, pmode, word))
                encoded = true;
        } else if (m == "cpse") {
            int d = need_register(ctx, ops, 0, "first register");
            int r = need_register(ctx, ops, 1, "second register");
            if (!ctx.failed()) {
                word = uint16_t(0x1000 | ((r & 0x10) << 5) | (d << 4) | (r & 0x0F));
                encoded = true;
            }
        } else if (m == "bst" || m == "bld") {
            int d = need_register(ctx, ops, 0, "register");
            long b = need_number(ctx, syms, ops, 1, "bit number");
            if (!ctx.failed()) check_range(ctx, b, 0, 7, "bit number");
            if (!ctx.failed()) {
                word = uint16_t((m == "bst" ? 0xFA00 : 0xF800) | (d << 4) | b);
                encoded = true;
            }
        } else if (m == "cbr" || m == "sbr") {
            // Aliases for andi with the complement, and ori.
            int d = need_register(ctx, ops, 0, "destination register");
            long k = need_number(ctx, syms, ops, 1, "bit mask");
            if (!ctx.failed() && d < 16)
                ctx.fail(m + " can only target r16-r31, not r" + std::to_string(d));
            if (!ctx.failed()) {
                unsigned kk = m == "cbr" ? (~unsigned(k) & 0xFF) : (unsigned(k) & 0xFF);
                uint16_t bb = m == "cbr" ? 0x7000 : 0x6000;
                word = uint16_t(bb | ((kk & 0xF0) << 4) | ((d - 16) << 4) | (kk & 0x0F));
                encoded = true;
            }
        } else if (m == "clr" || m == "tst" || m == "lsl" || m == "rol") {
            // Aliases that repeat their single operand as both sources.
            int d = need_register(ctx, ops, 0, "register");
            if (!ctx.failed() && encode_two_reg(m, d, d, word)) encoded = true;
        } else if (immediate_base(m, base)) {
            int d = need_register(ctx, ops, 0, "destination register");
            long k = need_number(ctx, syms, ops, 1, "immediate");
            if (!ctx.failed()) {
                if (d < 16)
                    ctx.fail(m + " can only target r16-r31, not r" + std::to_string(d));
                check_range(ctx, k, -128, 255, "immediate");
            }
            if (!ctx.failed()) {
                unsigned kk = unsigned(k) & 0xFF;
                word = uint16_t(base | ((kk & 0xF0) << 4) | ((d - 16) << 4) | (kk & 0x0F));
                encoded = true;
            }
        } else if (m == "movw") {
            int d = need_register(ctx, ops, 0, "destination register");
            int r = need_register(ctx, ops, 1, "source register");
            if (!ctx.failed() && ((d | r) & 1))
                ctx.fail("movw needs even register numbers");
            if (!ctx.failed()) {
                word = uint16_t(0x0100 | ((d / 2) << 4) | (r / 2));
                encoded = true;
            }
        } else if (m == "adiw" || m == "sbiw") {
            int d = need_register(ctx, ops, 0, "register pair");
            long k = need_number(ctx, syms, ops, 1, "immediate");
            if (!ctx.failed()) {
                if (d != 24 && d != 26 && d != 28 && d != 30)
                    ctx.fail(m + " only works on r24, r26, r28 or r30");
                check_range(ctx, k, 0, 63, "immediate");
            }
            if (!ctx.failed()) {
                word = uint16_t((m == "adiw" ? 0x9600 : 0x9700) |
                                ((k & 0x30) << 2) | (((d - 24) / 2) << 4) | (k & 0x0F));
                encoded = true;
            }
        } else if (m == "out" || m == "in") {
            long a = 0;
            int r = 0;
            if (m == "out") {
                a = need_number(ctx, syms, ops, 0, "I/O address");
                r = need_register(ctx, ops, 1, "source register");
            } else {
                r = need_register(ctx, ops, 0, "destination register");
                a = need_number(ctx, syms, ops, 1, "I/O address");
            }
            if (!ctx.failed()) check_range(ctx, a, 0, 0x3F, "I/O address");
            if (!ctx.failed()) {
                word = uint16_t((m == "out" ? 0xB800 : 0xB000) |
                                ((a & 0x30) << 5) | (r << 4) | (a & 0x0F));
                encoded = true;
            }
        } else if (m == "sbi" || m == "cbi" || m == "sbic" || m == "sbis") {
            long a = need_number(ctx, syms, ops, 0, "I/O address");
            long b = need_number(ctx, syms, ops, 1, "bit number");
            if (!ctx.failed()) {
                check_range(ctx, a, 0, 0x1F, "I/O address");
                check_range(ctx, b, 0, 7, "bit number");
            }
            if (!ctx.failed()) {
                uint16_t bb = m == "sbi" ? 0x9A00 : m == "cbi" ? 0x9800
                            : m == "sbic" ? 0x9900 : 0x9B00;
                word = uint16_t(bb | (a << 3) | b);
                encoded = true;
            }
        } else if (m == "sbrc" || m == "sbrs") {
            int r = need_register(ctx, ops, 0, "register");
            long b = need_number(ctx, syms, ops, 1, "bit number");
            if (!ctx.failed()) check_range(ctx, b, 0, 7, "bit number");
            if (!ctx.failed()) {
                word = uint16_t((m == "sbrc" ? 0xFC00 : 0xFE00) | (r << 4) | b);
                encoded = true;
            }
        } else if (m == "lds" || m == "sts") {
            int r = 0;
            long addr = 0;
            if (m == "lds") {
                r = need_register(ctx, ops, 0, "destination register");
                addr = need_number(ctx, syms, ops, 1, "address");
            } else {
                addr = need_number(ctx, syms, ops, 0, "address");
                r = need_register(ctx, ops, 1, "source register");
            }
            if (!ctx.failed()) check_range(ctx, addr, 0, 0xFFFF, "address");
            if (!ctx.failed()) {
                word = uint16_t((m == "lds" ? 0x9000 : 0x9200) | (r << 4));
                extra = uint16_t(addr);
                encoded = two_words = true;
            }
        } else if (m == "ldd" || m == "std") {
            int r = 0;
            char b = 0;
            long q = 0;
            bool form_ok = true;
            if (m == "ldd") {
                r = need_register(ctx, ops, 0, "destination register");
                if (ops.size() < 2 || !parse_displacement(ops[1], b, q)) form_ok = false;
            } else {
                if (ops.empty() || !parse_displacement(ops[0], b, q)) form_ok = false;
                r = need_register(ctx, ops, 1, "source register");
            }
            if (!form_ok) ctx.fail("expected Y+q or Z+q");
            if (!ctx.failed()) check_range(ctx, q, 0, 63, "displacement");
            if (!ctx.failed()) {
                uint16_t bb = (m == "ldd" ? 0x8000 : 0x8200) | (b == 'Y' ? 0x0008 : 0x0000);
                word = uint16_t(bb | ((q & 0x20) << 8) | ((q & 0x18) << 7) |
                                (r << 4) | (q & 0x07));
                encoded = true;
            }
        } else if (m == "call" || m == "jmp") {
            if (ops.empty()) ctx.fail("missing target");
            long target = 0;
            if (!ctx.failed() && resolve_target(ops[0], false, target)) {
                check_range(ctx, target, 0, 0xFFFF, "address");
                if (!ctx.failed()) {
                    word = m == "call" ? 0x940E : 0x940C;
                    extra = uint16_t(target);
                    encoded = two_words = true;
                }
            }
        } else if (m == "rjmp" || m == "rcall") {
            if (ops.empty()) ctx.fail("missing target");
            long target = 0;
            if (!ctx.failed() && resolve_target(ops[0], true, target)) {
                long k = target - pc - 1;
                check_range(ctx, k, -2048, 2047, "jump offset");
                if (!ctx.failed()) {
                    word = uint16_t((m == "rjmp" ? 0xC000 : 0xD000) | (unsigned(k) & 0x0FFF));
                    encoded = true;
                }
            }
        } else if (branch_base(m, base)) {
            if (ops.empty()) ctx.fail("missing target");
            long target = 0;
            if (!ctx.failed() && resolve_target(ops[0], true, target)) {
                long k = target - pc - 1;
                check_range(ctx, k, -64, 63, "branch offset");
                if (!ctx.failed()) {
                    word = uint16_t(base | ((unsigned(k) & 0x7F) << 3));
                    encoded = true;
                }
            }
        } else if (ops.size() >= 2 && parse_register(ops[0]) >= 0 &&
                   parse_register(ops[1]) >= 0) {
            int d = parse_register(ops[0]), r = parse_register(ops[1]);
            if (encode_two_reg(m, d, r, word)) encoded = true;
        }

        if (!encoded && !ctx.failed()) {
            int d = ops.empty() ? -1 : parse_register(ops[0]);
            if (d >= 0 && encode_one_reg(m, d, word)) encoded = true;
        }

        if (ctx.failed()) { result.error = ctx.error; return result; }
        if (!encoded) {
            result.error = "line " + std::to_string(line.number) +
                           ": unknown instruction '" + m + "'";
            return result;
        }

        img.put_word(word);                    // little-endian
        if (two_words) img.put_word(extra);
    }

    img.align_word();                          // never end mid-word
    result.code = std::move(img.bytes);
    result.ok = true;
    return result;
}

} // namespace ardio
