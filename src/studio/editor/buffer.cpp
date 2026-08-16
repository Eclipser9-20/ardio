#include "studio/editor/buffer.h"

namespace studio {
namespace {

bool is_cont(unsigned char b) { return (b & 0xC0) == 0x80; }

int utf8_len(unsigned char lead) {
    if (lead < 0x80) return 1;
    if ((lead & 0xE0) == 0xC0) return 2;
    if ((lead & 0xF0) == 0xE0) return 3;
    if ((lead & 0xF8) == 0xF0) return 4;
    return 1;
}

bool is_word(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
           c >= 0x80;
}

// Previous / next character boundary within a line.
int prev_boundary(const std::string& s, int col) {
    if (col <= 0) return 0;
    --col;
    while (col > 0 && is_cont(static_cast<unsigned char>(s[col]))) --col;
    return col;
}
int next_boundary(const std::string& s, int col) {
    if (col >= static_cast<int>(s.size())) return static_cast<int>(s.size());
    return col + utf8_len(static_cast<unsigned char>(s[col]));
}

}  // namespace

TextBuffer::TextBuffer() : lines_{""} {}

void TextBuffer::set_text(const std::string& text) {
    lines_.clear();
    std::string cur;
    for (char c : text) {
        if (c == '\n') {
            lines_.push_back(cur);
            cur.clear();
        } else if (c != '\r') {
            cur += c;
        }
    }
    lines_.push_back(cur);
    if (lines_.empty()) lines_.push_back("");
    cur_ = anchor_ = {0, 0};
    sel_ = false;
    dirty_ = false;
}

std::string TextBuffer::text() const {
    std::string out;
    for (size_t i = 0; i < lines_.size(); ++i) {
        out += lines_[i];
        if (i + 1 < lines_.size()) out += '\n';
    }
    return out;
}

Cursor TextBuffer::clamp(Cursor c) const {
    if (c.line < 0) c.line = 0;
    if (c.line >= line_count()) c.line = line_count() - 1;
    if (c.col < 0) c.col = 0;
    int n = static_cast<int>(lines_[c.line].size());
    if (c.col > n) c.col = n;
    // Snap off a continuation byte onto the nearest boundary.
    while (c.col > 0 && c.col < n && is_cont(static_cast<unsigned char>(lines_[c.line][c.col])))
        --c.col;
    return c;
}

void TextBuffer::put(Cursor c, bool extend) {
    c = clamp(c);
    if (extend) {
        if (!sel_) {
            anchor_ = cur_;
            sel_ = true;
        }
        cur_ = c;
    } else {
        cur_ = anchor_ = c;
        sel_ = false;
    }
}

void TextBuffer::selection_range(Cursor& lo, Cursor& hi) const {
    if (cur_ < anchor_) {
        lo = cur_;
        hi = anchor_;
    } else {
        lo = anchor_;
        hi = cur_;
    }
}

std::string TextBuffer::selected_text() const {
    if (!has_selection()) return "";
    Cursor lo, hi;
    selection_range(lo, hi);
    if (lo.line == hi.line) return lines_[lo.line].substr(lo.col, hi.col - lo.col);
    std::string out = lines_[lo.line].substr(lo.col);
    for (int l = lo.line + 1; l < hi.line; ++l) {
        out += '\n';
        out += lines_[l];
    }
    out += '\n';
    out += lines_[hi.line].substr(0, hi.col);
    return out;
}

void TextBuffer::delete_selection() {
    if (!has_selection()) return;
    Cursor lo, hi;
    selection_range(lo, hi);
    std::string tail = lines_[hi.line].substr(hi.col);
    lines_[lo.line] = lines_[lo.line].substr(0, lo.col) + tail;
    lines_.erase(lines_.begin() + lo.line + 1, lines_.begin() + hi.line + 1);
    cur_ = anchor_ = lo;
    sel_ = false;
    dirty_ = true;
}

void TextBuffer::insert(const std::string& s) {
    if (has_selection()) delete_selection();
    lines_[cur_.line].insert(cur_.col, s);
    cur_.col += static_cast<int>(s.size());
    anchor_ = cur_;
    sel_ = false;
    dirty_ = true;
}

void TextBuffer::insert_text(const std::string& s) {
    if (has_selection()) delete_selection();
    for (char c : s) {
        if (c == '\n')
            newline();
        else if (c != '\r')
            lines_[cur_.line].insert(cur_.col++, 1, c);
    }
    anchor_ = cur_;
    sel_ = false;
    dirty_ = true;
}

void TextBuffer::newline() {
    if (has_selection()) delete_selection();
    std::string tail = lines_[cur_.line].substr(cur_.col);
    lines_[cur_.line].erase(cur_.col);
    lines_.insert(lines_.begin() + cur_.line + 1, tail);
    cur_.line += 1;
    cur_.col = 0;
    anchor_ = cur_;
    sel_ = false;
    dirty_ = true;
}

void TextBuffer::backspace() {
    if (has_selection()) {
        delete_selection();
        return;
    }
    if (cur_.col > 0) {
        int p = prev_boundary(lines_[cur_.line], cur_.col);
        lines_[cur_.line].erase(p, cur_.col - p);
        cur_.col = p;
    } else if (cur_.line > 0) {
        int prev_len = static_cast<int>(lines_[cur_.line - 1].size());
        lines_[cur_.line - 1] += lines_[cur_.line];
        lines_.erase(lines_.begin() + cur_.line);
        cur_.line -= 1;
        cur_.col = prev_len;
    }
    anchor_ = cur_;
    sel_ = false;
    dirty_ = true;
}

void TextBuffer::del_forward() {
    if (has_selection()) {
        delete_selection();
        return;
    }
    std::string& ln = lines_[cur_.line];
    if (cur_.col < static_cast<int>(ln.size())) {
        int nx = next_boundary(ln, cur_.col);
        ln.erase(cur_.col, nx - cur_.col);
    } else if (cur_.line + 1 < line_count()) {
        ln += lines_[cur_.line + 1];
        lines_.erase(lines_.begin() + cur_.line + 1);
    }
    anchor_ = cur_;
    sel_ = false;
    dirty_ = true;
}

void TextBuffer::move_left(bool extend) {
    if (has_selection() && !extend) {
        Cursor lo, hi;
        selection_range(lo, hi);
        put(lo, false);
        return;
    }
    Cursor c = cur_;
    if (c.col > 0)
        c.col = prev_boundary(lines_[c.line], c.col);
    else if (c.line > 0) {
        c.line -= 1;
        c.col = static_cast<int>(lines_[c.line].size());
    }
    put(c, extend);
}

void TextBuffer::move_right(bool extend) {
    if (has_selection() && !extend) {
        Cursor lo, hi;
        selection_range(lo, hi);
        put(hi, false);
        return;
    }
    Cursor c = cur_;
    if (c.col < static_cast<int>(lines_[c.line].size()))
        c.col = next_boundary(lines_[c.line], c.col);
    else if (c.line + 1 < line_count()) {
        c.line += 1;
        c.col = 0;
    }
    put(c, extend);
}

void TextBuffer::move_up(bool extend) {
    Cursor c = cur_;
    if (c.line > 0) c.line -= 1;
    put(c, extend);  // clamp() lands col on a boundary within the shorter line
}

void TextBuffer::move_down(bool extend) {
    Cursor c = cur_;
    if (c.line + 1 < line_count()) c.line += 1;
    put(c, extend);
}

void TextBuffer::move_home(bool extend) { put({cur_.line, 0}, extend); }
void TextBuffer::move_end(bool extend) {
    put({cur_.line, static_cast<int>(lines_[cur_.line].size())}, extend);
}
void TextBuffer::move_doc_start(bool extend) { put({0, 0}, extend); }
void TextBuffer::move_doc_end(bool extend) {
    put({line_count() - 1, static_cast<int>(lines_.back().size())}, extend);
}

void TextBuffer::move_word_left(bool extend) {
    Cursor c = cur_;
    if (c.col == 0) {
        move_left(extend);
        return;
    }
    const std::string& s = lines_[c.line];
    while (c.col > 0 && !is_word(static_cast<unsigned char>(s[c.col - 1]))) --c.col;
    while (c.col > 0 && is_word(static_cast<unsigned char>(s[c.col - 1]))) --c.col;
    put(c, extend);
}

void TextBuffer::move_word_right(bool extend) {
    Cursor c = cur_;
    const std::string& s = lines_[c.line];
    int n = static_cast<int>(s.size());
    if (c.col >= n) {
        move_right(extend);
        return;
    }
    while (c.col < n && is_word(static_cast<unsigned char>(s[c.col]))) ++c.col;
    while (c.col < n && !is_word(static_cast<unsigned char>(s[c.col]))) ++c.col;
    put(c, extend);
}

void TextBuffer::set_cursor(Cursor c, bool extend) { put(c, extend); }

void TextBuffer::select_all() {
    anchor_ = {0, 0};
    sel_ = true;
    cur_ = {line_count() - 1, static_cast<int>(lines_.back().size())};
}

}  // namespace studio
