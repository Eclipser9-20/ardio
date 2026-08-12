#include "harness.h"
#include "ardio/protocol/programmer.h"
#include "ardio/platform/fake_serial.h"
#include "ardio/board.h"
#include <string>

namespace {

// Queues the responses a healthy bootloader gives for a full upload of
// `pages` flash pages: sync, enter progmode, signature, then per page a
// load-address ack and a prog-page ack, then leave progmode.
void queue_successful_session(ardio::FakeSerialPort& port, int pages,
                              const std::array<uint8_t, 3>& sig) {
    auto ok = [&] { port.to_read.push_back(0x14); port.to_read.push_back(0x10); };
    ok();                                    // get_sync
    ok();                                    // enter progmode
    port.to_read.push_back(0x14);            // signature
    port.to_read.push_back(sig[0]);
    port.to_read.push_back(sig[1]);
    port.to_read.push_back(sig[2]);
    port.to_read.push_back(0x10);
    for (int i = 0; i < pages; ++i) { ok(); ok(); }  // load address + prog page
    ok();                                    // leave progmode
}

ardio::HexImage one_page_image() {
    ardio::HexImage img;
    img.data.assign(128, 0xAB);
    return img;
}

} // namespace

TEST(upload_succeeds_on_healthy_bootloader) {
    const ardio::Board* nano = ardio::find_board_by_id("nano");
    CHECK(nano != nullptr);
    ardio::FakeSerialPort port;
    queue_successful_session(port, 1, nano->signature);

    auto result = ardio::upload_stk500v1(port, "/dev/fake", *nano, one_page_image(), nullptr);
    CHECK(result.ok);
    CHECK_EQ(result.baud_used, 115200);
}

TEST(upload_pulses_dtr_to_reset_board) {
    const ardio::Board* nano = ardio::find_board_by_id("nano");
    ardio::FakeSerialPort port;
    queue_successful_session(port, 1, nano->signature);

    ardio::upload_stk500v1(port, "/dev/fake", *nano, one_page_image(), nullptr);
    CHECK(port.dtr_history.size() >= 2);
    CHECK_EQ(int(port.dtr_history[0]), 1);   // assert reset
    CHECK_EQ(int(port.dtr_history[1]), 0);   // release
}

TEST(upload_reports_sync_stage_when_bootloader_silent) {
    const ardio::Board* nano = ardio::find_board_by_id("nano");
    ardio::FakeSerialPort port;   // no queued responses at all

    auto result = ardio::upload_stk500v1(port, "/dev/fake", *nano, one_page_image(), nullptr);
    CHECK(!result.ok);
    CHECK(result.stage == "sync");
    CHECK(result.error.find("57600") != std::string::npos);  // mentions both baud attempts
}

TEST(upload_reports_signature_mismatch_with_both_values) {
    const ardio::Board* nano = ardio::find_board_by_id("nano");
    ardio::FakeSerialPort port;
    queue_successful_session(port, 1, {0x1E, 0x95, 0x16});  // ATmega32U4, wrong chip

    auto result = ardio::upload_stk500v1(port, "/dev/fake", *nano, one_page_image(), nullptr);
    CHECK(!result.ok);
    CHECK(result.stage == "signature");
    CHECK(result.error.find("1e950f") != std::string::npos);
    CHECK(result.error.find("1e9516") != std::string::npos);
}

TEST(upload_splits_image_into_page_sized_writes) {
    const ardio::Board* nano = ardio::find_board_by_id("nano");
    ardio::FakeSerialPort port;
    ardio::HexImage img;
    img.data.assign(300, 0x5A);          // 3 pages: 128 + 128 + 44
    queue_successful_session(port, 3, nano->signature);

    auto result = ardio::upload_stk500v1(port, "/dev/fake", *nano, img, nullptr);
    CHECK(result.ok);
    // Count 0x64 (prog page) commands issued.
    int prog_pages = 0;
    for (size_t i = 0; i < port.written.size(); ++i)
        if (port.written[i] == 0x64) { ++prog_pages; i += 4 + 128; }
    CHECK_EQ(prog_pages, 3);
}

TEST(upload_rejects_image_larger_than_flash) {
    const ardio::Board* nano = ardio::find_board_by_id("nano");
    ardio::FakeSerialPort port;
    ardio::HexImage img;
    img.data.assign(40000, 0x00);        // > 32768

    auto result = ardio::upload_stk500v1(port, "/dev/fake", *nano, img, nullptr);
    CHECK(!result.ok);
    CHECK(result.stage == "size");
    CHECK(result.error.find("32768") != std::string::npos);
}
