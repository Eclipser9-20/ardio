#pragma once
#include <string>

namespace ardio {

struct Args {
    std::string command;     // "ports", "push", ...
    std::string positional;  // sketch or hex path
    std::string port;
    std::string board;
    int baud = 0;            // 0 = use config/board default
    bool monitor_after = false;
    bool help = false;
    std::string error;
};

Args parse_args(int argc, char** argv);
int run_command(const Args& args);

} // namespace ardio
