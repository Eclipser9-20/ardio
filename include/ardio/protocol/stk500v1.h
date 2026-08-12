#pragma once
#include <cstdint>
#include <vector>

namespace ardio::stk500v1 {

// Response bytes
inline constexpr uint8_t kInSync = 0x14;
inline constexpr uint8_t kOk     = 0x10;
inline constexpr uint8_t kNoSync = 0x15;
// Terminator appended to every command
inline constexpr uint8_t kCrcEop = 0x20;

std::vector<uint8_t> cmd_get_sync();
std::vector<uint8_t> cmd_enter_progmode();
std::vector<uint8_t> cmd_leave_progmode();
std::vector<uint8_t> cmd_read_signature();

// `word_address` is a WORD address (byte address / 2), sent little-endian.
std::vector<uint8_t> cmd_load_address(uint16_t word_address);

// `length` is a BYTE count, sent big-endian. 'F' selects flash (vs 'E' EEPROM).
std::vector<uint8_t> cmd_prog_page(const uint8_t* data, uint16_t length);
std::vector<uint8_t> cmd_read_page(uint16_t length);

bool is_ok_response(const std::vector<uint8_t>& resp);

} // namespace ardio::stk500v1
