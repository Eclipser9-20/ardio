#include "harness.h"
#include "ardio/hex.h"
#include <string>

TEST(hex_parses_single_data_record) {
    std::string err;
    auto img = ardio::parse_intel_hex(":03000000C0FFEE50\n:00000001FF\n", err);
    CHECK(img.has_value());
    CHECK_EQ(img->data.size(), size_t(3));
    CHECK_EQ(int(img->data[0]), 0xC0);
    CHECK_EQ(int(img->data[1]), 0xFF);
    CHECK_EQ(int(img->data[2]), 0xEE);
}

TEST(hex_fills_gaps_between_records_with_0xFF) {
    std::string err;
    // one byte at 0x0000, one byte at 0x0004 -- the gap is unprogrammed flash
    auto img = ardio::parse_intel_hex(":01000000AA55\n:0100040055A6\n:00000001FF\n", err);
    CHECK(img.has_value());
    CHECK_EQ(img->data.size(), size_t(5));
    CHECK_EQ(int(img->data[1]), 0xFF);
    CHECK_EQ(int(img->data[3]), 0xFF);
    CHECK_EQ(int(img->data[4]), 0x55);
}

TEST(hex_rejects_bad_checksum) {
    std::string err;
    auto img = ardio::parse_intel_hex(":03000000C0FFEE00\n:00000001FF\n", err);
    CHECK(!img.has_value());
    CHECK(err.find("checksum") != std::string::npos);
}

TEST(hex_rejects_missing_eof_record) {
    std::string err;
    auto img = ardio::parse_intel_hex(":03000000C0FFEE50\n", err);
    CHECK(!img.has_value());
}

TEST(hex_writer_round_trips_through_the_parser) {
    std::vector<uint8_t> data;
    for (int i = 0; i < 300; ++i) data.push_back(uint8_t(i * 7));

    std::string text = ardio::write_intel_hex(data);
    std::string err;
    auto back = ardio::parse_intel_hex(text, err);

    CHECK(back.has_value());
    CHECK(err.empty());
    CHECK_EQ(back->data.size(), data.size());
    bool identical = back->data == data;
    CHECK(identical);
}

TEST(hex_writer_emits_a_valid_first_record_and_eof) {
    // 4 bytes at address 0: ":04000000" + data + checksum
    std::string text = ardio::write_intel_hex({0x25, 0x9A, 0x2D, 0x98});
    CHECK(text.rfind(":04000000259A2D98", 0) == 0);
    CHECK(text.find(":00000001FF") != std::string::npos);
}
