// The settings screen: a live editor for the Theme. Every control mutates the
// same Theme the rest of the app draws from, so a change to a color or a size
// is visible the next frame across the whole UI -- which is the whole point of
// keeping appearance as data. It lives with theming/ because it edits nothing
// else.
#pragma once
#include "studio/theming/theme.h"
#include "studio/ui/ui.h"

namespace studio {

// Settings state that is not part of the Theme: which sections are expanded,
// and the LSP/Ardio preferences. Owned by the caller across frames. (The Theme
// holds the appearance values themselves.)
struct SettingsModel {
    bool theme_open = true;
    bool lsp_open = false;
    bool ardio_open = false;

    // Layout
    bool explorer_on_right = true;  // file explorer side in the editor

    // LSP (language server) preferences.
    bool lsp_enabled = true;
    bool lsp_completion = true;
    bool lsp_diagnostics = true;

    // Ardio (Arduino) preferences.
    bool ardio_autodetect = true;
    bool ardio_verbose = false;
    float ardio_baud = 115200;
};

// Draws the settings screen into `area`, editing `theme` and `s` in place.
// Returns true the frame the user asks to go back.
bool settings_view(Ui& ui, Rect area, Theme& theme, SettingsModel& s);

}  // namespace studio
