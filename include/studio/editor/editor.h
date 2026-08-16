// The editor view: draws a TextBuffer and turns input into edits.
//
// It owns the buffer plus the presentation state a buffer has no opinion on --
// scroll position, focus, an in-progress mouse drag. draw() paints the gutter,
// the text, the current-line highlight, the selection and the caret; handle()
// takes an SDL event (typing, keys, mouse, wheel) and applies it. Text is drawn
// with the mono font; tabs are normalized to spaces on load so every column is
// one cell and cursor math stays simple.
#pragma once
#include <string>

#include "studio/editor/buffer.h"
#include "studio/gfx/gfx.h"
#include "studio/theming/theme.h"

union SDL_Event;

namespace studio {

class Font;

class Editor {
public:
    void set_text(const std::string& text);  // load; tabs become spaces
    std::string text() const { return buf_.text(); }
    TextBuffer& buffer() { return buf_; }
    bool dirty() const { return buf_.dirty(); }
    void mark_saved() { buf_.clear_dirty(); }

    // The file this editor is bound to (for tabs).
    const std::string& path() const { return path_; }
    void set_path(const std::string& p) { path_ = p; }

    void set_focus(bool f) { focus_ = f; }
    bool focus() const { return focus_; }

    // 1-based caret position for a status bar. Column counts characters.
    int cursor_row() const { return buf_.cursor().line + 1; }
    int cursor_col() const;

    // Paint into `area`. Remembers metrics for hit-testing in handle().
    void draw(Gfx& g, Font& font, const Theme& theme, Rect area);

    // Apply an SDL event. Returns true if it changed anything (so the loop
    // knows to redraw). `font`/`theme` are needed to map the mouse to a cursor.
    bool handle(const SDL_Event& e, Font& font, const Theme& theme);

private:
    float line_height(Font& font, const Theme& theme) const;
    float char_width(Font& font, const Theme& theme) const;
    float gutter_width(Font& font, const Theme& theme) const;
    Cursor point_to_cursor(float x, float y, Font& font, const Theme& theme) const;
    void ensure_visible(Font& font, const Theme& theme);

    TextBuffer buf_;
    float scroll_y_ = 0;  // pixels scrolled down
    float scroll_x_ = 0;  // pixels scrolled right
    Rect area_{};         // last drawn area, for hit-testing
    bool focus_ = true;
    bool dragging_ = false;
    std::string path_;
};

}  // namespace studio
