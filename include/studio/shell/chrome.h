// Drawing the window's own frame: the rounded outline, the titlebar strip that
// replaces the system one, the three control dots, and the title text. This is
// the Splinner look made first-party -- one window we own, so it is drawn in
// the render loop rather than floated as separate overlay windows.
//
// draw() also fills in the ChromeLayout (control-dot rects, titlebar height) the
// window's hit-test reads, so the clickable regions and the drawn ones are
// computed in exactly one place.
#pragma once
#include <string>

#include "studio/fonts/font.h"
#include "studio/gfx/gfx.h"
#include "studio/shell/window.h"
#include "studio/theming/theme.h"

namespace studio {

// What the user did to the chrome this frame, returned so the app loop can act
// on it without the chrome knowing what a window "closing" means.
enum class ChromeAction { None, Close, Minimize, Maximize };

// Draws the frame into `bounds` (the whole window in points), fills `layout`
// for the hit-test, and returns any control the pointer pressed this frame.
// `mouse_x/y` and `mouse_down` describe the pointer; `focused` picks the accent.
// `font` draws the title.
// When `as_back` is set (in the editor/emulator), the close control is drawn as
// a back arrow instead of an ✕ -- it returns to the menu rather than quitting.
ChromeAction draw_chrome(Gfx& g, Font& font, const Theme& theme, Rect bounds,
                         const std::string& title, bool focused, bool as_back, float mouse_x,
                         float mouse_y, bool clicked, ChromeLayout& layout);

}  // namespace studio
