#pragma once
#include "ardio/platform/serial.h"
#include <algorithm>
#include <deque>
#include <vector>

namespace ardio {

// In-memory SerialPort for tests. Records everything written and DTR/RTS
// changes, and replays bytes queued in `to_read`.
class FakeSerialPort : public SerialPort {
public:
    std::vector<uint8_t> written;
    std::deque<uint8_t> to_read;
    std::vector<bool> dtr_history;
    std::vector<bool> rts_history;
    bool is_open = false;
    int opened_baud = 0;
    std::string opened_path;

    bool open(const std::string& path, int baud, std::string&) override {
        opened_path = path;
        opened_baud = baud;
        is_open = true;
        return true;
    }
    void close() override { is_open = false; }

    bool write(const uint8_t* data, size_t len) override {
        written.insert(written.end(), data, data + len);
        return true;
    }

    size_t read(uint8_t* out, size_t max, int) override {
        size_t n = std::min(max, to_read.size());
        for (size_t i = 0; i < n; ++i) { out[i] = to_read.front(); to_read.pop_front(); }
        return n;
    }

    void set_dtr(bool level) override { dtr_history.push_back(level); }
    void set_rts(bool level) override { rts_history.push_back(level); }
};

} // namespace ardio
