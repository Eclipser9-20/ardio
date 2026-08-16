// The one real OS window Ardio Studio owns.
//
// It is borderless and resizable: borderless because the frame and titlebar are
// ours to draw (see chrome), resizable because a hit-test hands the edges back
// to the window manager so the user still drags to resize a window that has no
// system border. The renderer is created on a GPU backend -- Metal, D3D12, or
// Vulkan -- chosen from the theme or left to SDL.
//
// This is the only file that knows SDL owns a window at all; the rest of the
// app draws through Gfx and reacts to a small set of translated events.
#pragma once
#include <string>

#include "studio/gfx/gfx.h"
#include "studio/theming/theme.h"

struct SDL_Window;
struct SDL_Renderer;

namespace studio {

// Where a point falls on the chrome, so the window can tell the manager to drag
// or resize instead of passing the click to the app. Recomputed each frame from
// the theme and the current size and shared with the hit-test callback.
struct ChromeLayout {
    float titlebar_height = 34.0f;
    float resize_margin = 6.0f;
    // Window-control hit boxes, in window points. A point inside one is a normal
    // click, not a titlebar drag, so the control works.
    Rect close_btn, min_btn;
    // Extra interactive regions the app places inside the titlebar (an editor
    // toolbar's buttons, a command bar). These are exempt from dragging too, so
    // toolbar controls can live in the same bar as the window controls. Refilled
    // each frame by whatever draws in the titlebar.
    Rect exempt[8];
    int exempt_count = 0;
};

class Window {
public:
    // Creates the window and its renderer. `title` seeds the titlebar text.
    // Returns false and leaves an error in error() if SDL could not open a
    // window or a GPU renderer.
    bool open(const std::string& title, int w, int h, const Theme& theme);
    void close();
    ~Window();

    SDL_Renderer* renderer() const { return renderer_; }
    SDL_Window* handle() const { return window_; }

    // Size in points (layout space) and in pixels (drawing space); equal until
    // high-DPI is turned on, kept separate so callers say which they mean.
    void size_points(int* w, int* h) const;

    bool focused() const { return focused_; }
    void set_focused(bool f) { focused_ = f; }

    // Device pixels per point (2.0 on a Retina display). The UI is laid out and
    // drawn in points -- the renderer scales to pixels -- but text is rasterized
    // at point*dpr so glyphs land on the physical pixel grid and stay crisp.
    float dpr() const { return dpr_; }

    // The layout the hit-test reads. The chrome updates it each frame; the
    // window installed a callback pointing here at open().
    ChromeLayout& layout() { return layout_; }

    const std::string& error() const { return error_; }

private:
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    ChromeLayout layout_;
    bool focused_ = true;
    float dpr_ = 1.0f;
    std::string error_;
};

}  // namespace studio
