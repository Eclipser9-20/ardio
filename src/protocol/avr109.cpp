#include "ardio/protocol/avr109.h"
#include "ardio/platform/ports.h"

#include <algorithm>
#include <chrono>
#include <set>
#include <thread>

namespace ardio::avr109 {
namespace {
constexpr uint8_t kCmdSoftwareId     = 'S';
constexpr uint8_t kCmdVersion        = 'V';
constexpr uint8_t kCmdSignature      = 's';
constexpr uint8_t kCmdEnterProgmode  = 'P';
constexpr uint8_t kCmdLeaveProgmode  = 'L';
constexpr uint8_t kCmdExitBootloader = 'E';
constexpr uint8_t kCmdSetAddress     = 'A';
constexpr uint8_t kCmdWriteBlock     = 'B';
constexpr uint8_t kCmdReadBlock      = 'g';
constexpr uint8_t kMemFlash          = 'F';
} // namespace

std::vector<uint8_t> cmd_software_id()      { return {kCmdSoftwareId}; }
std::vector<uint8_t> cmd_software_version() { return {kCmdVersion}; }
std::vector<uint8_t> cmd_read_signature()   { return {kCmdSignature}; }
std::vector<uint8_t> cmd_enter_progmode()   { return {kCmdEnterProgmode}; }
std::vector<uint8_t> cmd_leave_progmode()   { return {kCmdLeaveProgmode}; }
std::vector<uint8_t> cmd_exit_bootloader()  { return {kCmdExitBootloader}; }

std::vector<uint8_t> cmd_set_address(uint16_t word_address) {
    return {kCmdSetAddress,
            uint8_t((word_address >> 8) & 0xFF),  // big-endian, unlike STK500v1
            uint8_t(word_address & 0xFF)};
}

std::vector<uint8_t> cmd_write_block(const uint8_t* data, uint16_t length) {
    std::vector<uint8_t> out;
    out.reserve(size_t(length) + 4);
    out.push_back(kCmdWriteBlock);
    out.push_back(uint8_t((length >> 8) & 0xFF));
    out.push_back(uint8_t(length & 0xFF));
    out.push_back(kMemFlash);
    out.insert(out.end(), data, data + length);
    return out;
}

std::vector<uint8_t> cmd_read_block(uint16_t length) {
    return {kCmdReadBlock,
            uint8_t((length >> 8) & 0xFF),
            uint8_t(length & 0xFF),
            kMemFlash};
}

bool is_cr_response(const std::vector<uint8_t>& resp) {
    return resp.size() == 1 && resp[0] == kCr;
}

std::array<uint8_t, 3> signature_to_datasheet_order(const std::array<uint8_t, 3>& wire) {
    return {wire[2], wire[1], wire[0]};
}

} // namespace ardio::avr109

namespace ardio {
namespace {

constexpr int kResponseTimeoutMs = 500;
// The bootloader's own USB device takes a moment to enumerate after the reset,
// and the host adds its own driver-attach delay on top. Polling faster than
// this just burns CPU; polling much slower makes the wait feel like a hang.
constexpr int kPollIntervalMs = 100;

void sleep_ms(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

std::string hex3(const std::array<uint8_t, 3>& sig) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (uint8_t b : sig) { s += d[b >> 4]; s += d[b & 0xF]; }
    return s;
}

std::vector<uint8_t> transact(SerialPort& port, const std::vector<uint8_t>& cmd,
                              size_t expected) {
    if (!port.write(cmd.data(), cmd.size())) return {};
    std::vector<uint8_t> resp(expected);
    size_t n = port.read(resp.data(), expected, kResponseTimeoutMs);
    resp.resize(n);
    return resp;
}

// Every AVR109 command that carries no payload back answers with a bare CR.
// Anything else means the two sides have lost sync, and since the protocol has
// no resynchronisation mechanism there is nothing to do but stop and say which
// command it was -- the command letter is the only clue the user gets.
bool expect_cr(SerialPort& port, const std::vector<uint8_t>& cmd,
               std::string& error) {
    auto resp = transact(port, cmd, 1);
    if (avr109::is_cr_response(resp)) return true;
    std::string what(1, char(cmd[0]));
    if (resp.empty())
        error = "no answer to AVR109 command '" + what + "'";
    else
        error = "AVR109 command '" + what + "' answered 0x" +
                std::string(1, "0123456789abcdef"[resp[0] >> 4]) +
                std::string(1, "0123456789abcdef"[resp[0] & 0xF]) +
                " instead of CR";
    return false;
}

} // namespace

std::vector<std::string> SystemPortEnumerator::list_ports() {
    std::vector<std::string> paths;
    for (const auto& p : enumerate_ports()) paths.push_back(p.device);
    return paths;
}

void SystemPortEnumerator::wait(int ms) { sleep_ms(ms); }

TouchResetResult touch_reset_and_find_port(SerialPort& port, const std::string& path,
                                           PortEnumerator& ports, int timeout_ms,
                                           const ProgressFn& progress) {
    TouchResetResult result;
    auto report = [&](const std::string& msg) { if (progress) progress(msg); };

    // Snapshot first. Once the touch lands, the sketch's port disappears and
    // the bootloader's appears, and the only thing that distinguishes the new
    // one is that it was not in this set.
    std::vector<std::string> before = ports.list_ports();
    std::set<std::string> known(before.begin(), before.end());

    // The magic is the baud rate itself, not anything written: the USB CDC
    // stack in the sketch watches for a line-coding request of exactly 1200
    // and, when the host then drops DTR, jumps to the bootloader. So open at
    // 1200, lower DTR, and close -- no bytes are ever sent.
    std::string open_error;
    if (!port.open(path, 1200, open_error)) {
        result.error = "could not open " + path + " for the 1200-baud reset: " + open_error;
        return result;
    }
    port.set_dtr(false);
    sleep_ms(50);
    port.close();
    report("touched " + path + " at 1200 baud; waiting for the bootloader port");

    // The board is now rebooting into Caterina. Note that the bootloader only
    // stays up for about eight seconds before handing control back to the
    // sketch, so this wait has to be short and the programming that follows
    // has to start promptly.
    int waited = 0;
    while (waited < timeout_ms) {
        ports.wait(kPollIntervalMs);
        waited += kPollIntervalMs;
        for (const auto& candidate : ports.list_ports()) {
            if (known.count(candidate)) continue;
            result.ok = true;
            result.port = candidate;
            report("bootloader appeared on " + candidate);
            return result;
        }
    }

    result.error = "no new serial port appeared within " + std::to_string(timeout_ms) +
                   " ms of the 1200-baud reset on " + path +
                   ". These boards reboot into the bootloader under a different "
                   "port name; if none appeared the reset did not take. Try "
                   "pressing the board's reset button and running the upload "
                   "again immediately.";
    return result;
}

UploadResult upload_avr109(SerialPort& port, const std::string& path,
                           const Board& board, const HexImage& image,
                           PortEnumerator& ports, const ProgressFn& progress) {
    UploadResult result;
    auto report = [&](const std::string& msg) { if (progress) progress(msg); };

    if (image.data.size() > board.flash_size) {
        result.stage = "size";
        result.error = "sketch is " + std::to_string(image.data.size()) +
                       " bytes but " + board.name + " has only " +
                       std::to_string(board.flash_size) + " bytes of flash";
        return result;
    }

    auto reset = touch_reset_and_find_port(port, path, ports, 4000, progress);
    if (!reset.ok) {
        result.stage = "reset";
        result.error = reset.error;
        return result;
    }

    // The bootloader's CDC port ignores the baud rate entirely -- it is a USB
    // device, not a UART -- but the host still wants a number, so pick the one
    // the rest of the tooling uses and record it for the report.
    std::string open_error;
    if (!port.open(reset.port, 57600, open_error)) {
        result.stage = "open";
        result.error = "found the bootloader on " + reset.port +
                       " but could not open it: " + open_error;
        return result;
    }
    result.baud_used = 57600;

    auto id = transact(port, avr109::cmd_software_id(), avr109::kSoftwareIdLength);
    if (id.size() != avr109::kSoftwareIdLength) {
        result.stage = "sync";
        result.error = "no AVR109 bootloader answered on " + reset.port +
                       " (expected 7 identifier characters, got " +
                       std::to_string(id.size()) + ")";
        return result;
    }
    report("bootloader identifies as " + std::string(id.begin(), id.end()));

    auto sig = transact(port, avr109::cmd_read_signature(), 3);
    if (sig.size() != 3) {
        result.stage = "signature";
        result.error = "could not read device signature";
        return result;
    }
    std::array<uint8_t, 3> got =
        avr109::signature_to_datasheet_order({sig[0], sig[1], sig[2]});
    if (got != board.signature) {
        result.stage = "signature";
        result.error = "device signature mismatch: expected " + hex3(board.signature) +
                       " for " + board.name + " but found " + hex3(got) +
                       ". Wrong --board, or a different chip than expected.";
        return result;
    }
    report("signature ok (" + hex3(got) + ")");

    if (!expect_cr(port, avr109::cmd_enter_progmode(), result.error)) {
        result.stage = "sync";
        return result;
    }

    // A block may be any size the bootloader's buffer accepts; the flash page
    // size is always a safe choice and keeps addresses page-aligned.
    const uint32_t block = board.page_size ? board.page_size : 128;
    for (uint32_t offset = 0; offset < image.data.size(); offset += block) {
        uint32_t chunk = uint32_t(image.data.size()) - offset;
        if (chunk > block) chunk = block;

        uint16_t word_addr = uint16_t((image.base_address + offset) / 2);
        if (!expect_cr(port, avr109::cmd_set_address(word_addr), result.error)) {
            result.stage = "write";
            result.error += " while setting the address for byte offset " +
                            std::to_string(offset);
            return result;
        }
        if (!expect_cr(port,
                       avr109::cmd_write_block(image.data.data() + offset, uint16_t(chunk)),
                       result.error)) {
            result.stage = "write";
            result.error += " while writing the block at byte offset " +
                            std::to_string(offset);
            return result;
        }
        report("wrote " + std::to_string(offset + chunk) + "/" +
               std::to_string(image.data.size()) + " bytes");
    }

    if (!expect_cr(port, avr109::cmd_leave_progmode(), result.error)) {
        result.stage = "write";
        return result;
    }
    // 'E' hands control back to the sketch. The bootloader's USB device
    // disappears at this point, so a missing answer here is expected often
    // enough that it is reported but not treated as a failed upload -- the
    // flash was already committed by the time the last block was acknowledged.
    std::string exit_error;
    if (!expect_cr(port, avr109::cmd_exit_bootloader(), exit_error))
        report("note: " + exit_error + " (the board most likely already restarted)");

    result.ok = true;
    return result;
}

} // namespace ardio
