#include "harness.h"

#include "ardio/avr/token.h"

#include <string>
#include <vector>

using ardio::Tok;
using ardio::Token;
using ardio::tokenize;

namespace {

size_t kind_of(const Token& t) { return static_cast<size_t>(t.kind); }

std::vector<Token> lex(const std::string& s) { return tokenize(s); }

} // namespace

TEST(lexer_empty_source_yields_only_end) {
    auto t = lex("");
    CHECK_EQ(t.size(), size_t(1));
    CHECK(t[0].kind == Tok::End);
}

TEST(lexer_identifiers_and_positions) {
    auto t = lex("foo bar_1\n  $baz");
    CHECK_EQ(t.size(), size_t(4));
    CHECK(t[0].kind == Tok::Identifier);
    CHECK(t[0].text == "foo");
    CHECK_EQ(t[0].line, size_t(1));
    CHECK_EQ(t[0].column, size_t(1));
    CHECK(t[1].text == "bar_1");
    CHECK_EQ(t[1].column, size_t(5));
    CHECK(t[2].text == "$baz");
    CHECK_EQ(t[2].line, size_t(2));
    CHECK_EQ(t[2].column, size_t(3));
}

TEST(lexer_keywords_are_distinguished) {
    auto t = lex("int x const while typedefx");
    CHECK(t[0].kind == Tok::Keyword);
    CHECK(t[1].kind == Tok::Identifier);
    CHECK(t[2].kind == Tok::Keyword);
    CHECK(t[3].kind == Tok::Keyword);
    CHECK(t[4].kind == Tok::Identifier);
    CHECK(t[4].text == "typedefx");
}

TEST(lexer_is_keyword_table) {
    const char* words[] = {"void", "bool", "char", "int", "long", "short",
                           "signed", "unsigned", "const", "static", "struct",
                           "class", "public", "private", "return", "if",
                           "else", "while", "for", "do", "break", "continue",
                           "switch", "case", "default", "sizeof", "true",
                           "false", "new", "delete", "this", "enum", "typedef"};
    for (const char* w : words) CHECK(ardio::is_keyword(w));
    CHECK(!ardio::is_keyword("digitalWrite"));
    CHECK(!ardio::is_keyword("Int"));
    CHECK(!ardio::is_keyword(""));
}

TEST(lexer_decimal_numbers) {
    auto t = lex("0 7 42 1234567");
    CHECK(t[0].kind == Tok::Number);
    CHECK_EQ(t[0].value, 0L);
    CHECK_EQ(t[1].value, 7L);
    CHECK_EQ(t[2].value, 42L);
    CHECK_EQ(t[3].value, 1234567L);
}

TEST(lexer_hex_binary_octal) {
    auto t = lex("0xFF 0x1a 0X10 0b1011 0B01 0755 010");
    CHECK_EQ(t[0].value, 255L);
    CHECK_EQ(t[1].value, 26L);
    CHECK_EQ(t[2].value, 16L);
    CHECK_EQ(t[3].value, 11L);
    CHECK_EQ(t[4].value, 1L);
    CHECK_EQ(t[5].value, 493L);
    CHECK_EQ(t[6].value, 8L);
}

TEST(lexer_number_suffixes) {
    auto t = lex("10U 20L 30UL 0xFFuL 5ll");
    CHECK_EQ(t[0].value, 10L);
    CHECK(t[0].text == "10U");
    CHECK_EQ(t[1].value, 20L);
    CHECK_EQ(t[2].value, 30L);
    CHECK(t[2].text == "30UL");
    CHECK_EQ(t[3].value, 255L);
    CHECK_EQ(t[4].value, 5L);
    CHECK(t[5].kind == Tok::End);
}

TEST(lexer_char_literals_and_escapes) {
    auto t = lex("'a' '\\n' '\\t' '\\\\' '\\'' '\\0' '\\x41'");
    CHECK(t[0].kind == Tok::CharLit);
    CHECK_EQ(t[0].value, 97L);
    CHECK_EQ(t[1].value, 10L);
    CHECK_EQ(t[2].value, 9L);
    CHECK_EQ(t[3].value, 92L);
    CHECK_EQ(t[4].value, 39L);
    CHECK_EQ(t[5].value, 0L);
    CHECK_EQ(t[6].value, 65L);
}

TEST(lexer_string_literals) {
    auto t = lex("\"hi\" \"a\\nb\" \"q\\\"q\" \"\"");
    CHECK(t[0].kind == Tok::StringLit);
    CHECK(t[0].text == "hi");
    CHECK(t[1].text == std::string("a\nb"));
    CHECK(t[2].text == std::string("q\"q"));
    CHECK(t[3].text == "");
    CHECK_EQ(t[3].column, size_t(20));
}

TEST(lexer_string_escape_hex_and_nul) {
    auto t = lex("\"\\x41\\x42\" \"a\\0b\"");
    CHECK(t[0].text == "AB");
    CHECK_EQ(t[1].text.size(), size_t(3));
    CHECK_EQ(size_t(t[1].text[1]), size_t(0));
}

TEST(lexer_multichar_operators_longest_match) {
    auto t = lex(">>= >> > <<= << < -> ++ -- <= >= == != && || += -= *= /= %= &= |= ^= ::");
    const char* expect[] = {">>=", ">>", ">", "<<=", "<<", "<", "->", "++", "--",
                            "<=", ">=", "==", "!=", "&&", "||", "+=", "-=", "*=",
                            "/=", "%=", "&=", "|=", "^=", "::"};
    size_t n = sizeof(expect) / sizeof(expect[0]);
    CHECK_EQ(t.size(), n + 1);
    for (size_t i = 0; i < n; ++i) {
        CHECK(t[i].kind == Tok::Punct);
        CHECK(t[i].text == expect[i]);
    }
}

TEST(lexer_single_punctuators) {
    auto t = lex("{ } ( ) [ ] ; , . ~ ? : + - * / % & | ^ ! < > =");
    const char* expect[] = {"{", "}", "(", ")", "[", "]", ";", ",", ".", "~",
                            "?", ":", "+", "-", "*", "/", "%", "&", "|", "^",
                            "!", "<", ">", "="};
    size_t n = sizeof(expect) / sizeof(expect[0]);
    CHECK_EQ(t.size(), n + 1);
    for (size_t i = 0; i < n; ++i) CHECK(t[i].text == expect[i]);
}

TEST(lexer_comments_are_skipped) {
    auto t = lex("a // comment ; here\nb /* block\nspanning */ c");
    CHECK(t[0].text == "a");
    CHECK(t[1].text == "b");
    CHECK_EQ(t[1].line, size_t(2));
    CHECK(t[2].text == "c");
    CHECK_EQ(t[2].line, size_t(3));
    CHECK(t[3].kind == Tok::End);
}

TEST(lexer_unknown_character_becomes_punct) {
    auto t = lex("a @ b");
    CHECK(t[1].kind == Tok::Punct);
    CHECK(t[1].text == "@");
    CHECK_EQ(t[1].column, size_t(3));
    CHECK(t[2].text == "b");
}

TEST(lexer_unterminated_literals_do_not_hang) {
    auto a = lex("\"abc");
    CHECK(a[0].kind == Tok::StringLit);
    CHECK(a[0].text == "abc");
    CHECK(a[1].kind == Tok::End);

    auto b = lex("'a");
    CHECK(b[0].kind == Tok::CharLit);
    CHECK_EQ(b[0].value, 97L);

    auto c = lex("/* never closed");
    CHECK_EQ(c.size(), size_t(1));
    CHECK(c[0].kind == Tok::End);
}

TEST(lexer_realistic_statement) {
    auto t = lex("void setup() {\n  pinMode(13, OUTPUT);\n}\n");
    CHECK(t[0].kind == Tok::Keyword);
    CHECK(t[0].text == "void");
    CHECK(t[1].kind == Tok::Identifier);
    CHECK(t[1].text == "setup");
    CHECK(t[2].text == "(");
    CHECK(t[3].text == ")");
    CHECK(t[4].text == "{");
    CHECK(t[5].text == "pinMode");
    CHECK_EQ(t[5].line, size_t(2));
    CHECK_EQ(t[5].column, size_t(3));
    CHECK(t[7].kind == Tok::Number);
    CHECK_EQ(t[7].value, 13L);
    CHECK(t[8].text == ",");
    CHECK(t[9].text == "OUTPUT");
    CHECK(t[10].text == ")");
    CHECK(t[11].text == ";");
    CHECK(t[12].text == "}");
    CHECK(t[13].kind == Tok::End);
    CHECK_EQ(kind_of(t[13]), size_t(0));
}

TEST(lexer_end_token_position_is_after_last_input) {
    auto t = lex("ab\ncd");
    Token e = t.back();
    CHECK(e.kind == Tok::End);
    CHECK_EQ(e.line, size_t(2));
    CHECK_EQ(e.column, size_t(3));
}
