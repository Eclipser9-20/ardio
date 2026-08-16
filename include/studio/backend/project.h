// The .ardioproj project model.
//
// A project is a self-contained directory -- a plain folder cross-platform, and
// a Finder package once Studio ships as an .app that claims the extension:
//
//   MyThing.ardioproj/
//     project.ardiomanif   the manifest (board, build, upload settings)
//     src/                 sketch and source files (.ino, .cpp, .c)
//     include/             headers
//     build/Release/       compiler output (MyThing.hex)
//
// The manifest is our own tiny format: `[section]` headers and `key = value`
// lines, parsed by a small reader here rather than any library. Everything the
// IDE and the build need about a project is read from, and written back to,
// this one file, so a project is portable and hand-editable.
#pragma once
#include <string>

namespace studio {

// The parsed contents of project.ardiomanif. Unknown keys are ignored on read;
// only these fields are written back. Serialize() emits a formatted document --
// a titled banner, commented section rules, and aligned keys -- while parse()
// stays tolerant of comments (whole-line and inline) and whitespace.
struct Manifest {
    // Project
    std::string name;
    std::string version = "0.1.0";
    std::string board = "nano";           // board id from the board database
    std::string main = "src/main.ino";    // the entry sketch, project-relative

    // Build
    std::string build_config = "Release";
    int optimize = 2;                     // -O level
    std::string build_output;             // e.g. build/Release/Name.hex
    std::string defines;                  // extra -D defines, space-separated
    std::string warnings = "all";         // all | none | ...

    // Upload
    std::string upload_port = "auto";     // "auto" or a device path
    int upload_baud = 115200;
    std::string upload_protocol = "stk500v1";

    // Serial monitor
    int monitor_baud = 9600;

    // Parse manifest text (our [section]/key=value format, comments allowed).
    static Manifest parse(const std::string& text);
    // Serialize to the formatted document form, sections in a stable order.
    std::string serialize() const;
};

// An open project: where it lives on disk and what its manifest says.
struct Project {
    std::string root;  // path to the X.ardioproj directory
    Manifest manifest;

    std::string manifest_path() const { return root + "/project.ardiomanif"; }
    std::string src_dir() const { return root + "/src"; }
    std::string include_dir() const { return root + "/include"; }
    std::string build_dir() const { return root + "/build"; }
    std::string hex_path() const { return root + "/" + manifest.build_output; }
    std::string display_name() const { return manifest.name; }
};

// Create a new X.ardioproj under `parent_dir`: scaffolds src/, include/,
// build/Release/, a starter sketch and header, and the manifest. On success
// fills `out` and returns true; on failure returns false with a reason in `err`.
bool create_project(const std::string& parent_dir, const std::string& name,
                    const std::string& board, Project& out, std::string& err);

// Open an existing .ardioproj directory, reading its manifest. Returns false
// with a reason in `err` if the directory or manifest is missing/unreadable.
bool open_project(const std::string& root, Project& out, std::string& err);

// Write the manifest back to disk.
bool save_project(const Project& p, std::string& err);

}  // namespace studio
