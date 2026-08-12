// A two-pass assembler for the AVR instruction set.
//
// Pass 1 records where each label lands, because a branch may refer to a label
// defined further down. Pass 2 encodes, resolving labels to PC-relative
// offsets or absolute word addresses.
//
// Most AVR instructions are one 16-bit word, but CALL, JMP, LDS and STS are
// two, so pass 1 asks instruction_words() for each mnemonic rather than
// assuming a fixed size.

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

std::string_view strip_comment(std::string_view s) {
    for (size_t i = 0; i < s.size(); ++i)
        if (s[i] == ';' || s[i] == '#') return s.substr(0, i);
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

std::vector<std::string> split_operands(std::string_view s) {
    std::vector<std::string> out;
    while (!s.empty()) {
        size_t comma = s.find(',');
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

        size_t colon = raw.find(':');
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

long need_number(Ctx& ctx, const std::vector<std::string>& ops, size_t idx,
                 const char* what) {
    if (idx >= ops.size()) { ctx.fail(std::string("missing ") + what); return 0; }
    long v = 0;
    if (!parse_number(ops[idx], v)) {
        ctx.fail("expected a number, got '" + ops[idx] + "'");
        return 0;
    }
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

} // namespace

AssembleResult assemble(std::string_view source) {
    AssembleResult result;
    std::vector<Line> lines = split_lines(source);

    // ---- pass 1: label addresses, in words ---------------------------------
    std::map<std::string, long> labels;
    {
        long pc = 0;
        for (const Line& line : lines) {
            if (!line.label.empty()) {
                if (labels.count(line.label)) {
                    result.error = "line " + std::to_string(line.number) +
                                   ": duplicate label '" + line.label + "'";
                    return result;
                }
                labels[line.label] = pc;
            }
            if (!line.mnemonic.empty()) pc += instruction_words(line.mnemonic);
        }
    }

    // ---- pass 2: encode ----------------------------------------------------
    Ctx ctx;
    long pc = 0;
    std::vector<uint16_t> words;

    // Resolves an operand that may be a label or a literal.
    auto resolve_target = [&](const std::string& op, bool relative, long& out) {
        auto it = labels.find(op);
        if (it != labels.end()) { out = it->second; return true; }
        long n = 0;
        if (!parse_number(op, n)) {
            ctx.fail("undefined label '" + op + "'");
            return false;
        }
        out = relative ? pc + n : n;
        return true;
    };

    for (const Line& line : lines) {
        if (line.mnemonic.empty()) continue;
        ctx.line = line.number;
        const std::string& m = line.mnemonic;
        const auto& ops = line.operands;
        uint16_t word = 0, extra = 0;
        bool encoded = false, two_words = false;

        uint16_t base = 0;
        if (encode_no_operand(m, word)) {
            encoded = true;
        } else if (m == "clr" || m == "tst" || m == "lsl" || m == "rol") {
            // Aliases that repeat their single operand as both sources.
            int d = need_register(ctx, ops, 0, "register");
            if (!ctx.failed() && encode_two_reg(m, d, d, word)) encoded = true;
        } else if (immediate_base(m, base)) {
            int d = need_register(ctx, ops, 0, "destination register");
            long k = need_number(ctx, ops, 1, "immediate");
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
            long k = need_number(ctx, ops, 1, "immediate");
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
                a = need_number(ctx, ops, 0, "I/O address");
                r = need_register(ctx, ops, 1, "source register");
            } else {
                r = need_register(ctx, ops, 0, "destination register");
                a = need_number(ctx, ops, 1, "I/O address");
            }
            if (!ctx.failed()) check_range(ctx, a, 0, 0x3F, "I/O address");
            if (!ctx.failed()) {
                word = uint16_t((m == "out" ? 0xB800 : 0xB000) |
                                ((a & 0x30) << 5) | (r << 4) | (a & 0x0F));
                encoded = true;
            }
        } else if (m == "sbi" || m == "cbi" || m == "sbic" || m == "sbis") {
            long a = need_number(ctx, ops, 0, "I/O address");
            long b = need_number(ctx, ops, 1, "bit number");
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
            long b = need_number(ctx, ops, 1, "bit number");
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
                addr = need_number(ctx, ops, 1, "address");
            } else {
                addr = need_number(ctx, ops, 0, "address");
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

        words.push_back(word);
        if (two_words) words.push_back(extra);
        pc += two_words ? 2 : 1;
    }

    result.code.reserve(words.size() * 2);
    for (uint16_t w : words) {                 // little-endian
        result.code.push_back(uint8_t(w & 0xFF));
        result.code.push_back(uint8_t((w >> 8) & 0xFF));
    }
    result.ok = true;
    return result;
}

} // namespace ardio
