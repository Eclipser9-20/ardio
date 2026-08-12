#pragma once
#include <cstdint>
#include <cstddef>
#include <memory>
#include <string>

namespace ardio {

class SerialPort {
public:
    virtual ~SerialPort() = default;
    virtual bool open(const std::string& path, int baud, std::string& error) = 0;
    virtual void close() = 0;
    virtual bool write(const uint8_t* data, size_t len) = 0;
    // Returns bytes read; 0 on timeout.
    virtual size_t read(uint8_t* out, size_t max, int timeout_ms) = 0;
    virtual void set_dtr(bool level) = 0;
    virtual void set_rts(bool level) = 0;
};

std::unique_ptr<SerialPort> make_serial_port();

} // namespace ardio
