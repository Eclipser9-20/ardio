#include "ardio/cli.h"
#include "ardio/board.h"
#include "ardio/build.h"
#include "ardio/hex.h"
#include "ardio/monitor.h"
#include "ardio/platform/ports.h"
#include "ardio/protocol/avr109.h"
#include "ardio/protocol/programmer.h"
#include "ardio/toolchain.h"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace ardio {
namespace {

volatile bool g_stop = false;
void on_sigint(int) { g_stop = true; }

std::string usb_id_string(const PortInfo& p) {
    if (!p.has_usb_id) return "-";
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04x:%04x", p.usb.vid, p.usb.pid);
    return buf;
}

Config load_config() {
    const char* home = std::getenv("HOME");
    if (!home) return {};
    fs::path path = fs::path(home) / ".ardio" / "config.toml";
    std::ifstream in(path);
    if (!in) return {};
    std::stringstream ss;
    ss << in.rdbuf();
    std::string error;
    Config cfg = parse_config(ss.str(), error);
    if (!error.empty())
        std::fprintf(stderr, "warning: %s: %s\n", path.string().c_str(), error.c_str());
    return cfg;
}

std::vector<std::string> roots_for(const Config& cfg) {
    return cfg.search_roots.empty() ? default_search_roots() : cfg.search_roots;
}

int cmd_ports() {
    auto ports = enumerate_ports();
    if (ports.empty()) {
        std::printf("no serial ports found\n");
        return 1;
    }
    for (const PortInfo& p : ports) {
        auto boards = p.has_usb_id ? find_boards_by_usb(p.usb) : std::vector<const Board*>{};
        std::printf("%-28s %-12s %s\n", p.device.c_str(), usb_id_string(p).c_str(),
                    boards.empty() ? "(unrecognised)" : boards[0]->name.c_str());
    }
    return 0;
}

int cmd_boards() {
    for (const Board& b : board_database())
        std::printf("%-10s %-32s flash %uKB  page %u\n", b.id.c_str(), b.name.c_str(),
                    b.flash_size / 1024, b.page_size);
    return 0;
}

int cmd_doctor() {
    Config cfg = load_config();
    auto roots = roots_for(cfg);

    std::printf("search roots:\n");
    for (const std::string& r : roots) std::printf("  %s\n", r.c_str());

    std::printf("\ntools:\n");
    int missing = 0;
    for (const char* tool : {"avr-g++", "avr-objcopy"}) {
        ToolLocation loc = find_tool(tool, roots);
        if (loc.found)
            std::printf("  %-14s %s  (via %s)\n", tool, loc.path.c_str(),
                        loc.found_in_root.c_str());
        else {
            std::printf("  %-14s NOT FOUND\n", tool);
            ++missing;
        }
    }

    std::printf("\nports:\n");
    auto ports = enumerate_ports();
    if (ports.empty()) std::printf("  none\n");
    for (const PortInfo& p : ports)
        std::printf("  %-28s %s\n", p.device.c_str(), usb_id_string(p).c_str());

    std::printf("\ncompiler:\n  ardio's own AVR compiler handles .S, .c, .cpp and .ino\n");
    if (missing)
        std::printf("  an external toolchain is used only for sources ardio cannot\n"
                    "  compile yet (floating point, 32-bit arithmetic); install one\n"
                    "  with 'ardio toolchain fetch avr' if you need those.\n");
    return 0;
}

std::string tools_dir() {
    const char* home = std::getenv("HOME");
    return (fs::path(home ? home : ".") / ".ardio" / "tools").string();
}

int cmd_toolchain(const Args& args) {
    Config cfg = load_config();
    auto roots = roots_for(cfg);
    const std::string& sub = args.positional;

    if (sub.empty() || sub == "list") {
        std::printf("installed:\n");
        bool any = false;
        for (const char* tool : {"avr-g++", "avr-objcopy", "avr-gcc"}) {
            ToolLocation loc = find_tool(tool, roots);
            if (loc.found) {
                std::printf("  %-14s %s  (via %s)\n", tool, loc.path.c_str(),
                            loc.found_in_root.c_str());
                any = true;
            }
        }
        if (!any) std::printf("  (none found)\n");

        std::printf("\navailable to fetch for this machine (%s):\n",
                    host_triple().empty() ? "unsupported host" : host_triple().c_str());
        bool offered = false;
        for (const RemoteTool& t : remote_tool_registry()) {
            if (t.host != host_triple()) continue;
            std::printf("  %-6s %s  (%.1f MB)\n", t.package.c_str(), t.version.c_str(),
                        double(t.size_bytes) / 1e6);
            offered = true;
        }
        if (!offered) std::printf("  (no published build for this host)\n");
        std::printf("\nfetch with:  ardio toolchain fetch <package>\n");
        return 0;
    }

    // "ardio toolchain fetch avr" -> positional="fetch", positional2="avr"
    if (sub == "fetch") {
        const std::string& package = args.positional2;
        if (package.empty()) {
            std::fprintf(stderr,
                         "error: 'toolchain fetch' needs a package name, e.g.\n"
                         "         ardio toolchain fetch avr\n");
            return 2;
        }

        std::string host = host_triple();
        if (host.empty()) {
            std::fprintf(stderr,
                         "error: no published toolchain build for this platform yet\n");
            return 1;
        }
        const RemoteTool* tool = find_remote_tool(package, host);
        if (!tool) {
            std::fprintf(stderr,
                         "error: no '%s' toolchain available for %s.\n"
                         "       Run 'ardio toolchain list' to see what is offered.\n",
                         package.c_str(), host.c_str());
            return 1;
        }

        // Explicit consent. ardio never downloads anything silently.
        std::printf("about to download a toolchain:\n"
                    "  package   %s %s\n"
                    "  host      %s\n"
                    "  url       %s\n"
                    "  size      %.1f MB\n"
                    "  sha-256   %s\n"
                    "  install   %s\n\n"
                    "proceed? [y/N] ",
                    tool->package.c_str(), tool->version.c_str(), tool->host.c_str(),
                    tool->url.c_str(), double(tool->size_bytes) / 1e6,
                    tool->sha256.c_str(), tools_dir().c_str());
        std::fflush(stdout);

        int c = std::getchar();
        if (c != 'y' && c != 'Y') {
            std::printf("cancelled -- nothing was downloaded\n");
            return 1;
        }

        auto result = fetch_remote_tool(*tool, tools_dir(),
                                        [](const std::string& m) {
                                            std::printf("  %s\n", m.c_str());
                                        });
        if (!result.ok) {
            std::fprintf(stderr, "error: %s\n", result.error.c_str());
            return 1;
        }
        std::printf("\ninstalled to %s\n", result.installed_to.c_str());
        std::printf("run 'ardio doctor' to confirm it is being found.\n");
        return 0;
    }

    std::fprintf(stderr, "error: unknown toolchain subcommand '%s'. "
                         "Try 'list' or 'fetch <package>'.\n", sub.c_str());
    return 2;
}

// Resolves port + board, printing the reason on failure.
bool resolve(const Args& args, const Config& cfg, PortInfo& out_port,
             const Board*& out_board) {
    auto ports = enumerate_ports();
    std::string want_port = args.port.empty() ? cfg.default_port : args.port;
    std::string want_board = args.board.empty() ? cfg.default_board : args.board;

    // --manual means "this port or nothing". Say so plainly rather than
    // letting the generic "no serial ports found" advice stand in for it.
    if (args.port_is_manual) {
        bool present = false;
        for (const PortInfo& p : ports)
            if (p.device == args.port) { present = true; break; }
        if (!present) {
            std::string found;
            for (const PortInfo& p : ports) {
                found += "\n  " + p.device;
                if (!p.description.empty()) found += "  (" + p.description + ")";
            }
            if (found.empty()) found = " none";
            std::fprintf(stderr,
                         "error: --manual named '%s', but that port does not exist.\n"
                         "ports found:%s\n",
                         args.port.c_str(), found.c_str());
            return false;
        }
    }

    PortSelection sel = select_port(ports, want_port, want_board);
    if (!sel.port || !sel.board) {
        std::fprintf(stderr, "error: %s\n", sel.error.c_str());
        return false;
    }
    out_port = *sel.port;
    out_board = sel.board;
    return true;
}

int do_flash(const std::string& hex_path, const PortInfo& port, const Board& board) {
    std::ifstream in(hex_path);
    if (!in) {
        std::fprintf(stderr, "error: cannot read %s\n", hex_path.c_str());
        return 1;
    }
    std::stringstream ss;
    ss << in.rdbuf();

    std::string error;
    auto image = parse_intel_hex(ss.str(), error);
    if (!image) {
        std::fprintf(stderr, "error: %s: %s\n", hex_path.c_str(), error.c_str());
        return 1;
    }

    auto serial = make_serial_port();
    auto progress = [](const std::string& msg) { std::printf("  %s\n", msg.c_str()); };

    // Which conversation to have depends entirely on the board's bootloader.
    // The protocols have nothing in common beyond the serial line they run
    // over, so this is a real fork rather than a parameter.
    UploadResult result;
    switch (board.protocol) {
    case Protocol::Stk500v1:
        result = upload_stk500v1(*serial, port.device, board, *image, progress);
        break;
    case Protocol::Avr109: {
        // These boards run USB CDC from the sketch, so the bootloader shows up
        // as a different device at a different path once the touch reset takes
        // effect. The uploader needs to watch the port list to find it, which
        // is what the enumerator is for.
        SystemPortEnumerator ports;
        result = upload_avr109(*serial, port.device, board, *image, ports, progress);
        break;
    }
    case Protocol::Stk500v2:
        result.error = board.name +
                       " uses the stk500v2 bootloader. ardio implements the "
                       "protocol, but its code generator cannot yet address "
                       "the flash above 64 KB that this part has, so there is "
                       "nothing safe to send.";
        result.stage = "board";
        break;
    case Protocol::EspRom:
        result.error = board.name +
                       " uses the ESP ROM loader, which expects a raw binary "
                       "at a flash offset rather than an Intel HEX image.";
        result.stage = "board";
        break;
    }
    if (!result.ok) {
        std::fprintf(stderr, "error [%s]: %s\n", result.stage.c_str(), result.error.c_str());
        return 1;
    }
    std::printf("uploaded %zu bytes to %s at %d baud\n", image->data.size(),
                port.device.c_str(), result.baud_used);
    return 0;
}

// Reads the board's flash and writes it as Intel HEX. Trailing erased flash
// (0xFF) is dropped so a small sketch does not produce a 32 KB file.
int do_dump(const std::string& out_path, const PortInfo& port, const Board& board) {
    auto serial = make_serial_port();
    auto result = read_flash_stk500v1(*serial, port.device, board, 0,
                                      [](const std::string& msg) {
                                          std::printf("  %s\n", msg.c_str());
                                      });
    if (!result.ok) {
        std::fprintf(stderr, "error [%s]: %s\n", result.stage.c_str(),
                     result.error.c_str());
        return 1;
    }

    std::vector<uint8_t> data = result.data;
    while (!data.empty() && data.back() == 0xFF) data.pop_back();

    std::ofstream out(out_path);
    if (!out) {
        std::fprintf(stderr, "error: cannot write %s\n", out_path.c_str());
        return 1;
    }
    out << write_intel_hex(data);
    out.close();

    std::printf("saved %zu bytes from %s to %s\n", data.size(),
                port.device.c_str(), out_path.c_str());
    return 0;
}

int do_monitor(const PortInfo& port, int baud) {
    auto serial = make_serial_port();
    std::string error;
    if (!serial->open(port.device, baud, error)) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
    }
    std::printf("--- monitoring %s at %d baud (ctrl-c to stop) ---\n",
                port.device.c_str(), baud);
    g_stop = false;
    std::signal(SIGINT, on_sigint);
    run_monitor(*serial, g_stop);
    std::printf("\n");
    return 0;
}

void print_help() {
    std::printf(
        "ardio -- build, flash, and monitor Arduino boards\n"
        "\n"
        "usage: ardio <command> [sketch|file] [options]\n"
        "\n"
        "commands:\n"
        "  push [sketch]      build and upload\n"
        "  build [sketch]     compile only\n"
        "  flash <file.hex>   upload a prebuilt image\n"
        "  dump [file.hex]    save the board's current firmware\n"
        "  monitor            open the serial monitor\n"
        "  ports              list serial ports\n"
        "  boards             list supported boards\n"
        "  doctor             diagnose toolchains and ports\n"
        "  toolchain list     show installed and fetchable toolchains\n"
        "  toolchain fetch <pkg>\n"
        "                     download a toolchain (asks first)\n"
        "\n"
        "options (one dash or two -- '-port' and '--port' are the same):\n"
        "  -port <device>     serial port (default: auto-detect)\n"
        "  -manual <device>   use exactly this port, never auto-detect\n"
        "  -board <id>        board id (default: from USB id)\n"
        "  -baud <n>          monitor baud rate\n"
        "  -monitor, -m       open the monitor after a successful push\n"
        "  -backup [file]     save existing firmware before overwriting it\n"
        "  -help, -h          show this help\n");
}

} // namespace

int run_command(const Args& args) {
    if (!args.error.empty()) {
        std::fprintf(stderr, "error: %s\n", args.error.c_str());
        return 2;
    }
    if (args.help || args.command.empty()) {
        print_help();
        return 0;
    }

    if (args.command == "ports")     return cmd_ports();
    if (args.command == "boards")    return cmd_boards();
    if (args.command == "doctor")    return cmd_doctor();
    if (args.command == "toolchain") return cmd_toolchain(args);

    Config cfg = load_config();

    if (args.command == "build" || args.command == "push") {
        if (args.positional.empty()) {
            std::fprintf(stderr, "error: %s needs a sketch file\n", args.command.c_str());
            return 2;
        }

        PortInfo port;
        const Board* board = nullptr;

        if (args.command == "push") {
            // push needs a real board, so resolve port and board together.
            if (!resolve(args, cfg, port, board)) return 1;
        } else {
            // build has no port; pick the named board, or the config default.
            std::string want = args.board.empty() ? cfg.default_board : args.board;
            if (want.empty()) want = "nano";
            board = find_board_by_id(want);
            if (!board) {
                std::fprintf(stderr,
                             "error: unknown board '%s'. Run 'ardio boards'.\n",
                             want.c_str());
                return 2;
            }
        }

        // Deliberately not "build/" -- that is a common CMake output directory
        // and "build/ardio" would collide with the ardio binary itself.
        auto result = build_sketch(args.positional, *board, roots_for(cfg), ".ardio-build");
        if (!result.ok) {
            std::fprintf(stderr, "error: %s\n", result.error.c_str());
            return 1;
        }
        std::printf("built %s\n", result.hex_path.c_str());
        if (args.command == "build") return 0;

        if (args.backup_first) {
            std::string backup = args.backup_path.empty() ? "firmware-backup.hex"
                                                          : args.backup_path;
            std::printf("backing up existing firmware first\n");
            int brc = do_dump(backup, port, *board);
            if (brc != 0) {
                std::fprintf(stderr,
                             "error: backup failed, so nothing was overwritten\n");
                return brc;
            }
        }

        int rc = do_flash(result.hex_path, port, *board);
        if (rc != 0) return rc;
        if (args.monitor_after)
            return do_monitor(port, args.baud ? args.baud : cfg.monitor_baud);
        return 0;
    }

    if (args.command == "dump") {
        std::string out_path = args.positional.empty() ? "firmware-backup.hex"
                                                       : args.positional;
        PortInfo port;
        const Board* board = nullptr;
        if (!resolve(args, cfg, port, board)) return 1;
        return do_dump(out_path, port, *board);
    }

    if (args.command == "flash") {
        if (args.positional.empty()) {
            std::fprintf(stderr, "error: flash needs a .hex file\n");
            return 2;
        }
        PortInfo port;
        const Board* board = nullptr;
        if (!resolve(args, cfg, port, board)) return 1;
        return do_flash(args.positional, port, *board);
    }

    if (args.command == "monitor") {
        PortInfo port;
        const Board* board = nullptr;
        if (!resolve(args, cfg, port, board)) return 1;
        return do_monitor(port, args.baud ? args.baud : cfg.monitor_baud);
    }

    std::fprintf(stderr, "error: unknown command '%s'. Try 'ardio --help'.\n",
                 args.command.c_str());
    return 2;
}

} // namespace ardio
