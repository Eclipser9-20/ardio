#include "harness.h"
#include "ardio/board.h"
#include "ardio/platform/fake_serial.h"
#include "ardio/protocol/stk500v2.h"

#include <string>

using namespace ardio;
using namespace ardio::stk500v2;

namespace {

// --------------------------------------------------------- test scripting ---

// Builds the frame a bootloader sends back for `command`: the command byte, a
// status byte, then whatever that command returns.
std::vector<uint8_t> answer_frame(uint8_t sequence, Command command,
                                  uint8_t status = kStatusCmdOk,
                                  const std::vector<uint8_t>& data = {}) {
    std::vector<uint8_t> body{uint8_t(command), status};
    body.insert(body.end(), data.begin(), data.end());
    return encode_frame(sequence, body);
}

void queue(FakeSerialPort& port, const std::vector<uint8_t>& bytes) {
    port.to_read.insert(port.to_read.end(), bytes.begin(), bytes.end());
}

// A stand-in Mega 2560: 256KB of flash in 256-byte pages, one baud rate.
Board mega() {
    Board b;
    b.id = "mega";
    b.name = "Arduino Mega 2560 (ATmega2560)";
    b.mcu = "atmega2560";
    b.baud_rates = {115200};
    b.flash_size = 256 * 1024;
    b.page_size = 256;
    b.signature = {0x1E, 0x98, 0x01};
    return b;
}

HexImage image_of(size_t len, uint8_t fill = 0xAB) {
    HexImage img;
    img.data.assign(len, fill);
    return img;
}

// The answers a healthy bootloader gives for a whole upload of `pages` pages.
// Sequence numbers start at 1 and every answer echoes the one it replies to.
void queue_successful_session(FakeSerialPort& port, int pages,
                              const std::array<uint8_t, 3>& sig = {0x1E, 0x98, 0x01}) {
    uint8_t seq = 1;
    queue(port, answer_frame(seq++, Command::SignOn,
                             kStatusCmdOk, {8, 'S', 'T', 'K', '5', '0', '0', '_', '2'}));
    queue(port, answer_frame(seq++, Command::EnterProgmodeIsp));
    for (int i = 0; i < 3; ++i)
        queue(port, answer_frame(seq++, Command::ReadSignatureIsp, kStatusCmdOk,
                                 {sig[size_t(i)], kStatusCmdOk}));
    for (int i = 0; i < pages; ++i) {
        queue(port, answer_frame(seq++, Command::LoadAddress));
        queue(port, answer_frame(seq++, Command::ProgramFlashIsp));
    }
    queue(port, answer_frame(seq++, Command::LeaveProgmodeIsp));
}

void append(std::vector<uint8_t>& out, const std::vector<uint8_t>& more) {
    out.insert(out.end(), more.begin(), more.end());
}

} // namespace

// ---------------------------------------------------------------- framing ---

TEST(v2_frame_is_start_sequence_length_token_body_checksum) {
    // Hand-computed: 1B 01 00 01 0E 01, checksum = XOR of all six = 0x14.
    auto f = encode_frame(0x01, {uint8_t(Command::SignOn)});
    CHECK_EQ(f.size(), size_t(7));
    CHECK_EQ(int(f[0]), 0x1B);
    CHECK_EQ(int(f[1]), 0x01);
    CHECK_EQ(int(f[2]), 0x00);
    CHECK_EQ(int(f[3]), 0x01);
    CHECK_EQ(int(f[4]), 0x0E);
    CHECK_EQ(int(f[5]), 0x01);
    CHECK_EQ(int(f[6]), 0x14);
}

TEST(v2_frame_length_is_big_endian_over_a_long_body) {
    std::vector<uint8_t> body(0x0123, 0x00);
    body[0] = uint8_t(Command::ProgramFlashIsp);
    auto f = encode_frame(0x2A, body);
    CHECK_EQ(f.size(), body.size() + kFrameOverhead);
    CHECK_EQ(int(f[1]), 0x2A);
    CHECK_EQ(int(f[2]), 0x01);   // high byte of the length first
    CHECK_EQ(int(f[3]), 0x23);
    CHECK_EQ(int(f[4]), 0x0E);
}

TEST(v2_checksum_covers_the_start_byte) {
    // XOR of the frame with its own checksum byte cancels to zero only if the
    // checksum was taken over every byte including MESSAGE_START.
    auto f = encode_frame(0x07, {0x01, 0x02, 0x03});
    CHECK_EQ(int(checksum(f.data(), f.size())), 0);
    // Dropping the start byte from the sum would give a different answer.
    CHECK(checksum(f.data() + 1, f.size() - 1) != 0);
}

TEST(v2_round_trips_a_frame) {
    std::vector<uint8_t> body{uint8_t(Command::GetParameter), kParamSoftwareMajor};
    FrameError err = FrameError::BadStart;
    auto parsed = parse_frame(encode_frame(0x11, body), &err);
    CHECK(parsed.has_value());
    CHECK(err == FrameError::None);
    CHECK_EQ(int(parsed->sequence), 0x11);
    CHECK(parsed->body == body);
}

TEST(v2_parse_rejects_a_corrupted_frame) {
    auto f = encode_frame(0x03, {0x01, 0x02, 0x03});
    f[6] ^= 0x40;                      // flip a bit in the body
    FrameError err = FrameError::None;
    CHECK(!parse_frame(f, &err).has_value());
    CHECK(err == FrameError::BadChecksum);
}

TEST(v2_parse_rejects_a_corrupted_checksum_byte) {
    auto f = encode_frame(0x03, {0x01, 0x02, 0x03});
    f.back() ^= 0xFF;
    FrameError err = FrameError::None;
    CHECK(!parse_frame(f, &err).has_value());
    CHECK(err == FrameError::BadChecksum);
}

TEST(v2_parse_rejects_a_bad_start_token_length_and_runt) {
    auto good = encode_frame(0x03, {0x01, 0x02});
    FrameError err = FrameError::None;

    auto short_frame = std::vector<uint8_t>(good.begin(), good.begin() + 4);
    CHECK(!parse_frame(short_frame, &err).has_value());
    CHECK(err == FrameError::TooShort);

    auto bad_start = good;
    bad_start[0] = 0x1C;
    CHECK(!parse_frame(bad_start, &err).has_value());
    CHECK(err == FrameError::BadStart);

    auto bad_token = good;
    bad_token[4] = 0x0F;
    CHECK(!parse_frame(bad_token, &err).has_value());
    CHECK(err == FrameError::BadToken);

    auto bad_length = good;
    bad_length[3] = 0x05;              // claims five body bytes, carries two
    CHECK(!parse_frame(bad_length, &err).has_value());
    CHECK(err == FrameError::LengthMismatch);
}

TEST(v2_parse_frame_tolerates_a_null_error_pointer) {
    CHECK(parse_frame(encode_frame(1, {0x01}), nullptr).has_value());
    CHECK(!parse_frame({0x1B}, nullptr).has_value());
}

// ---------------------------------------------------------------- answers ---

TEST(v2_answer_splits_command_status_and_data) {
    auto a = parse_answer({uint8_t(Command::ReadSignatureIsp), kStatusCmdOk, 0x1E, 0x00});
    CHECK(a.has_value());
    CHECK_EQ(int(a->command), 0x1B);
    CHECK_EQ(int(a->status), 0x00);
    CHECK_EQ(a->data.size(), size_t(2));
    CHECK_EQ(int(a->data[0]), 0x1E);
}

TEST(v2_answer_needs_at_least_a_command_and_a_status) {
    CHECK(!parse_answer({}).has_value());
    CHECK(!parse_answer({0x01}).has_value());
    CHECK(parse_answer({0x01, 0x00}).has_value());
}

TEST(v2_failure_text_names_the_command_and_the_status) {
    std::string msg = describe_failure(uint8_t(Command::LoadAddress), kStatusCmdFailed);
    CHECK(msg.find("CMD_LOAD_ADDRESS") != std::string::npos);
    CHECK(msg.find("STATUS_CMD_FAILED") != std::string::npos);
    CHECK(msg.find("0xc0") != std::string::npos);

    // An unknown status still reports its value rather than going quiet.
    std::string odd = describe_failure(uint8_t(Command::SignOn), 0x42);
    CHECK(odd.find("CMD_SIGN_ON") != std::string::npos);
    CHECK(odd.find("0x42") != std::string::npos);
}

TEST(v2_command_names_cover_every_command_used) {
    CHECK(command_name(0x01) == "CMD_SIGN_ON");
    CHECK(command_name(0x02) == "CMD_SET_PARAMETER");
    CHECK(command_name(0x03) == "CMD_GET_PARAMETER");
    CHECK(command_name(0x06) == "CMD_LOAD_ADDRESS");
    CHECK(command_name(0x10) == "CMD_ENTER_PROGMODE_ISP");
    CHECK(command_name(0x11) == "CMD_LEAVE_PROGMODE_ISP");
    CHECK(command_name(0x13) == "CMD_PROGRAM_FLASH_ISP");
    CHECK(command_name(0x14) == "CMD_READ_FLASH_ISP");
    CHECK(command_name(0x1B) == "CMD_READ_SIGNATURE_ISP");
    CHECK(command_name(0x77) == "command 0x77");
}

// --------------------------------------------------------- command bodies ---

TEST(v2_sign_on_and_parameter_bodies) {
    CHECK(body_sign_on() == std::vector<uint8_t>{0x01});
    CHECK(body_get_parameter(kParamSoftwareMajor) ==
          (std::vector<uint8_t>{0x03, 0x91}));
    CHECK(body_set_parameter(kParamResetPolarity, 0x01) ==
          (std::vector<uint8_t>{0x02, 0x9E, 0x01}));
}

TEST(v2_load_address_is_big_endian_with_the_flash_bit_set) {
    // Byte address 0x0100 is word address 0x0080.
    auto b = body_load_address(0x0080);
    CHECK_EQ(b.size(), size_t(5));
    CHECK_EQ(int(b[0]), 0x06);
    CHECK_EQ(int(b[1]), 0x80);   // high byte first, top bit marks flash
    CHECK_EQ(int(b[2]), 0x00);
    CHECK_EQ(int(b[3]), 0x00);
    CHECK_EQ(int(b[4]), 0x80);
}

TEST(v2_load_address_carries_word_addresses_past_64k) {
    // The Mega case: byte address 0x20000 is word address 0x10000, which does
    // not fit in the 16 bits STK500v1 had. Dropping the high bytes would send
    // this page to word address 0x0000 and quietly overwrite the sketch's start.
    auto b = body_load_address(0x00010000);
    CHECK_EQ(b.size(), size_t(5));
    CHECK_EQ(int(b[0]), 0x06);
    CHECK_EQ(int(b[1]), 0x80);
    CHECK_EQ(int(b[2]), 0x01);
    CHECK_EQ(int(b[3]), 0x00);
    CHECK_EQ(int(b[4]), 0x00);

    // The last word address on a 256KB part, 0x1FFFF.
    auto top = body_load_address(0x0001FFFF);
    CHECK_EQ(int(top[1]), 0x80);
    CHECK_EQ(int(top[2]), 0x01);
    CHECK_EQ(int(top[3]), 0xFF);
    CHECK_EQ(int(top[4]), 0xFF);

    // A word address whose own bit 31 is set must not collide with the flash
    // marker: only the low seven bits of the top byte are address.
    auto masked = body_load_address(0xFF000000u);
    CHECK_EQ(int(masked[1]), 0xFF);
}

TEST(v2_program_flash_sends_byte_count_big_endian_then_data) {
    uint8_t data[256];
    for (int i = 0; i < 256; ++i) data[i] = uint8_t(i);
    auto b = body_program_flash_isp(data, 256);
    CHECK_EQ(b.size(), size_t(10 + 256));
    CHECK_EQ(int(b[0]), 0x13);
    CHECK_EQ(int(b[1]), 0x01);   // 256 == 0x0100, high byte first
    CHECK_EQ(int(b[2]), 0x00);
    CHECK_EQ(int(b[3]), 0xC1);   // paged write with a timed delay
    CHECK_EQ(int(b[5]), 0x40);   // load page, low byte
    CHECK_EQ(int(b[6]), 0x4C);   // write program memory page
    CHECK_EQ(int(b[10]), 0);     // first data byte
    CHECK_EQ(int(b.back()), 255);
}

TEST(v2_read_flash_and_read_signature_bodies) {
    auto r = body_read_flash_isp(0x0100);
    CHECK_EQ(r.size(), size_t(4));
    CHECK_EQ(int(r[0]), 0x14);
    CHECK_EQ(int(r[1]), 0x01);
    CHECK_EQ(int(r[2]), 0x00);
    CHECK_EQ(int(r[3]), 0x20);

    auto s = body_read_signature_isp(2);
    CHECK_EQ(s.size(), size_t(6));
    CHECK_EQ(int(s[0]), 0x1B);
    CHECK_EQ(int(s[1]), 0x00);
    CHECK_EQ(int(s[2]), 0x30);
    CHECK_EQ(int(s[3]), 0x00);
    CHECK_EQ(int(s[4]), 0x02);   // which signature byte to fetch
    CHECK_EQ(int(s[5]), 0x00);
}

TEST(v2_progmode_bodies_carry_the_isp_enable_instruction) {
    auto enter = body_enter_progmode_isp();
    CHECK_EQ(enter.size(), size_t(12));
    CHECK_EQ(int(enter[0]), 0x10);
    CHECK_EQ(int(enter[6]), 0x53);   // poll value echoed by the target
    CHECK_EQ(int(enter[7]), 3);      // on the third clocked byte
    CHECK_EQ(int(enter[8]), 0xAC);   // programming enable
    CHECK_EQ(int(enter[9]), 0x53);

    auto leave = body_leave_progmode_isp();
    CHECK_EQ(leave.size(), size_t(3));
    CHECK_EQ(int(leave[0]), 0x11);
}

// ----------------------------------------------------------------- upload ---

TEST(v2_upload_writes_exactly_the_expected_conversation) {
    FakeSerialPort port;
    queue_successful_session(port, 2);
    HexImage image = image_of(256 + 4);

    auto result = upload_stk500v2(port, "/dev/fake", mega(), image, nullptr);
    CHECK(result.ok);
    CHECK(result.error.empty());
    CHECK_EQ(result.baud_used, 115200);

    std::vector<uint8_t> expected;
    uint8_t seq = 1;
    append(expected, encode_frame(seq++, body_sign_on()));
    append(expected, encode_frame(seq++, body_enter_progmode_isp()));
    for (uint8_t i = 0; i < 3; ++i)
        append(expected, encode_frame(seq++, body_read_signature_isp(i)));
    append(expected, encode_frame(seq++, body_load_address(0)));
    append(expected, encode_frame(seq++, body_program_flash_isp(image.data.data(), 256)));
    append(expected, encode_frame(seq++, body_load_address(128)));   // word address
    append(expected, encode_frame(seq++,
                                  body_program_flash_isp(image.data.data() + 256, 4)));
    append(expected, encode_frame(seq++, body_leave_progmode_isp()));

    CHECK_EQ(port.written.size(), expected.size());
    CHECK(port.written == expected);
    CHECK_EQ(port.to_read.size(), size_t(0));   // every answer was consumed
}

TEST(v2_upload_starts_the_conversation_with_a_sign_on_frame) {
    FakeSerialPort port;
    queue_successful_session(port, 1);
    upload_stk500v2(port, "/dev/fake", mega(), image_of(16), nullptr);
    CHECK(port.written.size() > 7);
    CHECK_EQ(int(port.written[0]), 0x1B);
    CHECK_EQ(int(port.written[1]), 0x01);   // first sequence number
    CHECK_EQ(int(port.written[2]), 0x00);
    CHECK_EQ(int(port.written[3]), 0x01);
    CHECK_EQ(int(port.written[4]), 0x0E);
    CHECK_EQ(int(port.written[5]), 0x01);
    CHECK_EQ(int(port.written[6]), 0x14);
}

TEST(v2_upload_toggles_reset_before_signing_on) {
    FakeSerialPort port;
    queue_successful_session(port, 1);
    upload_stk500v2(port, "/dev/fake", mega(), image_of(16), nullptr);
    CHECK_EQ(port.dtr_history.size(), size_t(2));
    CHECK_EQ(int(port.dtr_history[0]), 1);
    CHECK_EQ(int(port.dtr_history[1]), 0);
}

TEST(v2_upload_reports_progress_once_per_page) {
    FakeSerialPort port;
    queue_successful_session(port, 3);
    std::vector<std::string> log;
    upload_stk500v2(port, "/dev/fake", mega(), image_of(256 * 2 + 1),
                    [&](const std::string& m) { log.push_back(m); });
    size_t wrote = 0;
    for (const std::string& m : log)
        if (m.rfind("wrote ", 0) == 0) ++wrote;
    CHECK_EQ(wrote, size_t(3));
}

TEST(v2_upload_loads_high_addresses_for_a_sketch_past_128k) {
    // A page at byte offset 0x20000 is word address 0x10000; the frame for it
    // must carry 0x80 0x01 0x00 0x00, not a truncated 16-bit address.
    FakeSerialPort port;
    const int pages = 4;
    queue_successful_session(port, pages);
    HexImage image = image_of(256 * pages);
    image.base_address = 0x20000;

    auto result = upload_stk500v2(port, "/dev/fake", mega(), image, nullptr);
    CHECK(result.ok);

    auto wanted = body_load_address(0x10000);
    bool found = false;
    for (size_t i = 0; i + wanted.size() <= port.written.size(); ++i)
        if (std::equal(wanted.begin(), wanted.end(), port.written.begin() + long(i)))
            found = true;
    CHECK(found);
    CHECK_EQ(int(wanted[1]), 0x80);
    CHECK_EQ(int(wanted[2]), 0x01);
}

// ---------------------------------------------------------- failure paths ---

TEST(v2_upload_reports_the_sync_stage_when_the_board_is_silent) {
    FakeSerialPort port;   // nothing queued at all
    auto result = upload_stk500v2(port, "/dev/fake", mega(), image_of(16), nullptr);
    CHECK(!result.ok);
    CHECK(result.stage == "sync");
    CHECK(result.error.find("115200") != std::string::npos);
}

TEST(v2_upload_detects_a_sequence_number_mismatch) {
    FakeSerialPort port;
    queue(port, answer_frame(1, Command::SignOn));
    queue(port, answer_frame(7, Command::EnterProgmodeIsp));   // should have been 2
    auto result = upload_stk500v2(port, "/dev/fake", mega(), image_of(16), nullptr);
    CHECK(!result.ok);
    CHECK(result.stage == "sync");
    CHECK(result.error.find("sequence number 7") != std::string::npos);
    CHECK(result.error.find("2 was sent") != std::string::npos);
    CHECK(result.error.find("CMD_ENTER_PROGMODE_ISP") != std::string::npos);
}

TEST(v2_upload_reports_a_corrupted_answer_rather_than_trusting_it) {
    FakeSerialPort port;
    queue(port, answer_frame(1, Command::SignOn));
    auto bad = answer_frame(2, Command::EnterProgmodeIsp);
    bad.back() ^= 0xFF;                     // wreck the checksum
    queue(port, bad);
    auto result = upload_stk500v2(port, "/dev/fake", mega(), image_of(16), nullptr);
    CHECK(!result.ok);
    CHECK(result.stage == "sync");
    CHECK(result.error.find("checksum") != std::string::npos);
}

TEST(v2_upload_reports_a_non_ok_status_naming_the_command) {
    FakeSerialPort port;
    queue(port, answer_frame(1, Command::SignOn));
    queue(port, answer_frame(2, Command::EnterProgmodeIsp, kStatusCmdFailed));
    auto result = upload_stk500v2(port, "/dev/fake", mega(), image_of(16), nullptr);
    CHECK(!result.ok);
    CHECK(result.stage == "sync");
    CHECK(result.error.find("CMD_ENTER_PROGMODE_ISP") != std::string::npos);
    CHECK(result.error.find("STATUS_CMD_FAILED") != std::string::npos);
}

TEST(v2_upload_reports_a_signature_mismatch_with_both_signatures) {
    FakeSerialPort port;
    queue_successful_session(port, 1, {0x1E, 0x95, 0x0F});   // an ATmega328P
    auto result = upload_stk500v2(port, "/dev/fake", mega(), image_of(16), nullptr);
    CHECK(!result.ok);
    CHECK(result.stage == "signature");
    CHECK(result.error.find("1e950f") != std::string::npos);   // what was found
    CHECK(result.error.find("1e9801") != std::string::npos);   // what was wanted
}

TEST(v2_upload_reports_the_write_stage_and_the_failing_offset) {
    FakeSerialPort port;
    uint8_t seq = 1;
    queue(port, answer_frame(seq++, Command::SignOn));
    queue(port, answer_frame(seq++, Command::EnterProgmodeIsp));
    const std::array<uint8_t, 3> sig{0x1E, 0x98, 0x01};
    for (int i = 0; i < 3; ++i)
        queue(port, answer_frame(seq++, Command::ReadSignatureIsp, kStatusCmdOk,
                                 {sig[size_t(i)], kStatusCmdOk}));
    queue(port, answer_frame(seq++, Command::LoadAddress));
    queue(port, answer_frame(seq++, Command::ProgramFlashIsp));
    queue(port, answer_frame(seq++, Command::LoadAddress));
    queue(port, answer_frame(seq++, Command::ProgramFlashIsp, kStatusCmdFailed));

    auto result = upload_stk500v2(port, "/dev/fake", mega(), image_of(256 * 2), nullptr);
    CHECK(!result.ok);
    CHECK(result.stage == "write");
    CHECK(result.error.find("CMD_PROGRAM_FLASH_ISP") != std::string::npos);
    CHECK(result.error.find("byte offset 256") != std::string::npos);
}

TEST(v2_upload_reports_the_finish_stage_when_leaving_progmode_fails) {
    FakeSerialPort port;
    uint8_t seq = 1;
    queue(port, answer_frame(seq++, Command::SignOn));
    queue(port, answer_frame(seq++, Command::EnterProgmodeIsp));
    const std::array<uint8_t, 3> sig{0x1E, 0x98, 0x01};
    for (int i = 0; i < 3; ++i)
        queue(port, answer_frame(seq++, Command::ReadSignatureIsp, kStatusCmdOk,
                                 {sig[size_t(i)], kStatusCmdOk}));
    queue(port, answer_frame(seq++, Command::LoadAddress));
    queue(port, answer_frame(seq++, Command::ProgramFlashIsp));
    queue(port, answer_frame(seq++, Command::LeaveProgmodeIsp, kStatusCmdFailed));

    auto result = upload_stk500v2(port, "/dev/fake", mega(), image_of(16), nullptr);
    CHECK(!result.ok);
    CHECK(result.stage == "finish");
    CHECK(result.error.find("CMD_LEAVE_PROGMODE_ISP") != std::string::npos);
}

TEST(v2_upload_treats_a_truncated_answer_as_a_failure_not_a_hang) {
    FakeSerialPort port;
    queue(port, answer_frame(1, Command::SignOn));
    auto enter = answer_frame(2, Command::EnterProgmodeIsp);
    enter.resize(enter.size() - 2);     // cut the tail off
    queue(port, enter);
    auto result = upload_stk500v2(port, "/dev/fake", mega(), image_of(16), nullptr);
    CHECK(!result.ok);
    CHECK(result.stage == "sync");
}

TEST(v2_upload_survives_boot_chatter_before_the_first_frame) {
    FakeSerialPort port;
    // A Mega that has just been reset can emit stray bytes before its
    // bootloader is listening; they must be skipped, not framed.
    queue(port, {0x00, 0xFF, 0x0E, 0x55});
    queue_successful_session(port, 1);
    auto result = upload_stk500v2(port, "/dev/fake", mega(), image_of(16), nullptr);
    CHECK(result.ok);
}

TEST(v2_upload_rejects_an_answer_to_the_wrong_command) {
    FakeSerialPort port;
    queue(port, answer_frame(1, Command::SignOn));
    queue(port, answer_frame(2, Command::ReadFlashIsp));   // never asked for
    auto result = upload_stk500v2(port, "/dev/fake", mega(), image_of(16), nullptr);
    CHECK(!result.ok);
    CHECK(result.error.find("CMD_ENTER_PROGMODE_ISP") != std::string::npos);
    CHECK(result.error.find("CMD_READ_FLASH_ISP") != std::string::npos);
}

TEST(v2_upload_rejects_an_image_larger_than_flash) {
    FakeSerialPort port;
    HexImage img;
    img.data.assign(mega().flash_size + 1, 0x00);
    auto result = upload_stk500v2(port, "/dev/fake", mega(), img, nullptr);
    CHECK(!result.ok);
    CHECK(result.stage == "size");
    CHECK_EQ(port.written.size(), size_t(0));   // never touched the port
}

TEST(v2_upload_rejects_an_empty_image) {
    FakeSerialPort port;
    auto result = upload_stk500v2(port, "/dev/fake", mega(), HexImage{}, nullptr);
    CHECK(!result.ok);
    CHECK(result.stage == "size");
}
