#include "ardio/avr/preprocess.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace ardio {
namespace {

constexpr int kMaxIncludeDepth = 32;
constexpr int kMaxExpansionDepth = 64;

// ------------------------------------------------------------- helpers -----

bool ident_start(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
bool ident_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

// A physical-source line after splicing and comment removal.
struct Line {
    std::string text;
    size_t number = 0;
};

// Replacement-list token: identifiers are kept whole so parameters can be
// matched; everything else is punctuation or a literal.
struct RTok {
    std::string text;
    bool ident = false;
    bool space_before = false;
};

struct Macro {
    bool function_like = false;
    bool variadic = false;
    std::vector<std::string> params;
    std::vector<RTok> body;
};

std::vector<RTok> split_tokens(const std::string& s) {
    std::vector<RTok> out;
    size_t i = 0;
    bool space = false;
    while (i < s.size()) {
        char c = s[i];
        if (std::isspace(static_cast<unsigned char>(c))) { space = true; ++i; continue; }
        RTok t;
        t.space_before = space;
        space = false;
        if (ident_start(c)) {
            size_t b = i;
            while (i < s.size() && ident_char(s[i])) ++i;
            t.text = s.substr(b, i - b);
            t.ident = true;
        } else if (c == '"' || c == '\'') {
            size_t b = i;
            char q = c;
            ++i;
            while (i < s.size()) {
                if (s[i] == '\\' && i + 1 < s.size()) { i += 2; continue; }
                if (s[i] == q) { ++i; break; }
                ++i;
            }
            t.text = s.substr(b, i - b);
        } else if (c == '#' && i + 1 < s.size() && s[i + 1] == '#') {
            t.text = "##";
            i += 2;
        } else {
            t.text = std::string(1, c);
            ++i;
        }
        out.push_back(t);
    }
    return out;
}

std::string stringize(const std::string& raw) {
    std::string out = "\"";
    for (char c : trim(raw)) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    out += '"';
    return out;
}

// Splices backslash-newlines, removes comments and returns logical lines with
// the physical line number each one started on.
std::vector<Line> clean_lines(const std::string& src) {
    std::vector<Line> lines;
    std::string cur;
    size_t line_no = 1;
    size_t start_line = 1;
    size_t i = 0;
    bool in_block = false;
    size_t n = src.size();

    auto flush = [&]() {
        lines.push_back({cur, start_line});
        cur.clear();
    };

    while (i < n) {
        char c = src[i];
        if (in_block) {
            if (c == '*' && i + 1 < n && src[i + 1] == '/') { in_block = false; i += 2; }
            else { if (c == '\n') ++line_no; ++i; }
            continue;
        }
        if (c == '\\' && i + 1 < n && src[i + 1] == '\n') { i += 2; ++line_no; continue; }
        if (c == '\\' && i + 2 < n && src[i + 1] == '\r' && src[i + 2] == '\n') { i += 3; ++line_no; continue; }
        if (c == '\r') { ++i; continue; }
        if (c == '\n') {
            flush();
            ++line_no;
            start_line = line_no;
            ++i;
            continue;
        }
        if (c == '/' && i + 1 < n && src[i + 1] == '/') {
            while (i < n && src[i] != '\n') {
                if (src[i] == '\\' && i + 1 < n && src[i + 1] == '\n') { ++line_no; i += 2; continue; }
                ++i;
            }
            continue;
        }
        if (c == '/' && i + 1 < n && src[i + 1] == '*') {
            in_block = true;
            cur += ' ';
            i += 2;
            continue;
        }
        if (c == '"' || c == '\'') {
            char q = c;
            cur += c;
            ++i;
            while (i < n && src[i] != '\n') {
                if (src[i] == '\\' && i + 1 < n) { cur += src[i]; cur += src[i + 1]; i += 2; continue; }
                cur += src[i];
                if (src[i] == q) { ++i; break; }
                ++i;
            }
            continue;
        }
        cur += c;
        ++i;
    }
    if (!cur.empty() || lines.empty()) flush();
    return lines;
}

// --------------------------------------------------------- preprocessor ----

struct Cond {
    bool parent_active = true;
    bool taken = false;    // some branch of this chain has been taken
    bool active = false;
};

class Preprocessor {
public:
    explicit Preprocessor(const std::vector<std::string>& paths) : paths_(paths) {}

    bool failed() const { return failed_; }
    const std::string& error() const { return error_; }
    std::string take_output() { return out_; }

    bool run(const std::string& text, const std::string& file, const std::string& dir, int depth);

private:
    void fail(const std::string& file, size_t line, const std::string& msg) {
        if (failed_) return;
        failed_ = true;
        error_ = file + ":" + std::to_string(line) + ": " + msg;
    }

    std::string expand(const std::string& in, const std::set<std::string>& active,
                       int depth, const std::string& file, size_t line);
    std::string substitute(const Macro& m, const std::vector<std::string>& raw,
                           const std::vector<std::string>& exp);
    bool eval_condition(const std::string& expr, const std::string& file, size_t line);
    bool do_include(const std::string& rest, const std::string& file, size_t line,
                    const std::string& dir, int depth);

    std::vector<std::string> paths_;
    std::map<std::string, Macro> macros_;
    std::set<std::string> once_;
    std::string out_;
    std::string error_;
    bool failed_ = false;
};

// Collects the argument list of a function-like invocation. `i` points at the
// '(' on entry and just past the matching ')' on return. Returns false if the
// list is unterminated.
bool collect_args(const std::string& s, size_t& i, std::vector<std::string>& args) {
    ++i; // consume '('
    int depth = 1;
    std::string cur;
    while (i < s.size()) {
        char c = s[i];
        if (c == '"' || c == '\'') {
            char q = c;
            cur += c;
            ++i;
            while (i < s.size()) {
                if (s[i] == '\\' && i + 1 < s.size()) { cur += s[i]; cur += s[i + 1]; i += 2; continue; }
                cur += s[i];
                if (s[i] == q) { ++i; break; }
                ++i;
            }
            continue;
        }
        if (c == '(' || c == '[') { ++depth; cur += c; ++i; continue; }
        if (c == ')' || c == ']') {
            --depth;
            if (depth == 0 && c == ')') {
                ++i;
                args.push_back(cur);
                return true;
            }
            cur += c;
            ++i;
            continue;
        }
        if (c == ',' && depth == 1) { args.push_back(cur); cur.clear(); ++i; continue; }
        cur += c;
        ++i;
    }
    return false;
}

std::string Preprocessor::substitute(const Macro& m, const std::vector<std::string>& raw,
                                     const std::vector<std::string>& exp) {
    auto index_of = [&](const std::string& name) -> int {
        for (size_t k = 0; k < m.params.size(); ++k)
            if (m.params[k] == name) return static_cast<int>(k);
        return -1;
    };

    std::string out;
    bool paste = false;
    const std::vector<RTok>& b = m.body;
    for (size_t i = 0; i < b.size(); ++i) {
        if (b[i].text == "##" && i > 0 && i + 1 < b.size()) {
            while (!out.empty() && out.back() == ' ') out.pop_back();
            paste = true;
            continue;
        }
        if (m.function_like && b[i].text == "#" && i + 1 < b.size() && b[i + 1].ident) {
            int p = index_of(b[i + 1].text);
            if (p >= 0) {
                if (b[i].space_before && !paste) out += ' ';
                out += stringize(raw[static_cast<size_t>(p)]);
                paste = false;
                ++i;
                continue;
            }
        }
        bool next_is_paste = (i + 1 < b.size() && b[i + 1].text == "##");
        bool use_raw = paste || next_is_paste;
        if (b[i].space_before && !paste) out += ' ';
        int p = b[i].ident && m.function_like ? index_of(b[i].text) : -1;
        if (p >= 0) out += trim(use_raw ? raw[static_cast<size_t>(p)] : exp[static_cast<size_t>(p)]);
        else out += b[i].text;
        paste = false;
    }
    return out;
}

std::string Preprocessor::expand(const std::string& in, const std::set<std::string>& active,
                                 int depth, const std::string& file, size_t line) {
    if (failed_) return in;
    if (depth > kMaxExpansionDepth) {
        fail(file, line, "macro expansion nested too deeply");
        return in;
    }
    std::string out;
    size_t i = 0;
    while (i < in.size()) {
        char c = in[i];
        if (c == '"' || c == '\'') {
            char q = c;
            out += c;
            ++i;
            while (i < in.size()) {
                if (in[i] == '\\' && i + 1 < in.size()) { out += in[i]; out += in[i + 1]; i += 2; continue; }
                out += in[i];
                if (in[i] == q) { ++i; break; }
                ++i;
            }
            continue;
        }
        if (!ident_start(c)) { out += c; ++i; continue; }

        size_t b = i;
        while (i < in.size() && ident_char(in[i])) ++i;
        std::string name = in.substr(b, i - b);

        auto it = macros_.find(name);
        if (it == macros_.end() || active.count(name)) { out += name; continue; }

        const Macro& m = it->second;
        if (!m.function_like) {
            std::set<std::string> next = active;
            next.insert(name);
            out += expand(substitute(m, {}, {}), next, depth + 1, file, line);
            continue;
        }

        size_t j = i;
        while (j < in.size() && std::isspace(static_cast<unsigned char>(in[j]))) ++j;
        if (j >= in.size() || in[j] != '(') { out += name; continue; }

        std::vector<std::string> raw;
        size_t k = j;
        if (!collect_args(in, k, raw)) {
            fail(file, line, "unterminated argument list for macro '" + name + "'");
            return out;
        }
        if (raw.size() == 1 && trim(raw[0]).empty() && m.params.empty()) raw.clear();
        if (raw.size() != m.params.size()) {
            fail(file, line, "macro '" + name + "' expects " + std::to_string(m.params.size()) +
                                 " argument(s), got " + std::to_string(raw.size()));
            return out;
        }
        std::vector<std::string> exp;
        exp.reserve(raw.size());
        for (const std::string& a : raw) exp.push_back(expand(a, active, depth + 1, file, line));
        if (failed_) return out;

        std::set<std::string> next = active;
        next.insert(name);
        out += expand(substitute(m, raw, exp), next, depth + 1, file, line);
        i = k;
    }
    return out;
}

// --------------------------------------------------- constant expressions --

class ExprEval {
public:
    ExprEval(const std::string& s) : s_(s) {}
    long parse() { long v = ternary(); skip(); return v; }
    bool ok() const { return ok_; }

private:
    void skip() { while (p_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[p_]))) ++p_; }
    bool eat(const char* op) {
        skip();
        size_t n = std::char_traits<char>::length(op);
        if (s_.compare(p_, n, op) == 0) {
            // Do not let "&" swallow the front of "&&", etc.
            if (n == 1 && p_ + 1 < s_.size() && (op[0] == '&' || op[0] == '|') && s_[p_ + 1] == op[0])
                return false;
            if (n == 1 && (op[0] == '<' || op[0] == '>' || op[0] == '=' || op[0] == '!') &&
                p_ + 1 < s_.size() && (s_[p_ + 1] == '=' || s_[p_ + 1] == op[0]))
                return false;
            p_ += n;
            return true;
        }
        return false;
    }

    long primary() {
        skip();
        if (p_ >= s_.size()) { ok_ = false; return 0; }
        if (eat("(")) {
            long v = ternary();
            if (!eat(")")) ok_ = false;
            return v;
        }
        if (eat("!")) return !primary();
        if (eat("~")) return ~primary();
        if (eat("-")) return -primary();
        if (eat("+")) return primary();
        char c = s_[p_];
        if (std::isdigit(static_cast<unsigned char>(c))) {
            size_t b = p_;
            int base = 10;
            if (c == '0' && p_ + 1 < s_.size() && (s_[p_ + 1] == 'x' || s_[p_ + 1] == 'X')) {
                base = 16;
                p_ += 2;
                b = p_;
            } else if (c == '0') {
                base = 8;
            }
            while (p_ < s_.size() && std::isalnum(static_cast<unsigned char>(s_[p_]))) ++p_;
            std::string num = s_.substr(b, p_ - b);
            while (!num.empty() && (num.back() == 'u' || num.back() == 'U' || num.back() == 'l' ||
                                    num.back() == 'L'))
                num.pop_back();
            if (num.empty()) return 0;
            try {
                return std::stol(num, nullptr, base);
            } catch (...) {
                ok_ = false;
                return 0;
            }
        }
        if (c == '\'') {
            ++p_;
            long v = 0;
            if (p_ < s_.size() && s_[p_] == '\\' && p_ + 1 < s_.size()) {
                char e = s_[p_ + 1];
                p_ += 2;
                switch (e) {
                    case 'n': v = '\n'; break;
                    case 't': v = '\t'; break;
                    case '0': v = 0; break;
                    case 'r': v = '\r'; break;
                    default: v = static_cast<unsigned char>(e);
                }
            } else if (p_ < s_.size()) {
                v = static_cast<unsigned char>(s_[p_]);
                ++p_;
            }
            if (p_ < s_.size() && s_[p_] == '\'') ++p_;
            return v;
        }
        if (ident_start(c)) {
            size_t b = p_;
            while (p_ < s_.size() && ident_char(s_[p_])) ++p_;
            std::string id = s_.substr(b, p_ - b);
            if (id == "true") return 1;
            return 0;  // undefined identifiers evaluate to zero
        }
        ok_ = false;
        ++p_;
        return 0;
    }

    long mul() {
        long v = primary();
        for (;;) {
            if (eat("*")) v *= primary();
            else if (eat("/")) { long r = primary(); v = r ? v / r : 0; }
            else if (eat("%")) { long r = primary(); v = r ? v % r : 0; }
            else return v;
        }
    }
    long add() {
        long v = mul();
        for (;;) {
            if (eat("+")) v += mul();
            else if (eat("-")) v -= mul();
            else return v;
        }
    }
    long shift() {
        long v = add();
        for (;;) {
            if (eat("<<")) v <<= add();
            else if (eat(">>")) v >>= add();
            else return v;
        }
    }
    long rel() {
        long v = shift();
        for (;;) {
            if (eat("<=")) v = v <= shift();
            else if (eat(">=")) v = v >= shift();
            else if (eat("<")) v = v < shift();
            else if (eat(">")) v = v > shift();
            else return v;
        }
    }
    long equality() {
        long v = rel();
        for (;;) {
            if (eat("==")) v = v == rel();
            else if (eat("!=")) v = v != rel();
            else return v;
        }
    }
    long band() { long v = equality(); while (eat("&")) v &= equality(); return v; }
    long bxor() { long v = band(); while (eat("^")) v ^= band(); return v; }
    long bor() { long v = bxor(); while (eat("|")) v |= bxor(); return v; }
    long land() { long v = bor(); while (eat("&&")) { long r = bor(); v = (v && r); } return v; }
    long lor() { long v = land(); while (eat("||")) { long r = land(); v = (v || r); } return v; }
    long ternary() {
        long c = lor();
        if (eat("?")) {
            long a = ternary();
            if (!eat(":")) { ok_ = false; return 0; }
            long b = ternary();
            return c ? a : b;
        }
        return c;
    }

    const std::string& s_;
    size_t p_ = 0;
    bool ok_ = true;
};

bool Preprocessor::eval_condition(const std::string& expr, const std::string& file, size_t line) {
    // Resolve defined(X) / defined X before macro expansion.
    std::string resolved;
    size_t i = 0;
    while (i < expr.size()) {
        if (ident_start(expr[i])) {
            size_t b = i;
            while (i < expr.size() && ident_char(expr[i])) ++i;
            std::string id = expr.substr(b, i - b);
            if (id == "defined") {
                size_t j = i;
                while (j < expr.size() && std::isspace(static_cast<unsigned char>(expr[j]))) ++j;
                bool paren = j < expr.size() && expr[j] == '(';
                if (paren) {
                    ++j;
                    while (j < expr.size() && std::isspace(static_cast<unsigned char>(expr[j]))) ++j;
                }
                size_t nb = j;
                while (j < expr.size() && ident_char(expr[j])) ++j;
                std::string name = expr.substr(nb, j - nb);
                if (paren) {
                    while (j < expr.size() && std::isspace(static_cast<unsigned char>(expr[j]))) ++j;
                    if (j >= expr.size() || expr[j] != ')') {
                        fail(file, line, "missing ')' after 'defined'");
                        return false;
                    }
                    ++j;
                }
                if (name.empty()) {
                    fail(file, line, "'defined' without a macro name");
                    return false;
                }
                resolved += macros_.count(name) ? '1' : '0';
                i = j;
                continue;
            }
            resolved += id;
            continue;
        }
        resolved += expr[i];
        ++i;
    }
    std::string full = expand(resolved, {}, 0, file, line);
    if (failed_) return false;
    if (trim(full).empty()) {
        fail(file, line, "expected a value after #if");
        return false;
    }
    ExprEval ev(full);
    long v = ev.parse();
    if (!ev.ok()) {
        fail(file, line, "invalid expression in conditional directive");
        return false;
    }
    return v != 0;
}

// -------------------------------------------------------------- includes ---

bool read_file(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

bool Preprocessor::do_include(const std::string& rest, const std::string& file, size_t line,
                              const std::string& dir, int depth) {
    std::string spec = trim(rest);
    if (!spec.empty() && spec.front() != '"' && spec.front() != '<') {
        spec = trim(expand(spec, {}, 0, file, line));
        if (failed_) return false;
    }
    if (spec.size() < 2) {
        fail(file, line, "malformed #include directive");
        return false;
    }
    char open = spec.front();
    char close = open == '<' ? '>' : '"';
    size_t end = spec.find(close, 1);
    if ((open != '<' && open != '"') || end == std::string::npos) {
        fail(file, line, "malformed #include directive");
        return false;
    }
    std::string name = spec.substr(1, end - 1);
    if (name.empty()) {
        fail(file, line, "empty file name in #include");
        return false;
    }

    namespace fs = std::filesystem;
    std::vector<std::string> candidates;
    if (open == '"' && !dir.empty()) candidates.push_back((fs::path(dir) / name).string());
    for (const std::string& p : paths_) candidates.push_back((fs::path(p) / name).string());
    if (open == '<' && !dir.empty()) candidates.push_back((fs::path(dir) / name).string());
    candidates.push_back(name);

    for (const std::string& cand : candidates) {
        std::error_code ec;
        if (!fs::is_regular_file(cand, ec)) continue;
        std::string canonical = fs::weakly_canonical(cand, ec).string();
        if (ec) canonical = cand;
        if (once_.count(canonical)) return true;
        std::string text;
        if (!read_file(cand, text)) continue;
        std::string sub_dir = fs::path(cand).parent_path().string();
        return run(text, canonical, sub_dir, depth + 1);
    }
    fail(file, line, "cannot open included file '" + name + "'");
    return false;
}

// --------------------------------------------------------------- driver ----

bool Preprocessor::run(const std::string& text, const std::string& file, const std::string& dir,
                       int depth) {
    if (failed_) return false;
    if (depth > kMaxIncludeDepth) {
        fail(file, 1, "#include nested more than " + std::to_string(kMaxIncludeDepth) + " levels deep");
        return false;
    }

    std::vector<Cond> conds;
    auto active = [&]() { return conds.empty() || conds.back().active; };

    std::vector<Line> lines = clean_lines(text);
    for (const Line& ln : lines) {
        if (failed_) return false;
        std::string body = trim(ln.text);

        if (body.empty() || body[0] != '#') {
            if (active() && !body.empty()) {
                std::string e = expand(ln.text, {}, 0, file, ln.number);
                if (failed_) return false;
                out_ += e;
                out_ += '\n';
            } else if (active()) {
                out_ += '\n';
            }
            continue;
        }

        // Split "#name rest".
        size_t p = 1;
        while (p < body.size() && std::isspace(static_cast<unsigned char>(body[p]))) ++p;
        size_t b = p;
        while (p < body.size() && ident_char(body[p])) ++p;
        std::string dir_name = body.substr(b, p - b);
        std::string rest = trim(body.substr(p));

        if (dir_name == "ifdef" || dir_name == "ifndef") {
            Cond c;
            c.parent_active = active();
            std::string name = rest;
            size_t sp = name.find_first_of(" \t");
            if (sp != std::string::npos) name = name.substr(0, sp);
            if (name.empty()) {
                fail(file, ln.number, "#" + dir_name + " without a macro name");
                return false;
            }
            bool defined = macros_.count(name) != 0;
            bool want = (dir_name == "ifdef") ? defined : !defined;
            c.active = c.parent_active && want;
            c.taken = c.active;
            conds.push_back(c);
            continue;
        }
        if (dir_name == "if") {
            Cond c;
            c.parent_active = active();
            bool v = c.parent_active ? eval_condition(rest, file, ln.number) : false;
            if (failed_) return false;
            c.active = c.parent_active && v;
            c.taken = c.active;
            conds.push_back(c);
            continue;
        }
        if (dir_name == "elif") {
            if (conds.empty()) {
                fail(file, ln.number, "#elif without matching #if");
                return false;
            }
            Cond& c = conds.back();
            if (c.taken || !c.parent_active) {
                c.active = false;
            } else {
                bool v = eval_condition(rest, file, ln.number);
                if (failed_) return false;
                c.active = v;
                c.taken = c.taken || v;
            }
            continue;
        }
        if (dir_name == "else") {
            if (conds.empty()) {
                fail(file, ln.number, "#else without matching #if");
                return false;
            }
            Cond& c = conds.back();
            c.active = c.parent_active && !c.taken;
            c.taken = c.taken || c.active;
            continue;
        }
        if (dir_name == "endif") {
            if (conds.empty()) {
                fail(file, ln.number, "#endif without matching #if");
                return false;
            }
            conds.pop_back();
            continue;
        }
        if (!active()) continue;

        if (dir_name == "define") {
            size_t q = 0;
            while (q < rest.size() && ident_char(rest[q])) ++q;
            std::string name = rest.substr(0, q);
            if (name.empty() || !ident_start(name[0])) {
                fail(file, ln.number, "#define without a valid macro name");
                return false;
            }
            Macro m;
            if (q < rest.size() && rest[q] == '(') {
                m.function_like = true;
                size_t close = rest.find(')', q);
                if (close == std::string::npos) {
                    fail(file, ln.number, "missing ')' in macro parameter list");
                    return false;
                }
                std::string plist = rest.substr(q + 1, close - q - 1);
                std::string cur;
                for (size_t k = 0; k <= plist.size(); ++k) {
                    if (k == plist.size() || plist[k] == ',') {
                        std::string pname = trim(cur);
                        if (!pname.empty()) {
                            if (!ident_start(pname[0])) {
                                fail(file, ln.number, "invalid macro parameter '" + pname + "'");
                                return false;
                            }
                            m.params.push_back(pname);
                        }
                        cur.clear();
                    } else {
                        cur += plist[k];
                    }
                }
                m.body = split_tokens(trim(rest.substr(close + 1)));
            } else {
                m.body = split_tokens(trim(rest.substr(q)));
            }
            macros_[name] = m;
            continue;
        }
        if (dir_name == "undef") {
            std::string name = rest;
            size_t sp = name.find_first_of(" \t");
            if (sp != std::string::npos) name = name.substr(0, sp);
            if (name.empty()) {
                fail(file, ln.number, "#undef without a macro name");
                return false;
            }
            macros_.erase(name);
            continue;
        }
        if (dir_name == "include") {
            if (!do_include(rest, file, ln.number, dir, depth)) return false;
            continue;
        }
        if (dir_name == "pragma") {
            if (trim(rest) == "once") once_.insert(file);
            continue;
        }
        if (dir_name == "error") {
            fail(file, ln.number, rest.empty() ? "#error" : rest);
            return false;
        }
        if (dir_name == "warning" || dir_name == "line" || dir_name.empty()) continue;

        fail(file, ln.number, "unknown directive '#" + dir_name + "'");
        return false;
    }

    if (!conds.empty()) {
        fail(file, lines.empty() ? 1 : lines.back().number, "unterminated conditional directive");
        return false;
    }
    return true;
}

} // namespace

PreprocessResult preprocess(const std::string& source,
                            const std::vector<std::string>& include_paths) {
    PreprocessResult r;
    Preprocessor pp(include_paths);
    pp.run(source, "<source>", std::filesystem::current_path().string(), 0);
    if (pp.failed()) {
        r.ok = false;
        r.error = pp.error();
        return r;
    }
    r.ok = true;
    r.text = pp.take_output();
    return r;
}

} // namespace ardio
