#include "studio/ui/ui.h"

namespace studio {
namespace {
// Encode a single code point as UTF-8 into a small buffer, for drawing a Nerd
// Font icon given its code point.
struct Utf8 {
    char b[5] = {};
    explicit Utf8(uint32_t cp) {
        if (cp < 0x80) {
            b[0] = static_cast<char>(cp);
        } else if (cp < 0x800) {
            b[0] = static_cast<char>(0xC0 | (cp >> 6));
            b[1] = static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            b[0] = static_cast<char>(0xE0 | (cp >> 12));
            b[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            b[2] = static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            b[0] = static_cast<char>(0xF0 | (cp >> 18));
            b[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            b[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            b[3] = static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
};
}  // namespace

void Ui::text_vcentered(Rect r, float x, std::string_view s, float px, Color c) {
    float y = r.y + (r.h - font_.line_height(px)) * 0.5f;
    font_.draw(g_, x, y, s, px, c);
}

void Ui::text_centered(Rect r, std::string_view s, float px, Color c) {
    float y = r.y + (r.h - font_.line_height(px)) * 0.5f;
    font_.draw_centered(g_, r.x, y, r.w, s, px, c);
}

bool Ui::button(Rect r, std::string_view label, bool primary) {
    const Palette& p = theme_.palette;
    float t = hot_t(r);  // smoothed hover 0..1
    if (primary) {
        g_.fill_rounded(r, 6, p.accent.mix(p.accent2, t * 0.6f));
        text_centered(r, label, theme_.fonts.ui_size, p.fg);
    } else {
        g_.fill_rounded(r, 6, p.surface.mix(p.raised, t));
        g_.stroke_rounded(r, 6, 1.0f, p.border.mix(p.border_focus, t));
        text_centered(r, label, theme_.fonts.ui_size, p.fg);
    }
    return clicked(r);
}

bool Ui::card(Rect r, uint32_t icon, std::string_view title, std::string_view sub) {
    const Palette& p = theme_.palette;
    float t = hot_t(r);  // smoothed hover 0..1

    // Flat VS Code-style card: a subtle raise on hover and a thin slate border,
    // both eased. No glow, no drop shadow -- restraint is the look.
    g_.fill_rounded(r, 6, p.surface.mix(p.raised, t));
    g_.stroke_rounded(r, 6, 1.0f, p.border.mix(p.border_focus, t));

    float text_x = r.x + 20;
    if (icon && font_.has(icon)) {
        float isz = theme_.fonts.ui_size * 1.7f;
        Utf8 u(icon);
        font_.draw(g_, r.x + 18, r.y + (r.h - font_.line_height(isz)) * 0.5f, u.b, isz,
                   p.muted.mix(p.fg, t));
        text_x = r.x + 18 + isz + 14;
    }

    float th = font_.line_height(theme_.fonts.ui_size);
    float sh = font_.line_height(theme_.fonts.title_size);
    float total = th + sh;
    float top = r.y + (r.h - total) * 0.5f;
    font_.draw(g_, text_x, top, title, theme_.fonts.ui_size, p.fg);
    font_.draw(g_, text_x, top + th, sub, theme_.fonts.title_size, p.muted);
    return clicked(r);
}

bool Ui::toggle(Rect r, bool& value) {
    const Palette& p = theme_.palette;
    // A pill track with a knob that eases between the off and on sides. `t`
    // springs toward the boolean state so the knob slides and the track fades.
    float h = r.h, w = h * 1.8f;
    Rect track{r.x, r.y, w, h};
    float t = anim_.smooth(anim_id(r.x, r.y), value ? 1.0f : 0.0f, 22.0f);
    g_.fill_rounded(track, h * 0.5f, p.surface.mix(p.accent2, t));
    if (t < 0.99f) g_.stroke_rounded(track, h * 0.5f, 1.0f, p.border.with_alpha(1.0f - t));
    float kd = h - 6;
    float off = track.x + 3, on = track.right() - kd - 3;
    float kx = off + (on - off) * t;
    g_.fill_rounded({kx, r.y + 3, kd, kd}, kd * 0.5f, p.fg.mix(p.bg, t * 0.4f));
    if (clicked(track)) {
        value = !value;
        return true;
    }
    return false;
}

bool Ui::slider(Rect r, float& value, float min, float max) {
    const Palette& p = theme_.palette;
    float t = (max > min) ? (value - min) / (max - min) : 0.0f;
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    float cy = r.y + r.h * 0.5f;
    // Track, filled portion, and knob.
    g_.fill_rounded({r.x, cy - 2, r.w, 4}, 2, p.surface.mix(p.fg, 0.12f));
    g_.fill_rounded({r.x, cy - 2, r.w * t, 4}, 2, p.accent);
    float knob = r.h * 0.5f;
    float kx = r.x + r.w * t - knob * 0.5f;
    bool hot = hover(r);
    g_.fill_rounded({kx, cy - knob * 0.5f, knob, knob}, knob * 0.5f, hot ? p.accent2 : p.fg);
    // Track the pointer while it is held anywhere over the row.
    if (mouse_down() && hover({r.x - 8, r.y - 8, r.w + 16, r.h + 16})) {
        float nt = (mouse_x() - r.x) / r.w;
        nt = nt < 0 ? 0 : nt > 1 ? 1 : nt;
        float nv = min + nt * (max - min);
        if (nv != value) {
            value = nv;
            return true;
        }
    }
    return false;
}

bool Ui::swatch(Rect r, Color col, bool selected) {
    g_.fill_rounded(r, 7, col);
    if (selected)
        g_.stroke_rounded(r.inset(-2), 9, 2, theme_.palette.fg);
    else if (hover(r))
        g_.stroke_rounded(r.inset(-2), 9, 2, theme_.palette.muted);
    return clicked(r);
}

bool Ui::section(Rect r, std::string_view label, bool& open) {
    const Palette& p = theme_.palette;
    float t = hot_t(r);
    if (t > 0.002f) g_.fill_rounded(r, 5, p.raised.with_alpha(t));
    float ty = r.y + (r.h - font_.line_height(theme_.fonts.ui_size)) * 0.5f;
    // Chevron: ▾ when open, ▸ when collapsed.
    font_.draw(g_, r.x + 8, ty, open ? "\xE2\x96\xBE" : "\xE2\x96\xB8", theme_.fonts.ui_size,
               p.muted);
    font_.draw(g_, r.x + 30, ty, label, theme_.fonts.ui_size, p.fg);
    if (clicked(r)) open = !open;
    return open;
}

bool Ui::row(Rect r, std::string_view label, bool selected) {
    const Palette& p = theme_.palette;
    float t = hot_t(r);
    if (selected) {
        g_.fill_rounded(r, 5, p.selection);  // solid VS Code active-selection blue
    } else if (t > 0.002f) {
        g_.fill_rounded(r, 5, p.raised.with_alpha(t));
    }
    text_vcentered(r, r.x + 14, label, theme_.fonts.ui_size,
                   selected ? p.fg : p.muted.mix(p.fg, t));
    return clicked(r);
}

}  // namespace studio
