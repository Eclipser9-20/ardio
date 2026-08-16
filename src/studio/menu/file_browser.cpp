#include "studio/menu/file_browser.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>

#include "studio/fonts/font.h"

namespace fs = std::filesystem;

namespace studio {

void FileBrowser::open(Mode m) {
    mode_ = m;
    active_ = true;
    selected_ = -1;
    scroll_ = 0;
    want_confirm_ = want_cancel_ = false;
    if (m == Mode::New) name_ = "Untitled";
    const char* home = std::getenv("HOME");
    cwd_ = home ? home : "/";
    scan();
}

void FileBrowser::scan() {
    entries_.clear();
    std::error_code ec;
    for (auto& e : fs::directory_iterator(cwd_, ec)) {
        if (!e.is_directory(ec)) continue;
        std::string name = e.path().filename().string();
        if (name.empty() || name[0] == '.') continue;
        Entry en;
        en.name = name;
        en.path = e.path().string();
        en.is_proj = e.path().extension() == ".ardioproj";
        entries_.push_back(std::move(en));
    }
    std::sort(entries_.begin(), entries_.end(), [](const Entry& a, const Entry& b) {
        std::string la = a.name, lb = b.name;
        std::transform(la.begin(), la.end(), la.begin(), ::tolower);
        std::transform(lb.begin(), lb.end(), lb.begin(), ::tolower);
        return la < lb;
    });
}

bool FileBrowser::handle(const SDL_Event& e) {
    if (!active_) return false;
    if (e.type == SDL_EVENT_TEXT_INPUT) {
        if (mode_ == Mode::New)
            for (const char* c = e.text.text; *c; ++c)
                if (*c != '/' && *c != ':') name_ += *c;
        return true;
    }
    if (e.type == SDL_EVENT_KEY_DOWN) {
        if (e.key.key == SDLK_BACKSPACE) {
            if (mode_ == Mode::New && !name_.empty()) name_.pop_back();
            return true;
        }
        if (e.key.key == SDLK_RETURN || e.key.key == SDLK_KP_ENTER) {
            want_confirm_ = true;
            return true;
        }
        if (e.key.key == SDLK_ESCAPE) {
            want_cancel_ = true;
            return true;
        }
    }
    if (e.type == SDL_EVENT_MOUSE_WHEEL && list_rect_.contains(e.wheel.mouse_x, e.wheel.mouse_y)) {
        scroll_ -= e.wheel.y * 48.0f;
        if (scroll_ > max_scroll_) scroll_ = max_scroll_;
        if (scroll_ < 0) scroll_ = 0;
        return true;
    }
    return false;
}

FileBrowser::Result FileBrowser::draw(Ui& ui, Rect screen) {
    const Theme& t = ui.theme();
    const Palette& p = t.palette;
    Font& f = ui.font();
    Gfx& g = ui.gfx();
    Result res;

    // Dim the background.
    g.fill_rect(screen, Color{0, 0, 0, 150});

    // Centered panel.
    float pw = 680, ph = 500;
    Rect panel{screen.x + (screen.w - pw) * 0.5f, screen.y + (screen.h - ph) * 0.5f, pw, ph};
    g.shadow(panel, 12, 20, p.shadow.with_alpha(0.6f));
    g.fill_rounded(panel, 12, p.surface);
    g.stroke_rounded(panel, 12, 1, p.border);

    float pad = 22;
    float x = panel.x + pad;
    float y = panel.y + pad;
    float w = panel.w - pad * 2;

    // Title.
    f.draw(g, x, y, mode_ == Mode::New ? "New Project" : "Open Project", t.fonts.ui_size * 1.4f,
           p.fg);
    y += f.line_height(t.fonts.ui_size * 1.4f) + 12;

    // Path bar: an "up" button and the current directory.
    Rect up{x, y, 34, 28};
    if (ui.button(up, "\xE2\x86\x91")) {  // up arrow
        fs::path parent = fs::path(cwd_).parent_path();
        if (!parent.empty()) {
            cwd_ = parent.string();
            selected_ = -1;
            scan();
        }
    }
    f.draw(g, x + 44, y + 5, cwd_, t.fonts.title_size, p.muted);
    y += 40;

    // Bottom-anchored layout: buttons at the very bottom, the name field (New)
    // just above them, and the directory list fills everything in between.
    const float btn_h = 34, field_h = 30, vgap = 14;
    float buttons_y = panel.bottom() - pad - btn_h;
    float field_y = buttons_y - vgap - field_h;
    float list_bottom = (mode_ == Mode::New ? field_y : buttons_y) - vgap;

    // Directory list.
    float list_h = list_bottom - y;
    Rect list{x, y, w, list_h};
    g.fill_rounded(list, 8, p.bg);
    g.stroke_rounded(list, 8, 1, p.border);
    float rowh = f.line_height(t.fonts.ui_size) + 10;
    // Remember geometry for wheel scrolling, and clamp.
    list_rect_ = list;
    max_scroll_ = entries_.size() * rowh + 12 - list_h;
    if (max_scroll_ < 0) max_scroll_ = 0;
    if (scroll_ > max_scroll_) scroll_ = max_scroll_;
    g.set_clip(list.inset(1));
    float ly = list.y + 6 - scroll_;
    for (size_t i = 0; i < entries_.size(); ++i) {
        const Entry& en = entries_[i];
        Rect r{list.x + 4, ly, list.w - 8, rowh};
        bool sel = static_cast<int>(i) == selected_;
        if (ly + rowh > list.y && ly < list.bottom()) {
            float ht = ui.hot_t(r);
            if (sel)
                g.fill_rounded(r, 6, p.selection);
            else if (ht > 0.002f)
                g.fill_rounded(r, 6, p.raised.with_alpha(ht));
            const char* icon = en.is_proj ? "\xEF\x8B\x9B" : "\xEF\x81\xBB";  // chip / folder
            f.draw(g, r.x + 10, r.y + 5, icon, t.fonts.ui_size, en.is_proj ? p.accent2 : p.muted);
            f.draw(g, r.x + 34, r.y + 5, en.name, t.fonts.ui_size, p.fg);
        }
        if (ui.clicked(r)) {
            if (mode_ == Mode::Open && en.is_proj) {
                selected_ = static_cast<int>(i);
            } else {
                cwd_ = en.path;  // navigate in
                selected_ = -1;
                scan();
            }
        }
        ly += rowh;
    }
    g.clear_clip();

    // Name field (New), sitting just above the buttons -- no overlap.
    if (mode_ == Mode::New) {
        f.draw(g, x, field_y + 7, "Name", t.fonts.title_size, p.muted);
        Rect field{x + 56, field_y, w - 56, field_h};
        g.fill_rounded(field, 6, p.bg);
        g.stroke_rounded(field, 6, 1, p.accent2);
        f.draw(g, field.x + 10, field.y + 6, name_ + "\xE2\x96\x8F", t.fonts.ui_size, p.fg);
    }

    // Cancel / confirm, anchored to the bottom-right.
    float bw = 110;
    Rect confirm{panel.right() - pad - bw, buttons_y, bw, btn_h};
    Rect cancel{confirm.x - bw - 12, buttons_y, bw, btn_h};
    if (ui.button(cancel, "Cancel")) want_cancel_ = true;
    bool can_confirm = mode_ == Mode::New ? !name_.empty() : selected_ >= 0;
    if (ui.button(confirm, mode_ == Mode::New ? "Create" : "Open", true) && can_confirm)
        want_confirm_ = true;

    // Resolve.
    if (want_cancel_) {
        res.done = true;
        res.ok = false;
    } else if (want_confirm_ && can_confirm) {
        res.done = true;
        res.ok = true;
        res.mode = mode_;
        if (mode_ == Mode::New) {
            res.dir = cwd_;
            res.name = name_;
        } else {
            res.dir = entries_[selected_].path;
        }
    }
    want_confirm_ = want_cancel_ = false;
    return res;
}

}  // namespace studio
