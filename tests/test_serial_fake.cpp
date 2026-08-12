#include "harness.h"
#include "ardio/platform/fake_serial.h"
#include <string>

TEST(fake_serial_records_writes) {
    ardio::FakeSerialPort port;
    std::string err;
    CHECK(port.open("/dev/fake", 115200, err));
    uint8_t data[] = {0x30, 0x20};
    CHECK(port.write(data, 2));
    CHECK_EQ(port.written.size(), size_t(2));
    CHECK_EQ(int(port.written[0]), 0x30);
}

TEST(fake_serial_replays_queued_reads) {
    ardio::FakeSerialPort port;
    port.to_read = {0x14, 0x10};
    uint8_t buf[8];
    size_t n = port.read(buf, 8, 100);
    CHECK_EQ(n, size_t(2));
    CHECK_EQ(int(buf[0]), 0x14);
    CHECK_EQ(int(buf[1]), 0x10);
}

TEST(fake_serial_read_returns_zero_when_empty) {
    ardio::FakeSerialPort port;
    uint8_t buf[8];
    CHECK_EQ(port.read(buf, 8, 10), size_t(0));
}

TEST(fake_serial_records_dtr_toggles) {
    ardio::FakeSerialPort port;
    port.set_dtr(false);
    port.set_dtr(true);
    CHECK_EQ(port.dtr_history.size(), size_t(2));
    CHECK_EQ(int(port.dtr_history[0]), 0);
    CHECK_EQ(int(port.dtr_history[1]), 1);
}
