// The whole of Ardio Studio's appearance, as data.
//
// Nothing in the UI hardcodes a color, a radius, or a size: every drawing call
// reads it from a Theme, and a Theme is a plain aggregate the settings screen
// edits field by field and the config layer reads and writes. That is the
// point -- appearance is meant to be pervasively customizable, so the values
// live in one editable place rather than scattered through the renderer as
// constants that only a recompile can change.
//
// The defaults are TokyoNight Night. A user who changes nothing gets that; a
// user who wants anything else changes these and the change reaches every
// widget the same frame.
#pragma once
#include <string>

#include "studio/core/color.h"

namespace studio {

// The named palette. Widgets refer to roles (accent, danger, surface) rather
// than literal colors, so retheming is a matter of repainting these eight
// fields and everything downstream follows.
struct Palette {
    Color bg      = Color::hex(0x1a1b26);  // window base
    Color surface = Color::hex(0x16161e);  // a panel sitting on the base
    Color fg      = Color::hex(0xc0caf5);  // primary text
    Color muted   = Color::hex(0x565f89);  // secondary text, disabled
    Color accent  = Color::hex(0x7aa2f7);  // primary accent (the blue border)
    Color accent2 = Color::hex(0xbb9af7);  // secondary accent (purple)
    Color focus   = Color::hex(0x7dcfff);  // the bright cyan a focused edge takes
    Color good    = Color::hex(0x9ece6a);  // running, success
    Color warn    = Color::hex(0xe0af68);  // paused, caution
    Color danger  = Color::hex(0xf7768e);  // error, stop
};

// The window frame and titlebar, straight from the look Splinner established:
// a rounded outline that brightens on focus, and an opaque titlebar strip that
// stands in for the system one so no platform's native controls ever show.
struct Chrome {
    float corner_radius   = 10.0f;  // the window's rounded corners
    float border_width    = 2.0f;   // outline stroke, unfocused
    float border_focused  = 3.0f;   // outline stroke, focused
    float border_alpha    = 0.55f;  // outline opacity, unfocused
    float border_focused_alpha = 0.95f;
    float titlebar_height = 30.0f;
    float titlebar_alpha  = 0.97f;  // strip is nearly opaque, a touch of depth
    float titlebar_radius = 8.0f;   // top corners of the strip only
    float control_size    = 12.0f;  // diameter of a window-control dot

    // Custom slate chrome, or defer to the OS window frame. A user who prefers
    // their platform's real titlebar flips this and the borderless path and all
    // our drawing of it switch off together.
    bool custom_chrome = true;
};

// Font selection. Two roles: a UI face for chrome and panels, and a mono face
// for code and register/serial dumps. Names resolve through the font loader;
// the mono default is the Nerd build so its glyphs are available for icons.
struct Fonts {
    std::string ui_family   = "JetBrainsMono Nerd Font";
    std::string mono_family = "JetBrainsMono Nerd Font Mono";
    float ui_size    = 13.0f;
    float title_size = 12.0f;
    float mono_size  = 13.0f;
};

// How the emulated board is drawn. Two rendering styles, switchable at runtime,
// plus the glow the live pins get.
enum class BoardStyle {
    HiFi,       // detailed vector board -- the default, the good-looking one
    Stylized,   // abstract instrument panel -- fast, most legible for debugging
};

struct BoardView {
    BoardStyle style = BoardStyle::HiFi;
    float led_glow = 1.0f;  // 0 disables the bloom on lit LEDs, 1 is full
};

// Frame pacing. Uncapped-but-vsynced is the default; a user chasing 120+ turns
// vsync off and sets a cap, or leaves it at 0 for truly uncapped.
struct Render {
    bool vsync = true;
    int  fps_cap = 0;       // 0 = uncapped (only meaningful with vsync off)
    // Preferred GPU backend by name ("metal", "direct3d12", "vulkan", "gpu"),
    // or empty to let SDL choose. Honored at window creation.
    std::string backend = "";
};

struct Theme {
    Palette   palette;
    Chrome    chrome;
    Fonts     fonts;
    BoardView board;
    Render    render;

    // The built-in default (TokyoNight Night). Equivalent to a default-
    // constructed Theme; named so intent reads clearly at call sites and so a
    // "reset appearance" action has something to assign.
    static Theme defaults() { return Theme{}; }
};

}  // namespace studio
