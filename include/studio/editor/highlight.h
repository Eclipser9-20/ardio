// A small C/C++/Arduino syntax highlighter for the editor.
//
// highlight_line tokenizes one line into contiguous colored spans that tile the
// whole line, so the editor can draw run after run without gaps. Block-comment
// state (/* ... */) crosses lines through the `in_block` flag, which the caller
// threads from the top of the file down to the first visible line and onward.
// It is deliberately a hand lexer, not a grammar: fast, good enough for code
// coloring, and easy to extend with more keywords.
#pragma once
#include <string>
#include <vector>

#include "studio/core/color.h"
#include "studio/theming/theme.h"

namespace studio {

struct Span {
    int start = 0;  // byte offset in the line
    int end = 0;    // one past the last byte
    Color color;
};

// Tokenize `line`, appending spans that cover it completely. `in_block` is the
// /* */ comment state on entry and is updated on exit.
void highlight_line(const std::string& line, bool& in_block, const Syntax& s,
                    std::vector<Span>& out);

}  // namespace studio
