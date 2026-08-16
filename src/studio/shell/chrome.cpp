#include "studio/shell/chrome.h"

namespace studio {
namespace {

// The window controls: minimize and close, right-aligned, each a square hit box
// the width of the titlebar's height. They are deliberately NOT colored dots --
// no traffic-light pastiche. At rest they are just a faint glyph on the bar;
// the box lights up only under the pointer, and only close warms toward danger.
void place_controls(const Theme& theme, Rect bounds, ChromeLayout& L) {
    const float box = theme.chrome.control_box;
    const float top = bounds.y + (theme.chrome.titlebar_height - box) * 0.5f;
    L.close_btn = {bounds.right() - box, top, box, box};
    L.min_btn = {bounds.right() - box * 2.0f, top, box, box};
}

// A crossed ✕ centered in `box`, drawn a couple of offset lines thick so it
// reads at UI weight rather than a hairline. Real icons arrive with the font.
void draw_close_glyph(Gfx& g, Rect box, float glyph, Color c) {
    const float cx = box.x + box.w * 0.5f, cy = box.y + box.h * 0.5f;
    const float h = glyph * 0.5f;
    for (float o = -0.5f; o <= 0.5f; o += 0.5f) {
        g.line(cx - h + o, cy - h, cx + h + o, cy + h, c);
        g.line(cx - h + o, cy + h, cx + h + o, cy - h, c);
    }
}

// A single horizontal bar for minimize.
void draw_min_glyph(Gfx& g, Rect box, float glyph, Color c) {
    const float cy = box.y + box.h * 0.5f;
    const float cx = box.x + box.w * 0.5f;
    g.fill_rect({cx - glyph * 0.5f, cy - 0.75f, glyph, 1.5f}, c);
}

void control(Gfx& g, Rect box, bool hot, Color hover_bg, Color glyph_rest, Color glyph_hot,
             void (*draw)(Gfx&, Rect, float, Color), float glyph) {
    if (hot) g.fill_rounded(box.inset(4), 6, hover_bg);
    draw(g, box, glyph, hot ? glyph_hot : glyph_rest);
}

}  // namespace

ChromeAction draw_chrome(Gfx& g, Font& font, const Theme& theme, Rect bounds,
                         const std::string& title, bool focused, bool as_back, float mouse_x,
                         float mouse_y, bool clicked, ChromeLayout& layout) {
    const Chrome& c = theme.chrome;
    const Palette& p = theme.palette;
    (void)title;
    layout.titlebar_height = c.titlebar_height;

    // No titlebar strip: the app draws to the top edge. The top region is just
    // an invisible drag strip (the hit-test reads layout.titlebar_height), with
    // the window controls floating over it at the top-right.
    place_controls(theme, bounds, layout);
    const bool over_close = layout.close_btn.contains(mouse_x, mouse_y);
    const bool over_min = layout.min_btn.contains(mouse_x, mouse_y);

    control(g, layout.min_btn, over_min, p.raised, p.muted, p.fg, draw_min_glyph, c.control_glyph);
    if (as_back) {
        // Back arrow: returns to the menu, so it is neutral, not danger-red.
        Rect b = layout.close_btn;
        if (over_close) g.fill_rounded(b.inset(4), 6, p.accent2.with_alpha(0.22f));
        font.draw_centered(g, b.x, b.y + (b.h - font.line_height(theme.fonts.ui_size)) * 0.5f, b.w,
                           "\xE2\x86\x90", theme.fonts.ui_size, over_close ? p.fg : p.muted);
    } else {
        control(g, layout.close_btn, over_close, p.danger.with_alpha(0.25f), p.muted, p.danger,
                draw_close_glyph, c.control_glyph);
    }

    // The outline last. A thin neutral slate edge -- brighter when focused,
    // never a colored glow.
    const float bw = focused ? c.border_focused : c.border_width;
    const Color edge = focused ? p.border_focus : p.border;
    g.stroke_rounded(bounds, c.corner_radius, bw, edge);

    if (clicked) {
        if (over_close) return ChromeAction::Close;
        if (over_min) return ChromeAction::Minimize;
    }
    return ChromeAction::None;
}

}  // namespace studio
