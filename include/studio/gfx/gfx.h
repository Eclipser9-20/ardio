// The drawing surface: a thin, GPU-accelerated 2D layer over SDL_Renderer.
//
// Every shape the chrome and widgets need bottoms out here -- filled and
// stroked rounded rectangles, plain rects, lines, and (for now) text through
// SDL's built-in debug font. Rounded shapes are triangulated and submitted as
// geometry so they ride the same GPU backend (Metal, D3D12, Vulkan) as
// everything else; nothing is drawn on the CPU.
//
// The class owns nothing but a borrowed renderer pointer. The window creates
// the renderer and hands it in; Gfx is a stateless façade over it, so it is
// cheap to construct per frame or hold for the program's life, whichever reads
// better where it is used.
#pragma once
#include <string>
#include <string_view>

#include "studio/core/color.h"

struct SDL_Renderer;

namespace studio {

struct Rect {
    float x = 0, y = 0, w = 0, h = 0;

    float right() const { return x + w; }
    float bottom() const { return y + h; }
    bool contains(float px, float py) const {
        return px >= x && px < x + w && py >= y && py < y + h;
    }
    // A copy inset by `d` on every side (negative grows it). Used constantly to
    // turn a panel rect into its content rect.
    Rect inset(float d) const { return {x + d, y + d, w - 2 * d, h - 2 * d}; }
};

class Gfx {
public:
    explicit Gfx(SDL_Renderer* r) : r_(r) {}

    // Whole-target clear to a solid color.
    void clear(Color c);

    // Axis-aligned filled rectangle. The fast path; rounded rects fall back to
    // geometry, this does not.
    void fill_rect(Rect box, Color c);

    // Filled rounded rectangle. `radius` is clamped to half the shorter side,
    // so passing a huge radius yields a stadium/■circle rather than artifacts.
    void fill_rounded(Rect box, float radius, Color c);

    // Stroked rounded rectangle, `width` pixels thick, drawn inward from the
    // edge so the stroke stays within `box`. This is the window outline.
    void stroke_rounded(Rect box, float radius, float width, Color c);

    // A single straight line. Thin; for thick lines use a filled rect.
    void line(float x0, float y0, float x1, float y1, Color c);

    // Debug text via SDL's built-in 8x8 font. A placeholder until libtruetype
    // arrives -- fixed size, ASCII only, but enough to label C1's window and
    // show an fps counter. `scale` multiplies the 8px cell.
    void debug_text(float x, float y, std::string_view s, Color c, float scale = 1.0f);

    // Width in pixels the debug font would occupy, for right-aligning.
    static float debug_text_width(std::string_view s, float scale = 1.0f);

    // The current drawable size in pixels.
    void output_size(int* w, int* h) const;

    SDL_Renderer* raw() const { return r_; }

private:
    SDL_Renderer* r_;
};

}  // namespace studio
