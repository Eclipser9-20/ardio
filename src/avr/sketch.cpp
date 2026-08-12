#include "ardio/avr/sketch.h"

#include <cctype>
#include <string>
#include <vector>

// The whole job is one careful scan. Everything that can hide a brace or a
// parenthesis -- comments, string literals, character literals, preprocessor
// directives -- is blanked out first, so the structural pass that follows sees
// only real code punctuation.

namespace ardio {
namespace {

bool is_ident_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

bool is_space(char c) {
    return std::isspace(static_cast<unsigned char>(c)) != 0;
}

// A blanked view of the source: comments, string and character literals, and
// preprocessor directives are all replaced by spaces. What is left is the bare
// code punctuation -- braces, parentheses, semicolons -- with no literal able
// to masquerade as structure.
//
// It is the same length as the input and keeps newlines in place, so offsets
// are interchangeable with the original text.
struct Masks {
    bool ok = true;
    std::string error;
    std::string code;
};

// True if `s` looks like a character literal starting at `i` (i.e. there is a
// closing quote on the same line). A bare apostrophe -- a digit separator, or
// stray text -- is then left alone instead of swallowing the rest of the file.
bool char_literal_closes(const std::string& s, size_t i) {
    for (size_t j = i + 1; j < s.size(); ++j) {
        if (s[j] == '\n') return false;
        if (s[j] == '\\') { ++j; continue; }
        if (s[j] == '\'') return true;
    }
    return false;
}

Masks build_masks(const std::string& s) {
    Masks m;
    m.code = s;

    auto blank = [&](size_t i) {
        if (s[i] != '\n') m.code[i] = ' ';
    };

    size_t i = 0;
    bool at_line_start = true;   // only whitespace seen so far on this line
    while (i < s.size()) {
        char c = s[i];

        if (c == '\n') { at_line_start = true; ++i; continue; }

        // Line comment.
        if (c == '/' && i + 1 < s.size() && s[i + 1] == '/') {
            while (i < s.size() && s[i] != '\n') { blank(i); ++i; }
            continue;
        }

        // Block comment.
        if (c == '/' && i + 1 < s.size() && s[i + 1] == '*') {
            size_t start = i;
            i += 2;
            bool closed = false;
            while (i < s.size()) {
                if (s[i] == '*' && i + 1 < s.size() && s[i + 1] == '/') { i += 2; closed = true; break; }
                ++i;
            }
            if (!closed) {
                m.ok = false;
                m.error = "unterminated block comment";
                return m;
            }
            for (size_t j = start; j < i; ++j) blank(j);
            at_line_start = false;
            continue;
        }

        // Preprocessor directive: blanked in `code` only, and it runs to the
        // end of the line unless the line is continued with a backslash.
        if (c == '#' && at_line_start) {
            while (i < s.size()) {
                if (s[i] == '\\' && i + 1 < s.size() && s[i + 1] == '\n') { blank(i); ++i; blank(i); ++i; continue; }
                if (s[i] == '\n') break;
                blank(i);
                ++i;
            }
            continue;
        }

        // String literal.
        if (c == '"') {
            size_t start = i;
            ++i;
            bool closed = false;
            while (i < s.size()) {
                if (s[i] == '\\' && i + 1 < s.size()) { i += 2; continue; }
                if (s[i] == '\n') break;
                if (s[i] == '"') { ++i; closed = true; break; }
                ++i;
            }
            if (!closed) {
                m.ok = false;
                m.error = "unterminated string literal";
                return m;
            }
            for (size_t j = start; j < i; ++j) blank(j);
            at_line_start = false;
            continue;
        }

        // Character literal.
        if (c == '\'' && char_literal_closes(s, i)) {
            size_t start = i;
            ++i;
            while (i < s.size()) {
                if (s[i] == '\\' && i + 1 < s.size()) { i += 2; continue; }
                if (s[i] == '\'') { ++i; break; }
                ++i;
            }
            for (size_t j = start; j < i; ++j) blank(j);
            at_line_start = false;
            continue;
        }

        if (!is_space(c)) at_line_start = false;
        ++i;
    }
    return m;
}

// ------------------------------------------------------------- scanning -----

size_t skip_space_back(const std::string& code, size_t i) {
    while (i > 0 && is_space(code[i - 1])) --i;
    return i;
}

// Collapses runs of whitespace so a declaration split over several lines
// becomes one tidy line.
std::string squeeze(const std::string& s) {
    std::string out;
    bool pending = false;
    for (char c : s) {
        if (is_space(c)) { pending = !out.empty(); continue; }
        if (pending) { out += ' '; pending = false; }
        out += c;
    }
    return out;
}

// A return type must be a plain type expression. Anything that smells like a
// control statement, a template, or an initialiser means this `(`...`)` `{`
// was not a function definition after all.
bool plausible_return_type(const std::string& t) {
    if (t.empty()) return false;
    for (char c : t) {
        if (is_ident_char(c) || is_space(c)) continue;
        if (c == '*' || c == '&' || c == ':' || c == '<' || c == '>' || c == ',') continue;
        return false;   // '=', '(', '[', '"' ... not a type
    }
    // Split into words and reject statement keywords and templates.
    static const char* const bad[] = {
        "if", "else", "for", "while", "do", "switch", "case", "return",
        "template", "using", "typedef", "new", "delete", "throw", "catch",
        "namespace", "enum", "extern",
    };
    std::string word;
    auto check = [&]() {
        if (word.empty()) return true;
        for (const char* b : bad) if (word == b) return false;
        return true;
    };
    for (char c : t) {
        if (is_ident_char(c)) { word += c; continue; }
        if (!check()) return false;
        word.clear();
    }
    return check();
}

// Removes default arguments from a parameter list `(int a, int b = 3)`. A
// default may be given only once in a translation unit, so the forward
// declaration must drop what the definition already states.
std::string strip_default_args(const std::string& params) {
    std::string out;
    int depth = 0;
    bool skipping = false;
    for (char c : params) {
        if (c == '(' || c == '[' || c == '{') { ++depth; }
        else if (c == ')' || c == ']' || c == '}') { --depth; }

        if (skipping) {
            // A default argument ends at the comma separating the next
            // parameter, or at the closing paren of the list itself.
            if (depth == 1 && c == ',') skipping = false;
            else if (depth == 0) skipping = false;
            else continue;
        } else if (depth == 1 && c == '=') {
            skipping = true;
            while (!out.empty() && is_space(out.back())) out.pop_back();
            continue;
        }
        out += c;
    }
    return out;
}

struct FunctionDef {
    size_t decl_start = 0;      // offset of the first character of the return type
    std::string declaration;    // e.g. "unsigned long readSensor(int pin);"
};

} // namespace

SketchResult preprocess_sketch(const std::string& ino_source) {
    SketchResult result;

    Masks masks = build_masks(ino_source);
    if (!masks.ok) {
        result.error = masks.error;
        return result;
    }
    const std::string& code = masks.code;

    std::vector<FunctionDef> funcs;
    int depth = 0;

    for (size_t i = 0; i < code.size(); ++i) {
        char c = code[i];
        if (c == '}') {
            if (depth > 0) --depth;
            continue;
        }
        if (c != '{') continue;

        // Only file scope. A '{' at depth > 0 is a body, a class body, a
        // namespace, or an initialiser -- functions in there get no
        // file-scope declaration.
        if (depth > 0) { ++depth; continue; }
        ++depth;

        // Walk back over any trailing specifiers (const, noexcept, ...) to the
        // ')' that would close a parameter list.
        size_t tail_end = skip_space_back(code, i);
        size_t k = tail_end;
        while (k > 0 && (is_ident_char(code[k - 1]) || is_space(code[k - 1]))) --k;
        if (k == 0 || code[k - 1] != ')') continue;      // not a parameter list
        size_t rparen = k - 1;
        std::string tail = squeeze(code.substr(k, tail_end - k));

        // Match the parameter list back to its '('.
        int pdepth = 0;
        size_t lparen = std::string::npos;
        for (size_t j = rparen + 1; j-- > 0;) {
            if (code[j] == ')') ++pdepth;
            else if (code[j] == '(') {
                --pdepth;
                if (pdepth == 0) { lparen = j; break; }
            }
            if (j == 0) break;
        }
        if (lparen == std::string::npos) continue;

        // The identifier immediately before '(' is the function name. If it is
        // not an identifier (a lambda's ']', say) this is not a definition.
        size_t name_end = skip_space_back(code, lparen);
        size_t name_start = name_end;
        while (name_start > 0 && is_ident_char(code[name_start - 1])) --name_start;
        if (name_start == name_end) continue;
        // A qualified name (`void Foo::bar()`) is an out-of-class member
        // definition; it cannot be redeclared at file scope, so leave it be.
        if (name_start >= 2 && code[name_start - 1] == ':' && code[name_start - 2] == ':') continue;

        // The return type runs back to the previous statement boundary.
        size_t decl_start = name_start;
        while (decl_start > 0) {
            char p = code[decl_start - 1];
            if (p == ';' || p == '{' || p == '}') break;
            --decl_start;
        }
        std::string ret = code.substr(decl_start, name_start - decl_start);
        if (!plausible_return_type(ret)) continue;

        // Skip forward over blanked-out comments and directives so decl_start
        // really is the first character of the return type.
        while (decl_start < name_start && is_space(code[decl_start])) ++decl_start;

        std::string decl = squeeze(ret) + " " +
                           squeeze(strip_default_args(
                               code.substr(name_start, rparen + 1 - name_start)));
        if (!tail.empty()) decl += " " + tail;
        decl += ";";

        FunctionDef def;
        def.decl_start = decl_start;
        def.declaration = decl;
        funcs.push_back(def);
    }

    if (depth != 0) {
        result.error = "unbalanced braces in sketch";
        return result;
    }

    // Declarations go in front of the first definition, at the start of its
    // line. That is after every #include the sketch wrote, and after any type
    // the signatures might mention, while leaving the user's own line order
    // untouched.
    size_t insert_at = ino_source.size();
    if (!funcs.empty()) {
        insert_at = funcs.front().decl_start;
        while (insert_at > 0 && ino_source[insert_at - 1] != '\n') --insert_at;
    }

    bool has_arduino_h = false;
    {
        size_t pos = 0;
        while ((pos = ino_source.find("Arduino.h", pos)) != std::string::npos) {
            size_t line_start = ino_source.rfind('\n', pos);
            line_start = (line_start == std::string::npos) ? 0 : line_start + 1;
            std::string line = ino_source.substr(line_start, pos - line_start);
            if (line.find('#') != std::string::npos && line.find("include") != std::string::npos) {
                has_arduino_h = true;
                break;
            }
            pos += 9;
        }
    }

    std::string out;
    if (!has_arduino_h) out += "#include <Arduino.h>\n";
    out += ino_source.substr(0, insert_at);
    if (!funcs.empty()) {
        for (const FunctionDef& f : funcs) out += f.declaration + "\n";
        out += ino_source.substr(insert_at);
    }

    result.ok = true;
    result.source = out;
    return result;
}

} // namespace ardio
