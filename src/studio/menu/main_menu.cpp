#include "studio/menu/main_menu.h"

#include <filesystem>

#include "ardio/board.h"

namespace studio {

MenuAction main_menu(Ui& ui, Rect area, MenuModel& model, const std::vector<std::string>& recents,
                     std::string& recent_out) {
    const Theme& t = ui.theme();
    const Palette& p = t.palette;
    Font& f = ui.font();
    const auto& boards = ardio::board_database();

    const float pad = 34;
    float x = area.x + pad;
    float y = area.y + pad;

    // Wordmark and tagline.
    float big = t.fonts.ui_size * 2.2f;
    ui.text(x, y, "Ardio Studio", big, p.fg);
    ui.text(x, y + f.line_height(big), "The from-scratch Arduino toolkit.", t.fonts.ui_size,
            p.muted);

    float top = y + f.line_height(big) + f.line_height(t.fonts.ui_size) + 26;

    // Left column: primary actions as cards.
    struct Action {
        uint32_t icon;
        const char* title;
        const char* sub;
        MenuAction act;
    };
    const Action actions[] = {
        {0xf067, "New Project", "Create a .ardioproj", MenuAction::NewSketch},
        {0xf07b, "Open…", "An existing .ardioproj", MenuAction::Open},
        {0xf04b, "Launch Emulator", "Run without hardware", MenuAction::Emulator},
        {0xf013, "Settings", "Appearance and behavior", MenuAction::Settings},
    };

    float col_w = (area.w - pad * 3) * 0.56f;
    float card_h = 74, gap = 14;
    float cy = top;
    MenuAction result = MenuAction::None;
    for (const auto& a : actions) {
        Rect r{x, cy, col_w, card_h};
        if (ui.card(r, a.icon, a.title, a.sub)) result = a.act;
        cy += card_h + gap;
    }

    // Recent projects, under the actions.
    cy += 16;
    ui.text(x, cy, "RECENT", t.fonts.title_size, p.muted);
    cy += 30;
    if (recents.empty()) {
        ui.text(x + 2, cy, "No recent projects yet.", t.fonts.ui_size, p.muted);
    } else {
        for (const auto& path : recents) {
            std::filesystem::path pp(path);
            std::string name = pp.stem().string();  // strip .ardioproj
            Rect r{x, cy, col_w, 30};
            if (ui.row(r, name, false)) {
                recent_out = path;
                result = MenuAction::OpenRecent;
            }
            cy += 32;
        }
    }

    // Right column: the target board picker, from the real board database.
    float rx = x + col_w + pad;
    float rw = area.right() - pad - rx;
    ui.text(rx, top - f.line_height(t.fonts.title_size) - 6, "TARGET BOARD", t.fonts.title_size,
            p.muted);
    float row_h = 34;
    float by = top;
    for (size_t i = 0; i < boards.size(); ++i) {
        Rect r{rx, by, rw, row_h};
        if (ui.row(r, boards[i].name, static_cast<int>(i) == model.board))
            model.board = static_cast<int>(i);
        by += row_h + 4;
    }

    return result;
}

}  // namespace studio
