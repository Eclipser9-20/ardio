#include "ardio/cli.h"
#include <cstdlib>
#include <string>

namespace ardio {

Args parse_args(int argc, char** argv) {
    Args a;
    if (argc < 2) { a.help = true; return a; }

    int i = 1;
    std::string first = argv[1];
    if (first == "--help" || first == "-h") { a.help = true; return a; }
    a.command = first;
    ++i;

    // A value-taking flag consumes the next argv entry.
    auto take_value = [&](const char* flag, std::string& dest) -> bool {
        if (i + 1 >= argc) {
            a.error = std::string("flag ") + flag + " needs a value";
            return false;
        }
        dest = argv[++i];
        return true;
    };

    for (; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--port")       { if (!take_value("--port", a.port)) return a; }
        else if (arg == "--board") { if (!take_value("--board", a.board)) return a; }
        else if (arg == "--baud") {
            std::string v;
            if (!take_value("--baud", v)) return a;
            a.baud = std::atoi(v.c_str());
            if (a.baud <= 0) { a.error = "--baud needs a positive number"; return a; }
        }
        else if (arg == "-m" || arg == "--monitor") { a.monitor_after = true; }
        else if (arg == "--help" || arg == "-h")    { a.help = true; }
        else if (!arg.empty() && arg[0] == '-') {
            a.error = "unknown flag '" + arg + "'";
            return a;
        }
        else if (a.positional.empty())  { a.positional = arg; }
        else if (a.positional2.empty()) { a.positional2 = arg; }
        else { a.error = "unexpected argument '" + arg + "'"; return a; }
    }
    return a;
}

} // namespace ardio
