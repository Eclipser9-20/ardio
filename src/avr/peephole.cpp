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

#include "ardio/avr/peephole.h"

#include <cctype>
#include <cstdlib>
#include <set>
#include <string>
#include <string_view>
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

bool is_known(const Line& l) { return in(known_mnemonics(), l.mnemonic); }
bool is_skip(const Line& l) { return in(skips(), l.mnemonic); }
bool is_unconditional_transfer(const Line& l) {
    return in(unconditional_transfers(), l.mnemonic);
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

} // namespace

std::string optimise_assembly(const std::string& assembly) {
    std::vector<Line> lines = split_lines(assembly);

    // Each rule can expose work for another -- deleting a POP can make a MOV
    // pair adjacent, a tail call can make the code after it unreachable -- so
    // the set runs to a fixed point. The cap is a safety net: every rule only
    // ever removes an instruction or shortens a run, so the loop is already
    // bounded by the size of the input.
    for (int round = 0; round < 16; ++round) {
        bool changed = false;
        changed |= rule_self_move(lines);
        changed |= rule_push_pop(lines);
        changed |= rule_store_then_load(lines);
        changed |= rule_repeated_load(lines);
        changed |= rule_pair_move(lines);
        changed |= rule_tail_call(lines);
        changed |= rule_jump_to_next(lines);
        changed |= rule_unreachable(lines);
        if (!changed) break;
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
