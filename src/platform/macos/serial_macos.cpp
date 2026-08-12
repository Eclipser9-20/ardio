#include "ardio/platform/serial.h"

#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <IOKit/serial/ioss.h>
#include <cerrno>
#include <cstring>
#include <poll.h>

namespace ardio {
namespace {

class MacSerialPort : public SerialPort {
public:
    ~MacSerialPort() override { close(); }

    bool open(const std::string& path, int baud, std::string& error) override {
        close();
        fd_ = ::open(path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (fd_ < 0) {
            error = "cannot open " + path + ": " + std::strerror(errno);
            return false;
        }

        termios tty{};
        if (tcgetattr(fd_, &tty) != 0) {
            error = "tcgetattr failed on " + path + ": " + std::strerror(errno);
            close();
            return false;
        }

        cfmakeraw(&tty);
        tty.c_cflag |= (CLOCAL | CREAD);
        tty.c_cflag &= ~unsigned(CSTOPB);   // 1 stop bit
        tty.c_cflag &= ~unsigned(CRTSCTS);  // no hardware flow control
        tty.c_cc[VMIN] = 0;
        tty.c_cc[VTIME] = 0;

        if (tcsetattr(fd_, TCSANOW, &tty) != 0) {
            error = "tcsetattr failed on " + path + ": " + std::strerror(errno);
            close();
            return false;
        }

        // IOSSIOSPEED sets arbitrary baud rates, including ones with no Bxxx
        // constant. Must be applied after tcsetattr, which would reset it.
        speed_t speed = speed_t(baud);
        if (ioctl(fd_, IOSSIOSPEED, &speed) == -1) {
            error = "cannot set baud " + std::to_string(baud) + " on " + path +
                    ": " + std::strerror(errno);
            close();
            return false;
        }

        tcflush(fd_, TCIOFLUSH);
        return true;
    }

    void close() override {
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    }

    bool write(const uint8_t* data, size_t len) override {
        size_t sent = 0;
        while (sent < len) {
            ssize_t n = ::write(fd_, data + sent, len - sent);
            if (n < 0) {
                if (errno == EAGAIN || errno == EINTR) continue;
                return false;
            }
            sent += size_t(n);
        }
        return true;
    }

    size_t read(uint8_t* out, size_t max, int timeout_ms) override {
        pollfd pfd{fd_, POLLIN, 0};
        size_t got = 0;
        while (got < max) {
            int rc = ::poll(&pfd, 1, timeout_ms);
            if (rc <= 0) break;                 // timeout or error
            ssize_t n = ::read(fd_, out + got, max - got);
            if (n <= 0) break;
            got += size_t(n);
        }
        return got;
    }

    void set_dtr(bool level) override { set_line(TIOCM_DTR, level); }
    void set_rts(bool level) override { set_line(TIOCM_RTS, level); }

private:
    void set_line(int flag, bool level) {
        if (fd_ < 0) return;
        // TIOCMBIS asserts the line, TIOCMBIC releases it.
        ioctl(fd_, level ? TIOCMBIS : TIOCMBIC, &flag);
    }

    int fd_ = -1;
};

} // namespace

std::unique_ptr<SerialPort> make_serial_port() {
    return std::make_unique<MacSerialPort>();
}

} // namespace ardio
