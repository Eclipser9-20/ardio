// The immediate-mode widget layer, in the spirit of libhike but for pixels: no
// retained tree, no widget objects, no invalidation. A screen calls button(),
// card(), row() each frame with a rect and its own state, and the call both
// draws the widget and reports whether it was clicked. The UI has no memory of
// its own beyond the pointer state handed in, so what is on screen is always
// exactly what the caller asked for this frame.
//
// One Ui is built per frame around the renderer, the font, the theme, and this
// frame's pointer input. Widgets read their colors from the theme, so the whole
// look retheme with the palette and nothing here hardcodes an appearance.
#pragma once
#include <cstdint>
#include <string_view>

#include "studio/fonts/font.h"
#include "studio/gfx/gfx.h"
#include "studio/theming/theme.h"
#include "studio/ui/anim.h"

namespace studio {

// This frame's pointer state, in window points.
struct Input {
    float mx = 0, my = 0;
    bool clicked = false;     // left button went down this frame (edge)
    bool mouse_down = false;  // left button is currently held (level)
};

class Ui {
public:
    Ui(Gfx& g, Font& font, const Theme& theme, const Input& in, Anim& anim)
        : g_(g), font_(font), theme_(theme), in_(in), anim_(anim) {}

    const Theme& theme() const { return theme_; }
    Gfx& gfx() { return g_; }
    Font& font() { return font_; }
    Anim& anim() { return anim_; }

    bool hover(Rect r) const { return r.contains(in_.mx, in_.my); }
    bool clicked(Rect r) const { return in_.clicked && hover(r); }
    float mouse_x() const { return in_.mx; }
    bool mouse_down() const { return in_.mouse_down; }

    // A smoothed 0..1 hover amount for `r`, animated toward 1 while hovered and
    // 0 otherwise. Widgets mix their colors by this for a soft fade.
    float hot_t(Rect r) { return anim_.smooth(anim_id(r.x, r.y), hover(r) ? 1.0f : 0.0f); }

    // Text helpers at a given pixel size.
    void text(float x, float y, std::string_view s, float px, Color c) {
        font_.draw(g_, x, y, s, px, c);
    }
    void text_vcentered(Rect r, float x, std::string_view s, float px, Color c);
    void text_centered(Rect r, std::string_view s, float px, Color c);

    // A pill button. `primary` fills it with the accent; otherwise it is a quiet
    // surface that lifts on hover. Returns true the frame it is clicked.
    bool button(Rect r, std::string_view label, bool primary = false);

    // A large action card: an optional Nerd-icon code point, a title, and a
    // sub-line. Lifts and shows an accent edge on hover. Returns true on click.
    bool card(Rect r, uint32_t icon, std::string_view title, std::string_view sub);

    // A selectable list row. `selected` paints the persistent selection; hover
    // is shown independently. Returns true on click.
    bool row(Rect r, std::string_view label, bool selected);

    // A switch bound to `value`. Toggles on click; returns true if it changed.
    bool toggle(Rect r, bool& value);

    // A horizontal slider bound to `value` in [min, max]. Tracks while the
    // pointer is held over it; returns true on any change this frame.
    bool slider(Rect r, float& value, float min, float max);

    // A color swatch. Draws `col`; `selected` rings it. Returns true on click.
    bool swatch(Rect r, Color col, bool selected);

    // A collapsible section header with a chevron. Toggles `open` on click and
    // returns the resulting open state, so callers can gate the body inline.
    bool section(Rect r, std::string_view label, bool& open);

private:
    Gfx& g_;
    Font& font_;
    const Theme& theme_;
    const Input& in_;
    Anim& anim_;
};

}  // namespace studio
