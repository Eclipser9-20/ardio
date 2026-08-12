#include "ardio/toolchain.h"

#include <cstdlib>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

namespace ardio {
namespace {

std::string home() {
    const char* h = std::getenv("HOME");
    return h ? h : "";
}

// Expands a leading "~/" against $HOME.
std::string expand(std::string_view p) {
    if (p.size() >= 2 && p[0] == '~' && p[1] == '/') return home() + std::string(p.substr(1));
    return std::string(p);
}

bool is_executable(const fs::path& p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec) &&
           (fs::status(p, ec).permissions() & fs::perms::owner_exec) != fs::perms::none;
}

// Looks for `name` directly in `root`, in root/bin, and one level down
// (vendor package layouts nest as root/<tool>/<version>/bin).
bool search_root(const fs::path& root, std::string_view name, std::string& out) {
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return false;

    for (const fs::path& candidate : {root / name, root / "bin" / name}) {
        if (is_executable(candidate)) { out = candidate.string(); return true; }
    }

    for (const auto& entry : fs::recursive_directory_iterator(
             root, fs::directory_options::skip_permission_denied, ec)) {
        if (ec) break;
        if (entry.is_regular_file(ec) && entry.path().filename() == name &&
            is_executable(entry.path())) {
            out = entry.path().string();
            return true;
        }
    }
    return false;
}

} // namespace

std::vector<std::string> default_search_roots() {
    return {
        "~/.ardio/tools",
        "/usr/local/ardio",
        "~/.arduino-create",
        "~/Library/Arduino15",
        "$PATH",
    };
}

ToolLocation find_tool(std::string_view name, const std::vector<std::string>& roots) {
    ToolLocation loc;
    for (const std::string& root : roots) {
        if (root == "$PATH") {
            const char* path_env = std::getenv("PATH");
            if (!path_env) continue;
            std::string_view rest(path_env);
            while (!rest.empty()) {
                size_t colon = rest.find(':');
                std::string_view dir = (colon == std::string_view::npos)
                                           ? rest : rest.substr(0, colon);
                fs::path candidate = fs::path(std::string(dir)) / name;
                if (is_executable(candidate)) {
                    loc.path = candidate.string();
                    loc.found_in_root = "$PATH";
                    loc.found = true;
                    return loc;
                }
                if (colon == std::string_view::npos) break;
                rest.remove_prefix(colon + 1);
            }
            continue;
        }

        std::string found;
        if (search_root(expand(root), name, found)) {
            loc.path = found;
            loc.found_in_root = root;
            loc.found = true;
            return loc;
        }
    }
    return loc;
}

} // namespace ardio
