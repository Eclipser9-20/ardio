// The file explorer: a lazy directory tree rooted at the open project.
//
// It scans a folder into nodes, expands them on demand, and draws an indented,
// icon-decorated list the user clicks to open a file or fold a directory. It
// holds only tree and selection state; opening a file is the caller's job --
// draw() returns the path that was clicked so the editor can load it.
#pragma once
#include <string>
#include <vector>

#include "studio/ui/ui.h"

namespace studio {

struct FileNode {
    std::string name;
    std::string path;
    bool is_dir = false;
    bool expanded = false;
    bool loaded = false;  // children have been scanned
    std::vector<FileNode> children;
};

class FileTree {
public:
    // Point the tree at a project root and scan its top level.
    void set_root(const std::string& path);
    bool has_root() const { return !root_.path.empty(); }

    // Mark a path as the selected/open file (highlights it if visible).
    void select(const std::string& path) { selected_ = path; }

    // Draw into `area`; returns the path of a file the user clicked to open this
    // frame, or "" if none.
    std::string draw(Ui& ui, Rect area);

private:
    void scan(FileNode& node);
    void draw_node(Ui& ui, FileNode& node, Rect area, int depth, float& y, std::string& open_path);

    FileNode root_;
    std::string selected_;
    float scroll_ = 0;
};

}  // namespace studio
