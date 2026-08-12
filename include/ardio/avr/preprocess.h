#pragma once
#include <string>
#include <vector>

// The C preprocessor used by ardio's AVR compiler front-end.
//
// It runs the classic translation phases over a source buffer: line splicing,
// comment removal, directive execution (#include / #define / #undef /
// conditionals / #pragma once) and macro expansion. The result is a plain text
// buffer ready for tokenize().

namespace ardio {

struct PreprocessResult {
    bool ok = false;
    std::string error;   // "file:line: message" when ok is false
    std::string text;    // preprocessed output when ok is true
};

// Preprocesses `source` as if it were the primary translation unit. Angled and
// quoted includes are looked up in `include_paths`; quoted includes also search
// the directory of the file that names them.
PreprocessResult preprocess(const std::string& source,
                            const std::vector<std::string>& include_paths);

} // namespace ardio
