#include "studio/theming/settings_view.h"

#include <cstdio>
#include <functional>

#include "studio/core/easing.h"

namespace studio {
namespace {

// A labeled control row: title on the left, control area on the right. Returns
// the control rect. `x0` is the indent (section bodies are indented).
Rect labeled(Ui& ui, float x0, float right, float y, std::string_view label, float control_w) {
    const Theme& t = ui.theme();
    ui.text(x0, y + 4, label, t.fonts.ui_size, t.palette.fg);
    return Rect{right - control_w, y, control_w, 26};
}

}  // namespace

bool settings_view(Ui& ui, Rect area, Theme& theme, SettingsModel& s) {
    const Palette& p = theme.palette;
    const float pad = 34;
    Rect body{area.x + pad, area.y + pad, area.w - pad * 2, area.h - pad * 2};
    const float ind = body.x + 24;
    const float right = body.x + body.w;
    const float rowh = 42;
    const float sec_h = 34;

    bool back = false;
    if (ui.button({body.x, body.y, 90, 30}, "\xE2\x80\xB9 Back")) back = true;
    ui.text(body.x + 110, body.y + 3, "Settings", theme.fonts.ui_size * 1.4f, p.fg);

    float y = body.y + 60;

    // A slider row that also shows its current value, so the setting is legible
    // rather than a mystery handle. The value is drawn to the right of the track.
    auto slider_row = [&](std::string_view label, float& v, float lo, float hi) {
        Rect c = labeled(ui, ind, right, y, label, 240);
        ui.slider({c.x, c.y, 168, c.h}, v, lo, hi);
        char buf[24];
        std::snprintf(buf, sizeof buf, "%d", static_cast<int>(v + 0.5f));
        ui.text(c.x + 180, c.y + 4, buf, ui.theme().fonts.title_size, ui.theme().palette.fg);
        y += rowh;
    };

    // A collapsible section: draws the header, then reveals its body clipped to
    // an eased height so opening/closing slides rather than snaps. `full_h` is
    // the body's height when fully open.
    auto section = [&](const char* name, bool& open, float full_h,
                       const std::function<void()>& draw_body) {
        Rect hdr{body.x, y, body.w, sec_h};
        // Openness springs toward the boolean; keyed apart from the header's
        // own hover animation so the two don't fight.
        float sf = ui.anim().smooth(anim_id(hdr.x, hdr.y) ^ 0x5EC70000ull, open ? 1.0f : 0.0f, 16.0f);
        ui.section(hdr, name, open);
        y += sec_h + 6;
        float vis = full_h * ease::out_cubic(sf);
        if (vis > 1.0f) {
            float top = y;
            ui.gfx().set_clip({body.x, top, body.w, vis});
            draw_body();  // draws its rows from `top`, advancing y to top+full_h
            ui.gfx().clear_clip();
            y = top + vis;  // sections below slide with the animating height
        }
    };

    // ---- Theme --------------------------------------------------------------
    section("Theme", s.theme_open, 326.0f, [&] {
        slider_row("Window roundness", theme.chrome.corner_radius, 0, 22);
        slider_row("UI text size", theme.fonts.ui_size, 12, 22);
        slider_row("Title text size", theme.fonts.title_size, 11, 20);
        {
            Rect c = labeled(ui, ind, right, y, "Custom titlebar", 52);
            ui.toggle({c.right() - 44, c.y, 44, 24}, theme.chrome.custom_chrome);
        }
        y += rowh;
        {
            Rect c = labeled(ui, ind, right, y, "Explorer on right", 52);
            ui.toggle({c.right() - 44, c.y, 44, 24}, s.explorer_on_right);
        }
        y += rowh;
        ui.text(ind, y, "Accent", theme.fonts.title_size, p.muted);
        y += 26;
        const Color accents[] = {
            Color::hex(0x0078d4), Color::hex(0x0e639c), Color::hex(0x2aa198),
            Color::hex(0x89d185), Color::hex(0xcca700), Color::hex(0xf14c4c),
            Color::hex(0xc586c0), Color::hex(0xff8c00),
        };
        float sw = 28, sg = 12, sx = ind;
        for (const Color& c : accents) {
            bool sel = c.r == p.accent2.r && c.g == p.accent2.g && c.b == p.accent2.b;
            if (ui.swatch({sx, y, sw, sw}, c, sel)) {
                theme.palette.accent = c;
                theme.palette.accent2 = c;
                theme.palette.selection = c.mix(theme.palette.bg, 0.55f);
            }
            sx += sw + sg;
        }
        y += sw + 18;
        if (ui.button({ind, y, 150, 30}, "Reset appearance")) {
            bool custom = theme.chrome.custom_chrome;
            theme = Theme::defaults();
            theme.chrome.custom_chrome = custom;
        }
        y += 44;
    });

    // ---- LSP ----------------------------------------------------------------
    section("LSP (Language Server)", s.lsp_open, 126.0f, [&] {
        auto pref = [&](std::string_view label, bool& v) {
            Rect c = labeled(ui, ind, right, y, label, 52);
            ui.toggle({c.right() - 44, c.y, 44, 24}, v);
            y += rowh;
        };
        pref("Enable language server", s.lsp_enabled);
        pref("Autocompletion", s.lsp_completion);
        pref("Inline diagnostics", s.lsp_diagnostics);
    });

    // ---- Ardio --------------------------------------------------------------
    section("Ardio (Arduino)", s.ardio_open, 126.0f, [&] {
        auto tog = [&](std::string_view label, bool& v) {
            Rect c = labeled(ui, ind, right, y, label, 52);
            ui.toggle({c.right() - 44, c.y, 44, 24}, v);
            y += rowh;
        };
        tog("Auto-detect board & port", s.ardio_autodetect);
        tog("Verbose upload", s.ardio_verbose);
        slider_row("Monitor baud", s.ardio_baud, 9600, 250000);
    });

    return back;
}

}  // namespace studio
