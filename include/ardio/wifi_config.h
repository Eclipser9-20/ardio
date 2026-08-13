#pragma once
#include <string>
#include <vector>

// Configuration for boards ardio reaches over the network rather than over a
// USB port on this machine.
//
// The case this exists for: the board is wired to a single-board computer's
// UART, and that computer is on the wifi. So "flashing over wifi" means
// reaching the host over the network and driving the serial port at its end.
// The board itself needs no network stack, which is what makes this work for
// an ATmega as readily as for a part that has wifi of its own.
//
// This is kept separate from the toolchain Config, which lives in
// ~/.ardio/config.toml and describes how to build. This describes how to
// reach hardware, it holds a secret, and it is written by a command rather
// than by hand -- three reasons not to mix them into one file.
namespace ardio {

// One remotely-reachable board.
struct WifiBoard {
    std::string name;        // what `ardio wifi flash <name>` takes
    std::string board_id;    // an id from the board database, e.g. "nano"

    std::string host;        // "user@host" as ssh would take it
    std::string device;      // the serial device at the far end, e.g. "/dev/serial0"

    // The network the host is on. Recorded for the user's benefit -- ardio
    // does not join a network, it only reaches a host that is already on one.
    std::string ssid;

    // A UART on a general-purpose header usually has only TX and RX. There are
    // no DTR or RTS lines, so the auto-reset trick that a USB-serial adapter
    // performs cannot happen, and without a reset the bootloader is never
    // listening when we start talking to it.
    //
    // These name GPIO lines on the host wired to the board instead. Zero means
    // not wired, in which case ardio asks for a reset by hand rather than
    // waiting silently for a bootloader that will never answer.
    int reset_gpio = 0;      // wired to the board's RESET
    int boot_gpio = 0;       // wired to GPIO0 on an ESP, which selects the ROM loader
    std::string gpio_chip = "gpiochip0";
};

struct WifiConfig {
    std::vector<WifiBoard> boards;
};

// ~/.config/ardio/configuration.json, honouring XDG_CONFIG_HOME.
std::string wifi_config_path();

// Both return false and set `error` on failure. A missing file is NOT a
// failure for load: it yields an empty config, since not having configured
// anything yet is the normal state rather than a problem.
bool load_wifi_config(WifiConfig& out, std::string& error);

// Writes with owner-only permissions.
//
// Deliberately absent: a wifi password. ardio never joins a network -- it
// reaches a host that is already on one, and ssh handles that authentication
// with keys the user already has. Storing a password ardio would never use
// would be a liability with no benefit, since a JSON file is plaintext and
// 0600 is obscurity from other local users rather than encryption. If a board
// that joins wifi itself is supported later, its credentials should go to the
// OS keychain rather than here.
bool save_wifi_config(const WifiConfig& config, std::string& error);

const WifiBoard* find_wifi_board(const WifiConfig& config, const std::string& name);

} // namespace ardio
