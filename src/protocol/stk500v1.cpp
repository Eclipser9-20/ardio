#include "ardio/protocol/stk500v1.h"

namespace ardio::stk500v1 {
namespace {
constexpr uint8_t kGetSync       = 0x30;
constexpr uint8_t kEnterProgmode = 0x50;
constexpr uint8_t kLeaveProgmode = 0x51;
constexpr uint8_t kLoadAddress   = 0x55;
constexpr uint8_t kProgPage      = 0x64;
constexpr uint8_t kReadPage      = 0x74;
constexpr uint8_t kReadSign      = 0x75;
constexpr uint8_t kMemFlash      = 'F';
} // namespace

std::vector<uint8_t> cmd_get_sync()        { return {kGetSync, kCrcEop}; }
std::vector<uint8_t> cmd_enter_progmode()  { return {kEnterProgmode, kCrcEop}; }
std::vector<uint8_t> cmd_leave_progmode()  { return {kLeaveProgmode, kCrcEop}; }
std::vector<uint8_t> cmd_read_signature()  { return {kReadSign, kCrcEop}; }

std::vector<uint8_t> cmd_load_address(uint16_t word_address) {
    return {kLoadAddress,
            uint8_t(word_address & 0xFF),
            uint8_t((word_address >> 8) & 0xFF),
            kCrcEop};
}

std::vector<uint8_t> cmd_prog_page(const uint8_t* data, uint16_t length) {
    std::vector<uint8_t> out;
    out.reserve(size_t(length) + 5);
    out.push_back(kProgPage);
    out.push_back(uint8_t((length >> 8) & 0xFF));  // big-endian
    out.push_back(uint8_t(length & 0xFF));
    out.push_back(kMemFlash);
    out.insert(out.end(), data, data + length);
    out.push_back(kCrcEop);
    return out;
}

std::vector<uint8_t> cmd_read_page(uint16_t length) {
    return {kReadPage,
            uint8_t((length >> 8) & 0xFF),
            uint8_t(length & 0xFF),
            kMemFlash,
            kCrcEop};
}

bool is_ok_response(const std::vector<uint8_t>& resp) {
    return resp.size() == 2 && resp[0] == kInSync && resp[1] == kOk;
}

} // namespace ardio::stk500v1
