#include "studio/editor/editor.h"

#include <SDL3/SDL.h>

#include <string>
#include <vector>

#include "studio/editor/highlight.h"
#include "studio/fonts/font.h"

namespace studio {
namespace {

// Advance `n` UTF-8 characters into `s` from byte 0, returning the byte offset.
int char_to_byte(const std::string& s, int n) {
    int i = 0, c = 0, len = static_cast<int>(s.size());
    while (i < len && c < n) {
        unsigned char b = static_cast<unsigned char>(s[i]);
        i += (b < 0x80) ? 1 : (b & 0xE0) == 0xC0 ? 2 : (b & 0xF0) == 0xE0 ? 3 : 4;
        ++c;
    }
    return i > len ? len : i;
}

}  // namespace

int Editor::cursor_col() const {
    Cursor c = buf_.cursor();
    const std::string& l = buf_.line(c.line);
    int chars = 0;
    for (int i = 0; i < c.col && i < static_cast<int>(l.size());) {
        unsigned char b = static_cast<unsigned char>(l[i]);
        i += (b < 0x80) ? 1 : (b & 0xE0) == 0xC0 ? 2 : (b & 0xF0) == 0xE0 ? 3 : 4;
        ++chars;
    }
    return chars + 1;
}

void Editor::set_text(const std::string& text) {
    // Normalize tabs to spaces so every column is one mono cell.
    std::string t;
    for (char c : text) {
        if (c == '\t')
            t += "    ";
        else
            t += c;
    }
    buf_.set_text(t);
    scroll_y_ = scroll_x_ = 0;
}

float Editor::line_height(Font& font, const Theme& theme) const {
    return font.line_height(theme.fonts.mono_size);
}
float Editor::char_width(Font& font, const Theme& theme) const {
    return font.measure("0", theme.fonts.mono_size);
}
float Editor::gutter_width(Font& font, const Theme& theme) const {
    int digits = 1, n = buf_.line_count();
    while (n >= 10) {
        n /= 10;
        ++digits;
    }
    if (digits < 3) digits = 3;
    return digits * char_width(font, theme) + 24;
}

Cursor Editor::point_to_cursor(float x, float y, Font& font, const Theme& theme) const {
    float lh = line_height(font, theme);
    float cw = char_width(font, theme);
    float text_x = area_.x + gutter_width(font, theme);
    int line = static_cast<int>((y - (area_.y + 6) + scroll_y_) / lh);
    if (line < 0) line = 0;
    if (line >= buf_.line_count()) line = buf_.line_count() - 1;
    float relx = x - text_x + scroll_x_;
    int ch = static_cast<int>(relx / cw + 0.5f);
    if (ch < 0) ch = 0;
    return {line, char_to_byte(buf_.line(line), ch)};
}

void Editor::ensure_visible(Font& font, const Theme& theme) {
    float lh = line_height(font, theme);
    Cursor c = buf_.cursor();
    float top = c.line * lh;
    if (top < scroll_y_) scroll_y_ = top;
    if (top + lh > scroll_y_ + area_.h) scroll_y_ = top + lh - area_.h;
    float maxs = buf_.line_count() * lh - area_.h;
    if (maxs < 0) maxs = 0;
    if (scroll_y_ > maxs) scroll_y_ = maxs;
    if (scroll_y_ < 0) scroll_y_ = 0;
}

void Editor::draw(Gfx& g, Font& font, const Theme& theme, Rect area) {
    area_ = area;
    const Palette& p = theme.palette;
    const float ms = theme.fonts.mono_size;
    const float lh = line_height(font, theme);
    const float gutter = gutter_width(font, theme);
    const float text_x = area.x + gutter - scroll_x_;
    const float top = area.y + 6;

    g.set_clip(area);
    g.fill_rect(area, p.bg);

    Cursor cur = buf_.cursor();
    Cursor lo, hi;
    bool has_sel = buf_.has_selection();
    if (has_sel) buf_.selection_range(lo, hi);

    int first = static_cast<int>(scroll_y_ / lh);
    if (first < 0) first = 0;
    int last = first + static_cast<int>(area.h / lh) + 2;
    if (last > buf_.line_count()) last = buf_.line_count();

    // Advance the block-comment state from the top of the file to the first
    // visible line, then keep threading it as we draw downward.
    std::vector<Span> spans;
    bool in_block = false;
    for (int i = 0; i < first; ++i) highlight_line(buf_.line(i), in_block, theme.syntax, spans);

    for (int i = first; i < last; ++i) {
        float y = top + i * lh - scroll_y_;
        const std::string& ln = buf_.line(i);

        // Current-line highlight (only when there is no selection).
        if (i == cur.line && !has_sel)
            g.fill_rect({area.x, y, area.w, lh}, p.raised.with_alpha(0.5f));

        // Selection.
        if (has_sel && i >= lo.line && i <= hi.line) {
            int sc = (i == lo.line) ? lo.col : 0;
            int ec = (i == hi.line) ? hi.col : static_cast<int>(ln.size());
            float sx = text_x + font.measure(ln.substr(0, sc), ms);
            float ex = text_x + font.measure(ln.substr(0, ec), ms);
            if (i < hi.line) ex += char_width(font, theme) * 0.5f;  // show the line break
            g.fill_rect({sx, y, ex - sx, lh}, p.selection);
        }

        // Line number, right-aligned in the gutter.
        std::string num = std::to_string(i + 1);
        float nx = area.x + gutter - 12 - font.measure(num, ms);
        font.draw(g, nx, y, num, ms, i == cur.line ? p.fg : p.muted);

        // The line text, drawn as syntax-highlighted runs.
        highlight_line(ln, in_block, theme.syntax, spans);
        float sx = text_x;
        for (const Span& sp : spans) {
            std::string piece = ln.substr(sp.start, sp.end - sp.start);
            font.draw(g, sx, y, piece, ms, sp.color);
            sx += font.measure(piece, ms);
        }
    }

    // Caret.
    if (focus_) {
        float cx = text_x + font.measure(buf_.line(cur.line).substr(0, cur.col), ms);
        float cy = top + cur.line * lh - scroll_y_;
        g.fill_rect({cx, cy, 2, lh}, p.accent2);
    }

    g.clear_clip();
}

bool Editor::handle(const SDL_Event& e, Font& font, const Theme& theme) {
    const bool shift = (SDL_GetModState() & SDL_KMOD_SHIFT) != 0;
    const bool cmd = (SDL_GetModState() & (SDL_KMOD_GUI | SDL_KMOD_CTRL)) != 0;
    const bool alt = (SDL_GetModState() & SDL_KMOD_ALT) != 0;

    switch (e.type) {
        case SDL_EVENT_TEXT_INPUT:
            buf_.insert(e.text.text);
            ensure_visible(font, theme);
            return true;

        case SDL_EVENT_KEY_DOWN:
            switch (e.key.key) {
                case SDLK_RETURN:
                case SDLK_KP_ENTER: buf_.newline(); break;
                case SDLK_BACKSPACE: buf_.backspace(); break;
                case SDLK_DELETE: buf_.del_forward(); break;
                case SDLK_TAB: buf_.insert("    "); break;
                case SDLK_LEFT:
                    if (alt) buf_.move_word_left(shift);
                    else if (cmd) buf_.move_home(shift);
                    else buf_.move_left(shift);
                    break;
                case SDLK_RIGHT:
                    if (alt) buf_.move_word_right(shift);
                    else if (cmd) buf_.move_end(shift);
                    else buf_.move_right(shift);
                    break;
                case SDLK_UP:
                    if (cmd) buf_.move_doc_start(shift);
                    else buf_.move_up(shift);
                    break;
                case SDLK_DOWN:
                    if (cmd) buf_.move_doc_end(shift);
                    else buf_.move_down(shift);
                    break;
                case SDLK_HOME: buf_.move_home(shift); break;
                case SDLK_END: buf_.move_end(shift); break;
                case SDLK_PAGEUP:
                    for (int i = 0; i < 20; ++i) buf_.move_up(shift);
                    break;
                case SDLK_PAGEDOWN:
                    for (int i = 0; i < 20; ++i) buf_.move_down(shift);
                    break;
                case SDLK_A:
                    if (cmd) buf_.select_all(); else return false;
                    break;
                case SDLK_C:
                    if (cmd) { if (buf_.has_selection()) SDL_SetClipboardText(buf_.selected_text().c_str()); }
                    else return false;
                    break;
                case SDLK_X:
                    if (cmd) {
                        if (buf_.has_selection()) {
                            SDL_SetClipboardText(buf_.selected_text().c_str());
                            buf_.backspace();
                        }
                    } else return false;
                    break;
                case SDLK_V:
                    if (cmd) {
                        char* clip = SDL_GetClipboardText();
                        if (clip) {
                            buf_.insert_text(clip);
                            SDL_free(clip);
                        }
                    } else return false;
                    break;
                default: return false;
            }
            ensure_visible(font, theme);
            return true;

        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            if (e.button.button == SDL_BUTTON_LEFT && area_.contains(e.button.x, e.button.y)) {
                buf_.set_cursor(point_to_cursor(e.button.x, e.button.y, font, theme), shift);
                dragging_ = true;
                return true;
            }
            return false;

        case SDL_EVENT_MOUSE_MOTION:
            if (dragging_) {
                buf_.set_cursor(point_to_cursor(e.motion.x, e.motion.y, font, theme), true);
                ensure_visible(font, theme);
                return true;
            }
            return false;

        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (e.button.button == SDL_BUTTON_LEFT) dragging_ = false;
            return false;

        case SDL_EVENT_MOUSE_WHEEL:
            if (area_.contains(e.wheel.mouse_x, e.wheel.mouse_y)) {
                scroll_y_ -= e.wheel.y * line_height(font, theme) * 3.0f;
                float maxs = buf_.line_count() * line_height(font, theme) - area_.h;
                if (maxs < 0) maxs = 0;
                if (scroll_y_ > maxs) scroll_y_ = maxs;
                if (scroll_y_ < 0) scroll_y_ = 0;
                return true;
            }
            return false;

        default: return false;
    }
}

}  // namespace studio
