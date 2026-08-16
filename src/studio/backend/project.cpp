#include "studio/backend/project.h"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace studio {
namespace {

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// Strip an inline comment: a '#' or ';' that follows whitespace. A leading '#'
// is a whole-line comment (handled by the caller); one mid-value is a note.
std::string strip_inline_comment(const std::string& s) {
    for (size_t i = 1; i < s.size(); ++i)
        if ((s[i] == '#' || s[i] == ';') && (s[i - 1] == ' ' || s[i - 1] == '\t'))
            return s.substr(0, i);
    return s;
}

// A left-aligned "key = value" with the key padded to a column, so a block of
// keys lines up. `col` is the pad width.
std::string kv(const std::string& key, const std::string& val, int col) {
    std::string out = key;
    while (static_cast<int>(out.size()) < col) out += ' ';
    return out + " = " + val;
}
std::string kv(const std::string& key, int val, int col) {
    return kv(key, std::to_string(val), col);
}

// A commented section rule, e.g. "# --- Build ----------------------".
std::string rule(const std::string& title) {
    std::string bar = "\xE2\x94\x80";  // U+2500 light horizontal
    std::string line = "# ";
    for (int i = 0; i < 3; ++i) line += bar;
    line += " " + title + " ";
    while (line.size() < 62) line += bar;
    return line;
}

bool write_file(const std::string& path, const std::string& content, std::string& err) {
    std::ofstream f(path, std::ios::binary);
    if (!f) {
        err = "could not write " + path;
        return false;
    }
    f << content;
    return true;
}

std::string read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// The starter sketch and header a new project ships with.
std::string starter_ino(const std::string& name) {
    return "#include \"" + name +
           ".hpp\"\n\nvoid setup() {\n    // runs once at power-on\n}\n\nvoid loop() {\n    // "
           "runs forever\n}\n";
}

}  // namespace

Manifest Manifest::parse(const std::string& text) {
    Manifest m;
    std::istringstream in(text);
    std::string line, section;
    while (std::getline(in, line)) {
        std::string s = trim(line);
        if (s.empty() || s[0] == '#') continue;
        if (s.front() == '[' && s.back() == ']') {
            section = s.substr(1, s.size() - 2);
            continue;
        }
        size_t eq = s.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(s.substr(0, eq));
        std::string val = trim(strip_inline_comment(s.substr(eq + 1)));
        auto as_int = [&](int& dst) { try { dst = std::stoi(val); } catch (...) {} };

        if (section.empty()) {
            if (key == "name") m.name = val;
            else if (key == "version") m.version = val;
            else if (key == "board") m.board = val;
            else if (key == "main") m.main = val;
        } else if (section == "build") {
            if (key == "config") m.build_config = val;
            else if (key == "optimize") as_int(m.optimize);
            else if (key == "output") m.build_output = val;
            else if (key == "defines") m.defines = val;
            else if (key == "warnings") m.warnings = val;
        } else if (section == "upload") {
            if (key == "port") m.upload_port = val;
            else if (key == "baud") as_int(m.upload_baud);
            else if (key == "protocol") m.upload_protocol = val;
        } else if (section == "monitor") {
            if (key == "baud") as_int(m.monitor_baud);
        }
    }
    return m;
}

std::string Manifest::serialize() const {
    const int col = 9;  // key alignment column
    std::ostringstream o;
    // A box-drawn title banner. UTF-8 light box-drawing characters.
    o << "# \xE2\x95\xAD" << std::string(58, ' ') << "\xE2\x95\xAE\n";
    auto banner = [&](const std::string& text) {
        std::string t = "  " + text;
        while (t.size() < 58) t += ' ';
        o << "# \xE2\x94\x82" << t << "\xE2\x94\x82\n";
    };
    banner("ardio project manifest");
    banner(name.empty() ? "" : name + "  v" + version);
    banner("generated + maintained by Ardio Studio");
    o << "# \xE2\x95\xB0" << std::string(58, ' ') << "\xE2\x95\xAF\n\n";

    o << kv("name", name, col) << "\n";
    o << kv("version", version, col) << "\n";
    o << kv("board", board, col) << "    # target board (run: ardio boards)\n";
    o << kv("main", main, col) << "\n\n";

    o << rule("Build") << "\n[build]\n";
    o << kv("config", build_config, col) << "\n";
    o << kv("optimize", optimize, col) << "    # -O level (0-3, s)\n";
    o << kv("output", build_output, col) << "\n";
    o << kv("defines", defines, col) << "    # extra -D flags, space-separated\n";
    o << kv("warnings", warnings, col) << "\n\n";

    o << rule("Upload") << "\n[upload]\n";
    o << kv("port", upload_port, col) << "    # auto, or a device path\n";
    o << kv("baud", upload_baud, col) << "\n";
    o << kv("protocol", upload_protocol, col) << "\n\n";

    o << rule("Serial Monitor") << "\n[monitor]\n";
    o << kv("baud", monitor_baud, col) << "\n";
    return o.str();
}

bool create_project(const std::string& parent_dir, const std::string& name,
                    const std::string& board, Project& out, std::string& err) {
    if (name.empty()) {
        err = "project name is empty";
        return false;
    }
    std::string root = parent_dir + "/" + name + ".ardioproj";
    std::error_code ec;
    if (fs::exists(root, ec)) {
        err = "a project already exists at " + root;
        return false;
    }
    fs::create_directories(root + "/src", ec);
    fs::create_directories(root + "/include", ec);
    fs::create_directories(root + "/build/Release", ec);
    if (ec) {
        err = "could not create project directories: " + ec.message();
        return false;
    }

    Manifest m;
    m.name = name;
    m.board = board.empty() ? "nano" : board;
    m.main = "src/" + name + ".ino";
    m.build_config = "Release";
    m.build_output = "build/Release/" + name + ".hex";

    if (!write_file(root + "/src/" + name + ".ino", starter_ino(name), err)) return false;
    if (!write_file(root + "/include/" + name + ".hpp", "#pragma once\n", err)) return false;
    if (!write_file(root + "/project.ardiomanif", m.serialize(), err)) return false;

    out.root = root;
    out.manifest = m;
    return true;
}

bool open_project(const std::string& root, Project& out, std::string& err) {
    std::error_code ec;
    if (!fs::is_directory(root, ec)) {
        err = root + " is not a project directory";
        return false;
    }
    std::string mp = root + "/project.ardiomanif";
    if (!fs::exists(mp, ec)) {
        err = "no project.ardiomanif in " + root;
        return false;
    }
    out.root = root;
    out.manifest = Manifest::parse(read_file(mp));
    if (out.manifest.name.empty()) out.manifest.name = fs::path(root).stem().string();
    return true;
}

bool save_project(const Project& p, std::string& err) {
    return write_file(p.manifest_path(), p.manifest.serialize(), err);
}

}  // namespace studio
