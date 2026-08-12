// Fallback platform layer for targets whose native implementation has not
// landed yet. It keeps ardio building and running everywhere: commands that do
// not touch hardware (boards, doctor, build) work normally, and the ones that
// do report the limitation instead of failing to link.

#include "ardio/platform/ports.h"
#include "ardio/platform/serial.h"

namespace ardio {
namespace {

class UnsupportedSerialPort : public SerialPort {
public:
    bool open(const std::string&, int, std::string& error) override {
        error = "serial I/O is not implemented for this platform yet "
                "(ardio currently supports macOS)";
        return false;
    }
    void close() override {}
    bool write(const uint8_t*, size_t) override { return false; }
    size_t read(uint8_t*, size_t, int) override { return 0; }
    void set_dtr(bool) override {}
    void set_rts(bool) override {}
};

} // namespace

std::vector<PortInfo> enumerate_ports() { return {}; }

std::unique_ptr<SerialPort> make_serial_port() {
    return std::make_unique<UnsupportedSerialPort>();
}

} // namespace ardio
