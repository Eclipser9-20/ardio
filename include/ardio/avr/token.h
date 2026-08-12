#pragma once
#include <string>
#include <vector>

namespace ardio {

enum class Tok {
    End, Identifier, Number, StringLit, CharLit, Punct, Keyword,
};

struct Token {
    Tok kind = Tok::End;
    std::string text;   // identifier name, punctuator, or keyword spelling
    long value = 0;     // for Number / CharLit
    size_t line = 0;
    size_t column = 0;
};

// Splits preprocessed source into tokens. Never fails: anything unrecognised
// becomes a Punct token so the parser can report it with position.
std::vector<Token> tokenize(const std::string& source);

bool is_keyword(const std::string& word);

} // namespace ardio
