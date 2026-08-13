#pragma once
#include "ardio/protocol/programmer.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace ardio::avr109 {

// AVR109 ("butterfly") is line-oriented rather than framed: a command is one
// ASCII letter, optionally followed by raw argument bytes, and the answer is
// either a fixed-length payload or a bare carriage return meaning "done".
// There is no checksum and no length prefix anywhere, so the two sides stay in
// step only as long as both agree exactly how many bytes each answer carries.
inline constexpr uint8_t kCr = 0x0D;

// Caterina, the bootloader on the 32U4 boards, answers 'S' with these seven
// characters. Other AVR109 bootloaders use their own, so this is a hint for
// diagnostics rather than something to gate on.
inline constexpr char kCaterinaId[] = "CATERIN";
inline constexpr size_t kSoftwareIdLength = 7;

std::vector<uint8_t> cmd_software_id();      // 'S' -> 7 characters
std::vector<uint8_t> cmd_software_version(); // 'V' -> 2 characters
std::vector<uint8_t> cmd_read_signature();   // 's' -> 3 bytes
std::vector<uint8_t> cmd_enter_progmode();   // 'P' -> CR
std::vector<uint8_t> cmd_leave_progmode();   // 'L' -> CR
std::vector<uint8_t> cmd_exit_bootloader();  // 'E' -> CR

// 'A' takes a WORD address (byte address / 2) and, unlike STK500v1, sends it
// big-endian: high byte first.
std::vector<uint8_t> cmd_set_address(uint16_t word_address);

// 'B' takes a big-endian BYTE count, then a memory-type character, then that
// many data bytes. 'F' selects flash ('E' would select EEPROM). The answer is
// a single CR once the page is committed.
std::vector<uint8_t> cmd_write_block(const uint8_t* data, uint16_t length);

// 'g' is the read counterpart: count big-endian, then 'F'. The answer is the
// raw bytes with no CR after them.
std::vector<uint8_t> cmd_read_block(uint16_t length);

// True when the answer is exactly one carriage return, which is how AVR109
// acknowledges every command that has nothing to return.
bool is_cr_response(const std::vector<uint8_t>& resp);

// AVR109 reports the signature low byte first, which is the REVERSE of the
// order STK500v1 uses and of the order the datasheets and the board database
// write it. An ATmega32U4 is 0x1E 0x95 0x87 in datasheet order but arrives
// here as 0x87 0x95 0x1E. Getting this backwards produces a "signature
// mismatch" against a board that is in fact perfectly correct, so every
// comparison goes through this conversion rather than reversing by hand.
std::array<uint8_t, 3> signature_to_datasheet_order(const std::array<uint8_t, 3>& wire);

} // namespace ardio::avr109

namespace ardio {

// The 32U4 boards speak USB directly from the sketch, so the port you can see
// belongs to the running program, not to a bootloader. Touching that port at
// 1200 baud makes the sketch reboot into Caterina, which enumerates as a
// *different* USB device and therefore appears under a *different* path. The
// only way to find that path without hardware-specific guessing is to compare
// the set of ports before and after. Enumeration is behind this interface so
// the search can be tested without a board attached; the real implementation
// forwards to the platform port list.
class PortEnumerator {
public:
    virtual ~PortEnumerator() = default;
    // Device paths only, in any order.
    virtual std::vector<std::string> list_ports() = 0;
    // Called between polls. Overridable so tests advance instantly.
    virtual void wait(int ms) = 0;
};

// Enumerator backed by the platform port list and a real sleep.
class SystemPortEnumerator : public PortEnumerator {
public:
    std::vector<std::string> list_ports() override;
    void wait(int ms) override;
};

struct TouchResetResult {
    bool ok = false;
    std::string error;
    std::string port;  // path the bootloader appeared on
};

// Records the ports present, opens `path` at 1200 baud and closes it again,
// then polls until a port appears that was not there before. `port` is used
// only for the touch and is left closed on return.
TouchResetResult touch_reset_and_find_port(SerialPort& port, const std::string& path,
                                           PortEnumerator& ports, int timeout_ms,
                                           const ProgressFn& progress);

// Touch-resets the board, then programs the image over AVR109 on whichever
// port the bootloader appeared on. Stages, for UploadResult::stage: "size",
// "reset", "open", "sync", "signature", "write".
UploadResult upload_avr109(SerialPort& port, const std::string& path,
                           const Board& board, const HexImage& image,
                           PortEnumerator& ports, const ProgressFn& progress);

} // namespace ardio
