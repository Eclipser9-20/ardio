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
// VS Code "Dark Modern" palette, a notch darker overall: flat slate, muted, no
// neon. Accent is used sparingly (buttons, active selection), never as a glow.
struct Palette {
    Color bg      = Color::hex(0x141414);  // editor background (darker)
    Color bg2     = Color::hex(0x141414);  // flat -- no gradient
    Color surface = Color::hex(0x1a1a1a);  // side panels, cards, lists
    Color raised  = Color::hex(0x242424);  // hovered list row / card
    Color border   = Color::hex(0x2b2b2b);  // panel dividers, window edge
    Color border_focus = Color::hex(0x3a3a3a);  // window edge when focused
    Color fg      = Color::hex(0xcccccc);  // primary text
    Color muted   = Color::hex(0x7d7d7d);  // secondary text, disabled
    Color accent  = Color::hex(0x0e639c);  // primary accent (button blue)
    Color accent2 = Color::hex(0x0078d4);  // active/focus accent (brighter blue)
    Color selection = Color::hex(0x04395e); // active list selection
    Color good    = Color::hex(0x89d185);  // running, success
    Color warn    = Color::hex(0xcca700);  // paused, caution
    Color danger  = Color::hex(0xf14c4c);  // error, stop
    Color shadow  = Color::hex(0x000000);  // drop-shadow base color
};

// The window frame and titlebar, straight from the look Splinner established:
// a rounded outline that brightens on focus, and an opaque titlebar strip that
// stands in for the system one. The window controls are our own -- subtle
// monochrome glyphs on the right, not colored dots; this is an IDE, not a
// browser toy, and nothing here is meant to read as a system traffic light.
struct Chrome {
    float corner_radius   = 10.0f;  // the window's rounded corners
    float border_width    = 1.5f;   // outline stroke, unfocused
    float border_focused  = 2.0f;   // outline stroke, focused
    float border_alpha    = 0.45f;  // outline opacity, unfocused
    float border_focused_alpha = 0.90f;
    float titlebar_height = 34.0f;
    float titlebar_alpha  = 1.0f;   // strip is fully opaque; it is the chrome
    float titlebar_radius = 8.0f;   // top corners of the strip only
    float control_box     = 34.0f;  // hit box of a window control (square)
    float control_glyph   = 10.0f;  // drawn size of the — / ✕ inside it

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
    float ui_size    = 16.0f;
    float title_size = 15.0f;
    float mono_size  = 15.0f;
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

// Syntax-highlight colors for the editor, VS Code Dark+ by default. Like the
// rest of the theme these are data, so a user can retint code to taste.
struct Syntax {
    Color deflt    = Color::hex(0xcccccc);  // plain text
    Color keyword  = Color::hex(0x569cd6);  // const, class, static...
    Color control  = Color::hex(0xc586c0);  // if, for, return...
    Color type     = Color::hex(0x4ec9b0);  // int, void, uint8_t...
    Color str       = Color::hex(0xce9178);  // "strings"
    Color number   = Color::hex(0xb5cea8);  // 42, 0xFF, HIGH
    Color comment  = Color::hex(0x6a9955);  // // and /* */
    Color preproc  = Color::hex(0xc586c0);  // #include, #define
    Color function = Color::hex(0xdcdcaa);  // name(
};

// The renderer's one real knob: which GPU backend to ask SDL for. This is not a
// performance dial the user fiddles with -- the app draws only when something
// changes and is idle otherwise -- it just names the platform API. Empty lets
// SDL pick (Metal on macOS, D3D12/Vulkan on Windows/Linux).
struct Render {
    std::string backend = "";
};

struct Theme {
    Palette   palette;
    Chrome    chrome;
    Fonts     fonts;
    BoardView board;
    Render    render;
    Syntax    syntax;

    // The built-in default (TokyoNight Night). Equivalent to a default-
    // constructed Theme; named so intent reads clearly at call sites and so a
    // "reset appearance" action has something to assign.
    static Theme defaults() { return Theme{}; }
};

}  // namespace studio
