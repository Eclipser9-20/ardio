#pragma once
#include <string>

namespace ardio {

struct Args {
    std::string command;     // "ports", "push", "toolchain", ...
    std::string positional;  // sketch/hex path, or a subcommand
    std::string positional2; // second operand, e.g. "toolchain fetch <pkg>"
    std::string port;
    bool port_is_manual = false;  // named via --manual: never auto-detect
    std::string board;
    int baud = 0;            // 0 = use config/board default
    bool monitor_after = false;
    bool backup_first = false;   // save existing firmware before writing
    std::string backup_path;
    bool help = false;
    std::string error;
};

Args parse_args(int argc, char** argv);
int run_command(const Args& args);

} // namespace ardio
