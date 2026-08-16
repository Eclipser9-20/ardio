// A custom, in-app file browser -- the New/Open dialog, styled like the rest of
// Studio instead of the native OS panel. It navigates directories, and either
// picks a location + name for a new .ardioproj, or selects an existing one to
// open. It is modal: while active it draws over everything and takes input
// first.
#pragma once
#include <string>
#include <vector>

#include "studio/ui/ui.h"

union SDL_Event;

namespace studio {

class FileBrowser {
public:
    enum class Mode { New, Open };
    struct Result {
        bool done = false;  // the dialog resolved this frame
        bool ok = false;    // confirmed (vs cancelled)
        Mode mode = Mode::New;
        std::string dir;    // parent location (New) or the .ardioproj path (Open)
        std::string name;   // project name (New)
    };

    void open(Mode m);
    void close() { active_ = false; }
    bool active() const { return active_; }

    // Draw the modal into the whole window `screen`. Returns a resolved Result
    // when the user confirms or cancels this frame.
    Result draw(Ui& ui, Rect screen);

    // Text/key input for the name field and Enter/Esc. Returns true if consumed.
    bool handle(const SDL_Event& e);

private:
    struct Entry {
        std::string name;
        std::string path;
        bool is_proj = false;  // a .ardioproj directory
    };
    void scan();

    Mode mode_ = Mode::New;
    bool active_ = false;
    std::string cwd_;
    std::vector<Entry> entries_;
    std::string name_ = "Untitled";
    int selected_ = -1;  // selected .ardioproj in Open mode
    float scroll_ = 0;
    Rect list_rect_{};    // last drawn list area, for wheel hit-testing
    float max_scroll_ = 0;
    bool want_confirm_ = false;
    bool want_cancel_ = false;
};

}  // namespace studio
