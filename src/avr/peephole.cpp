// A peephole optimiser over finished AVR assembly text.
//
// The code generator emits assembly straight from the AST with no intermediate
// representation, so it produces plenty of locally redundant code: a value
// stored to a frame slot and immediately read back, a register saved around a
// subexpression that never touches it, a jump to the very next line. All of
// that is visible in the text, and rewriting the text needs no new machinery.
//
// The rules below are deliberately timid. Every one of them either deletes an
// instruction or replaces a short run with a shorter equivalent, and each is
// guarded by three invariants:
//
//   * Nothing is rewritten across a label. Any label may be the target of a
//     jump from anywhere, so what holds on the way in is unknowable; a label
//     therefore ends every window.
//   * No instruction that sets flags is ever deleted from live code. The
//     instructions these rules remove -- MOV, MOVW, LDD, STD, PUSH, POP, CALL,
//     RET, RJMP -- leave SREG untouched, so no following branch can notice.
//   * The skip instructions (CPSE, SBRC, SBRS, SBIC, SBIS) conditionally skip
//     the single instruction after them. Nothing is deleted or replaced
//     directly after one, since that would change what gets skipped.
//
// Anything the optimiser cannot classify -- a directive, an unknown mnemonic,
// an operand form it does not recognise -- is preserved verbatim and acts as a
// barrier that ends any window it falls in.
//
// Three of the transforms below reach further than a peephole window:
//
//   * Dead-write and redundant-load removal follow a register forwards through
//     straight-line code, giving up at the first label, control transfer or
//     instruction the optimiser cannot read.
//   * Identical-tail merging notices that two functions finish with the same
//     run of instructions and lets the earlier one jump into the later one's
//     copy.
//   * Outlining turns a run of instructions that recurs across the whole
//     program into a subroutine reached by RCALL.
//
// The last two change the distance between an instruction and its target, so
// they need to know how many words each line assembles to. That is computable
// for instructions but not for directives, so both are switched off entirely
// for any input carrying a line the optimiser cannot size.

#include "ardio/avr/peephole.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ardio {
namespace {

// ---------------------------------------------------------------- lexing ---
//
// Line splitting mirrors the assembler's own, so the optimiser sees exactly
// the instructions the assembler will. The original text is kept alongside the
// parse so that untouched lines -- comments, spacing, directives -- come out
// character for character as they went in.

std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
        s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.remove_suffix(1);
    return s;
}

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

size_t find_top_level(std::string_view s, char want) {
    char quote = 0;
    int depth = 0;
    for (size_t i = 0; i < s.size(); ++i) {
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

struct Line {
    std::string text;                   // rendered output for this line
    std::string indent;                 // leading whitespace of the original
    std::string label;                  // "" when the line carries no label
    std::string mnemonic;               // lowercased; "" for a label-only line
    std::vector<std::string> operands;
    bool dead = false;                  // dropped from the output entirely
};

std::vector<Line> split_lines(const std::string& src) {
    std::vector<Line> lines;
    size_t i = 0;
    while (i <= src.size()) {
        size_t end = src.find('\n', i);
        bool last = end == std::string::npos;
        if (last) end = src.size();
        std::string_view raw_text = std::string_view(src).substr(i, end - i);
        i = end + 1;
        // The text after the final newline is not a line of its own.
        if (last && raw_text.empty() && (src.empty() || src.back() == '\n')) break;

        Line line;
        line.text = std::string(raw_text);
        size_t first = raw_text.find_first_not_of(" \t");
        line.indent = first == std::string_view::npos
                          ? std::string()
                          : std::string(raw_text.substr(0, first));

        std::string_view raw = trim(strip_comment(raw_text));
        if (!raw.empty()) {
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
        }
        lines.push_back(line);
        if (last) break;
    }
    return lines;
}

// --------------------------------------------------------------- operands ---

// Register number for "r12"/"R12", or -1 for anything else. The pointer names
// X, Y and Z deliberately do not parse: they name pairs, and every rule here
// reasons about single registers.
int register_number(const std::string& op) {
    if (op.size() < 2 || (op[0] != 'r' && op[0] != 'R')) return -1;
    long value = 0;
    for (size_t i = 1; i < op.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(op[i]))) return -1;
        value = value * 10 + (op[i] - '0');
        if (value > 31) return -1;
    }
    return int(value);
}

// Parses the "Y+q" displacement form used by LDD and STD. Only Y is accepted:
// Y is the frame pointer, so a Y+q slot is stack memory that nothing else can
// be watching, whereas Z is a general pointer that could in principle address
// a memory-mapped register.
bool parse_frame_slot(const std::string& op, long& q) {
    if (op.size() < 3) return false;
    if (op[0] != 'Y' && op[0] != 'y') return false;
    if (op[1] != '+') return false;
    long value = 0;
    for (size_t i = 2; i < op.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(op[i]))) return false;
        value = value * 10 + (op[i] - '0');
        if (value > 63) return false;
    }
    q = value;
    return true;
}

// True for a plain symbol name. The assembler reads a bare number after a
// PC-relative instruction as an offset but after CALL or JMP as an address, so
// a CALL may only be turned into an RJMP when its target is a name, whose
// meaning does not depend on which instruction it follows.
bool is_symbol_name(const std::string& op) {
    if (op.empty()) return false;
    char c = op[0];
    if (!(std::isalpha(static_cast<unsigned char>(c)) || c == '_' || c == '.'))
        return false;
    for (char ch : op)
        if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' || ch == '.' ||
              ch == '$'))
            return false;
    return true;
}

// ------------------------------------------------------- instruction facts ---

bool in(const std::set<std::string>& set, const std::string& m) {
    return set.find(m) != set.end();
}

// Writes its first operand's register.
const std::set<std::string>& writes_first_operand() {
    static const std::set<std::string> s = {
        "mov", "add", "adc", "sub", "sbc", "and", "or", "eor", "inc", "dec",
        "com", "neg", "asr", "lsr", "ror", "swap", "pop", "ldi", "subi",
        "sbci", "andi", "ori", "ld", "ldd", "lds", "in", "lsl", "rol", "clr",
        "sbr", "cbr", "bld",
    };
    return s;
}

// Writes its first operand's register and the one above it.
const std::set<std::string>& writes_first_operand_pair() {
    static const std::set<std::string> s = {"movw", "adiw", "sbiw"};
    return s;
}

// Writes no register at all (some set flags, which no rule here depends on).
const std::set<std::string>& writes_nothing() {
    static const std::set<std::string> s = {
        "cp", "cpc", "cpi", "push", "st", "std", "sts", "out", "nop", "tst",
        "sei", "cli", "sec", "clc", "sen", "cln", "sez", "clz", "ses", "cls",
        "sev", "clv", "set", "clt", "seh", "clh", "bst", "sbi", "cbi", "wdr",
        "sleep", "break",
    };
    return s;
}

// Conditionally skips the instruction that follows it.
const std::set<std::string>& skips() {
    static const std::set<std::string> s = {"cpse", "sbrc", "sbrs", "sbic", "sbis"};
    return s;
}

// Transfers control, unconditionally, to somewhere else.
const std::set<std::string>& unconditional_transfers() {
    static const std::set<std::string> s = {"rjmp", "jmp", "ret", "reti", "ijmp"};
    return s;
}

// Every mnemonic above plus the ones that transfer control conditionally or
// with a return address. Anything outside this set is unknown to the optimiser
// and is treated as an opaque barrier.
const std::set<std::string>& known_mnemonics() {
    static const std::set<std::string> s = [] {
        std::set<std::string> all;
        for (const auto* set : {&writes_first_operand(), &writes_first_operand_pair(),
                                &writes_nothing(), &skips(), &unconditional_transfers()})
            all.insert(set->begin(), set->end());
        for (const char* m : {"call", "rcall", "icall", "mul", "lpm", "brne",
                              "breq", "brcs", "brlo", "brcc", "brsh", "brmi",
                              "brpl", "brge", "brlt", "brts", "brtc", "brvs",
                              "brvc", "brhs", "brhc", "brie", "brid"})
            all.insert(m);
        return all;
    }();
    return s;
}

// Every conditional branch. They all read SREG and all leave the register file
// alone.
const std::set<std::string>& branches() {
    static const std::set<std::string> s = {
        "brne", "breq", "brcs", "brlo", "brcc", "brsh", "brmi", "brpl",
        "brge", "brlt", "brts", "brtc", "brvs", "brvc", "brhs", "brhc",
        "brie", "brid", "brbs", "brbc",
    };
    return s;
}

// Anything that can send control somewhere other than the following
// instruction: jumps, branches, calls and returns alike. Following a register
// or a flag past one of these would mean reasoning about two successors, so
// every forward scan here stops at one.
const std::set<std::string>& control_transfers() {
    static const std::set<std::string> s = [] {
        std::set<std::string> all = branches();
        all.insert(unconditional_transfers().begin(), unconditional_transfers().end());
        for (const char* m : {"call", "rcall", "icall"}) all.insert(m);
        return all;
    }();
    return s;
}

bool is_known(const Line& l) { return in(known_mnemonics(), l.mnemonic); }
bool is_skip(const Line& l) { return in(skips(), l.mnemonic); }
bool is_unconditional_transfer(const Line& l) {
    return in(unconditional_transfers(), l.mnemonic);
}
bool is_control_transfer(const Line& l) { return in(control_transfers(), l.mnemonic); }

// Base register of a pointer operand: "X", "-Y", "Z+", "Y+7" and so on. Such
// an operand reads the pair it names, which is invisible in the operand text
// if only `register_number` is consulted.
int pointer_base(const std::string& op) {
    size_t i = (!op.empty() && op[0] == '-') ? 1 : 0;
    if (i >= op.size()) return -1;
    char c = char(std::toupper(static_cast<unsigned char>(op[i])));
    int base = c == 'X' ? 26 : c == 'Y' ? 28 : c == 'Z' ? 30 : -1;
    if (base < 0) return -1;
    std::string rest = op.substr(i + 1);
    if (rest.empty() || rest == "+") return base;
    if (rest[0] != '+') return -1;
    for (size_t k = 1; k < rest.size(); ++k)
        if (!std::isdigit(static_cast<unsigned char>(rest[k]))) return -1;
    return base;
}

// Which operands an instruction reads. Instructions absent from all three sets
// are treated as reading everything.
const std::set<std::string>& reads_both_operands() {
    static const std::set<std::string> s = {
        "add", "adc", "sub", "sbc", "and", "or", "eor", "subi", "sbci",
        "andi", "ori", "sbr", "cbr", "cp", "cpc", "cpi", "cpse", "mul",
    };
    return s;
}
const std::set<std::string>& reads_first_operand_only() {
    static const std::set<std::string> s = {
        "inc", "dec", "com", "neg", "asr", "lsr", "ror", "swap", "lsl",
        "rol", "push", "tst", "bst", "sbrc", "sbrs",
    };
    return s;
}
const std::set<std::string>& reads_second_operand_only() {
    static const std::set<std::string> s = {"mov", "out", "st", "std", "sts"};
    return s;
}
// Reads no register at all. CLR is here because it is an EOR of a register
// with itself: the result does not depend on what was in it.
const std::set<std::string>& reads_no_register() {
    static const std::set<std::string> s = {
        "ldi", "ld",  "ldd", "lds", "in",  "pop", "clr", "bld", "nop",
        "sei", "cli", "sec", "clc", "sen", "cln", "sez", "clz", "ses",
        "cls", "sev", "clv", "set", "clt", "seh", "clh", "sbi", "cbi",
        "sbic", "sbis", "wdr", "sleep", "break",
    };
    return s;
}

// True when `l` may read register `reg`. Unclassified instructions and
// anything that transfers control read everything, so a scan that meets one
// has to stop.
bool reads_register(const Line& l, int reg) {
    if (!is_known(l)) return true;
    const std::string& m = l.mnemonic;
    if (is_control_transfer(l)) return true;
    if (m == "lpm" && l.operands.empty()) return reg == 30 || reg == 31;
    if (m == "lpm") return true;              // the Z-addressing forms
    for (const std::string& op : l.operands) {
        int p = pointer_base(op);
        if (p >= 0 && (reg == p || reg == p + 1)) return true;
    }
    auto op_reg = [&](size_t i) { return i < l.operands.size() ? register_number(l.operands[i]) : -1; };
    if (m == "movw") { int s = op_reg(1); return s >= 0 ? (reg == s || reg == s + 1) : true; }
    if (m == "adiw" || m == "sbiw") {
        int d = op_reg(0);
        return d >= 0 ? (reg == d || reg == d + 1) : true;
    }
    if (in(reads_both_operands(), m)) return op_reg(0) == reg || op_reg(1) == reg;
    if (in(reads_first_operand_only(), m)) return op_reg(0) == reg;
    if (in(reads_second_operand_only(), m)) return op_reg(1) == reg;
    if (in(reads_no_register(), m)) return false;
    return true;
}

// ------------------------------------------------------------------ flags ---
//
// A handful of rules below delete or fold an instruction that writes SREG. That
// is only sound when nothing can observe the flags it would have left, so each
// one asks `flags_dead_after` first: it walks forward looking for an
// instruction that rewrites the whole arithmetic set (C, Z, N, V and S with it)
// before anything reads one. Reaching a label, a control transfer or an
// unreadable instruction means the answer is unknown, which counts as "live".

bool reads_flags(const Line& l) {
    static const std::set<std::string> s = [] {
        std::set<std::string> all = branches();
        for (const char* m : {"adc", "sbc", "sbci", "cpc", "ror", "rol"}) all.insert(m);
        return all;
    }();
    return in(s, l.mnemonic);
}

// Writes C, Z, N, V and S, so whatever they held before is gone.
bool writes_all_arithmetic_flags(const Line& l) {
    static const std::set<std::string> s = {
        "add", "adc", "sub", "subi", "sbc", "sbci", "cp",   "cpc", "cpi",
        "neg", "com", "lsr", "lsl",  "ror", "rol",  "asr",  "adiw", "sbiw",
    };
    return in(s, l.mnemonic);
}

// Anything that reads or moves the stack pointer. IN and OUT are included
// because SPL and SPH are I/O registers, so an OUT can be the stack pointer
// being reloaded.
bool touches_stack(const Line& l) {
    static const std::set<std::string> s = {"push", "pop",  "call", "rcall",
                                            "icall", "ret", "reti", "in", "out"};
    return in(s, l.mnemonic);
}

// True when `l` is known not to change the value of register `reg`. An
// instruction the optimiser cannot classify is assumed to change everything.
bool preserves_register(const Line& l, int reg) {
    if (!is_known(l)) return false;
    if (in(writes_nothing(), l.mnemonic)) return true;
    if (l.mnemonic == "mul") return reg != 0 && reg != 1;
    if (l.mnemonic == "lpm") return !l.operands.empty() ? false : reg != 0;
    if (in(writes_first_operand(), l.mnemonic)) {
        if (l.operands.empty()) return false;
        int d = register_number(l.operands[0]);
        return d >= 0 && d != reg;
    }
    if (in(writes_first_operand_pair(), l.mnemonic)) {
        if (l.operands.empty()) return false;
        int d = register_number(l.operands[0]);
        return d >= 0 && d != reg && d + 1 != reg;
    }
    return false;   // everything else transfers control or is unclassified
}

// ---------------------------------------------------------------- editing ---

std::string render(const Line& l) {
    if (l.mnemonic.empty()) return l.indent + l.label + ":";
    std::string body = l.indent + l.mnemonic;
    for (size_t i = 0; i < l.operands.size(); ++i)
        body += (i == 0 ? " " : ", ") + l.operands[i];
    return l.label.empty() ? body : l.label + ":\n" + body;
}

// Removes the instruction on a line. A label on that line survives as a
// label-only line: something may jump to it, and it must keep acting as a
// barrier for later rules.
void kill(Line& l) {
    if (l.label.empty()) {
        l.dead = true;
        l.mnemonic.clear();
        l.operands.clear();
        l.text.clear();
    } else {
        l.mnemonic.clear();
        l.operands.clear();
        l.text = l.indent + l.label + ":";
    }
}

void replace(Line& l, const std::string& mnemonic, std::vector<std::string> operands) {
    l.mnemonic = mnemonic;
    l.operands = std::move(operands);
    l.text = render(l);
}

// Index of the next line that still holds something, or npos.
size_t next_live(const std::vector<Line>& lines, size_t i) {
    for (size_t j = i + 1; j < lines.size(); ++j)
        if (!lines[j].dead && (!lines[j].label.empty() || !lines[j].mnemonic.empty()))
            return j;
    return std::string::npos;
}

size_t prev_live(const std::vector<Line>& lines, size_t i) {
    for (size_t j = i; j-- > 0;)
        if (!lines[j].dead && (!lines[j].label.empty() || !lines[j].mnemonic.empty()))
            return j;
    return std::string::npos;
}

// True when the instruction at `i` may be skipped by the one before it, in
// which case rewriting it would change which instruction the skip lands on.
bool may_be_skipped(const std::vector<Line>& lines, size_t i) {
    size_t p = prev_live(lines, i);
    return p != std::string::npos && is_skip(lines[p]);
}

// True when no instruction after `i` can observe the flags left by the
// instruction at `i`. See the note above `reads_flags`. The search is bounded:
// straight-line runs in generated code are short, and giving up early only
// ever means declining a rewrite.
bool flags_dead_after(const std::vector<Line>& lines, size_t i) {
    int budget = 64;
    for (size_t j = next_live(lines, i); j != std::string::npos;
         j = next_live(lines, j)) {
        if (--budget < 0) return false;
        const Line& l = lines[j];
        if (!l.label.empty()) return false;          // control could arrive here
        if (!is_known(l)) return false;
        if (reads_flags(l)) return false;
        if (writes_all_arithmetic_flags(l)) return true;
        if (is_skip(l)) return false;                // both successors to check
        if (is_control_transfer(l)) return false;
    }
    return false;
}

// ------------------------------------------------------------- addressing ---
//
// RJMP and RCALL reach +/-2048 words. Merging a tail or outlining a run puts a
// relative transfer where there was none, so both need to know how far apart
// two lines will end up. Every instruction the optimiser knows has a fixed
// size, but a directive can assemble to anything, so a file containing one
// gets no offsets at all and both transforms stand down.

int word_size(const Line& l) {
    if (l.mnemonic.empty()) return 0;
    if (l.mnemonic == "call" || l.mnemonic == "jmp" || l.mnemonic == "lds" ||
        l.mnemonic == "sts")
        return 2;
    return 1;
}

bool word_offsets(const std::vector<Line>& lines, std::vector<long>& offsets) {
    offsets.assign(lines.size() + 1, 0);
    long pc = 0;
    for (size_t i = 0; i < lines.size(); ++i) {
        offsets[i] = pc;
        const Line& l = lines[i];
        if (l.dead || l.mnemonic.empty()) continue;
        if (!is_known(l)) return false;               // a directive: size unknown
        pc += word_size(l);
    }
    offsets[lines.size()] = pc;
    return true;
}

// Kept well inside the architectural +/-2048 so that a jump stays in range even
// though the offsets were measured before the rest of the pass ran. Every rule
// here only ever removes words, so a distance measured now can only shrink --
// except for the outlined subroutines appended at the end, whose length is
// added in explicitly at the call site.
const long kRelativeReach = 1800;

// ------------------------------------------------------------------ rules ---

// mov rA, rA -- a register moved onto itself. MOV touches no flags, so the
// instruction has no effect at all.
bool rule_self_move(std::vector<Line>& lines) {
    bool changed = false;
    for (auto& l : lines) {
        if (l.mnemonic != "mov" || l.operands.size() != 2) continue;
        int d = register_number(l.operands[0]);
        int s = register_number(l.operands[1]);
        if (d < 0 || d != s) continue;
        kill(l);
        changed = true;
    }
    return changed;
}

// push rX ... pop rX, where the span between provably leaves rX alone and
// leaves the stack balanced. Both halves then cancel: rX ends up holding what
// it already held, and SP is where it started.
//
// The span may not contain a label (control could enter with rX holding
// something else, or leave without reaching the pop), anything that touches
// the stack, or any instruction the optimiser cannot classify.
bool rule_push_pop(std::vector<Line>& lines) {
    bool changed = false;
    for (size_t i = 0; i < lines.size(); ++i) {
        Line& push = lines[i];
        if (push.mnemonic != "push" || push.operands.size() != 1) continue;
        int reg = register_number(push.operands[0]);
        if (reg < 0) continue;

        for (size_t j = next_live(lines, i); j != std::string::npos;
             j = next_live(lines, j)) {
            const Line& mid = lines[j];
            if (!mid.label.empty()) break;              // never rewrite across a label
            if (mid.mnemonic == "pop" && mid.operands.size() == 1 &&
                register_number(mid.operands[0]) == reg) {
                kill(lines[i]);
                kill(lines[j]);
                changed = true;
                break;
            }
            if (touches_stack(mid)) break;
            if (is_skip(mid)) break;
            if (!preserves_register(mid, reg)) break;
        }
    }
    return changed;
}

// std Y+q, rX followed straight away by ldd rX, Y+q -- the value is already in
// rX, so the load reads back what it just wrote.
bool rule_store_then_load(std::vector<Line>& lines) {
    bool changed = false;
    for (size_t i = 0; i < lines.size(); ++i) {
        const Line& store = lines[i];
        if (store.mnemonic != "std" || store.operands.size() != 2) continue;
        long q = 0;
        if (!parse_frame_slot(store.operands[0], q)) continue;
        int reg = register_number(store.operands[1]);
        if (reg < 0) continue;

        size_t j = next_live(lines, i);
        if (j == std::string::npos) continue;
        const Line& load = lines[j];
        if (!load.label.empty() || load.mnemonic != "ldd" || load.operands.size() != 2)
            continue;
        long lq = 0;
        if (register_number(load.operands[0]) != reg ||
            !parse_frame_slot(load.operands[1], lq) || lq != q)
            continue;
        kill(lines[j]);
        changed = true;
    }
    return changed;
}

// The same ldd twice in a row: the second reloads a slot nothing has written
// into a register nothing has changed.
bool rule_repeated_load(std::vector<Line>& lines) {
    bool changed = false;
    for (size_t i = 0; i < lines.size(); ++i) {
        const Line& first = lines[i];
        if (first.mnemonic != "ldd" || first.operands.size() != 2) continue;
        long q = 0;
        if (!parse_frame_slot(first.operands[1], q)) continue;
        int reg = register_number(first.operands[0]);
        if (reg < 0) continue;

        size_t j = next_live(lines, i);
        if (j == std::string::npos) continue;
        const Line& second = lines[j];
        if (!second.label.empty() || second.mnemonic != "ldd" ||
            second.operands.size() != 2)
            continue;
        long sq = 0;
        if (register_number(second.operands[0]) != reg ||
            !parse_frame_slot(second.operands[1], sq) || sq != q)
            continue;
        kill(lines[j]);
        changed = true;
    }
    return changed;
}

// Two MOVs that together copy an even-aligned register pair become one MOVW.
// Either order works, because a MOVW-able pair is either identical (rejected
// here, and already removed by the self-move rule) or entirely disjoint, so
// neither MOV can feed the other.
bool rule_pair_move(std::vector<Line>& lines) {
    bool changed = false;
    for (size_t i = 0; i < lines.size(); ++i) {
        const Line& a = lines[i];
        if (a.mnemonic != "mov" || a.operands.size() != 2) continue;
        size_t j = next_live(lines, i);
        if (j == std::string::npos) continue;
        const Line& b = lines[j];
        if (!b.label.empty() || b.mnemonic != "mov" || b.operands.size() != 2) continue;

        int da = register_number(a.operands[0]), sa = register_number(a.operands[1]);
        int db = register_number(b.operands[0]), sb = register_number(b.operands[1]);
        if (da < 0 || sa < 0 || db < 0 || sb < 0) continue;

        int d = -1, s = -1;
        if ((da % 2) == 0 && db == da + 1 && (sa % 2) == 0 && sb == sa + 1) {
            d = da; s = sa;                              // low half first
        } else if ((db % 2) == 0 && da == db + 1 && (sb % 2) == 0 && sa == sb + 1) {
            d = db; s = sb;                              // high half first
        } else {
            continue;
        }
        if (d == s) continue;                            // a pair of no-ops

        replace(lines[i], "movw", {"r" + std::to_string(d), "r" + std::to_string(s)});
        kill(lines[j]);
        changed = true;
    }
    return changed;
}

// call X followed by ret is a tail call: jumping to X directly lets X's own
// return go straight to our caller. This drops a whole instruction and a
// return, and CALL, RET and RJMP all leave the flags alone.
bool rule_tail_call(std::vector<Line>& lines) {
    bool changed = false;
    for (size_t i = 0; i < lines.size(); ++i) {
        const Line& call = lines[i];
        if ((call.mnemonic != "call" && call.mnemonic != "rcall") ||
            call.operands.size() != 1)
            continue;
        if (!is_symbol_name(call.operands[0])) continue;
        if (may_be_skipped(lines, i)) continue;

        size_t j = next_live(lines, i);
        if (j == std::string::npos) continue;
        const Line& ret = lines[j];
        // A label on the RET means something else can land on it, so it has to
        // stay.
        if (!ret.label.empty() || ret.mnemonic != "ret" || !ret.operands.empty())
            continue;

        replace(lines[i], "rjmp", {call.operands[0]});
        kill(lines[j]);
        changed = true;
    }
    return changed;
}

// rjmp to a label that is already the next thing in the file. Falling through
// gets there for free.
bool rule_jump_to_next(std::vector<Line>& lines) {
    bool changed = false;
    for (size_t i = 0; i < lines.size(); ++i) {
        const Line& jump = lines[i];
        if ((jump.mnemonic != "rjmp" && jump.mnemonic != "jmp") ||
            jump.operands.size() != 1)
            continue;
        if (may_be_skipped(lines, i)) continue;

        for (size_t j = next_live(lines, i); j != std::string::npos;
             j = next_live(lines, j)) {
            if (lines[j].label == jump.operands[0]) {
                kill(lines[i]);
                changed = true;
                break;
            }
            if (!lines[j].mnemonic.empty()) break;       // reached real code
        }
    }
    return changed;
}

// Instructions between an unconditional transfer and the next label can never
// run: control left, and nothing can jump back in without a label to aim at.
bool rule_unreachable(std::vector<Line>& lines) {
    bool changed = false;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (lines[i].dead || !is_unconditional_transfer(lines[i])) continue;
        // If a skip precedes the transfer, the instruction after it is exactly
        // what the skip jumps to, and is very much reachable.
        if (may_be_skipped(lines, i)) continue;

        for (size_t j = next_live(lines, i); j != std::string::npos;
             j = next_live(lines, j)) {
            if (!lines[j].label.empty()) break;
            if (!is_known(lines[j])) break;              // directive or unknown
            kill(lines[j]);
            changed = true;
        }
    }
    return changed;
}

// ldi rD, K where rD already provably holds K. The first LDI reaches the
// second through straight-line code that writes neither the register nor
// anything the optimiser cannot read, so the second one loads a value that is
// already there. LDI touches no flags, so dropping it is invisible.
//
// The constant is compared as text. "5" and "0x05" are the same number, but
// the code generator spells a given value one way, and refusing to fold two
// spellings only costs an opportunity.
bool rule_redundant_immediate(std::vector<Line>& lines) {
    bool changed = false;
    for (size_t i = 0; i < lines.size(); ++i) {
        const Line& first = lines[i];
        if (first.mnemonic != "ldi" || first.operands.size() != 2) continue;
        int reg = register_number(first.operands[0]);
        if (reg < 0) continue;
        const std::string value = first.operands[1];

        int budget = 64;
        for (size_t j = next_live(lines, i); j != std::string::npos;
             j = next_live(lines, j)) {
            if (--budget < 0) break;
            const Line& l = lines[j];
            if (!l.label.empty()) break;              // rD is unknown past a label
            if (!is_known(l)) break;
            if (is_skip(l)) break;                    // the next line may not run
            if (l.mnemonic == "ldi" && l.operands.size() == 2 &&
                register_number(l.operands[0]) == reg && l.operands[1] == value) {
                kill(lines[j]);
                changed = true;
                break;
            }
            if (!preserves_register(l, reg)) break;   // also catches every transfer
        }
    }
    return changed;
}

// A plain non-negative constant, for the ADIW/SBIW folding below. Anything
// symbolic or signed is refused: its value is not known here.
bool parse_constant(const std::string& op, long& value) {
    if (op.empty()) return false;
    size_t i = 0;
    int base = 10;
    if (op.size() > 2 && op[0] == '0' && (op[1] == 'x' || op[1] == 'X')) {
        base = 16;
        i = 2;
    }
    if (i >= op.size()) return false;
    long v = 0;
    for (; i < op.size(); ++i) {
        int digit;
        char c = char(std::tolower(static_cast<unsigned char>(op[i])));
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (base == 16 && c >= 'a' && c <= 'f') digit = 10 + (c - 'a');
        else return false;
        v = v * base + digit;
        if (v > 0xFFFF) return false;
    }
    value = v;
    return true;
}

// Two adjacent constant adjustments of the same pointer pair fold into one:
// adiw r24, 4 then adiw r24, 6 is adiw r24, 10, and adiw r24, 4 then
// sbiw r24, 4 is nothing at all.
//
// The register result is identical, and so are Z, N and S, which depend only
// on it. C and V are not: they describe the addition that produced the result,
// and one addition of ten carries differently from two of four and six. The
// fold therefore only happens where nothing can read the flags afterwards.
bool rule_merge_pointer_adjust(std::vector<Line>& lines) {
    bool changed = false;
    for (size_t i = 0; i < lines.size(); ++i) {
        const Line& first = lines[i];
        if ((first.mnemonic != "adiw" && first.mnemonic != "sbiw") ||
            first.operands.size() != 2)
            continue;
        int reg = register_number(first.operands[0]);
        long k1 = 0;
        if (reg < 0 || !parse_constant(first.operands[1], k1)) continue;
        if (may_be_skipped(lines, i)) continue;

        size_t j = next_live(lines, i);
        if (j == std::string::npos) continue;
        const Line& second = lines[j];
        if (!second.label.empty()) continue;
        if ((second.mnemonic != "adiw" && second.mnemonic != "sbiw") ||
            second.operands.size() != 2)
            continue;
        long k2 = 0;
        if (register_number(second.operands[0]) != reg ||
            !parse_constant(second.operands[1], k2))
            continue;

        long net = (first.mnemonic == "adiw" ? k1 : -k1) +
                   (second.mnemonic == "adiw" ? k2 : -k2);
        if (net > 63 || net < -63) continue;          // no single instruction fits
        if (!flags_dead_after(lines, j)) continue;

        if (net == 0) {
            kill(lines[i]);
            kill(lines[j]);
        } else {
            replace(lines[i], net > 0 ? "adiw" : "sbiw",
                    {first.operands[0], std::to_string(net > 0 ? net : -net)});
            kill(lines[j]);
        }
        changed = true;
    }
    return changed;
}

// clr rX immediately followed by ldi rX, k. The LDI overwrites every bit the
// CLR wrote, so the only thing the CLR still contributes is its flags -- and
// LDI leaves those alone, so they survive the pair. The CLR goes only when
// nothing downstream can read them.
bool rule_clear_before_load(std::vector<Line>& lines) {
    bool changed = false;
    for (size_t i = 0; i < lines.size(); ++i) {
        const Line& clr = lines[i];
        if (clr.mnemonic != "clr" || clr.operands.size() != 1) continue;
        int reg = register_number(clr.operands[0]);
        if (reg < 0) continue;
        if (may_be_skipped(lines, i)) continue;

        size_t j = next_live(lines, i);
        if (j == std::string::npos) continue;
        const Line& load = lines[j];
        if (!load.label.empty() || load.mnemonic != "ldi" || load.operands.size() != 2)
            continue;
        if (register_number(load.operands[0]) != reg) continue;
        if (!flags_dead_after(lines, j)) continue;

        kill(lines[i]);
        changed = true;
    }
    return changed;
}

// A register written and then written again before anything reads it. The
// first write produced a value nobody could ever see.
//
// Only writes that read nothing and touch no flags qualify -- LDI, MOV and
// MOVW -- so removing one cannot disturb a later branch. Every memory load is
// deliberately absent: LDS and the pointer-relative forms can name a
// peripheral register, where the read itself is the point. POP and IN are out
// for the same reason, one moving the stack pointer and the other reading I/O.
//
// MOVW is the pair case: the copy goes only when both halves are overwritten.
//
// The scan also stops at anything touching the stack. A POP overwrites its
// register, so it would count as the second write, but pairing a POP with what
// came before it belongs to the push/pop rule, not this one.
bool rule_dead_write(std::vector<Line>& lines) {
    bool changed = false;
    for (size_t i = 0; i < lines.size(); ++i) {
        const Line& write = lines[i];
        std::vector<int> pending;
        if ((write.mnemonic == "ldi" || write.mnemonic == "mov") &&
            write.operands.size() == 2) {
            int d = register_number(write.operands[0]);
            if (d < 0) continue;
            pending = {d};
        } else if (write.mnemonic == "movw" && write.operands.size() == 2) {
            int d = register_number(write.operands[0]);
            if (d < 0 || d % 2 != 0) continue;
            pending = {d, d + 1};
        } else {
            continue;
        }
        if (may_be_skipped(lines, i)) continue;

        int budget = 64;
        for (size_t j = next_live(lines, i); j != std::string::npos && !pending.empty();
             j = next_live(lines, j)) {
            if (--budget < 0) break;
            const Line& l = lines[j];
            if (!l.label.empty()) break;              // another path may read it
            if (!is_known(l) || is_skip(l) || is_control_transfer(l)) break;
            if (touches_stack(l)) break;

            bool read = false;
            for (int reg : pending)
                if (reads_register(l, reg)) read = true;
            if (read) break;

            pending.erase(std::remove_if(pending.begin(), pending.end(),
                                         [&](int reg) {
                                             return !preserves_register(l, reg);
                                         }),
                          pending.end());
        }
        if (pending.empty()) {
            kill(lines[i]);
            changed = true;
        }
    }
    return changed;
}

// --------------------------------------------------------- tail merging ---
//
// Functions that share an ending share code: two epilogues that pop the same
// callee-saved registers and return are the same instructions twice over. The
// earlier one is replaced by a jump into the later one's copy.
//
// This is sound because the instructions executed are identical -- what the
// pops find on the stack is the caller's business, not the tail's. The care is
// all in the edges:
//
//   * The run being deleted may carry no label below its first line. A label
//     there could be jumped to from inside the function that is losing the
//     code, and the jump would land in the other function.
//   * The run must end in an unconditional transfer, so control leaves rather
//     than falling out of one function into the next.
//   * The instruction before the run may not be a skip: the jump that replaces
//     the run is what would get skipped, not the whole tail.
//   * The jump is relative, so the two runs have to be within reach.

// Compares two lines as instructions, ignoring labels and spacing.
bool same_instruction(const Line& a, const Line& b) {
    return a.mnemonic == b.mnemonic && a.operands == b.operands;
}

std::string fresh_label(const std::string& stem, int& counter,
                        std::set<std::string>& used) {
    for (;;) {
        std::string name = stem + std::to_string(counter++);
        if (used.insert(name).second) return name;
    }
}

bool rule_merge_tails(std::vector<Line>& lines, int& counter,
                      std::set<std::string>& used) {
    if (getenv("NOTAIL")) return false;
    std::vector<long> offsets;
    if (!word_offsets(lines, offsets)) return false;   // a directive: sizes unknown

    // Every line that ends a run of straight-line code by leaving it.
    std::vector<size_t> ends;
    for (size_t i = 0; i < lines.size(); ++i)
        if (!lines[i].dead && is_unconditional_transfer(lines[i])) ends.push_back(i);

    // How far back two runs agree, and whether the earlier one may be deleted.
    auto common_run = [&](size_t a, size_t b, size_t& a_top, size_t& b_top,
                          long& saved) {
        size_t pa = a, pb = b;
        size_t best_a = std::string::npos, best_b = 0;
        long words = 0, best_saved = 0;
        for (;;) {
            if (pa == std::string::npos || pb == std::string::npos) break;
            if (!same_instruction(lines[pa], lines[pb])) break;
            words += word_size(lines[pa]);
            // The earlier run is about to disappear, so past its first line it
            // may hold nothing anybody can jump to.
            bool deletable = pa == a || lines[pa].label.empty();
            if (!deletable) break;
            if (pb <= a) break;                        // the two runs overlap
            if (!may_be_skipped(lines, pa) && words - 1 > best_saved) {
                best_saved = words - 1;
                best_a = pa;
                best_b = pb;
            }
            if (!lines[pa].label.empty()) break;       // cannot extend past it
            pa = prev_live(lines, pa);
            pb = prev_live(lines, pb);
        }
        if (best_a == std::string::npos || best_saved <= 0) return false;
        a_top = best_a;
        b_top = best_b;
        saved = best_saved;
        return true;
    };

    bool changed = false;
    std::vector<bool> spent(lines.size(), false);
    for (size_t x = 0; x < ends.size(); ++x) {
        size_t a = ends[x];
        if (spent[a] || lines[a].dead) continue;
        size_t best_a = 0, best_b = 0;
        long best_saved = 0;
        for (size_t y = x + 1; y < ends.size(); ++y) {
            size_t b = ends[y];
            if (spent[b] || lines[b].dead) continue;
            size_t ta = 0, tb = 0;
            long saved = 0;
            if (!common_run(a, b, ta, tb, saved)) continue;
            if (saved <= best_saved) continue;
            // The replacement jump must reach: it sits where the earlier run
            // started and aims at the later run's first line.
            long distance = offsets[tb] - (offsets[ta] + 1);
            if (distance > kRelativeReach || distance < -kRelativeReach) continue;
            best_saved = saved;
            best_a = ta;
            best_b = tb;
        }
        if (best_saved <= 0) continue;

        if (lines[best_b].label.empty()) {
            lines[best_b].label = fresh_label(".Ltail", counter, used);
            lines[best_b].text = render(lines[best_b]);
        }
        const std::string target = lines[best_b].label;

        // Drop the earlier run, keeping only a jump where it started.
        for (size_t k = next_live(lines, best_a); k != std::string::npos && k <= a;
             k = next_live(lines, k))
            kill(lines[k]);
        replace(lines[best_a], "rjmp", {target});
        for (size_t k = best_a; k <= a && k < lines.size(); ++k) spent[k] = true;
        changed = true;
    }
    return changed;
}

// ------------------------------------------------------------- outlining ---
//
// A run of instructions that recurs across the whole program can live in one
// place and be reached by RCALL. That trades the run's words at each site for
// one call, against one copy of the run plus a RET, so it pays only when the
// run is long enough and common enough; the arithmetic below works out whether
// it does rather than assuming a threshold.
//
// What may be outlined is deliberately narrow:
//
//   * No labels, so nothing can jump into the middle of a copy.
//   * No jumps, branches, calls or returns: a relative jump out of the run
//     would aim somewhere else once the run has moved, and a conditional
//     branch would give the run two exits.
//   * Nothing that touches the stack or the stack pointer. RCALL pushes a
//     return address, so an unbalanced POP inside the run would take that
//     address instead of the caller's data.
//   * No skip instructions, whose effect depends on what physically follows
//     them, and no run that starts directly after one, since the RCALL alone
//     would be skipped in place of the whole run.
//
// Flags cross the boundary safely without a check: RCALL and RET are among the
// few AVR instructions that leave SREG completely alone, so a run that sets
// flags for a branch after it still does.

const size_t kMaxOutlineLength = 12;

bool rule_outline(std::vector<Line>& lines, int& counter, std::set<std::string>& used,
                  size_t& generated_from) {
    bool changed = false;
    if (getenv("NOOUT")) return false;
    for (int pass = 0; pass < 64; ++pass) {
        std::vector<long> offsets;
        if (!word_offsets(lines, offsets)) return changed;
        const long end_of_program = offsets[lines.size()];

        // Candidate instructions, in program order, tagged with which run of
        // consecutive candidates they belong to.
        std::vector<size_t> flat;
        std::vector<int> run_of;
        std::vector<int> region_of;
        std::vector<int> id_of;
        std::map<std::string, int> ids;
        int run = 0;
        int region = 0;
        size_t expected = std::string::npos;
        for (size_t i = 0; i < lines.size() && i < generated_from; ++i) {
            const Line& l = lines[i];
            if (l.dead) continue;
            if (!l.label.empty()) ++region;      // a new stretch of the program
            if (l.mnemonic.empty()) continue;
            bool usable = l.label.empty() && is_known(l) && !is_skip(l) &&
                          !is_control_transfer(l) && !touches_stack(l);
            if (!usable) {
                expected = std::string::npos;
                continue;
            }
            if (i != expected) ++run;
            std::string key = l.mnemonic;
            for (const std::string& op : l.operands) key += '\x01' + op;
            auto it = ids.emplace(key, int(ids.size())).first;
            flat.push_back(i);
            run_of.push_back(run);
            region_of.push_back(region);
            id_of.push_back(it->second);
            expected = next_live(lines, i);
        }

        // The best (length, occurrence set) by words saved.
        size_t best_n = 0;
        std::vector<size_t> best_sites;
        std::vector<bool> best_far;
        long best_saved = 0;

        for (size_t n = 2; n <= kMaxOutlineLength && n <= flat.size(); ++n) {
            std::unordered_map<unsigned long long, std::vector<size_t>> buckets;
            for (size_t p = 0; p + n <= flat.size(); ++p) {
                if (run_of[p + n - 1] != run_of[p]) continue;
                unsigned long long h = 1469598103934665603ull;
                for (size_t t = 0; t < n; ++t) {
                    h ^= static_cast<unsigned long long>(id_of[p + t]) + 1;
                    h *= 1099511628211ull;
                }
                buckets[h].push_back(p);
            }
            for (const auto& bucket : buckets) {
                if (bucket.second.size() < 2) continue;
                const size_t first = bucket.second.front();
                long body = 0;
                for (size_t t = 0; t < n; ++t) body += word_size(lines[flat[first + t]]);

                // Occurrences of the same run, left to right and not
                // overlapping, each classified by whether an RCALL reaches the
                // subroutine that will sit at the end of the program.
                std::vector<size_t> sites;
                std::vector<bool> far;
                long call_words = 0;
                size_t taken_to = 0;
                const long target = end_of_program + body + 1;
                for (size_t p : bucket.second) {
                    bool equal = true;
                    for (size_t t = 0; t < n && equal; ++t)
                        equal = id_of[p + t] == id_of[first + t];
                    if (!equal) continue;              // a hash collision
                    if (!sites.empty() && p < taken_to) continue;
                    if (may_be_skipped(lines, flat[p])) continue;
                    long distance = target - (offsets[flat[p]] + 1);
                    bool needs_call = distance > kRelativeReach || distance < -kRelativeReach;
                    sites.push_back(p);
                    far.push_back(needs_call);
                    call_words += needs_call ? 2 : 1;
                    taken_to = p + n;
                }
                // Two copies is a coincidence. Every site outlined also costs
                // two more bytes of return-address stack on the paths through
                // it, and at two sites the whole prize is one call's worth of
                // words, so the bar is three.
                if (sites.size() < 3) continue;
                // A run that recurs across the program is worth a subroutine;
                // one that only repeats inside a single stretch of
                // straight-line code is a loop the code generator failed to
                // roll, and turning it into calls buys much less. Requiring two
                // separate stretches keeps the transform to what it is for.
                bool spread = false;
                for (size_t s = 1; s < sites.size(); ++s)
                    if (region_of[sites[s]] != region_of[sites[0]]) spread = true;
                if (!spread) continue;
                long saved = long(sites.size()) * body - call_words - (body + 1);
                if (saved <= best_saved) continue;
                best_saved = saved;
                best_n = n;
                best_sites = sites;
                best_far = far;
            }
        }

        if (best_saved <= 0) return changed;

        // Move one copy out to a subroutine and call it from every site.
        const std::string name = fresh_label(".Loutlined", counter, used);
        std::vector<Line> body;
        for (size_t t = 0; t < best_n; ++t) {
            Line copy = lines[flat[best_sites[0] + t]];
            copy.label.clear();
            copy.indent = "    ";
            copy.dead = false;
            copy.text = render(copy);
            body.push_back(copy);
        }
        body.front().label = name;
        body.front().text = render(body.front());
        Line ret;
        ret.indent = "    ";
        ret.mnemonic = "ret";
        ret.text = render(ret);
        body.push_back(ret);

        for (size_t s = 0; s < best_sites.size(); ++s) {
            size_t p = best_sites[s];
            for (size_t t = 1; t < best_n; ++t) kill(lines[flat[p + t]]);
            replace(lines[flat[p]], best_far[s] ? "call" : "rcall", {name});
        }
        if (generated_from > lines.size()) generated_from = lines.size();
        lines.insert(lines.end(), body.begin(), body.end());
        changed = true;
    }
    return changed;
}

} // namespace

std::string optimise_assembly(const std::string& assembly) {
    std::vector<Line> lines = split_lines(assembly);

    // Each rule can expose work for another -- deleting a POP can make a MOV
    // pair adjacent, a tail call can make the code after it unreachable -- so
    // the set runs to a fixed point. The cap is a safety net: every rule only
    // ever removes an instruction or shortens a run, so the loop is already
    // bounded by the size of the input.
    auto local_rules = [&] {
        for (int round = 0; round < 16; ++round) {
            bool changed = false;
            changed |= rule_self_move(lines);
            changed |= rule_push_pop(lines);
            changed |= rule_store_then_load(lines);
            changed |= rule_repeated_load(lines);
            changed |= rule_pair_move(lines);
            changed |= rule_redundant_immediate(lines);
            changed |= rule_merge_pointer_adjust(lines);
            changed |= rule_clear_before_load(lines);
            changed |= rule_dead_write(lines);
            changed |= rule_tail_call(lines);
            changed |= rule_jump_to_next(lines);
            changed |= rule_unreachable(lines);
            if (!changed) break;
        }
    };

    local_rules();

    // The two whole-program transforms run after the local rules have settled,
    // so they work on code that is already as small as the peephole can make
    // it, and are followed by another sweep to clean up what they expose -- an
    // RCALL followed by a RET becomes a tail call, a jump to the next line
    // disappears.
    //
    // Names generated here must not collide with anything already in the file.
    std::set<std::string> used_labels;
    for (const Line& l : lines)
        if (!l.label.empty()) used_labels.insert(l.label);
    int counter = 0;
    size_t generated_from = size_t(-1);

    // Tail merging and outlining are implemented below but disabled: their
    // safety guards do not yet hold. Outlining a run that a skip instruction
    // guards makes the skip jump over the whole run instead of one instruction,
    // and merging a tail whose middle is a jump target strands that target.
    // Both emit wrong code, so they stay off until the guards are correct.
    // The tests that pin those cases are marked accordingly.
    constexpr bool kStructuralRulesReady = false;
    if (kStructuralRulesReady) {
        if (rule_merge_tails(lines, counter, used_labels)) local_rules();
        if (rule_outline(lines, counter, used_labels, generated_from)) local_rules();
    }

    std::string out;
    out.reserve(assembly.size());
    for (size_t i = 0; i < lines.size(); ++i) {
        if (lines[i].dead) continue;
        out += lines[i].text;
        out += '\n';
    }
    // A source with no trailing newline gets one back only if it had one; the
    // split above turns a trailing newline into a final empty line.
    if (!assembly.empty() && assembly.back() != '\n' && !out.empty())
        out.pop_back();
    return out;
}

} // namespace ardio
