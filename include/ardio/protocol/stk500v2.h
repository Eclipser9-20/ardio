#pragma once
#include "ardio/board.h"
#include "ardio/hex.h"
#include "ardio/platform/serial.h"
#include "ardio/protocol/programmer.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// STK500 version 2, as spoken by the Mega 2560 and Mega ADK bootloaders
// (stk500v2/wiring).
//
// Despite the name this is not an extension of STK500v1; it shares neither the
// command bytes nor the shape of the exchange. v1 sends a bare opcode with a
// CRC_EOP terminator and gets back two framing bytes. v2 sends a framed message
// with a sequence number, an explicit length and a checksum, and every answer
// echoes the command byte plus a status byte. The ISP commands are also a level
// lower: where v1's "program page" is a bootloader convenience, v2's
// CMD_PROGRAM_FLASH_ISP carries the actual SPI instruction bytes the programmer
// should clock out at the target.
//
// Everything above the driver at the bottom is a pure function over bytes, so
// the framing, the sequence tracking and every failure message can be checked
// byte-for-byte without hardware.
namespace ardio::stk500v2 {

// ----------------------------------------------------------------- framing ---

inline constexpr uint8_t kMessageStart = 0x1B;
inline constexpr uint8_t kToken        = 0x0E;

// A frame is: MESSAGE_START, sequence, length (u16 big-endian), TOKEN, body,
// checksum -- where the checksum is the XOR of every preceding byte of the
// frame, the start byte included.
inline constexpr size_t kFrameOverhead = 6;

// The largest body the length field can describe. Real bootloaders accept far
// less, but a declared length beyond this cannot be honest.
inline constexpr size_t kMaxBodySize = 0xFFFF;

// ---------------------------------------------------------------- commands ---

enum class Command : uint8_t {
    SignOn             = 0x01,
    SetParameter       = 0x02,
    GetParameter       = 0x03,
    LoadAddress        = 0x06,
    EnterProgmodeIsp   = 0x10,
    LeaveProgmodeIsp   = 0x11,
    ProgramFlashIsp    = 0x13,
    ReadFlashIsp       = 0x14,
    ReadSignatureIsp   = 0x1B,
};

// ------------------------------------------------------------------ status ---

inline constexpr uint8_t kStatusCmdOk           = 0x00;
inline constexpr uint8_t kStatusCmdTimeout      = 0x80;
inline constexpr uint8_t kStatusRdyBsyTimeout   = 0x81;
inline constexpr uint8_t kStatusSetParamMissing = 0x82;
inline constexpr uint8_t kStatusCmdFailed       = 0xC0;
inline constexpr uint8_t kStatusChecksumError   = 0xC1;
inline constexpr uint8_t kStatusCmdUnknown      = 0xC9;

// --------------------------------------------------------------- parameters ---

inline constexpr uint8_t kParamHardwareVersion = 0x90;
inline constexpr uint8_t kParamSoftwareMajor   = 0x91;
inline constexpr uint8_t kParamSoftwareMinor   = 0x92;
inline constexpr uint8_t kParamVTarget         = 0x94;
inline constexpr uint8_t kParamResetPolarity   = 0x9E;

// -------------------------------------------------------- naming for humans ---

// "CMD_LOAD_ADDRESS" for a known opcode, "command 0x42" for anything else.
std::string command_name(uint8_t command);

// "STATUS_CMD_FAILED (0xc0)" for a known status, "status 0x42" otherwise.
std::string status_name(uint8_t status);

// The message an unexpected status turns into, naming both the command that was
// refused and the status that came back.
std::string describe_failure(uint8_t command, uint8_t status);

// ------------------------------------------------------------ encode/decode ---

// XOR of every byte in `data`. Applied to a whole frame minus its checksum
// byte, this is the checksum; applied to the frame including it, the result is
// zero.
uint8_t checksum(const uint8_t* data, size_t len);

// Wraps `body` in a frame stamped with `sequence`.
std::vector<uint8_t> encode_frame(uint8_t sequence, const std::vector<uint8_t>& body);

struct Frame {
    uint8_t sequence = 0;
    std::vector<uint8_t> body;
};

// Why a frame could not be decoded. Kept separate from the frame itself so the
// driver can say which of these went wrong rather than "bad answer".
enum class FrameError {
    None,
    TooShort,       // fewer bytes than the overhead alone requires
    BadStart,       // first byte is not MESSAGE_START
    BadToken,       // the byte after the length field is not TOKEN
    LengthMismatch, // the declared body length does not match the bytes present
    BadChecksum,    // the trailing XOR does not close the frame
};

std::string frame_error_text(FrameError err);

// Decodes a complete frame. `err` receives the reason on failure and
// FrameError::None on success; pass nullptr to ignore it.
std::optional<Frame> parse_frame(const std::vector<uint8_t>& frame,
                                 FrameError* err = nullptr);

// The body of every answer: the command being answered, a status byte, then
// whatever that command returns.
struct Answer {
    uint8_t command = 0;
    uint8_t status = 0;
    std::vector<uint8_t> data;
};

// Splits an answer body. Returns nullopt for a body too short to carry both a
// command byte and a status byte.
std::optional<Answer> parse_answer(const std::vector<uint8_t>& body);

// --------------------------------------------------------- command bodies ----

std::vector<uint8_t> body_sign_on();
std::vector<uint8_t> body_get_parameter(uint8_t parameter);
std::vector<uint8_t> body_set_parameter(uint8_t parameter, uint8_t value);

// `word_address` is a WORD address (byte address / 2), sent big-endian across
// four bytes with the top bit of the first set.
//
// That top bit is what makes this command work on a Mega at all. The AVR's ISP
// load-page instructions only carry a 16-bit word address, so anything past the
// first 128KB of flash needs the target's extended-address (RAMPZ) latch loaded
// as well. Setting bit 31 tells the programmer that this address is a flash
// address and that the high bytes are meaningful, so it emits the extended
// load-address instruction before the page write. The Mega 2560 has 256KB of
// flash -- 128K words -- so word addresses genuinely run past 0xFFFF, and a
// programmer that drops the high bytes will happily write the second half of a
// large sketch on top of the first half. The result still verifies page by page
// as it is written, boots, and is silently wrong.
std::vector<uint8_t> body_load_address(uint32_t word_address);

std::vector<uint8_t> body_enter_progmode_isp();
std::vector<uint8_t> body_leave_progmode_isp();

// `length` is a BYTE count, sent big-endian, followed by the ISP instruction
// bytes the programmer clocks out at the target, then the data itself.
std::vector<uint8_t> body_program_flash_isp(const uint8_t* data, uint16_t length);
std::vector<uint8_t> body_read_flash_isp(uint16_t length);

// `index` selects which of the three signature bytes to fetch; the ISP
// instruction can only return one at a time.
std::vector<uint8_t> body_read_signature_isp(uint8_t index);

// ----------------------------------------------------------------- driver ----

// Resets the board via DTR/RTS, signs on, checks the device signature, and
// writes the image page by page over CMD_PROGRAM_FLASH_ISP.
//
// UNVERIFIED AGAINST HARDWARE. Every byte this produces and every failure path
// is checked against a scripted bootloader, but no physical Mega 2560 has been
// flashed with it yet.
//
// UploadResult::stage names the phase that failed: "size", "open", "sync",
// "signature", "write" or "finish".
UploadResult upload_stk500v2(SerialPort& port, const std::string& path,
                             const Board& board, const HexImage& image,
                             const ProgressFn& progress);

} // namespace ardio::stk500v2
