#include "studio/editor/file_tree.h"

#include <algorithm>
#include <filesystem>

#include "studio/fonts/font.h"

namespace fs = std::filesystem;

namespace studio {
namespace {

// Nerd Font icon (UTF-8) for a node. Directories get an open/closed folder;
// files a glyph chosen by extension, defaulting to a generic file.
const char* icon_for(const FileNode& n) {
    if (n.is_dir) return n.expanded ? "\xEF\x81\xBC" : "\xEF\x81\xBB";  // folder-open / folder
    std::string ext = fs::path(n.name).extension().string();
    if (ext == ".ino") return "\xEF\x8B\x9B";                       // microchip (sketch)
    if (ext == ".cpp" || ext == ".cc" || ext == ".cxx") return "\xEE\x98\x9D";  // C++
    if (ext == ".c") return "\xEE\x98\x9E";                          // C
    if (ext == ".h" || ext == ".hpp" || ext == ".hh") return "\xEF\x83\xBE";    // header
    if (ext == ".hex" || ext == ".bin") return "\xEF\x92\x9C";      // binary
    if (ext == ".md") return "\xEF\x92\x89";                         // markdown
    if (ext == ".json" || ext == ".toml" || ext == ".ardiomanif") return "\xEF\x87\x82";  // config
    return "\xEF\x85\x9B";                                          // generic file
}

}  // namespace

void FileTree::set_root(const std::string& path) {
    root_ = {};
    root_.path = path;
    root_.name = fs::path(path).filename().string();
    root_.is_dir = true;
    root_.expanded = true;
    scan(root_);
}

void FileTree::scan(FileNode& node) {
    node.children.clear();
    std::error_code ec;
    for (auto& e : fs::directory_iterator(node.path, ec)) {
        FileNode c;
        c.path = e.path().string();
        c.name = e.path().filename().string();
        c.is_dir = e.is_directory(ec);
        // The manifest is managed by the project wizard, not hand-edited -- hide
        // it, along with dotfiles.
        if (c.name.empty() || c.name[0] == '.' || c.name == "project.ardiomanif") continue;
        node.children.push_back(std::move(c));
    }
    // Directories first, then files, each alphabetical (case-insensitive).
    std::sort(node.children.begin(), node.children.end(), [](const FileNode& a, const FileNode& b) {
        if (a.is_dir != b.is_dir) return a.is_dir;
        std::string la = a.name, lb = b.name;
        std::transform(la.begin(), la.end(), la.begin(), ::tolower);
        std::transform(lb.begin(), lb.end(), lb.begin(), ::tolower);
        return la < lb;
    });
    node.loaded = true;
}

void FileTree::draw_node(Ui& ui, FileNode& node, Rect area, int depth, float& y,
                         std::string& open_path) {
    const Theme& t = ui.theme();
    const Palette& p = t.palette;
    Font& f = ui.font();
    const float ms = t.fonts.ui_size;
    const float rowh = f.line_height(ms) + 8;

    Rect row{area.x, y, area.w, rowh};
    bool visible = y + rowh > area.y && y < area.bottom();
    bool selected = !node.is_dir && node.path == selected_;

    if (visible) {
        float ht = ui.hot_t(row);
        if (selected)
            ui.gfx().fill_rect(row, p.selection);
        else if (ht > 0.002f)
            ui.gfx().fill_rect(row, p.raised.with_alpha(ht));

        float x = area.x + 8 + depth * 14.0f;
        float ty = y + (rowh - f.line_height(ms)) * 0.5f;
        // Chevron for directories.
        if (node.is_dir)
            f.draw(ui.gfx(), x, ty, node.expanded ? "\xE2\x96\xBE" : "\xE2\x96\xB8", ms, p.muted);
        // Icon + name.
        Color icon_col = node.is_dir ? p.accent2 : p.muted;
        f.draw(ui.gfx(), x + 16, ty, icon_for(node), ms, icon_col);
        f.draw(ui.gfx(), x + 38, ty, node.name, ms, (selected ? p.fg : p.fg));
    }

    if (ui.clicked(row)) {
        if (node.is_dir) {
            node.expanded = !node.expanded;
            if (node.expanded && !node.loaded) scan(node);
        } else {
            selected_ = node.path;
            open_path = node.path;
        }
    }
    y += rowh;

    if (node.is_dir && node.expanded)
        for (auto& c : node.children) draw_node(ui, c, area, depth + 1, y, open_path);
}

std::string FileTree::draw(Ui& ui, Rect area) {
    const Theme& t = ui.theme();
    const Palette& p = t.palette;
    Font& f = ui.font();

    // Header: "EXPLORER" over the project name.
    float hx = area.x + 14;
    f.draw(ui.gfx(), hx, area.y + 12, "EXPLORER", t.fonts.title_size, p.muted);
    if (has_root())
        f.draw(ui.gfx(), hx, area.y + 12 + f.line_height(t.fonts.title_size) + 4, root_.name,
               t.fonts.ui_size, p.fg);

    float top = area.y + 12 + f.line_height(t.fonts.title_size) + f.line_height(t.fonts.ui_size) + 14;
    Rect list{area.x, top, area.w, area.bottom() - top};

    ui.gfx().set_clip(list);
    std::string open_path;
    float y = top - scroll_;
    if (has_root())
        for (auto& c : root_.children) draw_node(ui, c, list, 0, y, open_path);
    ui.gfx().clear_clip();
    return open_path;
}

}  // namespace studio
