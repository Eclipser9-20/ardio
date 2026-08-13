#include "ardio/protocol/stk500v2.h"

#include <array>
#include <chrono>
#include <thread>

namespace ardio::stk500v2 {
namespace {

constexpr int kReadTimeoutMs = 500;

// A silent port returns nothing rather than blocking, so a byte-at-a-time read
// needs a bound on how many empty reads count as "gone quiet".
constexpr int kMaxEmptyReads = 8;

// How much leading noise to step over while hunting for MESSAGE_START. A Mega
// that has just been reset can emit a few stray bytes before its bootloader is
// listening, and those must not be mistaken for a frame.
constexpr int kMaxLeadingNoise = 256;

void sleep_ms(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

std::string hex2(uint8_t v) {
    static const char* d = "0123456789abcdef";
    std::string s = "0x";
    s += d[v >> 4];
    s += d[v & 0xF];
    return s;
}

std::string hex3(const std::array<uint8_t, 3>& sig) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (uint8_t b : sig) { s += d[b >> 4]; s += d[b & 0xF]; }
    return s;
}

} // namespace

// -------------------------------------------------------- naming for humans ---

std::string command_name(uint8_t command) {
    switch (command) {
        case uint8_t(Command::SignOn):           return "CMD_SIGN_ON";
        case uint8_t(Command::SetParameter):     return "CMD_SET_PARAMETER";
        case uint8_t(Command::GetParameter):     return "CMD_GET_PARAMETER";
        case uint8_t(Command::LoadAddress):      return "CMD_LOAD_ADDRESS";
        case uint8_t(Command::EnterProgmodeIsp): return "CMD_ENTER_PROGMODE_ISP";
        case uint8_t(Command::LeaveProgmodeIsp): return "CMD_LEAVE_PROGMODE_ISP";
        case uint8_t(Command::ProgramFlashIsp):  return "CMD_PROGRAM_FLASH_ISP";
        case uint8_t(Command::ReadFlashIsp):     return "CMD_READ_FLASH_ISP";
        case uint8_t(Command::ReadSignatureIsp): return "CMD_READ_SIGNATURE_ISP";
        default: return "command " + hex2(command);
    }
}

std::string status_name(uint8_t status) {
    const char* name = nullptr;
    switch (status) {
        case kStatusCmdOk:           name = "STATUS_CMD_OK"; break;
        case kStatusCmdTimeout:      name = "STATUS_CMD_TOUT"; break;
        case kStatusRdyBsyTimeout:   name = "STATUS_RDY_BSY_TOUT"; break;
        case kStatusSetParamMissing: name = "STATUS_SET_PARAM_MISSING"; break;
        case kStatusCmdFailed:       name = "STATUS_CMD_FAILED"; break;
        case kStatusChecksumError:   name = "STATUS_CKSUM_ERROR"; break;
        case kStatusCmdUnknown:      name = "STATUS_CMD_UNKNOWN"; break;
        default: return "status " + hex2(status);
    }
    return std::string(name) + " (" + hex2(status) + ")";
}

std::string describe_failure(uint8_t command, uint8_t status) {
    return command_name(command) + " was refused by the bootloader: " +
           status_name(status);
}

// ------------------------------------------------------------ encode/decode ---

uint8_t checksum(const uint8_t* data, size_t len) {
    uint8_t x = 0;
    for (size_t i = 0; i < len; ++i) x ^= data[i];
    return x;
}

std::vector<uint8_t> encode_frame(uint8_t sequence, const std::vector<uint8_t>& body) {
    std::vector<uint8_t> out;
    out.reserve(body.size() + kFrameOverhead);
    out.push_back(kMessageStart);
    out.push_back(sequence);
    out.push_back(uint8_t((body.size() >> 8) & 0xFF));   // big-endian length
    out.push_back(uint8_t(body.size() & 0xFF));
    out.push_back(kToken);
    out.insert(out.end(), body.begin(), body.end());
    // The checksum covers the start byte too, which is what makes a frame that
    // begins mid-stream fail to close rather than decode into nonsense.
    out.push_back(checksum(out.data(), out.size()));
    return out;
}

std::string frame_error_text(FrameError err) {
    switch (err) {
        case FrameError::None:           return "no error";
        case FrameError::TooShort:       return "the answer was too short to be a frame";
        case FrameError::BadStart:       return "the answer did not begin with MESSAGE_START";
        case FrameError::BadToken:       return "the answer had no TOKEN after its length field";
        case FrameError::LengthMismatch: return "the answer's declared length did not match its size";
        case FrameError::BadChecksum:    return "the answer's checksum did not match its contents";
    }
    return "the answer was malformed";
}

std::optional<Frame> parse_frame(const std::vector<uint8_t>& frame, FrameError* err) {
    auto fail = [&](FrameError e) -> std::optional<Frame> {
        if (err) *err = e;
        return std::nullopt;
    };
    if (err) *err = FrameError::None;

    if (frame.size() < kFrameOverhead) return fail(FrameError::TooShort);
    if (frame[0] != kMessageStart)     return fail(FrameError::BadStart);
    if (frame[4] != kToken)            return fail(FrameError::BadToken);

    const size_t declared = (size_t(frame[2]) << 8) | frame[3];
    if (declared + kFrameOverhead != frame.size()) return fail(FrameError::LengthMismatch);

    // XORing the whole frame, checksum byte included, must cancel to zero.
    if (checksum(frame.data(), frame.size()) != 0) return fail(FrameError::BadChecksum);

    Frame out;
    out.sequence = frame[1];
    out.body.assign(frame.begin() + 5, frame.end() - 1);
    return out;
}

std::optional<Answer> parse_answer(const std::vector<uint8_t>& body) {
    if (body.size() < 2) return std::nullopt;
    Answer a;
    a.command = body[0];
    a.status = body[1];
    a.data.assign(body.begin() + 2, body.end());
    return a;
}

// ---------------------------------------------------------- command bodies ---

std::vector<uint8_t> body_sign_on() {
    return {uint8_t(Command::SignOn)};
}

std::vector<uint8_t> body_get_parameter(uint8_t parameter) {
    return {uint8_t(Command::GetParameter), parameter};
}

std::vector<uint8_t> body_set_parameter(uint8_t parameter, uint8_t value) {
    return {uint8_t(Command::SetParameter), parameter, value};
}

std::vector<uint8_t> body_load_address(uint32_t word_address) {
    // Bit 31 marks the address as a flash address; clear, it would mean EEPROM.
    // See the header for why the three high bytes matter on a Mega.
    return {uint8_t(Command::LoadAddress),
            uint8_t(((word_address >> 24) & 0x7F) | 0x80),
            uint8_t((word_address >> 16) & 0xFF),
            uint8_t((word_address >> 8) & 0xFF),
            uint8_t(word_address & 0xFF)};
}

std::vector<uint8_t> body_enter_progmode_isp() {
    // timeout, stabDelay, cmdexeDelay, synchLoops, byteDelay, pollValue,
    // pollIndex, then the four bytes of the ISP "programming enable"
    // instruction. The delays are in milliseconds and are the conventional
    // values; pollValue 0x53 is the echo byte that instruction returns on the
    // third clocked byte, which is why pollIndex is 3.
    return {uint8_t(Command::EnterProgmodeIsp),
            200, 100, 25, 32, 0, 0x53, 3,
            0xAC, 0x53, 0x00, 0x00};
}

std::vector<uint8_t> body_leave_progmode_isp() {
    return {uint8_t(Command::LeaveProgmodeIsp), 1, 1};  // preDelay, postDelay
}

std::vector<uint8_t> body_program_flash_isp(const uint8_t* data, uint16_t length) {
    std::vector<uint8_t> out;
    out.reserve(size_t(length) + 10);
    out.push_back(uint8_t(Command::ProgramFlashIsp));
    out.push_back(uint8_t((length >> 8) & 0xFF));   // big-endian byte count
    out.push_back(uint8_t(length & 0xFF));
    // mode 0xC1: paged write, issue the page-write instruction at the end of the
    // page, and wait out the write with a timed delay rather than value polling.
    // Value polling is unreliable when the page being written happens to
    // contain the poll value itself, and flash pages full of 0xFF do.
    out.push_back(0xC1);
    out.push_back(10);                              // delay in milliseconds
    out.push_back(0x40);                            // load page, low byte
    out.push_back(0x4C);                            // write program memory page
    out.push_back(0x20);                            // read, used when polling
    out.push_back(0x00);                            // poll value 1
    out.push_back(0x00);                            // poll value 2
    out.insert(out.end(), data, data + length);
    return out;
}

std::vector<uint8_t> body_read_flash_isp(uint16_t length) {
    return {uint8_t(Command::ReadFlashIsp),
            uint8_t((length >> 8) & 0xFF),
            uint8_t(length & 0xFF),
            0x20};                                  // read program memory, low byte
}

std::vector<uint8_t> body_read_signature_isp(uint8_t index) {
    // retAddr 0x00, then the ISP "read signature byte" instruction with the
    // wanted index in its third byte.
    return {uint8_t(Command::ReadSignatureIsp),
            0x00,
            0x30, 0x00, index, 0x00};
}

// ------------------------------------------------------------------ driver ---

namespace {

// Pulses DTR/RTS to yank RESET, dropping the board into its bootloader. The
// Mega's USB bridge drives RESET through the same auto-reset capacitor as the
// smaller boards, so the sequence is the one STK500v1 uses.
void pulse_reset(SerialPort& port) {
    port.set_dtr(true);
    port.set_rts(true);
    sleep_ms(50);
    port.set_dtr(false);
    port.set_rts(false);
    sleep_ms(50);
}

bool read_exact(SerialPort& port, size_t count, std::vector<uint8_t>& out) {
    size_t got = 0;
    int empty = 0;
    out.resize(count);
    while (got < count) {
        size_t n = port.read(out.data() + got, count - got, kReadTimeoutMs);
        if (n == 0) {
            if (++empty >= kMaxEmptyReads) return false;
            continue;
        }
        empty = 0;
        got += n;
    }
    return true;
}

// Pulls one whole frame off the port, stepping over any pre-frame noise.
std::optional<std::vector<uint8_t>> read_frame(SerialPort& port) {
    std::vector<uint8_t> frame;
    int empty = 0;
    for (int skipped = 0; skipped < kMaxLeadingNoise;) {
        uint8_t b = 0;
        if (port.read(&b, 1, kReadTimeoutMs) != 1) {
            if (++empty >= kMaxEmptyReads) return std::nullopt;
            continue;
        }
        empty = 0;
        if (b == kMessageStart) { frame.push_back(b); break; }
        ++skipped;
    }
    if (frame.empty()) return std::nullopt;

    std::vector<uint8_t> header;                      // sequence, length, token
    if (!read_exact(port, 4, header)) return std::nullopt;
    frame.insert(frame.end(), header.begin(), header.end());

    const size_t declared = (size_t(header[1]) << 8) | header[2];
    if (declared > kMaxBodySize) return std::nullopt;

    std::vector<uint8_t> rest;                        // body plus checksum
    if (!read_exact(port, declared + 1, rest)) return std::nullopt;
    frame.insert(frame.end(), rest.begin(), rest.end());
    return frame;
}

struct Transaction {
    bool ok = false;
    Answer answer;
    std::string error;
};

// Sends one command under the next sequence number and reads its answer back.
// Every way this can go wrong produces its own message, because "the board did
// not answer" and "the board refused the command" call for different fixes.
Transaction transact(SerialPort& port, const std::vector<uint8_t>& body, uint8_t& sequence) {
    Transaction t;
    const uint8_t command = body.empty() ? 0 : body[0];
    const uint8_t sent_sequence = sequence;

    auto frame = encode_frame(sent_sequence, body);
    if (!port.write(frame.data(), frame.size())) {
        t.error = "could not send " + command_name(command) + " to the port";
        return t;
    }
    // The number advances whether or not the answer arrives; a bootloader that
    // eventually replies to a command we gave up on must not look like a valid
    // answer to the next one.
    sequence = uint8_t(sequence + 1);

    auto raw = read_frame(port);
    if (!raw) {
        t.error = "no answer to " + command_name(command);
        return t;
    }

    FrameError err = FrameError::None;
    auto parsed = parse_frame(*raw, &err);
    if (!parsed) {
        t.error = "answer to " + command_name(command) + " was rejected: " +
                  frame_error_text(err);
        return t;
    }
    if (parsed->sequence != sent_sequence) {
        t.error = "answer to " + command_name(command) + " carried sequence number " +
                  std::to_string(parsed->sequence) + " but " +
                  std::to_string(sent_sequence) + " was sent; the conversation is "
                  "out of step";
        return t;
    }

    auto answer = parse_answer(parsed->body);
    if (!answer) {
        t.error = "answer to " + command_name(command) +
                  " was too short to carry a status byte";
        return t;
    }
    if (answer->command != command) {
        t.error = "expected an answer to " + command_name(command) + " but got one to " +
                  command_name(answer->command);
        return t;
    }
    if (answer->status != kStatusCmdOk) {
        t.error = describe_failure(answer->command, answer->status);
        return t;
    }

    t.ok = true;
    t.answer = *answer;
    return t;
}

} // namespace

UploadResult upload_stk500v2(SerialPort& port, const std::string& path,
                             const Board& board, const HexImage& image,
                             const ProgressFn& progress) {
    UploadResult result;
    auto report = [&](const std::string& msg) { if (progress) progress(msg); };

    if (image.data.empty()) {
        result.stage = "size";
        result.error = "sketch image is empty";
        return result;
    }
    if (image.data.size() > board.flash_size) {
        result.stage = "size";
        result.error = "sketch is " + std::to_string(image.data.size()) +
                       " bytes but " + board.name + " has only " +
                       std::to_string(board.flash_size) + " bytes of flash";
        return result;
    }

    // The sequence number is per-session and starts at 1; zero is what a
    // bootloader that has not seen a command yet reports, so starting there
    // would make a stale answer indistinguishable from a fresh one.
    uint8_t sequence = 1;

    std::string tried;
    bool signed_on = false;
    for (int baud : board.baud_rates) {
        if (!tried.empty()) tried += ", ";
        tried += std::to_string(baud);

        std::string open_error;
        if (!port.open(path, baud, open_error)) {
            result.stage = "open";
            result.error = open_error;
            return result;
        }
        report("trying " + std::to_string(baud) + " baud");
        pulse_reset(port);

        if (transact(port, body_sign_on(), sequence).ok) {
            signed_on = true;
            result.baud_used = baud;
            break;
        }
        port.close();
    }

    if (!signed_on) {
        result.stage = "sync";
        result.error = "no response from the STK500v2 bootloader on " + path +
                       " at any known baud rate (tried " + tried +
                       "). Check the board is plugged in and not held in reset.";
        return result;
    }
    report("signed on at " + std::to_string(result.baud_used) + " baud");

    auto enter = transact(port, body_enter_progmode_isp(), sequence);
    if (!enter.ok) {
        result.stage = "sync";
        result.error = enter.error;
        return result;
    }

    // Each signature byte needs its own command; the ISP instruction underneath
    // only returns one byte at a time.
    std::array<uint8_t, 3> got{};
    for (uint8_t index = 0; index < 3; ++index) {
        auto sig = transact(port, body_read_signature_isp(index), sequence);
        if (!sig.ok) {
            result.stage = "signature";
            result.error = sig.error;
            return result;
        }
        if (sig.answer.data.empty()) {
            result.stage = "signature";
            result.error = "signature byte " + std::to_string(index) +
                           " came back with no data";
            return result;
        }
        got[index] = sig.answer.data[0];
    }
    if (got != board.signature) {
        result.stage = "signature";
        result.error = "device signature mismatch: expected " + hex3(board.signature) +
                       " for " + board.name + " but found " + hex3(got) +
                       ". Wrong --board, or a different chip than expected.";
        return result;
    }
    report("signature ok (" + hex3(got) + ")");

    const uint32_t page = board.page_size;
    for (uint32_t offset = 0; offset < image.data.size(); offset += page) {
        uint32_t chunk = uint32_t(image.data.size()) - offset;
        if (chunk > page) chunk = page;

        const uint32_t word_address = (image.base_address + offset) / 2;
        auto addr = transact(port, body_load_address(word_address), sequence);
        if (!addr.ok) {
            result.stage = "write";
            result.error = addr.error + " (at byte offset " + std::to_string(offset) + ")";
            return result;
        }

        auto write = transact(port,
                              body_program_flash_isp(image.data.data() + offset,
                                                     uint16_t(chunk)),
                              sequence);
        if (!write.ok) {
            result.stage = "write";
            result.error = write.error + " (at byte offset " + std::to_string(offset) + ")";
            return result;
        }
        report("wrote " + std::to_string(offset + chunk) + "/" +
               std::to_string(image.data.size()) + " bytes");
    }

    auto leave = transact(port, body_leave_progmode_isp(), sequence);
    if (!leave.ok) {
        result.stage = "finish";
        result.error = leave.error +
                       "; the sketch may be written but the board not restarted";
        return result;
    }

    result.ok = true;
    return result;
}

} // namespace ardio::stk500v2
