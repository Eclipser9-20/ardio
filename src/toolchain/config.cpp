#include "ardio/toolchain.h"
#include <cstdlib>

namespace ardio {
namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
        s.remove_suffix(1);
    return s;
}

// Strips a trailing `# comment`, but not a '#' inside a quoted string.
std::string_view strip_comment(std::string_view s) {
    bool in_string = false;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '"') in_string = !in_string;
        else if (s[i] == '#' && !in_string) return s.substr(0, i);
    }
    return s;
}

bool unquote(std::string_view v, std::string& out) {
    v = trim(v);
    if (v.size() < 2 || v.front() != '"' || v.back() != '"') return false;
    out = std::string(v.substr(1, v.size() - 2));
    return true;
}

bool parse_array(std::string_view v, std::vector<std::string>& out) {
    v = trim(v);
    if (v.size() < 2 || v.front() != '[' || v.back() != ']') return false;
    v = v.substr(1, v.size() - 2);
    while (!v.empty()) {
        size_t comma = v.find(',');
        std::string_view item = (comma == std::string_view::npos) ? v : v.substr(0, comma);
        std::string s;
        item = trim(item);
        if (!item.empty()) {
            if (!unquote(item, s)) return false;
            out.push_back(s);
        }
        if (comma == std::string_view::npos) break;
        v.remove_prefix(comma + 1);
    }
    return true;
}

} // namespace

Config parse_config(std::string_view text, std::string& error) {
    Config cfg;
    size_t line_no = 0, i = 0;

    while (i < text.size()) {
        size_t end = text.find('\n', i);
        if (end == std::string_view::npos) end = text.size();
        std::string_view line = trim(strip_comment(text.substr(i, end - i)));
        i = end + 1;
        ++line_no;
        if (line.empty()) continue;

        size_t eq = line.find('=');
        if (eq == std::string_view::npos) {
            error = "line " + std::to_string(line_no) + ": expected 'key = value'";
            return cfg;
        }
        std::string_view key = trim(line.substr(0, eq));
        std::string_view val = trim(line.substr(eq + 1));

        auto bad_value = [&] {
            error = "line " + std::to_string(line_no) + ": bad value for '" +
                    std::string(key) + "'";
        };

        if (key == "default_board") {
            if (!unquote(val, cfg.default_board)) { bad_value(); return cfg; }
        } else if (key == "default_port") {
            if (!unquote(val, cfg.default_port)) { bad_value(); return cfg; }
        } else if (key == "monitor_baud") {
            cfg.monitor_baud = std::atoi(std::string(val).c_str());
            if (cfg.monitor_baud <= 0) { bad_value(); return cfg; }
        } else if (key == "search_roots") {
            if (!parse_array(val, cfg.search_roots)) { bad_value(); return cfg; }
        } else {
            error = "line " + std::to_string(line_no) + ": unknown key '" +
                    std::string(key) + "'";
            return cfg;
        }
    }
    return cfg;
}

} // namespace ardio
