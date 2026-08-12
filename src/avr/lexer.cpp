// Lexer for the embedded C++ subset accepted by the AVR front end.
#include "ardio/avr/token.h"

#include <array>
#include <cctype>
#include <string_view>

namespace ardio {
namespace {

constexpr std::string_view kKeywords[] = {
    "void", "bool", "char", "int", "long", "short", "signed", "unsigned",
    "const", "static", "struct", "class", "public", "private", "return",
    "if", "else", "while", "for", "do", "break", "continue", "switch",
    "case", "default", "sizeof", "true", "false", "new", "delete", "this",
    "enum", "typedef",
};

// Ordered longest-first so the first match is always the longest match.
constexpr std::string_view kPunct[] = {
    "<<=", ">>=",
    "->", "++", "--", "<<", ">>", "<=", ">=", "==", "!=", "&&", "||",
    "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "::",
    "{", "}", "(", ")", "[", "]", ";", ",", ".", "~", "?", ":",
    "+", "-", "*", "/", "%", "&", "|", "^", "!", "<", ">", "=", "#",
};

bool is_ident_start(unsigned char c) { return std::isalpha(c) || c == '_' || c == '$'; }
bool is_ident_cont(unsigned char c) { return std::isalnum(c) || c == '_' || c == '$'; }

int hex_value(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Cursor over the source that keeps 1-based line/column in sync.
class Cursor {
public:
    explicit Cursor(const std::string& s) : src_(s) {}

    bool eof() const { return pos_ >= src_.size(); }
    size_t line() const { return line_; }
    size_t column() const { return column_; }
    size_t pos() const { return pos_; }

    char peek(size_t ahead = 0) const {
        size_t p = pos_ + ahead;
        return p < src_.size() ? src_[p] : '\0';
    }

    bool starts_with(std::string_view sv) const {
        return src_.compare(pos_, sv.size(), sv) == 0;
    }

    char get() {
        char c = src_[pos_++];
        if (c == '\n') { ++line_; column_ = 1; } else { ++column_; }
        return c;
    }

    void skip(size_t n) { for (size_t i = 0; i < n && !eof(); ++i) get(); }

private:
    const std::string& src_;
    size_t pos_ = 0;
    size_t line_ = 1;
    size_t column_ = 1;
};

// Reads the body of one escape sequence (the backslash is already consumed).
long read_escape(Cursor& c) {
    if (c.eof()) return '\\';
    char e = c.get();
    switch (e) {
        case 'n': return '\n';
        case 't': return '\t';
        case 'r': return '\r';
        case '0': return 0;
        case 'a': return '\a';
        case 'b': return '\b';
        case 'f': return '\f';
        case 'v': return '\v';
        case '\\': return '\\';
        case '\'': return '\'';
        case '"': return '"';
        case '?': return '?';
        case 'x': {
            long v = 0;
            int digits = 0;
            while (!c.eof() && hex_value(static_cast<unsigned char>(c.peek())) >= 0) {
                v = v * 16 + hex_value(static_cast<unsigned char>(c.get()));
                ++digits;
            }
            return digits ? v : 'x';
        }
        default: return static_cast<unsigned char>(e);
    }
}

void lex_number(Cursor& c, Token& t) {
    std::string text;
    long value = 0;
    if (c.peek() == '0' && (c.peek(1) == 'x' || c.peek(1) == 'X')) {
        text += c.get();
        text += c.get();
        while (!c.eof() && (hex_value(static_cast<unsigned char>(c.peek())) >= 0 || c.peek() == '\'')) {
            char d = c.get();
            if (d == '\'') continue;
            text += d;
            value = value * 16 + hex_value(static_cast<unsigned char>(d));
        }
    } else if (c.peek() == '0' && (c.peek(1) == 'b' || c.peek(1) == 'B')) {
        text += c.get();
        text += c.get();
        while (!c.eof() && (c.peek() == '0' || c.peek() == '1' || c.peek() == '\'')) {
            char d = c.get();
            if (d == '\'') continue;
            text += d;
            value = value * 2 + (d - '0');
        }
    } else if (c.peek() == '0' && c.peek(1) >= '0' && c.peek(1) <= '7') {
        text += c.get();
        while (!c.eof() && ((c.peek() >= '0' && c.peek() <= '7') || c.peek() == '\'')) {
            char d = c.get();
            if (d == '\'') continue;
            text += d;
            value = value * 8 + (d - '0');
        }
    } else {
        while (!c.eof() && (std::isdigit(static_cast<unsigned char>(c.peek())) || c.peek() == '\'')) {
            char d = c.get();
            if (d == '\'') continue;
            text += d;
            value = value * 10 + (d - '0');
        }
    }
    // Optional integer suffixes: any mix of u/U and l/L.
    while (!c.eof()) {
        char s = c.peek();
        if (s == 'u' || s == 'U' || s == 'l' || s == 'L') text += c.get();
        else break;
    }
    t.kind = Tok::Number;
    t.text = text;
    t.value = value;
}

} // namespace

bool is_keyword(const std::string& word) {
    for (std::string_view k : kKeywords)
        if (k == word) return true;
    return false;
}

std::vector<Token> tokenize(const std::string& source) {
    std::vector<Token> out;
    Cursor c(source);

    for (;;) {
        // Whitespace and comments.
        bool progressed = true;
        while (progressed && !c.eof()) {
            progressed = false;
            while (!c.eof() && std::isspace(static_cast<unsigned char>(c.peek()))) {
                c.get();
                progressed = true;
            }
            if (c.starts_with("//")) {
                while (!c.eof() && c.peek() != '\n') c.get();
                progressed = true;
            } else if (c.starts_with("/*")) {
                c.skip(2);
                while (!c.eof() && !c.starts_with("*/")) c.get();
                if (!c.eof()) c.skip(2);
                progressed = true;
            }
        }
        if (c.eof()) break;

        Token t;
        t.line = c.line();
        t.column = c.column();
        char ch = c.peek();

        if (is_ident_start(static_cast<unsigned char>(ch))) {
            std::string name;
            while (!c.eof() && is_ident_cont(static_cast<unsigned char>(c.peek()))) name += c.get();
            t.text = name;
            if (is_keyword(name)) {
                t.kind = Tok::Keyword;
                if (name == "true") t.value = 1;
            } else {
                t.kind = Tok::Identifier;
            }
        } else if (std::isdigit(static_cast<unsigned char>(ch))) {
            lex_number(c, t);
        } else if (ch == '\'') {
            c.get();
            long value = 0;
            while (!c.eof() && c.peek() != '\'') {
                // Multi-character literals keep the last character's value.
                if (c.peek() == '\\') { c.get(); value = read_escape(c); }
                else value = static_cast<unsigned char>(c.get());
            }
            if (!c.eof()) c.get(); // closing quote
            t.kind = Tok::CharLit;
            t.value = value;
            t.text = std::string(1, static_cast<char>(value));
        } else if (ch == '"') {
            c.get();
            std::string s;
            while (!c.eof() && c.peek() != '"') {
                if (c.peek() == '\\') {
                    c.get();
                    s += static_cast<char>(read_escape(c));
                } else {
                    s += c.get();
                }
            }
            if (!c.eof()) c.get(); // closing quote
            t.kind = Tok::StringLit;
            t.text = s;
        } else {
            t.kind = Tok::Punct;
            std::string_view match;
            for (std::string_view p : kPunct) {
                if (c.starts_with(p)) { match = p; break; }
            }
            if (match.empty()) {
                // Unrecognised byte: emit it so the parser can report a position.
                t.text = std::string(1, c.get());
            } else {
                t.text = std::string(match);
                c.skip(match.size());
            }
        }
        out.push_back(t);
    }

    Token end;
    end.kind = Tok::End;
    end.line = c.line();
    end.column = c.column();
    out.push_back(end);
    return out;
}

} // namespace ardio
