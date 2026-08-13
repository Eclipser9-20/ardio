#include "harness.h"
#include "ardio/platform/fake_serial.h"
#include "ardio/protocol/avr109.h"

#include <string>
#include <vector>

using namespace ardio;

namespace {

// Enumerator driven by a scripted list of snapshots: each call to wait()
// advances to the next one, so a test can say "this port set, then that one"
// without any real delay.
class FakePorts : public PortEnumerator {
public:
    std::vector<std::vector<std::string>> snapshots;
    size_t index = 0;
    int total_waited_ms = 0;

    std::vector<std::string> list_ports() override {
        if (snapshots.empty()) return {};
        return snapshots[index < snapshots.size() ? index : snapshots.size() - 1];
    }
    void wait(int ms) override {
        total_waited_ms += ms;
        if (index + 1 < snapshots.size()) ++index;
    }
};

Board leonardo() {
    Board b;
    b.id = "leonardo";
    b.name = "Test 32U4 board";
    b.mcu = "atmega32u4";
    b.protocol = Protocol::Avr109;
    b.baud_rates = {57600};
    b.flash_size = 28672;
    b.page_size = 128;
    b.signature = {0x1E, 0x95, 0x87};
    return b;
}

void queue(FakeSerialPort& port, std::initializer_list<uint8_t> bytes) {
    for (uint8_t b : bytes) port.to_read.push_back(b);
}

void queue_id(FakeSerialPort& port) {
    for (char c : std::string("CATERIN")) port.to_read.push_back(uint8_t(c));
}

} // namespace

// ------------------------------------------------------------- encoding ---

TEST(avr109_simple_commands_are_single_letters) {
    CHECK_EQ(avr109::cmd_software_id().size(), size_t(1));
    CHECK_EQ(int(avr109::cmd_software_id()[0]), 'S');
    CHECK_EQ(int(avr109::cmd_software_version()[0]), 'V');
    CHECK_EQ(int(avr109::cmd_read_signature()[0]), 's');
    CHECK_EQ(int(avr109::cmd_enter_progmode()[0]), 'P');
    CHECK_EQ(int(avr109::cmd_leave_progmode()[0]), 'L');
    CHECK_EQ(int(avr109::cmd_exit_bootloader()[0]), 'E');
}

TEST(avr109_set_address_is_big_endian_word_address) {
    // Byte address 0x2468 is word address 0x1234, high byte first.
    auto c = avr109::cmd_set_address(0x1234);
    CHECK_EQ(c.size(), size_t(3));
    CHECK_EQ(int(c[0]), 'A');
    CHECK_EQ(int(c[1]), 0x12);
    CHECK_EQ(int(c[2]), 0x34);
}

TEST(avr109_set_address_byte_order_is_opposite_of_stk500v1) {
    auto c = avr109::cmd_set_address(0x0080);
    CHECK_EQ(int(c[1]), 0x00);  // high byte leads here; STK500v1 leads with low
    CHECK_EQ(int(c[2]), 0x80);
}

TEST(avr109_write_block_encodes_count_marker_and_data) {
    uint8_t data[128];
    for (int i = 0; i < 128; ++i) data[i] = uint8_t(i);
    auto c = avr109::cmd_write_block(data, 128);
    // 'B', 0x00, 0x80, 'F', <128 bytes>, and nothing after
    CHECK_EQ(c.size(), size_t(4 + 128));
    CHECK_EQ(int(c[0]), 'B');
    CHECK_EQ(int(c[1]), 0x00);
    CHECK_EQ(int(c[2]), 0x80);
    CHECK_EQ(int(c[3]), 'F');
    CHECK_EQ(int(c[4]), 0);
    CHECK_EQ(int(c[131]), 127);
}

TEST(avr109_write_block_count_high_byte) {
    std::vector<uint8_t> data(0x0102, 0xAB);
    auto c = avr109::cmd_write_block(data.data(), 0x0102);
    CHECK_EQ(int(c[1]), 0x01);
    CHECK_EQ(int(c[2]), 0x02);
    CHECK_EQ(c.size(), size_t(4 + 0x0102));
}

TEST(avr109_read_block_is_g_count_flash) {
    auto c = avr109::cmd_read_block(128);
    CHECK_EQ(c.size(), size_t(4));
    CHECK_EQ(int(c[0]), 'g');
    CHECK_EQ(int(c[1]), 0x00);
    CHECK_EQ(int(c[2]), 0x80);
    CHECK_EQ(int(c[3]), 'F');
}

TEST(avr109_cr_response_is_exactly_one_carriage_return) {
    CHECK(avr109::is_cr_response({0x0D}));
    CHECK(!avr109::is_cr_response({}));
    CHECK(!avr109::is_cr_response({0x0D, 0x0D}));
    CHECK(!avr109::is_cr_response({0x3F}));
}

// ------------------------------------------------------------ signature ---

TEST(avr109_signature_arrives_reversed_relative_to_stk500v1) {
    // An ATmega32U4 is 0x1E 0x95 0x87 in datasheet order, which is exactly what
    // STK500v1 puts on the wire. AVR109 sends the same three bytes backwards.
    const std::array<uint8_t, 3> stk_order{0x1E, 0x95, 0x87};
    const std::array<uint8_t, 3> avr109_wire{0x87, 0x95, 0x1E};
    CHECK(avr109_wire[0] == stk_order[2]);
    CHECK(avr109_wire[2] == stk_order[0]);
    auto converted = avr109::signature_to_datasheet_order(avr109_wire);
    CHECK_EQ(int(converted[0]), 0x1E);
    CHECK_EQ(int(converted[1]), 0x95);
    CHECK_EQ(int(converted[2]), 0x87);
    CHECK(converted == stk_order);
}

TEST(avr109_signature_conversion_is_its_own_inverse) {
    const std::array<uint8_t, 3> wire{0x0F, 0x94, 0x1E};
    auto once = avr109::signature_to_datasheet_order(wire);
    auto twice = avr109::signature_to_datasheet_order(once);
    CHECK(twice == wire);
}

// ----------------------------------------------------------- port swap ----

TEST(avr109_touch_reset_selects_the_newly_appeared_port) {
    FakeSerialPort serial;
    FakePorts ports;
    ports.snapshots = {{"/dev/cu.sketch"}, {"/dev/cu.bootloader"}};

    auto r = touch_reset_and_find_port(serial, "/dev/cu.sketch", ports, 2000, nullptr);
    CHECK(r.ok);
    CHECK(r.port == "/dev/cu.bootloader");
    // The touch itself must be at 1200 baud and must send no bytes at all.
    CHECK_EQ(serial.opened_baud, 1200);
    CHECK(serial.opened_path == "/dev/cu.sketch");
    CHECK_EQ(serial.written.size(), size_t(0));
    CHECK(!serial.is_open);
}

TEST(avr109_touch_reset_ignores_ports_that_were_already_present) {
    FakeSerialPort serial;
    FakePorts ports;
    // The original port lingers alongside the new one; the new one still wins.
    ports.snapshots = {{"/dev/cu.a", "/dev/cu.sketch"},
                       {"/dev/cu.a", "/dev/cu.sketch"},
                       {"/dev/cu.a", "/dev/cu.sketch", "/dev/cu.new"}};

    auto r = touch_reset_and_find_port(serial, "/dev/cu.sketch", ports, 2000, nullptr);
    CHECK(r.ok);
    CHECK(r.port == "/dev/cu.new");
}

TEST(avr109_touch_reset_times_out_when_no_new_port_appears) {
    FakeSerialPort serial;
    FakePorts ports;
    ports.snapshots = {{"/dev/cu.sketch"}};

    auto r = touch_reset_and_find_port(serial, "/dev/cu.sketch", ports, 300, nullptr);
    CHECK(!r.ok);
    CHECK(r.port.empty());
    CHECK(r.error.find("no new serial port appeared") != std::string::npos);
    CHECK(r.error.find("/dev/cu.sketch") != std::string::npos);
    CHECK_EQ(ports.total_waited_ms, 300);
}

TEST(avr109_touch_reset_reports_a_port_that_will_not_open) {
    class RefusingPort : public FakeSerialPort {
    public:
        bool open(const std::string&, int, std::string& error) override {
            error = "permission denied";
            return false;
        }
    } serial;
    FakePorts ports;
    ports.snapshots = {{"/dev/cu.sketch"}};

    auto r = touch_reset_and_find_port(serial, "/dev/cu.sketch", ports, 300, nullptr);
    CHECK(!r.ok);
    CHECK(r.error.find("1200-baud reset") != std::string::npos);
    CHECK(r.error.find("permission denied") != std::string::npos);
}

// -------------------------------------------------------------- upload ----

TEST(avr109_upload_writes_the_expected_conversation) {
    FakeSerialPort serial;
    FakePorts ports;
    ports.snapshots = {{"/dev/cu.sketch"}, {"/dev/cu.bootloader"}};

    queue_id(serial);                          // answer to 'S'
    queue(serial, {0x87, 0x95, 0x1E});         // answer to 's', reversed order
    queue(serial, {0x0D});                     // 'P'
    queue(serial, {0x0D});                     // 'A'
    queue(serial, {0x0D});                     // 'B'
    queue(serial, {0x0D});                     // 'L'
    queue(serial, {0x0D});                     // 'E'

    HexImage image;
    image.data = {0x0C, 0x94, 0x5C, 0x00};

    auto r = upload_avr109(serial, "/dev/cu.sketch", leonardo(), image, ports, nullptr);
    CHECK(r.ok);
    CHECK(r.error.empty());

    const std::vector<uint8_t> expected = {
        'S',
        's',
        'P',
        'A', 0x00, 0x00,
        'B', 0x00, 0x04, 'F', 0x0C, 0x94, 0x5C, 0x00,
        'L',
        'E',
    };
    CHECK_EQ(serial.written.size(), expected.size());
    CHECK(serial.written == expected);
    // Programming happens on the bootloader's port, never the sketch's.
    CHECK(serial.opened_path == "/dev/cu.bootloader");
}

TEST(avr109_upload_splits_the_image_into_page_sized_blocks) {
    FakeSerialPort serial;
    FakePorts ports;
    ports.snapshots = {{"/dev/cu.sketch"}, {"/dev/cu.bootloader"}};

    queue_id(serial);
    queue(serial, {0x87, 0x95, 0x1E});
    for (int i = 0; i < 1 + 2 * 2 + 2; ++i) serial.to_read.push_back(0x0D);

    HexImage image;
    image.data.assign(200, 0xA5);  // two blocks: 128 then 72

    auto r = upload_avr109(serial, "/dev/cu.sketch", leonardo(), image, ports, nullptr);
    CHECK(r.ok);

    // 'S' + 's' + 'P' + (3 + 4 + 128) + (3 + 4 + 72) + 'L' + 'E'
    CHECK_EQ(serial.written.size(), size_t(1 + 1 + 1 + 135 + 79 + 1 + 1));
    // The second address command is at word address 128/2 = 0x0040.
    size_t second = 3 + 135;
    CHECK_EQ(int(serial.written[second]), 'A');
    CHECK_EQ(int(serial.written[second + 1]), 0x00);
    CHECK_EQ(int(serial.written[second + 2]), 0x40);
    CHECK_EQ(int(serial.written[second + 3]), 'B');
    CHECK_EQ(int(serial.written[second + 4]), 0x00);
    CHECK_EQ(int(serial.written[second + 5]), 72);
    CHECK_EQ(int(serial.written[second + 6]), 'F');
}

TEST(avr109_upload_names_the_command_that_did_not_answer_cr) {
    FakeSerialPort serial;
    FakePorts ports;
    ports.snapshots = {{"/dev/cu.sketch"}, {"/dev/cu.bootloader"}};

    queue_id(serial);
    queue(serial, {0x87, 0x95, 0x1E});
    queue(serial, {0x3F});  // '?' -- Caterina's "unknown command" answer to 'P'

    HexImage image;
    image.data = {0x00, 0x00};

    auto r = upload_avr109(serial, "/dev/cu.sketch", leonardo(), image, ports, nullptr);
    CHECK(!r.ok);
    CHECK(r.stage == "sync");
    CHECK(r.error.find("'P'") != std::string::npos);
    CHECK(r.error.find("3f") != std::string::npos);
}

TEST(avr109_upload_reports_a_silent_block_write) {
    FakeSerialPort serial;
    FakePorts ports;
    ports.snapshots = {{"/dev/cu.sketch"}, {"/dev/cu.bootloader"}};

    queue_id(serial);
    queue(serial, {0x87, 0x95, 0x1E});
    queue(serial, {0x0D});  // 'P'
    queue(serial, {0x0D});  // 'A'
    // Nothing queued for 'B'.

    HexImage image;
    image.data = {0x01, 0x02};

    auto r = upload_avr109(serial, "/dev/cu.sketch", leonardo(), image, ports, nullptr);
    CHECK(!r.ok);
    CHECK(r.stage == "write");
    CHECK(r.error.find("no answer to AVR109 command 'B'") != std::string::npos);
    CHECK(r.error.find("byte offset 0") != std::string::npos);
}

TEST(avr109_upload_rejects_a_mismatched_signature) {
    FakeSerialPort serial;
    FakePorts ports;
    ports.snapshots = {{"/dev/cu.sketch"}, {"/dev/cu.bootloader"}};

    queue_id(serial);
    queue(serial, {0x0F, 0x94, 0x1E});  // an ATmega328P, not a 32U4

    HexImage image;
    image.data = {0x00};

    auto r = upload_avr109(serial, "/dev/cu.sketch", leonardo(), image, ports, nullptr);
    CHECK(!r.ok);
    CHECK(r.stage == "signature");
    CHECK(r.error.find("1e940f") != std::string::npos);  // reported datasheet-order
    CHECK(r.error.find("1e9587") != std::string::npos);
}

TEST(avr109_upload_reports_a_bootloader_that_never_identifies) {
    FakeSerialPort serial;
    FakePorts ports;
    ports.snapshots = {{"/dev/cu.sketch"}, {"/dev/cu.bootloader"}};
    // Nothing queued at all: the port opened but nothing is listening.

    HexImage image;
    image.data = {0x00};

    auto r = upload_avr109(serial, "/dev/cu.sketch", leonardo(), image, ports, nullptr);
    CHECK(!r.ok);
    CHECK(r.stage == "sync");
    CHECK(r.error.find("/dev/cu.bootloader") != std::string::npos);
}

TEST(avr109_upload_rejects_an_image_larger_than_flash) {
    FakeSerialPort serial;
    FakePorts ports;
    ports.snapshots = {{"/dev/cu.sketch"}, {"/dev/cu.bootloader"}};

    HexImage image;
    image.data.assign(40000, 0xFF);

    auto r = upload_avr109(serial, "/dev/cu.sketch", leonardo(), image, ports, nullptr);
    CHECK(!r.ok);
    CHECK(r.stage == "size");
    CHECK_EQ(serial.written.size(), size_t(0));
}

TEST(avr109_upload_fails_at_the_reset_stage_when_no_port_appears) {
    FakeSerialPort serial;
    FakePorts ports;
    ports.snapshots = {{"/dev/cu.sketch"}};

    HexImage image;
    image.data = {0x00};

    auto r = upload_avr109(serial, "/dev/cu.sketch", leonardo(), image, ports, nullptr);
    CHECK(!r.ok);
    CHECK(r.stage == "reset");
    CHECK(r.error.find("no new serial port appeared") != std::string::npos);
}
