#include "harness.h"
#include "ardio/protocol/stk500v1.h"

using namespace ardio::stk500v1;

TEST(stk_get_sync_is_0x30_then_eop) {
    auto c = cmd_get_sync();
    CHECK_EQ(c.size(), size_t(2));
    CHECK_EQ(int(c[0]), 0x30);
    CHECK_EQ(int(c[1]), 0x20);
}

TEST(stk_enter_and_leave_progmode) {
    auto enter = cmd_enter_progmode();
    CHECK_EQ(int(enter[0]), 0x50);
    CHECK_EQ(int(enter[1]), 0x20);
    auto leave = cmd_leave_progmode();
    CHECK_EQ(int(leave[0]), 0x51);
    CHECK_EQ(int(leave[1]), 0x20);
}

TEST(stk_read_signature_is_0x75) {
    auto c = cmd_read_signature();
    CHECK_EQ(int(c[0]), 0x75);
    CHECK_EQ(int(c[1]), 0x20);
}

TEST(stk_load_address_sends_word_address_little_endian) {
    // byte address 0x0100 is word address 0x0080
    auto c = cmd_load_address(0x0080);
    CHECK_EQ(c.size(), size_t(4));
    CHECK_EQ(int(c[0]), 0x55);
    CHECK_EQ(int(c[1]), 0x80);  // low byte first
    CHECK_EQ(int(c[2]), 0x00);
    CHECK_EQ(int(c[3]), 0x20);
}

TEST(stk_load_address_high_byte) {
    auto c = cmd_load_address(0x1234);
    CHECK_EQ(int(c[1]), 0x34);
    CHECK_EQ(int(c[2]), 0x12);
}

TEST(stk_prog_page_sends_byte_length_big_endian_and_flash_marker) {
    uint8_t data[128];
    for (int i = 0; i < 128; ++i) data[i] = uint8_t(i);
    auto c = cmd_prog_page(data, 128);
    // 0x64, len_hi, len_lo, 'F', <128 bytes>, 0x20
    CHECK_EQ(c.size(), size_t(5 + 128));
    CHECK_EQ(int(c[0]), 0x64);
    CHECK_EQ(int(c[1]), 0x00);  // length high byte first
    CHECK_EQ(int(c[2]), 128);
    CHECK_EQ(int(c[3]), 'F');
    CHECK_EQ(int(c[4]), 0);
    CHECK_EQ(int(c[131]), 127);
    CHECK_EQ(int(c.back()), 0x20);
}

TEST(stk_read_page_requests_flash) {
    auto c = cmd_read_page(128);
    CHECK_EQ(c.size(), size_t(5));
    CHECK_EQ(int(c[0]), 0x74);
    CHECK_EQ(int(c[1]), 0x00);
    CHECK_EQ(int(c[2]), 128);
    CHECK_EQ(int(c[3]), 'F');
    CHECK_EQ(int(c[4]), 0x20);
}

TEST(ok_response_is_insync_then_ok) {
    CHECK(is_ok_response({0x14, 0x10}));
    CHECK(!is_ok_response({0x14, 0x11}));
    CHECK(!is_ok_response({0x00, 0x10}));
    CHECK(!is_ok_response({0x14}));
    CHECK(!is_ok_response({}));
}
