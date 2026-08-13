#include "ardio/wifi_config.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace ardio {
namespace {

// A JSON writer and reader sized to this one file.
//
// ardio takes no third-party dependencies, and pulling in a general JSON
// library to read a list of four-field records would be a poor trade. The
// reader below is not a general JSON parser and does not pretend to be: it
// reads the shape this file writes, and says so rather than half-accepting
// something else.

void write_escaped(std::ostream& out, const std::string& text) {
    out << '"';
    for (char c : text) {
        switch (c) {
        case '"':  out << "\\\""; break;
        case '\\': out << "\\\\"; break;
        case '\n': out << "\\n";  break;
        case '\r': out << "\\r";  break;
        case '\t': out << "\\t";  break;
        default:
            // Control characters would produce invalid JSON, so they are
            // escaped rather than passed through. Nothing we write should
            // contain one, but a device path comes from the user.
            if (static_cast<unsigned char>(c) < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out << buf;
            } else {
                out << c;
            }
        }
    }
    out << '"';
}

// Finds "key": "value" or "key": number within `text`. Good enough for the
// flat records this file holds, and it refuses to guess: a key that is absent
// leaves the destination untouched.
bool find_string(const std::string& text, const std::string& key, std::string& out) {
    std::string needle = "\"" + key + "\"";
    size_t at = text.find(needle);
    if (at == std::string::npos) return false;
    at = text.find(':', at + needle.size());
    if (at == std::string::npos) return false;
    at = text.find('"', at);
    if (at == std::string::npos) return false;
    ++at;

    std::string value;
    while (at < text.size() && text[at] != '"') {
        if (text[at] == '\\' && at + 1 < text.size()) {
            ++at;
            switch (text[at]) {
            case 'n': value += '\n'; break;
            case 'r': value += '\r'; break;
            case 't': value += '\t'; break;
            default:  value += text[at];
            }
        } else {
            value += text[at];
        }
        ++at;
    }
    out = value;
    return true;
}

bool find_int(const std::string& text, const std::string& key, int& out) {
    std::string needle = "\"" + key + "\"";
    size_t at = text.find(needle);
    if (at == std::string::npos) return false;
    at = text.find(':', at + needle.size());
    if (at == std::string::npos) return false;
    ++at;
    while (at < text.size() && (text[at] == ' ' || text[at] == '\t')) ++at;
    if (at >= text.size() || !(std::isdigit(static_cast<unsigned char>(text[at])) ||
                               text[at] == '-'))
        return false;
    out = std::atoi(text.c_str() + at);
    return true;
}

// Splits the boards array into one substring per record, so the flat key
// search above cannot read a field out of a neighbouring board.
std::vector<std::string> split_records(const std::string& text) {
    std::vector<std::string> records;
    size_t at = text.find('[');
    if (at == std::string::npos) return records;

    int depth = 0;
    size_t start = 0;
    for (size_t i = at; i < text.size(); ++i) {
        if (text[i] == '{') {
            if (depth == 0) start = i;
            ++depth;
        } else if (text[i] == '}') {
            --depth;
            if (depth == 0) records.push_back(text.substr(start, i - start + 1));
        } else if (text[i] == ']' && depth == 0) {
            break;
        }
    }
    return records;
}

} // namespace

std::string wifi_config_path() {
    // XDG_CONFIG_HOME first, because a user who has set it means it.
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"))
        if (*xdg) return (fs::path(xdg) / "ardio" / "configuration.json").string();
    const char* home = std::getenv("HOME");
    if (!home) return {};
    return (fs::path(home) / ".config" / "ardio" / "configuration.json").string();
}

bool load_wifi_config(WifiConfig& out, std::string& error) {
    out.boards.clear();

    std::string path = wifi_config_path();
    if (path.empty()) {
        error = "cannot locate a config directory: HOME is not set";
        return false;
    }

    std::ifstream in(path);
    // Not having configured anything yet is the ordinary state, not an error.
    if (!in) return true;

    std::stringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();

    for (const std::string& record : split_records(text)) {
        WifiBoard board;
        find_string(record, "name", board.name);
        find_string(record, "board", board.board_id);
        find_string(record, "host", board.host);
        find_string(record, "device", board.device);
        find_string(record, "ssid", board.ssid);
        find_string(record, "gpio_chip", board.gpio_chip);
        find_int(record, "reset_gpio", board.reset_gpio);
        find_int(record, "boot_gpio", board.boot_gpio);

        if (board.name.empty()) {
            error = path + ": a board entry has no name";
            return false;
        }
        out.boards.push_back(board);
    }
    return true;
}

bool save_wifi_config(const WifiConfig& config, std::string& error) {
    std::string path = wifi_config_path();
    if (path.empty()) {
        error = "cannot locate a config directory: HOME is not set";
        return false;
    }

    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    if (ec) {
        error = "cannot create " + fs::path(path).parent_path().string() + ": " +
                ec.message();
        return false;
    }

    std::ostringstream out;
    out << "{\n  \"boards\": [\n";
    for (size_t i = 0; i < config.boards.size(); ++i) {
        const WifiBoard& b = config.boards[i];
        out << "    {\n";
        out << "      \"name\": ";       write_escaped(out, b.name);      out << ",\n";
        out << "      \"board\": ";      write_escaped(out, b.board_id);  out << ",\n";
        out << "      \"host\": ";       write_escaped(out, b.host);      out << ",\n";
        out << "      \"device\": ";     write_escaped(out, b.device);    out << ",\n";
        out << "      \"ssid\": ";       write_escaped(out, b.ssid);      out << ",\n";
        out << "      \"gpio_chip\": ";  write_escaped(out, b.gpio_chip); out << ",\n";
        out << "      \"reset_gpio\": " << b.reset_gpio << ",\n";
        out << "      \"boot_gpio\": "  << b.boot_gpio  << "\n";
        out << "    }" << (i + 1 < config.boards.size() ? "," : "") << "\n";
    }
    out << "  ]\n}\n";

    // Written to a temporary and renamed, so an interrupted write cannot leave
    // a half-written file where a valid one used to be.
    std::string temp = path + ".new";
    {
        std::ofstream file(temp, std::ios::trunc);
        if (!file) {
            error = "cannot write " + temp;
            return false;
        }
        file << out.str();
        if (!file) {
            error = "failed while writing " + temp;
            return false;
        }
    }

    fs::permissions(temp, fs::perms::owner_read | fs::perms::owner_write,
                    fs::perm_options::replace, ec);
    if (ec) {
        error = "cannot set permissions on " + temp + ": " + ec.message();
        return false;
    }

    fs::rename(temp, path, ec);
    if (ec) {
        error = "cannot replace " + path + ": " + ec.message();
        return false;
    }
    return true;
}

const WifiBoard* find_wifi_board(const WifiConfig& config, const std::string& name) {
    for (const WifiBoard& b : config.boards)
        if (b.name == name) return &b;
    return nullptr;
}

} // namespace ardio
