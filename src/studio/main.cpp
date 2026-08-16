// Ardio Studio -- entry point and the main loop.
//
// The loop is event-driven, not a game loop: it blocks in SDL_WaitEvent and
// draws a frame only after something actually happens -- a click, a move, a
// resize, focus changing. Idle, it uses no CPU at all. When it does draw, the
// present is vsynced to the real display refresh, so a burst of redraws during
// a drag is paced to the panel (120Hz here) without any framerate machinery.
//
// For this first cut the content area is a placeholder; the menu, editor and
// cockpit hang off this same loop as they land.
#include <SDL3/SDL.h>
// We keep our own main(); SDL_MAIN_HANDLED stops SDL_main.h from redefining it,
// while still giving us SDL_SetMainReady() to call before init on Windows.
#define SDL_MAIN_HANDLED
#include <SDL3/SDL_main.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "ardio/board.h"
#include "studio/backend/jobs.h"
#include "studio/backend/project.h"
#include "studio/core/easing.h"
#include "studio/editor/editor.h"
#include "studio/editor/file_tree.h"
#include "studio/fonts/font.h"
#include "studio/menu/file_browser.h"
#include "studio/gfx/gfx.h"
#include "studio/menu/main_menu.h"
#include "studio/shell/chrome.h"
#include "studio/shell/window.h"
#include "studio/theming/settings_view.h"
#include "studio/theming/theme.h"
#include "studio/ui/ui.h"

using namespace studio;

namespace {

// Locate the bundled fonts. Checked in order: next to the executable, under
// ../share/ardio/fonts (a normal install), then the dev source tree.
std::string find_font(const std::string& file) {
    if (const char* base = SDL_GetBasePath()) {
        std::string b(base);
        for (const std::string& p :
             {b + file, b + "fonts/" + file, b + "../share/ardio/fonts/" + file})
            if (SDL_GetPathInfo(p.c_str(), nullptr)) return p;
    }
#ifdef ARDIO_FONTS_DIR
    std::string dev = std::string(ARDIO_FONTS_DIR) + "/" + file;
    if (SDL_GetPathInfo(dev.c_str(), nullptr)) return dev;
#endif
    return file;
}

std::string read_text_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Recent projects, one path per line under ~/.ardio.
std::string recents_path() {
    const char* home = std::getenv("HOME");
    return home ? std::string(home) + "/.ardio/studio-recents" : "";
}
std::vector<std::string> load_recents() {
    std::vector<std::string> v;
    std::ifstream f(recents_path());
    std::string line;
    while (std::getline(f, line))
        if (!line.empty()) v.push_back(line);
    return v;
}
void add_recent(std::vector<std::string>& v, const std::string& path) {
    v.erase(std::remove(v.begin(), v.end(), path), v.end());
    v.insert(v.begin(), path);
    if (v.size() > 8) v.resize(8);
    std::string rp = recents_path();
    if (rp.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(rp).parent_path(), ec);
    std::ofstream f(rp);
    for (auto& p : v) f << p << "\n";
}

}  // namespace

int main(int, char**) {
    // We do not use SDL_main, so on Windows SDL must be told main is ready
    // before init. A no-op elsewhere.
    SDL_SetMainReady();

    Theme theme = Theme::defaults();

    Window win;
    if (!win.open("Ardio Studio", 1100, 720, theme)) {
        std::fprintf(stderr, "ardio-studio: %s\n", win.error().c_str());
        return 1;
    }
    Gfx g(win.renderer());

    Font font;
    if (!font.load(win.renderer(), find_font("JetBrainsMonoNerdFontMono-Regular.ttf"), win.dpr())) {
        std::fprintf(stderr, "ardio-studio: could not load the UI font\n");
        return 1;
    }

    // The worker pool: heavy work (build, flash, emulate) runs here, never on
    // the UI thread. Completions wake the loop via jobs.wake_event().
    Jobs jobs;
    std::fprintf(stderr, "ardio-studio: GPU backend = %s, %u worker threads\n",
                 SDL_GetRendererName(win.renderer()), jobs.worker_count());

    bool running = true;
    float mouse_x = 0, mouse_y = 0;
    bool clicked = false;
    bool mouse_down = false;
    MenuModel menu;
    SettingsModel settings;
    std::vector<Editor> tabs;
    int active_tab = -1;
    FileTree file_tree;
    Project current_project;
    bool has_project = false;
    bool sidebar_visible = true;
    FileBrowser browser;
    std::string pending_board;  // board id for a project being created
    std::vector<std::string> recents = load_recents();
    std::string recent_out;
    Anim anim;  // retained animation state for the immediate-mode UI
    enum class Screen { Menu, Settings, Emulator, Editor };
    Screen screen = Screen::Menu;
    // Screen-transition state: a horizontal slide from `prev` to `screen` as
    // `trans` eases 0->1, in direction `dir` (+1 forward, -1 back).
    Screen prev_screen = Screen::Menu;
    float trans = 1.0f, trans_dir = 1.0f, frame_dt = 1.0f / 60.0f;
    bool more = true;
    auto go = [&](Screen target, float dir) {
        if (target == screen) return;
        // The editor wants keyboard text events; start/stop them at its edges.
        if (target == Screen::Editor) SDL_StartTextInput(win.handle());
        else if (screen == Screen::Editor) SDL_StopTextInput(win.handle());
        prev_screen = screen;
        screen = target;
        trans = 0.0f;
        trans_dir = dir;
    };

    // --- Tabs ---
    auto active_editor = [&]() -> Editor* {
        return (active_tab >= 0 && active_tab < static_cast<int>(tabs.size())) ? &tabs[active_tab]
                                                                               : nullptr;
    };
    auto open_file = [&](const std::string& path) {
        // The manifest is wizard-managed and never opened as a document.
        if (std::filesystem::path(path).filename() == "project.ardiomanif") return;
        for (size_t i = 0; i < tabs.size(); ++i)
            if (tabs[i].path() == path) {
                active_tab = static_cast<int>(i);
                return;
            }
        Editor e;
        e.set_text(read_text_file(path));
        e.mark_saved();
        e.set_path(path);
        tabs.push_back(std::move(e));
        active_tab = static_cast<int>(tabs.size()) - 1;
        SDL_StartTextInput(win.handle());
    };
    auto close_tab = [&](int i) {
        if (i < 0 || i >= static_cast<int>(tabs.size())) return;
        tabs.erase(tabs.begin() + i);
        if (tabs.empty()) active_tab = -1;
        else if (active_tab >= static_cast<int>(tabs.size())) active_tab = static_cast<int>(tabs.size()) - 1;
    };

    // Load an opened/created project into the editor + explorer and slide there.
    auto load_into_editor = [&](const Project& p) {
        current_project = p;
        has_project = true;
        tabs.clear();
        active_tab = -1;
        file_tree.set_root(p.root);
        std::string main_file = p.root + "/" + p.manifest.main;
        file_tree.select(main_file);
        open_file(main_file);
        add_recent(recents, p.root);
        go(Screen::Editor, 1.0f);
    };

    // Act on a resolved custom file browser: create or open a project, load it.
    auto process_browser = [&](const FileBrowser::Result& r) {
        browser.close();
        SDL_StopTextInput(win.handle());
        if (!r.ok) return;
        Project p;
        std::string err;
        bool ok;
        if (r.mode == FileBrowser::Mode::New)
            ok = create_project(r.dir, r.name, pending_board, p, err);
        else
            ok = open_project(r.dir, p, err);
        if (ok) load_into_editor(p);
        else std::fprintf(stderr, "ardio-studio: %s\n", err.c_str());
    };

    auto draw = [&]() {
        int W = 0, H = 0;
        win.size_points(&W, &H);
        Rect bounds{0, 0, static_cast<float>(W), static_cast<float>(H)};

        // Clear to fully transparent, then paint the slate body as a rounded
        // rect with a subtle vertical gradient. The corners outside that rect
        // stay transparent, so the window's shape itself is rounded rather than
        // a round stroke in square corners.
        g.clear(Color{0, 0, 0, 0});
        g.fill_rounded_gradient(bounds, theme.chrome.corner_radius, theme.palette.bg,
                                theme.palette.bg2);

        // The landing screen fills the area below the titlebar.
        Rect content = {0, theme.chrome.titlebar_height, bounds.w,
                        bounds.h - theme.chrome.titlebar_height};
        // Advance the screen slide (~200ms), then draw either the single
        // current screen, or -- while transitioning -- the outgoing and
        // incoming screens offset horizontally and eased.
        if (trans < 1.0f) {
            trans += frame_dt * 5.0f;
            if (trans > 1.0f) trans = 1.0f;
        }
        bool transitioning = trans < 1.0f;

        Input live{mouse_x, mouse_y, clicked, mouse_down};
        Input dead{-1e4f, -1e4f, false, false};  // offscreen, no clicks
        Ui ui_live(g, font, theme, live, anim);
        Ui ui_dead(g, font, theme, dead, anim);

        auto render_one = [&](Screen s, Rect area, Ui& ui, bool interactive) {
            switch (s) {
                case Screen::Menu: {
                    MenuAction a = main_menu(ui, area, menu, recents, recent_out);
                    if (!interactive) break;
                    if (a == MenuAction::Settings) {
                        go(Screen::Settings, 1.0f);
                    } else if (a == MenuAction::Emulator) {
                        go(Screen::Emulator, 1.0f);
                    } else if (a == MenuAction::OpenRecent) {
                        Project p;
                        std::string err;
                        if (open_project(recent_out, p, err)) load_into_editor(p);
                        else std::fprintf(stderr, "ardio-studio: %s\n", err.c_str());
                    } else if (a == MenuAction::NewSketch) {
                        // Our own browser picks the location + name; the board
                        // comes from the picker.
                        const auto& boards = ardio::board_database();
                        int bi = (menu.board >= 0 && menu.board < (int)boards.size()) ? menu.board : 0;
                        pending_board = boards[bi].id;
                        browser.open(FileBrowser::Mode::New);
                        SDL_StartTextInput(win.handle());
                    } else if (a == MenuAction::Open) {
                        browser.open(FileBrowser::Mode::Open);
                        SDL_StartTextInput(win.handle());
                    }
                    break;
                }
                case Screen::Settings:
                    if (settings_view(ui, area, theme, settings) && interactive)
                        go(Screen::Menu, -1.0f);
                    break;
                case Screen::Emulator:
                    ui.text(area.x + 34, area.y + 40, "Emulator cockpit \xE2\x80\x94 coming next.",
                            theme.fonts.ui_size, theme.palette.muted);
                    break;
                case Screen::Editor: {
                    const float topbar = 42, tabh = 34, sth = 24, pad = 8;
                    const Palette& p = theme.palette;
                    Gfx& g2 = ui.gfx();
                    Font& f2 = ui.font();
                    Editor* aed = active_editor();

                    // --- Top bar: explorer toggle (left), command bar (center), Build (right) ---
                    Rect exbtn{area.x + 12, area.y + 8, 32, 26};
                    if (ui.button(exbtn, "\xEF\x85\x9C") && interactive) sidebar_visible = !sidebar_visible;

                    float cw = 380;
                    Rect cmd{area.x + (area.w - cw) * 0.5f, area.y + 7, cw, 28};
                    g2.fill_rounded(cmd, 6, p.surface.mix(p.raised, ui.hot_t(cmd)));
                    g2.stroke_rounded(cmd, 6, 1, p.border);
                    f2.draw(g2, cmd.x + 12, cmd.y + 6, "\xEF\x80\x82  Search or run a command\xE2\x80\xA6",
                            theme.fonts.title_size, p.muted);

                    // Build, leaving room for the window controls on the far right.
                    Rect build{area.right() - 96 - 108, area.y + 7, 108, 28};
                    if (ui.button(build, "\xEF\x81\x8B  Build", true) && interactive) { /* soon */ }

                    // --- Body: editor panel + explorer, rounded, between top and status bars ---
                    Rect region{area.x + pad, area.y + topbar, area.w - pad * 2,
                                area.h - topbar - sth - pad};
                    Rect edR = region;
                    if (sidebar_visible) {
                        const float sbw = 248, gap = 8;
                        Rect sb;
                        if (settings.explorer_on_right) {
                            sb = {region.right() - sbw, region.y, sbw, region.h};
                            edR = {region.x, region.y, region.w - sbw - gap, region.h};
                        } else {
                            sb = {region.x, region.y, sbw, region.h};
                            edR = {region.x + sbw + gap, region.y, region.w - sbw - gap, region.h};
                        }
                        g2.fill_rounded(sb, 10, p.surface);
                        std::string opened = file_tree.draw(ui, sb);
                        if (interactive && !opened.empty()) open_file(opened);
                    }

                    g2.fill_rounded(edR, 10, p.bg);

                    // Tab bar (closeable tabs).
                    int close_req = -1;
                    float txp = edR.x + 8;
                    for (size_t i = 0; i < tabs.size(); ++i) {
                        std::string nm = std::filesystem::path(tabs[i].path()).filename().string();
                        bool act = static_cast<int>(i) == active_tab;
                        std::string label = tabs[i].dirty() ? nm + " \xE2\x97\x8F" : nm;
                        float w = f2.measure(label, theme.fonts.ui_size) + 46;
                        Rect tabr{txp, edR.y + 5, w, tabh - 6};
                        g2.fill_rounded(tabr, 6, act ? p.raised : p.surface.mix(p.raised, ui.hot_t(tabr) * 0.5f));
                        if (act)
                            g2.fill_rounded({tabr.x + 6, tabr.bottom() - 2, tabr.w - 12, 2}, 1, p.accent2);
                        f2.draw(g2, tabr.x + 12,
                                tabr.y + (tabr.h - f2.line_height(theme.fonts.ui_size)) * 0.5f, label,
                                theme.fonts.ui_size, act ? (tabs[i].dirty() ? p.warn : p.fg) : p.muted);
                        Rect xr{tabr.right() - 22, tabr.y + (tabr.h - 16) * 0.5f, 16, 16};
                        if (ui.hover(xr)) g2.fill_rounded(xr, 4, p.raised);
                        f2.draw_centered(g2, xr.x, xr.y + (16 - f2.line_height(theme.fonts.title_size)) * 0.5f,
                                         xr.w, "\xC3\x97", theme.fonts.title_size, ui.hover(xr) ? p.fg : p.muted);
                        if (interactive && ui.clicked(xr)) close_req = static_cast<int>(i);
                        else if (interactive && ui.clicked(tabr)) active_tab = static_cast<int>(i);
                        txp += w + 4;
                    }

                    Rect ec{edR.x + 2, edR.y + tabh, edR.w - 4, edR.h - tabh - 2};
                    if (aed) aed->draw(g2, f2, theme, ec);
                    else f2.draw(g2, ec.x + 20, ec.y + 20, "No file open.", theme.fonts.ui_size, p.muted);

                    if (close_req >= 0) {
                        close_tab(close_req);
                        aed = active_editor();
                    }

                    // --- Status bar ---
                    Rect status{area.x, area.bottom() - sth, area.w, sth};
                    g2.fill_rect(status, p.surface);
                    g2.fill_rect({status.x, status.y, status.w, 1}, p.border);
                    float sy = status.y + (sth - f2.line_height(theme.fonts.title_size)) * 0.5f;
                    if (has_project)
                        f2.draw(g2, status.x + 12, sy, "\xEF\x8B\x9B " + current_project.manifest.board,
                                theme.fonts.title_size, p.muted);
                    if (aed) {
                        char pos[64];
                        std::snprintf(pos, sizeof pos, "Ln %d, Col %d  \xE2\x80\xA2  Arduino",
                                      aed->cursor_row(), aed->cursor_col());
                        f2.draw(g2, status.right() - f2.measure(pos, theme.fonts.title_size) - 12, sy,
                                pos, theme.fonts.title_size, p.muted);
                    }
                    break;
                }
            }
        };

        if (transitioning) {
            float te = ease::in_out_cubic(trans);
            float w = bounds.w;
            Rect pa = content;
            pa.x = content.x - te * w * trans_dir;
            Rect ca = content;
            ca.x = content.x + (1.0f - te) * w * trans_dir;
            render_one(prev_screen, pa, ui_dead, false);
            render_one(screen, ca, ui_dead, false);
        } else {
            bool modal = browser.active();
            render_one(screen, content, modal ? ui_dead : ui_live, !modal);
        }
        more = anim.active() || transitioning;

        bool as_back = (screen == Screen::Editor || screen == Screen::Emulator);
        ChromeAction act = draw_chrome(g, font, theme, bounds, "Ardio Studio", win.focused(),
                                       as_back, mouse_x, mouse_y, clicked, win.layout());
        if (act == ChromeAction::Close) {
            // In the editor or emulator, the close control returns to the menu;
            // at the menu (or settings) it quits.
            if (screen == Screen::Editor || screen == Screen::Emulator) go(Screen::Menu, -1.0f);
            else running = false;
        } else if (act == ChromeAction::Minimize) {
            SDL_MinimizeWindow(win.handle());
        }

        // The custom file browser draws on top of everything, modally.
        if (browser.active()) {
            FileBrowser::Result r = browser.draw(ui_live, bounds);
            if (r.done) process_browser(r);
        }

        SDL_RenderPresent(win.renderer());
    };

    auto handle = [&](const SDL_Event& e) {
        // The modal file browser takes input first when open.
        if (browser.active() && browser.handle(e)) return;
        // The active editor tab gets first refusal on events while it is the
        // screen; it returns false for anything it does not use (e.g. Cmd-Q) so
        // those fall through to the window's own handling below.
        if (screen == Screen::Editor)
            if (Editor* aed = active_editor(); aed && aed->handle(e, font, theme)) return;
        switch (e.type) {
            case SDL_EVENT_QUIT:
                running = false;
                break;
            case SDL_EVENT_WINDOW_FOCUS_GAINED:
                win.set_focused(true);
                break;
            case SDL_EVENT_WINDOW_FOCUS_LOST:
                win.set_focused(false);
                break;
            case SDL_EVENT_MOUSE_MOTION:
                mouse_x = e.motion.x;
                mouse_y = e.motion.y;
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
                if (e.button.button == SDL_BUTTON_LEFT) {
                    clicked = true;
                    mouse_down = true;
                }
                break;
            case SDL_EVENT_MOUSE_BUTTON_UP:
                if (e.button.button == SDL_BUTTON_LEFT) mouse_down = false;
                break;
            case SDL_EVENT_KEY_DOWN:
                if (e.key.key == SDLK_Q && (e.key.mod & (SDL_KMOD_GUI | SDL_KMOD_CTRL))) {
                    running = false;
                } else if (e.key.key == SDLK_S && (e.key.mod & (SDL_KMOD_GUI | SDL_KMOD_CTRL)) &&
                           screen == Screen::Editor) {
                    if (Editor* aed = active_editor(); aed && !aed->path().empty()) {
                        std::ofstream(aed->path(), std::ios::binary) << aed->text();
                        aed->mark_saved();
                    }
                }
                break;
            default:
                // A finished background job wakes us to run its UI-thread
                // completion callback here, on the thread that owns the screen.
                if (e.type == jobs.wake_event()) jobs.drain();
                break;
        }
    };

    Uint64 prev = SDL_GetTicksNS();
    anim.begin_frame(1.0f / 60.0f);
    draw();  // first frame

    while (running) {
        SDL_Event e;
        clicked = false;
        if (more) {
            // Something is still animating (hover, toggle, or a screen slide):
            // don't block. Drain pending events and render the next frame;
            // vsync paces it to the display.
            while (SDL_PollEvent(&e)) handle(e);
        } else {
            // Settled: sleep until the next event, at zero CPU.
            if (!SDL_WaitEvent(&e)) break;
            handle(e);
            while (SDL_PollEvent(&e)) handle(e);
        }
        Uint64 now = SDL_GetTicksNS();
        float dt = static_cast<float>(now - prev) / 1e9f;
        prev = now;
        if (dt > 0.1f) dt = 0.1f;  // clamp after a long idle so nothing jumps
        frame_dt = dt;
        anim.begin_frame(dt);
        draw();
    }

    win.close();
    SDL_Quit();
    return 0;
}
