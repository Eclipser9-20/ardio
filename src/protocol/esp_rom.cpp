#include "ardio/protocol/esp_rom.h"

#include <chrono>
#include <string>
#include <thread>

namespace ardio::esp_rom {
namespace {

void put_u32_le(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(uint8_t(v & 0xFF));
    out.push_back(uint8_t((v >> 8) & 0xFF));
    out.push_back(uint8_t((v >> 16) & 0xFF));
    out.push_back(uint8_t((v >> 24) & 0xFF));
}

uint32_t get_u32_le(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) |
           (uint32_t(p[3]) << 24);
}

void sleep_ms(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

std::string hex32(uint32_t v) {
    static const char* d = "0123456789abcdef";
    std::string s = "0x";
    for (int shift = 28; shift >= 0; shift -= 4) s += d[(v >> shift) & 0xF];
    return s;
}

} // namespace

// ------------------------------------------------------------------ SLIP ---

std::vector<uint8_t> slip_encode(const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> out;
    out.reserve(payload.size() + 2);
    out.push_back(kSlipEnd);
    for (uint8_t b : payload) {
        if (b == kSlipEnd) {
            out.push_back(kSlipEsc);
            out.push_back(kSlipEscEnd);
        } else if (b == kSlipEsc) {
            out.push_back(kSlipEsc);
            out.push_back(kSlipEscEsc);
        } else {
            out.push_back(b);
        }
    }
    out.push_back(kSlipEnd);
    return out;
}

std::optional<std::vector<uint8_t>> slip_decode(const std::vector<uint8_t>& frame) {
    if (frame.size() < 2 || frame.front() != kSlipEnd || frame.back() != kSlipEnd)
        return std::nullopt;

    std::vector<uint8_t> out;
    out.reserve(frame.size());
    for (size_t i = 1; i + 1 < frame.size(); ++i) {
        uint8_t b = frame[i];
        if (b == kSlipEnd) return std::nullopt;   // a delimiter inside the body
        if (b != kSlipEsc) {
            out.push_back(b);
            continue;
        }
        if (i + 2 >= frame.size()) return std::nullopt;  // escape with no partner
        uint8_t next = frame[++i];
        if (next == kSlipEscEnd)      out.push_back(kSlipEnd);
        else if (next == kSlipEscEsc) out.push_back(kSlipEsc);
        else                          return std::nullopt;
    }
    return out;
}

// -------------------------------------------------------------- commands ---

uint8_t checksum(const uint8_t* data, size_t len) {
    uint8_t sum = 0xEF;   // the seed the ROM expects
    for (size_t i = 0; i < len; ++i) sum ^= data[i];
    return sum;
}

std::vector<uint8_t> request_body(Command cmd, const std::vector<uint8_t>& payload,
                                  uint32_t checksum_field) {
    std::vector<uint8_t> out;
    out.reserve(kHeaderSize + payload.size());
    out.push_back(kDirRequest);
    out.push_back(uint8_t(cmd));
    out.push_back(uint8_t(payload.size() & 0xFF));         // size, little-endian
    out.push_back(uint8_t((payload.size() >> 8) & 0xFF));
    put_u32_le(out, checksum_field);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

std::vector<uint8_t> packet(Command cmd, const std::vector<uint8_t>& payload,
                            uint32_t checksum_field) {
    return slip_encode(request_body(cmd, payload, checksum_field));
}

std::vector<uint8_t> cmd_sync() {
    std::vector<uint8_t> payload{0x07, 0x07, 0x12, 0x20};
    payload.insert(payload.end(), 32, 0x55);
    return packet(Command::Sync, payload);
}

std::vector<uint8_t> cmd_flash_begin(uint32_t total_size, uint32_t blocks,
                                     uint32_t block_size, uint32_t offset) {
    std::vector<uint8_t> payload;
    put_u32_le(payload, total_size);
    put_u32_le(payload, blocks);
    put_u32_le(payload, block_size);
    put_u32_le(payload, offset);
    return packet(Command::FlashBegin, payload);
}

std::vector<uint8_t> cmd_flash_data(const uint8_t* data, size_t len, uint32_t sequence) {
    std::vector<uint8_t> payload;
    put_u32_le(payload, uint32_t(len));
    put_u32_le(payload, sequence);
    put_u32_le(payload, 0);   // reserved
    put_u32_le(payload, 0);   // reserved
    payload.insert(payload.end(), data, data + len);
    // Only the data half is checksummed -- the four header words are not.
    return packet(Command::FlashData, payload, checksum(data, len));
}

std::vector<uint8_t> cmd_flash_end(bool reboot) {
    std::vector<uint8_t> payload;
    put_u32_le(payload, reboot ? 0u : 1u);   // 0 reboots, 1 stays in the loader
    return packet(Command::FlashEnd, payload);
}

std::vector<uint8_t> cmd_read_reg(uint32_t address) {
    std::vector<uint8_t> payload;
    put_u32_le(payload, address);
    return packet(Command::ReadReg, payload);
}

// -------------------------------------------------------------- replies ----

std::optional<Reply> parse_reply(const std::vector<uint8_t>& body) {
    if (body.size() < kHeaderSize) return std::nullopt;
    if (body[0] != kDirReply) return std::nullopt;

    size_t size = size_t(body[2]) | (size_t(body[3]) << 8);
    if (body.size() != kHeaderSize + size) return std::nullopt;

    Reply r;
    r.cmd = body[1];
    r.value = get_u32_le(body.data() + 4);
    r.payload.assign(body.begin() + kHeaderSize, body.end());
    // The ESP8266 ROM ends every reply payload with a status byte and an error
    // byte. A payload shorter than that is a malformed reply.
    if (r.payload.size() < 2) return std::nullopt;
    r.status_ok = r.payload[r.payload.size() - 2] == 0;
    r.error = r.payload.back();
    return r;
}

// --------------------------------------------------------------- driver ----

namespace {

constexpr int kReadTimeoutMs = 200;
// A real port blocks for kReadTimeoutMs per empty read, so this bounds a frame
// wait at roughly 600ms; against a test double that returns immediately it just
// keeps the loop from spinning.
constexpr int kMaxEmptyReads = 3;
constexpr int kSyncAttempts = 5;

// Reads one SLIP frame, discarding any bytes before the opening delimiter --
// the ROM prints boot chatter at a different baud rate that lands as garbage.
std::optional<std::vector<uint8_t>> read_frame(SerialPort& port) {
    std::vector<uint8_t> frame;
    bool in_frame = false;
    int empty = 0;
    while (empty < kMaxEmptyReads) {
        uint8_t b = 0;
        if (port.read(&b, 1, kReadTimeoutMs) != 1) { ++empty; continue; }
        empty = 0;
        if (!in_frame) {
            if (b != kSlipEnd) continue;      // pre-frame noise
            in_frame = true;
            frame.push_back(b);
            continue;
        }
        frame.push_back(b);
        if (b == kSlipEnd) {
            if (frame.size() == 2) {          // 0xC0 0xC0: an empty run-in frame
                frame.resize(1);
                continue;
            }
            return frame;
        }
    }
    return std::nullopt;
}

// Writes a command and reads back the matching reply.
std::optional<Reply> transact(SerialPort& port, const std::vector<uint8_t>& request,
                              Command expect) {
    if (!port.write(request.data(), request.size())) return std::nullopt;
    for (int i = 0; i < 8; ++i) {
        auto frame = read_frame(port);
        if (!frame) return std::nullopt;
        auto body = slip_decode(*frame);
        if (!body) return std::nullopt;
        auto reply = parse_reply(*body);
        if (!reply) return std::nullopt;
        // The ROM answers a single SYNC several times over, so surplus SYNC
        // replies are still in flight when the next command goes out. Skipping
        // anything that is not the command just asked for absorbs them without
        // a separate drain step -- and a drain would be wrong here, since there
        // is no way to tell "one reply too many" from "the reply I want" except
        // by its command byte.
        if (reply->cmd == uint8_t(expect)) return reply;
    }
    return std::nullopt;
}

} // namespace

void enter_bootloader(SerialPort& port) {
    // On NodeMCU / ESP-12E / ESP-12F carrier boards DTR and RTS do not reach
    // the chip directly: they drive a two-transistor pair so that DTR controls
    // GPIO0 and RTS controls RESET, and each asserted line pulls its pin LOW.
    // The pair also cancels out when both lines are asserted together, which is
    // exactly why the sequence below never asserts both at once -- that is what
    // stops an ordinary terminal opening the port from resetting the board.
    //
    // The chip samples GPIO0 on the rising edge of RESET: low means "start the
    // ROM loader", high means "boot the flashed firmware". So GPIO0 must
    // already be held low before RESET is released.
    port.set_dtr(false);   // GPIO0 high
    port.set_rts(true);    // RESET low -- chip held in reset
    sleep_ms(100);
    port.set_dtr(true);    // GPIO0 low
    port.set_rts(false);   // RESET released; GPIO0 sampled low -> ROM loader
    sleep_ms(50);
    port.set_dtr(false);   // let GPIO0 float back up now it has been latched
    sleep_ms(50);
}

UploadResult upload_esp_rom(SerialPort& port, const std::string& path,
                            const Board& board, const HexImage& image,
                            const ProgressFn& progress) {
    UploadResult result;
    auto report = [&](const std::string& msg) { if (progress) progress(msg); };

    if (image.data.size() > board.flash_size) {
        result.stage = "size";
        result.error = "firmware is " + std::to_string(image.data.size()) +
                       " bytes but " + board.name + " has only " +
                       std::to_string(board.flash_size) + " bytes of flash";
        return result;
    }
    if (image.data.empty()) {
        result.stage = "size";
        result.error = "firmware image is empty";
        return result;
    }

    std::string tried;
    bool synced = false;
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
        enter_bootloader(port);

        // SYNC goes out repeatedly until the ROM answers; a board that has just
        // been reset ignores the first few.
        for (int attempt = 0; attempt < kSyncAttempts && !synced; ++attempt) {
            auto reply = transact(port, cmd_sync(), Command::Sync);
            if (reply && reply->status_ok) synced = true;
        }
        if (synced) {
            result.baud_used = baud;
            break;
        }
        port.close();
    }

    if (!synced) {
        result.stage = "sync";
        result.error = "no response from the ESP8266 ROM loader on " + path +
                       " (tried " + tried +
                       " baud). Check the board is plugged in, and that nothing "
                       "else holds GPIO0 or RESET.";
        return result;
    }
    report("synced at " + std::to_string(result.baud_used) + " baud");

    auto chip = transact(port, cmd_read_reg(kChipIdRegister), Command::ReadReg);
    if (!chip || !chip->status_ok) {
        result.stage = "chip";
        result.error = "could not read the chip ID register";
        return result;
    }
    if (chip->value != kEsp8266ChipId) {
        result.stage = "chip";
        result.error = "chip ID mismatch: expected " + hex32(kEsp8266ChipId) +
                       " for " + board.name + " but found " + hex32(chip->value) +
                       ". Wrong --board, or a different Espressif part.";
        return result;
    }
    report("chip id ok (" + hex32(chip->value) + ")");

    const uint32_t block = board.page_size ? board.page_size : kFlashBlockSize;
    const uint32_t total = uint32_t(image.data.size());
    const uint32_t blocks = (total + block - 1) / block;

    auto begin = transact(port, cmd_flash_begin(total, blocks, block, image.base_address),
                          Command::FlashBegin);
    if (!begin || !begin->status_ok) {
        result.stage = "erase";
        result.error = "the ROM loader refused to begin a flash write of " +
                       std::to_string(total) + " bytes at offset " +
                       std::to_string(image.base_address);
        return result;
    }
    report("erased " + std::to_string(blocks) + " blocks");

    // Every FLASH_DATA block is a full block; a short tail is padded with 0xFF,
    // which is what erased flash reads back as anyway.
    for (uint32_t seq = 0; seq < blocks; ++seq) {
        uint32_t offset = seq * block;
        uint32_t chunk = total - offset;
        if (chunk > block) chunk = block;

        std::vector<uint8_t> payload(image.data.begin() + offset,
                                     image.data.begin() + offset + chunk);
        payload.resize(block, 0xFF);

        auto reply = transact(port, cmd_flash_data(payload.data(), payload.size(), seq),
                              Command::FlashData);
        if (!reply || !reply->status_ok) {
            result.stage = "write";
            result.error = "block " + std::to_string(seq) + " of " +
                           std::to_string(blocks) + " failed at byte offset " +
                           std::to_string(offset);
            return result;
        }
        report("wrote " + std::to_string(offset + chunk) + "/" +
               std::to_string(total) + " bytes");
    }

    auto end = transact(port, cmd_flash_end(true), Command::FlashEnd);
    if (!end || !end->status_ok) {
        result.stage = "finish";
        result.error = "the ROM loader did not accept the end-of-flash command; "
                       "the firmware may be written but the board not restarted";
        return result;
    }

    result.ok = true;
    return result;
}

} // namespace ardio::esp_rom
