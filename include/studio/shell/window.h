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
    float titlebar_height = 30.0f;
    float resize_margin = 6.0f;
    // Control-dot rects (close, minimize, maximize), in window points. A point
    // inside one of these is a normal click, not a drag, so the button works.
    Rect close_btn, min_btn, max_btn;
};

class Window {
public:
    // Creates the window and its renderer. `title` seeds the titlebar text.
    // Returns false and leaves an error in error() if SDL could not open a
    // window or a GPU renderer.
    bool open(const std::string& title, int w, int h, const Theme& theme);
    void close();
    ~Window();

    // Applies vsync and the backend hints from the theme. Safe to call again
    // when the user changes them in settings; only vsync can change live.
    void apply_render_settings(const Theme& theme);

    SDL_Renderer* renderer() const { return renderer_; }
    SDL_Window* handle() const { return window_; }

    // Size in points (layout space) and in pixels (drawing space); equal until
    // high-DPI is turned on, kept separate so callers say which they mean.
    void size_points(int* w, int* h) const;

    bool focused() const { return focused_; }
    void set_focused(bool f) { focused_ = f; }

    // The layout the hit-test reads. The chrome updates it each frame; the
    // window installed a callback pointing here at open().
    ChromeLayout& layout() { return layout_; }

    const std::string& error() const { return error_; }

private:
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    ChromeLayout layout_;
    bool focused_ = true;
    std::string error_;
};

}  // namespace studio
