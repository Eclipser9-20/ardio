#include "harness.h"
#include "ardio/board.h"
#include <string>

TEST(board_database_contains_nano) {
    const ardio::Board* b = ardio::find_board_by_id("nano");
    CHECK(b != nullptr);
    CHECK(b->mcu == "atmega328p");
    CHECK_EQ(int(b->page_size), 128);
    CHECK_EQ(int(b->flash_size), 32768);
    CHECK(b->protocol == ardio::Protocol::Stk500v1);
}

TEST(nano_signature_is_atmega328p) {
    const ardio::Board* b = ardio::find_board_by_id("nano");
    CHECK(b != nullptr);
    CHECK_EQ(int(b->signature[0]), 0x1E);
    CHECK_EQ(int(b->signature[1]), 0x95);
    CHECK_EQ(int(b->signature[2]), 0x0F);
}

TEST(nano_tries_both_bootloader_baud_rates_new_first) {
    const ardio::Board* b = ardio::find_board_by_id("nano");
    CHECK(b != nullptr);
    CHECK_EQ(b->baud_rates.size(), size_t(2));
    CHECK_EQ(b->baud_rates[0], 115200);
    CHECK_EQ(b->baud_rates[1], 57600);
}

TEST(ch340_usb_id_identifies_nano) {
    auto matches = ardio::find_boards_by_usb({0x1A86, 0x7523});
    CHECK_EQ(matches.size(), size_t(1));
    CHECK(matches[0]->id == "nano");
}

TEST(unknown_usb_id_matches_nothing) {
    auto matches = ardio::find_boards_by_usb({0xDEAD, 0xBEEF});
    CHECK_EQ(matches.size(), size_t(0));
}

TEST(unknown_board_id_returns_null) {
    CHECK(ardio::find_board_by_id("teapot") == nullptr);
}
