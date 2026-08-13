#pragma once
#include <string>
#include <vector>

namespace ardio {

struct Args {
    std::string command;     // "ports", "push", "toolchain", ...
    std::string positional;  // sketch/hex path, or a subcommand
    std::string positional2; // second operand, e.g. "toolchain fetch <pkg>"
    std::string positional3; // third, e.g. "wifi flash <name> <sketch>"
    std::string port;
    bool port_is_manual = false;  // named via --manual: never auto-detect
    std::string board;
    int baud = 0;            // 0 = use config/board default
    bool monitor_after = false;
    bool backup_first = false;   // save existing firmware before writing
    std::string backup_path;
    // --- emulate ------------------------------------------------------
    // How long to run, as the user wrote it ("2s", "500ms", "1000000c").
    // Kept as text so the command can report the unit back in its own error
    // rather than silently defaulting a value it failed to understand.
    std::string run_for;

    // Parts to wire up, each "kind:pin" ("led:13", "button:2"). Repeatable,
    // because a breadboard with one component on it is not very interesting.
    std::vector<std::string> wires;

    // Text fed to the sketch's serial input before running.
    std::string serial_input;

    // Report which execution core was selected and stop. Worth having as a
    // flag because the native translator and the portable core differ by
    // orders of magnitude in speed, and a user seeing slow emulation should
    // be able to find out which one they got.
    bool explain = false;

    // --- configure wifi / wifi flash ----------------------------------
    std::string host;        // "user@host", as ssh takes it
    std::string device;      // the serial device at the far end
    std::string ssid;        // recorded for reference; ardio never joins a network
    std::string gpio_chip;
    int reset_gpio = 0;      // host GPIO wired to the board's RESET
    int boot_gpio = 0;       // host GPIO wired to GPIO0 on an ESP

    bool help = false;
    std::string error;
};

Args parse_args(int argc, char** argv);
int run_command(const Args& args);

} // namespace ardio
