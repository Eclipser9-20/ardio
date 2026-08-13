#include "ardio/platform/remote_serial.h"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace ardio {
namespace {

// Quotes a string for a remote shell. The command we build is interpreted by
// a shell on the far end, so a device path is not just data -- without this a
// path containing a semicolon would run whatever follows it as the remote
// user.
std::string shell_quote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}

} // namespace

// A serial port at the far end of an ssh connection.
//
// The board is wired to a single-board computer's UART and that computer is on
// the network, so this is what "flashing over wifi" means in practice: the
// bytes travel over the network to the host, and the host puts them on the
// wire. The board needs no network stack of its own, which is why this works
// for an ATmega exactly as it does for a part that has wifi built in.
//
// The remote end is a plain `cat`, with the tty configured by `stty` first.
// Nothing needs to be installed there -- no agent, no copy of ardio, no Python
// -- which matters because the whole point of this project is not depending on
// somebody else's script to move bytes.
class RemoteSerialPort : public SerialPort {
public:
    RemoteSerialPort(std::string host, std::string gpio_chip, int reset_gpio,
                     int boot_gpio)
        : host_(std::move(host)), gpio_chip_(std::move(gpio_chip)),
          reset_gpio_(reset_gpio), boot_gpio_(boot_gpio) {}

    ~RemoteSerialPort() override { close(); }

    bool open(const std::string& path, int baud, std::string& error) override {
        close();

        // -T is deliberate: without it ssh allocates a tty, and a tty performs
        // newline translation and interprets control characters. A bootloader
        // protocol is binary, so a 0x0A silently becoming 0x0D 0x0A corrupts
        // the image in a way that only shows up as a verification failure much
        // later. BatchMode keeps a password prompt from hanging a flash
        // forever; a key that is not set up should fail immediately and say so.
        std::string remote =
            "stty -F " + shell_quote(path) + " " + std::to_string(baud) +
            " raw -echo -hupcl clocal min 1 time 0 && exec cat " + shell_quote(path);

        std::string command =
            "exec ssh -T -o BatchMode=yes -o StrictHostKeyChecking=accept-new " +
            shell_quote(host_) + " " + shell_quote(remote);

        if (!spawn_pipe(command, error)) return false;
        device_ = path;
        return true;
    }

    void close() override {
        if (to_child_ >= 0) { ::close(to_child_); to_child_ = -1; }
        if (from_child_ >= 0) { ::close(from_child_); from_child_ = -1; }
        if (pid_ > 0) {
            int status = 0;
            ::kill(pid_, SIGTERM);
            ::waitpid(pid_, &status, 0);
            pid_ = -1;
        }
    }

    bool write(const uint8_t* data, size_t len) override {
        size_t sent = 0;
        while (sent < len) {
            ssize_t n = ::write(to_child_, data + sent, len - sent);
            if (n < 0) {
                if (errno == EINTR) continue;
                return false;
            }
            sent += size_t(n);
        }
        return true;
    }

    size_t read(uint8_t* out, size_t max, int timeout_ms) override {
        struct pollfd fd { from_child_, POLLIN, 0 };
        int ready = ::poll(&fd, 1, timeout_ms);
        if (ready <= 0) return 0;
        ssize_t n = ::read(from_child_, out, max);
        return n > 0 ? size_t(n) : 0;
    }

    // A UART on a general-purpose header has TX and RX and nothing else, so
    // there are no modem control lines to toggle. The auto-reset that a
    // USB-serial adapter performs is therefore impossible here, and pretending
    // otherwise would leave the caller talking to a bootloader that was never
    // started.
    //
    // When a GPIO line has been configured as wired to the board, we drive
    // that instead, which reproduces the same effect. When one has not, the
    // request is recorded and reported rather than silently dropped, so the
    // command can tell the user to press reset themselves.
    void set_dtr(bool level) override {
        // DTR is the line a USB-serial adapter uses to reset the board.
        if (reset_gpio_ > 0) drive_gpio(reset_gpio_, level ? 0 : 1);
        else reset_unavailable_ = true;
    }

    void set_rts(bool level) override {
        // RTS selects the ROM loader on an ESP by holding GPIO0 down.
        if (boot_gpio_ > 0) drive_gpio(boot_gpio_, level ? 0 : 1);
        else boot_unavailable_ = true;
    }

    bool needs_manual_reset() const { return reset_unavailable_ || boot_unavailable_; }

private:
    bool spawn_pipe(const std::string& command, std::string& error) {
        int in_pipe[2], out_pipe[2];
        if (::pipe(in_pipe) != 0 || ::pipe(out_pipe) != 0) {
            error = "cannot create pipes: " + std::string(std::strerror(errno));
            return false;
        }

        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, in_pipe[0], STDIN_FILENO);
        posix_spawn_file_actions_adddup2(&actions, out_pipe[1], STDOUT_FILENO);
        posix_spawn_file_actions_addclose(&actions, in_pipe[1]);
        posix_spawn_file_actions_addclose(&actions, out_pipe[0]);

        const char* argv[] = {"/bin/sh", "-c", command.c_str(), nullptr};
        int rc = ::posix_spawn(&pid_, "/bin/sh", &actions, nullptr,
                               const_cast<char* const*>(argv), environ);
        posix_spawn_file_actions_destroy(&actions);
        ::close(in_pipe[0]);
        ::close(out_pipe[1]);

        if (rc != 0) {
            ::close(in_pipe[1]);
            ::close(out_pipe[0]);
            error = "cannot start ssh: " + std::string(std::strerror(rc));
            pid_ = -1;
            return false;
        }

        to_child_ = in_pipe[1];
        from_child_ = out_pipe[0];
        return true;
    }

    // Drives one GPIO line on the host for a moment. Each call is its own ssh
    // connection, which is slow, but reset happens a handful of times per
    // flash rather than per byte, and a persistent channel would mean holding
    // a second connection open for the whole session.
    void drive_gpio(int line, int value) {
        // libgpiod v2 spells the chip as an option; v1 took it as the first
        // positional argument. v2 is what current distributions ship, so that
        // is what is written here -- on a host still carrying v1, gpioset will
        // reject this and the flash will report that the reset failed rather
        // than appearing to work.
        std::string remote = "gpioset -c " + shell_quote(gpio_chip_) + " " +
                             std::to_string(line) + "=" + std::to_string(value);
        std::string command = "ssh -T -o BatchMode=yes " + shell_quote(host_) + " " +
                              shell_quote(remote) + " >/dev/null 2>&1";
        int rc = std::system(command.c_str());
        if (rc != 0) gpio_failed_ = true;
    }

    std::string host_, gpio_chip_, device_;
    int reset_gpio_ = 0, boot_gpio_ = 0;
    pid_t pid_ = -1;
    int to_child_ = -1, from_child_ = -1;
    bool reset_unavailable_ = false, boot_unavailable_ = false;
    bool gpio_failed_ = false;
};

std::unique_ptr<SerialPort> make_remote_serial_port(const std::string& host,
                                                    const std::string& gpio_chip,
                                                    int reset_gpio, int boot_gpio) {
    return std::make_unique<RemoteSerialPort>(host, gpio_chip, reset_gpio, boot_gpio);
}

} // namespace ardio
