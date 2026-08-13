#include "harness.h"
#include "ardio/board.h"
#include "ardio/platform/fake_serial.h"
#include "ardio/protocol/esp_rom.h"

#include <string>

using namespace ardio;
using namespace ardio::esp_rom;

namespace {

// --------------------------------------------------------- test scripting ---

// Builds the frame an ESP8266 ROM sends back for `cmd`: a reply header with a
// value word, then a status byte and an error byte.
std::vector<uint8_t> reply_frame(Command cmd, uint32_t value = 0, uint8_t status = 0,
                                 uint8_t error = 0) {
    std::vector<uint8_t> body{kDirReply, uint8_t(cmd), 0x02, 0x00};
    body.push_back(uint8_t(value & 0xFF));
    body.push_back(uint8_t((value >> 8) & 0xFF));
    body.push_back(uint8_t((value >> 16) & 0xFF));
    body.push_back(uint8_t((value >> 24) & 0xFF));
    body.push_back(status);
    body.push_back(error);
    return slip_encode(body);
}

void queue(FakeSerialPort& port, const std::vector<uint8_t>& bytes) {
    port.to_read.insert(port.to_read.end(), bytes.begin(), bytes.end());
}

// The replies a healthy ROM gives for a whole upload: the sync answer plus the
// surplus copies it repeats, the chip ID, the erase ack, one ack per block, and
// the end-of-flash ack.
void queue_successful_session(FakeSerialPort& port, int blocks,
                              uint32_t chip_id = kEsp8266ChipId) {
    queue(port, reply_frame(Command::Sync));
    for (int i = 0; i < 3; ++i) queue(port, reply_frame(Command::Sync));  // surplus
    queue(port, reply_frame(Command::ReadReg, chip_id));
    queue(port, reply_frame(Command::FlashBegin));
    for (int i = 0; i < blocks; ++i) queue(port, reply_frame(Command::FlashData));
    queue(port, reply_frame(Command::FlashEnd));
}

HexImage image_of(size_t len, uint8_t fill = 0xAB) {
    HexImage img;
    img.data.assign(len, fill);
    return img;
}

const Board& esp() {
    const Board* b = find_board_by_id("esp8266");
    return *b;
}

} // namespace

// ------------------------------------------------------------------ SLIP ---

TEST(esp_slip_encode_wraps_payload_in_delimiters) {
    auto out = slip_encode({0x01, 0x02, 0x03});
    CHECK_EQ(out.size(), size_t(5));
    CHECK_EQ(int(out.front()), 0xC0);
    CHECK_EQ(int(out.back()), 0xC0);
    CHECK_EQ(int(out[1]), 0x01);
    CHECK_EQ(int(out[3]), 0x03);
}

TEST(esp_slip_encode_escapes_a_literal_end_byte) {
    auto out = slip_encode({0xC0});
    CHECK_EQ(out.size(), size_t(4));
    CHECK_EQ(int(out[1]), 0xDB);
    CHECK_EQ(int(out[2]), 0xDC);
}

TEST(esp_slip_encode_escapes_a_literal_escape_byte) {
    auto out = slip_encode({0xDB});
    CHECK_EQ(out.size(), size_t(4));
    CHECK_EQ(int(out[1]), 0xDB);
    CHECK_EQ(int(out[2]), 0xDD);
}

TEST(esp_slip_round_trips_every_byte_value_including_both_escapes) {
    std::vector<uint8_t> payload;
    for (int i = 0; i < 256; ++i) payload.push_back(uint8_t(i));
    auto decoded = slip_decode(slip_encode(payload));
    CHECK(decoded.has_value());
    CHECK(*decoded == payload);
}

TEST(esp_slip_round_trips_an_empty_payload) {
    auto decoded = slip_decode(slip_encode({}));
    CHECK(decoded.has_value());
    CHECK_EQ(decoded->size(), size_t(0));
}

TEST(esp_slip_decode_rejects_a_frame_with_no_delimiters) {
    CHECK(!slip_decode({0x01, 0x02}).has_value());
}

TEST(esp_slip_decode_rejects_a_truncated_frame) {
    CHECK(!slip_decode({0xC0, 0x01, 0x02}).has_value());
}

TEST(esp_slip_decode_rejects_a_dangling_escape) {
    CHECK(!slip_decode({0xC0, 0xDB, 0xC0}).has_value());
}

TEST(esp_slip_decode_rejects_an_unknown_escape_pair) {
    CHECK(!slip_decode({0xC0, 0xDB, 0x99, 0xC0}).has_value());
}

// -------------------------------------------------------------- checksum ---

TEST(esp_checksum_of_no_data_is_the_bare_seed) {
    CHECK_EQ(int(checksum(nullptr, 0)), 0xEF);
}

TEST(esp_checksum_xors_every_byte_into_the_seed) {
    const uint8_t data[] = {0x01, 0x02, 0x04};
    CHECK_EQ(int(checksum(data, 3)), 0xEF ^ 0x01 ^ 0x02 ^ 0x04);
}

TEST(esp_checksum_cancels_repeated_bytes) {
    const uint8_t data[] = {0x5A, 0x5A};
    CHECK_EQ(int(checksum(data, 2)), 0xEF);
}

// ------------------------------------------------------ packet structure ---

TEST(esp_request_header_is_direction_command_size_and_checksum) {
    auto body = request_body(Command::ReadReg, {0xAA, 0xBB});
    CHECK_EQ(body.size(), size_t(10));
    CHECK_EQ(int(body[0]), 0x00);   // request direction
    CHECK_EQ(int(body[1]), 0x0A);   // READ_REG
    CHECK_EQ(int(body[2]), 0x02);   // size, little-endian
    CHECK_EQ(int(body[3]), 0x00);
    CHECK_EQ(int(body[4]), 0x00);   // checksum field, zero for non-FLASH_DATA
    CHECK_EQ(int(body[7]), 0x00);
    CHECK_EQ(int(body[8]), 0xAA);
    CHECK_EQ(int(body[9]), 0xBB);
}

TEST(esp_request_size_field_is_little_endian_for_large_payloads) {
    auto body = request_body(Command::FlashData, std::vector<uint8_t>(0x0102, 0x00));
    CHECK_EQ(int(body[2]), 0x02);
    CHECK_EQ(int(body[3]), 0x01);
}

TEST(esp_packet_is_the_body_slip_framed) {
    auto body = request_body(Command::Sync, {0x01});
    auto framed = packet(Command::Sync, {0x01});
    CHECK(framed == slip_encode(body));
}

TEST(esp_sync_command_carries_the_magic_payload) {
    auto decoded = slip_decode(cmd_sync());
    CHECK(decoded.has_value());
    CHECK_EQ(decoded->size(), size_t(8 + 36));
    CHECK_EQ(int((*decoded)[1]), 0x08);   // SYNC
    CHECK_EQ(int((*decoded)[2]), 36);     // payload size
    CHECK_EQ(int((*decoded)[8]), 0x07);
    CHECK_EQ(int((*decoded)[9]), 0x07);
    CHECK_EQ(int((*decoded)[10]), 0x12);
    CHECK_EQ(int((*decoded)[11]), 0x20);
    for (size_t i = 12; i < decoded->size(); ++i) CHECK_EQ(int((*decoded)[i]), 0x55);
}

TEST(esp_flash_begin_carries_four_little_endian_words) {
    auto decoded = slip_decode(cmd_flash_begin(0x1000, 1, 0x1000, 0x40200000));
    CHECK(decoded.has_value());
    CHECK_EQ(int((*decoded)[1]), 0x02);   // FLASH_BEGIN
    CHECK_EQ(int((*decoded)[2]), 16);     // four u32 words
    CHECK_EQ(int((*decoded)[8]), 0x00);   // total size 0x1000
    CHECK_EQ(int((*decoded)[9]), 0x10);
    CHECK_EQ(int((*decoded)[12]), 0x01);  // one block
    CHECK_EQ(int((*decoded)[16]), 0x00);  // block size 0x1000
    CHECK_EQ(int((*decoded)[17]), 0x10);
    CHECK_EQ(int((*decoded)[23]), 0x40);  // offset 0x40200000, top byte
}

TEST(esp_flash_data_checksums_only_the_data_not_its_header_words) {
    const std::vector<uint8_t> data{0x11, 0x22};
    auto decoded = slip_decode(cmd_flash_data(data.data(), data.size(), 7));
    CHECK(decoded.has_value());
    CHECK_EQ(int((*decoded)[1]), 0x03);                    // FLASH_DATA
    CHECK_EQ(int((*decoded)[2]), int(16 + data.size()));   // header words + data
    CHECK_EQ(int((*decoded)[4]), 0xEF ^ 0x11 ^ 0x22);      // checksum field
    CHECK_EQ(int((*decoded)[5]), 0x00);                    // ... zero-extended
    CHECK_EQ(int((*decoded)[8]), 0x02);                    // data length
    CHECK_EQ(int((*decoded)[12]), 0x07);                   // sequence number
    CHECK_EQ(int((*decoded)[24]), 0x11);                   // the data itself
    CHECK_EQ(int((*decoded)[25]), 0x22);
}

TEST(esp_flash_data_escapes_data_bytes_that_collide_with_slip) {
    const std::vector<uint8_t> data{0xC0, 0xDB};
    auto raw = cmd_flash_data(data.data(), data.size(), 0);
    // Both bytes had to grow an escape, so the frame is longer than the body.
    auto decoded = slip_decode(raw);
    CHECK(decoded.has_value());
    CHECK_EQ(raw.size(), decoded->size() + 4);   // two delimiters, two escapes
    CHECK_EQ(int(decoded->back()), 0xDB);
    CHECK_EQ(int((*decoded)[decoded->size() - 2]), 0xC0);
}

TEST(esp_flash_end_reboot_flag_is_zero_to_run_the_firmware) {
    auto reboot = slip_decode(cmd_flash_end(true));
    auto stay = slip_decode(cmd_flash_end(false));
    CHECK(reboot.has_value() && stay.has_value());
    CHECK_EQ(int((*reboot)[1]), 0x04);   // FLASH_END
    CHECK_EQ(int((*reboot)[8]), 0x00);
    CHECK_EQ(int((*stay)[8]), 0x01);
}

TEST(esp_read_reg_carries_the_address_little_endian) {
    auto decoded = slip_decode(cmd_read_reg(kChipIdRegister));
    CHECK(decoded.has_value());
    CHECK_EQ(int((*decoded)[1]), 0x0A);
    CHECK_EQ(int((*decoded)[8]), 0x10);
    CHECK_EQ(int((*decoded)[9]), 0x00);
    CHECK_EQ(int((*decoded)[10]), 0xF0);
    CHECK_EQ(int((*decoded)[11]), 0x3F);
}

// --------------------------------------------------------------- replies ---

TEST(esp_parse_reply_reads_command_value_and_status) {
    auto frame = reply_frame(Command::ReadReg, 0xFFF0C101);
    auto body = slip_decode(frame);
    CHECK(body.has_value());
    auto reply = parse_reply(*body);
    CHECK(reply.has_value());
    CHECK_EQ(int(reply->cmd), 0x0A);
    CHECK(reply->value == kEsp8266ChipId);
    CHECK(reply->status_ok);
}

TEST(esp_parse_reply_reports_a_failing_status_and_its_error_code) {
    auto body = slip_decode(reply_frame(Command::FlashData, 0, 0x01, 0x05));
    auto reply = parse_reply(*body);
    CHECK(reply.has_value());
    CHECK(!reply->status_ok);
    CHECK_EQ(int(reply->error), 0x05);
}

TEST(esp_parse_reply_rejects_a_request_direction_byte) {
    CHECK(!parse_reply(request_body(Command::Sync, {0x00, 0x00})).has_value());
}

TEST(esp_parse_reply_rejects_a_short_body) {
    CHECK(!parse_reply({0x01, 0x08, 0x02}).has_value());
}

TEST(esp_parse_reply_rejects_a_size_field_that_disagrees_with_the_bytes) {
    std::vector<uint8_t> body{0x01, 0x08, 0x08, 0x00, 0, 0, 0, 0, 0x00, 0x00};
    CHECK(!parse_reply(body).has_value());   // claims 8 payload bytes, carries 2
}

// ---------------------------------------------------------------- board ----

TEST(esp_board_entry_uses_the_rom_protocol_and_4k_blocks) {
    const Board* b = find_board_by_id("esp8266");
    CHECK(b != nullptr);
    CHECK(b->protocol == Protocol::EspRom);
    CHECK_EQ(int(b->page_size), 4096);
    CHECK_EQ(int(b->flash_size), 4 * 1024 * 1024);
}

TEST(esp_board_claims_the_cp2102_bridge) {
    auto matches = find_boards_by_usb({0x10C4, 0xEA60});
    CHECK_EQ(matches.size(), size_t(1));
    CHECK(matches[0]->id == "esp8266");
}

TEST(esp_board_leaves_the_ch340_id_to_the_nano_so_detection_stays_unambiguous) {
    // A CH340 is the same silicon on both boards and carries nothing that tells
    // them apart, so it deliberately resolves to exactly one board rather than
    // making every Nano need --board.
    auto matches = find_boards_by_usb({0x1A86, 0x7523});
    CHECK_EQ(matches.size(), size_t(1));
    CHECK(matches[0]->id == "nano");
}

// --------------------------------------------------------------- upload ----

TEST(esp_upload_succeeds_against_a_scripted_rom) {
    FakeSerialPort port;
    queue_successful_session(port, 1);
    auto result = upload_esp_rom(port, "/dev/fake", esp(), image_of(1000), nullptr);
    CHECK(result.ok);
    CHECK(result.error.empty());
    CHECK_EQ(result.baud_used, 115200);
}

TEST(esp_upload_holds_gpio0_low_across_the_release_of_reset) {
    FakeSerialPort port;
    queue_successful_session(port, 1);
    upload_esp_rom(port, "/dev/fake", esp(), image_of(1000), nullptr);

    // DTR drives GPIO0, RTS drives RESET, and each asserted line pulls its pin
    // low. GPIO0 must already be low when RESET is released.
    CHECK(port.dtr_history.size() >= 3);
    CHECK(port.rts_history.size() >= 2);
    CHECK_EQ(int(port.dtr_history[0]), 0);   // GPIO0 high
    CHECK_EQ(int(port.rts_history[0]), 1);   // RESET asserted low
    CHECK_EQ(int(port.dtr_history[1]), 1);   // GPIO0 pulled low first
    CHECK_EQ(int(port.rts_history[1]), 0);   // then RESET released
    CHECK_EQ(int(port.dtr_history[2]), 0);   // GPIO0 let go afterwards
}

TEST(esp_upload_sends_the_expected_command_sequence) {
    FakeSerialPort port;
    queue_successful_session(port, 1);
    upload_esp_rom(port, "/dev/fake", esp(), image_of(1000), nullptr);

    // Pull the command byte out of each frame the driver wrote.
    std::vector<uint8_t> commands;
    std::vector<uint8_t> frame;
    for (uint8_t b : port.written) {
        frame.push_back(b);
        if (frame.size() > 1 && b == kSlipEnd) {
            auto body = slip_decode(frame);
            CHECK(body.has_value());
            commands.push_back((*body)[1]);
            frame.clear();
        } else if (frame.size() == 1 && b != kSlipEnd) {
            frame.clear();
        }
    }
    CHECK_EQ(commands.size(), size_t(5));
    CHECK_EQ(int(commands[0]), 0x08);   // SYNC
    CHECK_EQ(int(commands[1]), 0x0A);   // READ_REG for the chip ID
    CHECK_EQ(int(commands[2]), 0x02);   // FLASH_BEGIN
    CHECK_EQ(int(commands[3]), 0x03);   // FLASH_DATA
    CHECK_EQ(int(commands[4]), 0x04);   // FLASH_END
}

TEST(esp_upload_pads_a_short_final_block_to_the_full_block_size) {
    FakeSerialPort port;
    queue_successful_session(port, 1);
    upload_esp_rom(port, "/dev/fake", esp(), image_of(1000), nullptr);

    // The FLASH_DATA frame is the fourth one written; its data length word must
    // be a whole 4096-byte block even though only 1000 bytes were supplied.
    size_t frames = 0;
    std::vector<uint8_t> frame;
    bool checked = false;
    for (uint8_t b : port.written) {
        frame.push_back(b);
        if (frame.size() > 1 && b == kSlipEnd) {
            ++frames;
            if (frames == 4) {
                auto body = slip_decode(frame);
                CHECK(body.has_value());
                CHECK_EQ(body->size(), size_t(8 + 16 + 4096));
                CHECK_EQ(int((*body)[8]), 0x00);    // 4096, little-endian
                CHECK_EQ(int((*body)[9]), 0x10);
                CHECK_EQ(int((*body)[24 + 999]), 0xAB);   // last real byte
                CHECK_EQ(int((*body)[24 + 1000]), 0xFF);  // first padding byte
                checked = true;
            }
            frame.clear();
        } else if (frame.size() == 1 && b != kSlipEnd) {
            frame.clear();
        }
    }
    CHECK(checked);
}

TEST(esp_upload_writes_every_block_of_a_multi_block_image) {
    FakeSerialPort port;
    queue_successful_session(port, 3);
    auto result = upload_esp_rom(port, "/dev/fake", esp(), image_of(4096 * 2 + 5), nullptr);
    CHECK(result.ok);
}

TEST(esp_upload_reports_progress_for_each_block) {
    FakeSerialPort port;
    queue_successful_session(port, 2);
    std::vector<std::string> log;
    upload_esp_rom(port, "/dev/fake", esp(), image_of(4096 + 1),
                   [&](const std::string& m) { log.push_back(m); });
    size_t wrote = 0;
    for (const std::string& m : log)
        if (m.rfind("wrote ", 0) == 0) ++wrote;
    CHECK_EQ(wrote, size_t(2));
}

TEST(esp_upload_drains_the_surplus_sync_replies_before_moving_on) {
    FakeSerialPort port;
    queue_successful_session(port, 1);   // queues three extra SYNC answers
    auto result = upload_esp_rom(port, "/dev/fake", esp(), image_of(16), nullptr);
    CHECK(result.ok);
    CHECK_EQ(port.to_read.size(), size_t(0));   // nothing left unread
}

// ---------------------------------------------------------- failure paths ---

TEST(esp_upload_reports_the_sync_stage_when_the_rom_is_silent) {
    FakeSerialPort port;   // nothing queued at all
    auto result = upload_esp_rom(port, "/dev/fake", esp(), image_of(16), nullptr);
    CHECK(!result.ok);
    CHECK(result.stage == "sync");
    CHECK(result.error.find("115200") != std::string::npos);
}

TEST(esp_upload_reports_the_chip_stage_on_a_foreign_chip_id) {
    FakeSerialPort port;
    queue_successful_session(port, 1, 0x00F01D83);
    auto result = upload_esp_rom(port, "/dev/fake", esp(), image_of(16), nullptr);
    CHECK(!result.ok);
    CHECK(result.stage == "chip");
    CHECK(result.error.find("0x00f01d83") != std::string::npos);   // what was found
    CHECK(result.error.find("0xfff0c101") != std::string::npos);   // what was wanted
}

TEST(esp_upload_reports_the_erase_stage_when_flash_begin_fails) {
    FakeSerialPort port;
    queue(port, reply_frame(Command::Sync));
    queue(port, reply_frame(Command::ReadReg, kEsp8266ChipId));
    queue(port, reply_frame(Command::FlashBegin, 0, 0x01, 0x05));   // bad status
    auto result = upload_esp_rom(port, "/dev/fake", esp(), image_of(16), nullptr);
    CHECK(!result.ok);
    CHECK(result.stage == "erase");
}

TEST(esp_upload_reports_the_write_stage_and_the_failing_block) {
    FakeSerialPort port;
    queue(port, reply_frame(Command::Sync));
    queue(port, reply_frame(Command::ReadReg, kEsp8266ChipId));
    queue(port, reply_frame(Command::FlashBegin));
    queue(port, reply_frame(Command::FlashData));
    queue(port, reply_frame(Command::FlashData, 0, 0x01, 0x09));   // second fails
    auto result = upload_esp_rom(port, "/dev/fake", esp(), image_of(4096 * 2), nullptr);
    CHECK(!result.ok);
    CHECK(result.stage == "write");
    CHECK(result.error.find("block 1") != std::string::npos);
    CHECK(result.error.find("4096") != std::string::npos);   // the byte offset
}

TEST(esp_upload_reports_the_finish_stage_when_flash_end_fails) {
    FakeSerialPort port;
    queue(port, reply_frame(Command::Sync));
    queue(port, reply_frame(Command::ReadReg, kEsp8266ChipId));
    queue(port, reply_frame(Command::FlashBegin));
    queue(port, reply_frame(Command::FlashData));
    queue(port, reply_frame(Command::FlashEnd, 0, 0x01, 0x02));
    auto result = upload_esp_rom(port, "/dev/fake", esp(), image_of(16), nullptr);
    CHECK(!result.ok);
    CHECK(result.stage == "finish");
}

TEST(esp_upload_treats_a_truncated_reply_as_a_failure_not_a_hang) {
    FakeSerialPort port;
    queue(port, reply_frame(Command::Sync));
    queue(port, reply_frame(Command::ReadReg, kEsp8266ChipId));
    auto begin = reply_frame(Command::FlashBegin);
    begin.resize(begin.size() - 3);   // cut the tail off, delimiter included
    queue(port, begin);
    auto result = upload_esp_rom(port, "/dev/fake", esp(), image_of(16), nullptr);
    CHECK(!result.ok);
    CHECK(result.stage == "erase");
}

TEST(esp_upload_survives_boot_chatter_before_the_first_frame) {
    FakeSerialPort port;
    // A real ESP8266 dumps garbage at a different baud rate before the ROM
    // loader answers; it must be skipped, not mistaken for a frame.
    queue(port, {0x1B, 0x5B, 0x00, 0xFF});
    queue_successful_session(port, 1);
    auto result = upload_esp_rom(port, "/dev/fake", esp(), image_of(16), nullptr);
    CHECK(result.ok);
}

TEST(esp_upload_rejects_an_image_larger_than_flash) {
    FakeSerialPort port;
    HexImage img;
    img.data.assign(esp().flash_size + 1, 0x00);
    auto result = upload_esp_rom(port, "/dev/fake", esp(), img, nullptr);
    CHECK(!result.ok);
    CHECK(result.stage == "size");
    CHECK_EQ(port.written.size(), size_t(0));   // never touched the port
}

TEST(esp_upload_rejects_an_empty_image) {
    FakeSerialPort port;
    auto result = upload_esp_rom(port, "/dev/fake", esp(), HexImage{}, nullptr);
    CHECK(!result.ok);
    CHECK(result.stage == "size");
}
