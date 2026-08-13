#include "ardio/cli.h"
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace ardio {
namespace {

// Every option is spelled the same way with one dash or two: "-port" and
// "--port" are the same flag. The leading dashes carry no meaning here.
enum class Flag { None, Port, Board, Baud, Monitor, Backup, Help, Manual,
                  For, Wire, Input, Explain };

struct FlagSpec {
    std::string_view name;
    Flag id;
    bool abbreviable;  // may be reached by a unique prefix
};

// Order matters only for the ambiguity message, which lists in table order.
constexpr FlagSpec kFlags[] = {
    {"port",    Flag::Port,    true},
    {"board",   Flag::Board,   true},
    {"baud",    Flag::Baud,    true},
    {"backup",  Flag::Backup,  true},
    {"monitor", Flag::Monitor, true},
    {"manual",  Flag::Manual,  true},
    {"help",    Flag::Help,    true},
    // emulate options.
    {"for",     Flag::For,     true},
    {"wire",    Flag::Wire,    true},
    {"input",   Flag::Input,   true},
    {"explain", Flag::Explain, true},
    // Single-letter forms. These are exact-only: "-m" is monitor and can
    // never be read as a prefix of "manual".
    {"m",       Flag::Monitor, false},
    {"h",       Flag::Help,    false},
};

// Disambiguation rule: an exact name match always wins. Only if nothing
// matches exactly do we try prefixes, and a prefix must be unique. That is
// what keeps "-m" meaning monitor even though "manual" also starts with 'm'.
Flag lookup(std::string_view name, std::string& error, std::string_view spelled) {
    for (const FlagSpec& f : kFlags)
        if (f.name == name) return f.id;

    std::vector<std::string_view> hits;
    Flag found = Flag::None;
    for (const FlagSpec& f : kFlags) {
        if (!f.abbreviable) continue;
        if (f.name.size() > name.size() && f.name.substr(0, name.size()) == name) {
            hits.push_back(f.name);
            found = f.id;
        }
    }
    if (hits.size() == 1) return found;
    if (hits.size() > 1) {
        std::string s;
        for (std::string_view h : hits) s += std::string(s.empty() ? "" : ", ") + "--" + std::string(h);
        error = "ambiguous flag '" + std::string(spelled) + "' -- could be " + s;
        return Flag::None;
    }
    error = "unknown flag '" + std::string(spelled) + "'";
    return Flag::None;
}

// True for "-x" and "--x", false for "-" and for plain operands.
bool is_flag(const std::string& arg) {
    if (arg.size() < 2 || arg[0] != '-') return false;
    if (arg == "--") return false;
    return true;
}

std::string_view strip_dashes(const std::string& arg) {
    std::string_view v = arg;
    v.remove_prefix(v.size() > 1 && v[1] == '-' ? 2 : 1);
    return v;
}

} // namespace

Args parse_args(int argc, char** argv) {
    Args a;
    if (argc < 2) { a.help = true; return a; }

    int i = 1;
    std::string first = argv[1];
    if (is_flag(first)) {
        std::string err;
        if (lookup(strip_dashes(first), err, first) == Flag::Help) { a.help = true; return a; }
    }
    a.command = first;
    ++i;

    // A value-taking flag consumes the next argv entry.
    auto take_value = [&](const std::string& flag, std::string& dest) -> bool {
        if (i + 1 >= argc) {
            a.error = "flag " + flag + " needs a value";
            return false;
        }
        dest = argv[++i];
        return true;
    };

    for (; i < argc; ++i) {
        std::string arg = argv[i];
        if (!is_flag(arg)) {
            if (a.positional.empty())       { a.positional = arg; }
            else if (a.positional2.empty()) { a.positional2 = arg; }
            else { a.error = "unexpected argument '" + arg + "'"; return a; }
            continue;
        }

        switch (lookup(strip_dashes(arg), a.error, arg)) {
        case Flag::Port:
            if (!take_value(arg, a.port)) return a;
            break;
        case Flag::Manual:
            // Same destination as --port, but auto-detection is refused
            // outright so a typo cannot silently pick a different board.
            if (!take_value(arg, a.port)) return a;
            a.port_is_manual = true;
            break;
        case Flag::Board:
            if (!take_value(arg, a.board)) return a;
            break;
        case Flag::Baud: {
            std::string v;
            if (!take_value(arg, v)) return a;
            a.baud = std::atoi(v.c_str());
            if (a.baud <= 0) { a.error = arg + " needs a positive number"; return a; }
            break;
        }
        case Flag::Monitor:
            a.monitor_after = true;
            break;
        case Flag::Backup:
            a.backup_first = true;
            // An optional filename may follow, but not another flag.
            if (i + 1 < argc && !is_flag(argv[i + 1])) a.backup_path = argv[++i];
            break;
        case Flag::For:
            if (!take_value(arg, a.run_for)) return a;
            break;
        case Flag::Wire: {
            // Repeatable rather than last-one-wins: wiring two parts to a
            // board is the normal case, not a mistake to be resolved.
            std::string v;
            if (!take_value(arg, v)) return a;
            a.wires.push_back(v);
            break;
        }
        case Flag::Input:
            if (!take_value(arg, a.serial_input)) return a;
            break;
        case Flag::Explain:
            a.explain = true;
            break;
        case Flag::Help:
            a.help = true;
            break;
        case Flag::None:
            return a;  // lookup() filled in a.error
        }
    }
    return a;
}

} // namespace ardio
