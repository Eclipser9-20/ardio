// The landing screen: what Ardio Studio shows when it opens. A wordmark, the
// primary actions as cards, and the list of supported boards to target. It is a
// pure function of the frame -- given the ui context, an area, and the little
// state it keeps (which board is selected), it draws itself and returns the
// action the user took, if any. The app loop decides what those actions mean.
#pragma once
#include <string>
#include <vector>

#include "studio/ui/ui.h"

namespace studio {

enum class MenuAction { None, NewSketch, Open, Emulator, Settings, OpenRecent };

// The menu's own persistent state, owned by the caller across frames.
struct MenuModel {
    int board = 0;  // index into the board database
};

// `recents` are project paths (most-recent first). If the user clicks one,
// returns OpenRecent and writes its path to `recent_out`.
MenuAction main_menu(Ui& ui, Rect area, MenuModel& model, const std::vector<std::string>& recents,
                     std::string& recent_out);

}  // namespace studio
