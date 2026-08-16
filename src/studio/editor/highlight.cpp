#include "studio/editor/highlight.h"

#include <cctype>
#include <unordered_set>

namespace studio {
namespace {

const std::unordered_set<std::string> kControl = {
    "if", "else", "for", "while", "do", "switch", "case", "default", "break", "continue",
    "return", "goto"};

const std::unordered_set<std::string> kKeyword = {
    "sizeof",   "new",      "delete",   "this",     "namespace", "using",    "template",
    "typename", "class",    "struct",   "enum",     "union",     "public",   "private",
    "protected","virtual",  "override", "const",    "constexpr", "static",   "extern",
    "inline",   "volatile", "mutable",  "friend",   "operator",  "throw",    "try",
    "catch",    "true",     "false",    "nullptr",  "typedef",   "explicit", "register"};

const std::unordered_set<std::string> kType = {
    "void",    "bool",     "char",     "int",     "short",   "long",    "float",   "double",
    "unsigned","signed",   "auto",     "size_t",  "uint8_t", "uint16_t","uint32_t","uint64_t",
    "int8_t",  "int16_t",  "int32_t",  "int64_t", "wchar_t", "byte",    "word",    "boolean",
    "String"};

const std::unordered_set<std::string> kConst = {
    "HIGH", "LOW",         "INPUT",   "OUTPUT", "INPUT_PULLUP", "LED_BUILTIN",
    "true", "false",       "NULL",    "PI",     "A0",           "A1"};

bool is_ident_start(unsigned char c) { return std::isalpha(c) || c == '_' || c >= 0x80; }
bool is_ident(unsigned char c) { return std::isalnum(c) || c == '_' || c >= 0x80; }

}  // namespace

void highlight_line(const std::string& line, bool& in_block, const Syntax& s,
                    std::vector<Span>& out) {
    out.clear();
    int n = static_cast<int>(line.size());
    int i = 0;
    auto push = [&](int a, int b, Color c) {
        if (b > a) out.push_back({a, b, c});
    };

    while (i < n) {
        // Inside a running block comment: consume up to and including */.
        if (in_block) {
            int start = i;
            while (i < n) {
                if (line[i] == '*' && i + 1 < n && line[i + 1] == '/') {
                    i += 2;
                    in_block = false;
                    break;
                }
                ++i;
            }
            push(start, i, s.comment);
            continue;
        }

        char c = line[i];

        if (c == '/' && i + 1 < n && line[i + 1] == '/') {  // line comment
            push(i, n, s.comment);
            i = n;
            continue;
        }
        if (c == '/' && i + 1 < n && line[i + 1] == '*') {  // block comment start
            int start = i;
            i += 2;
            bool closed = false;
            while (i < n) {
                if (line[i] == '*' && i + 1 < n && line[i + 1] == '/') {
                    i += 2;
                    closed = true;
                    break;
                }
                ++i;
            }
            push(start, i, s.comment);
            if (!closed) in_block = true;
            continue;
        }
        if (c == '"' || c == '\'') {  // string / char literal
            char q = c;
            int start = i++;
            while (i < n) {
                if (line[i] == '\\' && i + 1 < n) {
                    i += 2;
                    continue;
                }
                if (line[i] == q) {
                    ++i;
                    break;
                }
                ++i;
            }
            push(start, i, s.str);
            continue;
        }
        if (c == '#') {  // preprocessor directive keyword
            int start = i++;
            while (i < n && is_ident(static_cast<unsigned char>(line[i]))) ++i;
            push(start, i, s.preproc);
            continue;
        }
        if (std::isdigit(static_cast<unsigned char>(c)) ||
            (c == '.' && i + 1 < n && std::isdigit(static_cast<unsigned char>(line[i + 1])))) {
            int start = i++;
            while (i < n) {
                unsigned char d = static_cast<unsigned char>(line[i]);
                if (std::isalnum(d) || d == '.' || d == 'x' || d == 'X')
                    ++i;
                else
                    break;
            }
            push(start, i, s.number);
            continue;
        }
        if (is_ident_start(static_cast<unsigned char>(c))) {
            int start = i++;
            while (i < n && is_ident(static_cast<unsigned char>(line[i]))) ++i;
            std::string w = line.substr(start, i - start);
            Color col = s.deflt;
            if (kControl.count(w)) col = s.control;
            else if (kKeyword.count(w)) col = s.keyword;
            else if (kType.count(w)) col = s.type;
            else if (kConst.count(w)) col = s.number;
            else {
                int j = i;
                while (j < n && line[j] == ' ') ++j;
                if (j < n && line[j] == '(') col = s.function;
            }
            push(start, i, col);
            continue;
        }

        // Any other single character (operators, punctuation, whitespace).
        push(i, i + 1, s.deflt);
        ++i;
    }
}

}  // namespace studio
