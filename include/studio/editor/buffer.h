// The text buffer: lines of UTF-8 text, a cursor, a selection, and the editing
// operations over them. It knows nothing about rendering, fonts, or SDL -- it
// is pure logic, so it can be reasoned about and tested on its own. The editor
// view drives it in response to input and then draws whatever state it holds.
//
// A column is a byte offset within its line. Cursor motion always lands on a
// UTF-8 character boundary, so multi-byte characters are never split, but the
// representation stays a plain std::string per line.
#pragma once
#include <string>
#include <vector>

namespace studio {

struct Cursor {
    int line = 0;
    int col = 0;  // byte offset within lines_[line]
    bool operator==(const Cursor& o) const { return line == o.line && col == o.col; }
    bool operator!=(const Cursor& o) const { return !(*this == o); }
    bool operator<(const Cursor& o) const {
        return line < o.line || (line == o.line && col < o.col);
    }
};

class TextBuffer {
public:
    TextBuffer();

    void set_text(const std::string& text);  // load, splitting on '\n'
    std::string text() const;                // join with '\n'

    const std::vector<std::string>& lines() const { return lines_; }
    int line_count() const { return static_cast<int>(lines_.size()); }
    const std::string& line(int i) const { return lines_[i]; }

    Cursor cursor() const { return cur_; }
    bool has_selection() const { return sel_ && anchor_ != cur_; }
    void selection_range(Cursor& lo, Cursor& hi) const;  // normalized
    std::string selected_text() const;

    bool dirty() const { return dirty_; }
    void clear_dirty() { dirty_ = false; }

    // --- editing (each replaces the selection first, if any) ---
    void insert(const std::string& utf8);  // no newlines expected; use newline()
    void insert_text(const std::string& utf8);  // may contain newlines (paste)
    void newline();
    void backspace();
    void del_forward();

    // --- movement; `extend` grows the selection instead of collapsing it ---
    void move_left(bool extend);
    void move_right(bool extend);
    void move_up(bool extend);
    void move_down(bool extend);
    void move_home(bool extend);
    void move_end(bool extend);
    void move_doc_start(bool extend);
    void move_doc_end(bool extend);
    void move_word_left(bool extend);
    void move_word_right(bool extend);
    void set_cursor(Cursor c, bool extend);  // clamps into range
    void select_all();

private:
    void put(Cursor c, bool extend);  // moves cursor, manages the selection
    void delete_selection();
    Cursor clamp(Cursor c) const;

    std::vector<std::string> lines_;
    Cursor cur_;
    Cursor anchor_;
    bool sel_ = false;
    bool dirty_ = false;
};

}  // namespace studio
