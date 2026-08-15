#include "studio/shell/window.h"

#include <SDL3/SDL.h>

namespace studio {
namespace {

// The window manager asks this for every point it needs classified: which edge
// to resize from, whether the titlebar should drag the window, or whether the
// app gets the click. It reads the ChromeLayout the shell keeps up to date, so
// the draggable region and the drawn titlebar are always the same rect.
SDL_HitTestResult SDLCALL hit_test(SDL_Window* w, const SDL_Point* pt, void* data) {
    const ChromeLayout* L = static_cast<const ChromeLayout*>(data);
    int W = 0, H = 0;
    SDL_GetWindowSize(w, &W, &H);
    const float m = L->resize_margin;
    const bool left = pt->x < m, right = pt->x > W - m;
    const bool top = pt->y < m, bottom = pt->y > H - m;

    if (top && left) return SDL_HITTEST_RESIZE_TOPLEFT;
    if (top && right) return SDL_HITTEST_RESIZE_TOPRIGHT;
    if (bottom && left) return SDL_HITTEST_RESIZE_BOTTOMLEFT;
    if (bottom && right) return SDL_HITTEST_RESIZE_BOTTOMRIGHT;
    if (left) return SDL_HITTEST_RESIZE_LEFT;
    if (right) return SDL_HITTEST_RESIZE_RIGHT;
    if (top) return SDL_HITTEST_RESIZE_TOP;
    if (bottom) return SDL_HITTEST_RESIZE_BOTTOM;

    if (pt->y < L->titlebar_height) {
        const float x = static_cast<float>(pt->x), y = static_cast<float>(pt->y);
        // The control dots stay clickable; everything else on the bar drags.
        if (L->close_btn.contains(x, y) || L->min_btn.contains(x, y) ||
            L->max_btn.contains(x, y))
            return SDL_HITTEST_NORMAL;
        return SDL_HITTEST_DRAGGABLE;
    }
    return SDL_HITTEST_NORMAL;
}

}  // namespace

bool Window::open(const std::string& title, int w, int h, const Theme& theme) {
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        error_ = SDL_GetError();
        return false;
    }

    Uint32 flags = SDL_WINDOW_RESIZABLE;
    if (theme.chrome.custom_chrome) flags |= SDL_WINDOW_BORDERLESS;

    window_ = SDL_CreateWindow(title.c_str(), w, h, flags);
    if (!window_) {
        error_ = SDL_GetError();
        return false;
    }
    SDL_SetWindowMinimumSize(window_, 640, 400);

    // A named backend from the theme, or NULL to let SDL pick the best GPU one
    // for the platform (Metal on macOS, D3D12/Vulkan on Windows/Linux).
    const char* backend = theme.render.backend.empty() ? nullptr : theme.render.backend.c_str();
    renderer_ = SDL_CreateRenderer(window_, backend);
    if (!renderer_) {
        error_ = SDL_GetError();
        return false;
    }

    layout_.titlebar_height = theme.chrome.titlebar_height;
    if (theme.chrome.custom_chrome)
        SDL_SetWindowHitTest(window_, hit_test, &layout_);

    apply_render_settings(theme);
    return true;
}

void Window::apply_render_settings(const Theme& theme) {
    if (renderer_) SDL_SetRenderVSync(renderer_, theme.render.vsync ? 1 : SDL_RENDERER_VSYNC_DISABLED);
}

void Window::size_points(int* w, int* h) const { SDL_GetWindowSize(window_, w, h); }

void Window::close() {
    if (renderer_) {
        SDL_DestroyRenderer(renderer_);
        renderer_ = nullptr;
    }
    if (window_) {
        SDL_DestroyWindow(window_);
        window_ = nullptr;
    }
}

Window::~Window() { close(); }

}  // namespace studio
