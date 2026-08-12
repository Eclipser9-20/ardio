#include "ardio/hex.h"

namespace ardio {
namespace {

int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

// Reads two hex chars at `pos`, advancing it. Returns -1 on malformed input.
int hex_byte(std::string_view s, size_t& pos) {
    if (pos + 1 >= s.size()) return -1;
    int hi = hex_nibble(s[pos]), lo = hex_nibble(s[pos + 1]);
    if (hi < 0 || lo < 0) return -1;
    pos += 2;
    return (hi << 4) | lo;
}

} // namespace

std::optional<HexImage> parse_intel_hex(std::string_view text, std::string& error) {
    HexImage img;
    bool saw_eof = false;
    size_t line_no = 0;

    size_t i = 0;
    while (i < text.size()) {
        size_t end = text.find('\n', i);
        if (end == std::string_view::npos) end = text.size();
        std::string_view line = text.substr(i, end - i);
        i = end + 1;
        ++line_no;

        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.remove_suffix(1);
        if (line.empty()) continue;

        if (line[0] != ':') {
            error = "line " + std::to_string(line_no) + ": record does not start with ':'";
            return std::nullopt;
        }

        size_t p = 1;
        int len = hex_byte(line, p), addr_hi = hex_byte(line, p),
            addr_lo = hex_byte(line, p), type = hex_byte(line, p);
        if (len < 0 || addr_hi < 0 || addr_lo < 0 || type < 0) {
            error = "line " + std::to_string(line_no) + ": truncated record header";
            return std::nullopt;
        }

        uint32_t addr = uint32_t(addr_hi) << 8 | uint32_t(addr_lo);
        int sum = len + addr_hi + addr_lo + type;

        std::vector<uint8_t> payload;
        payload.reserve(size_t(len));
        for (int n = 0; n < len; ++n) {
            int b = hex_byte(line, p);
            if (b < 0) {
                error = "line " + std::to_string(line_no) + ": truncated data";
                return std::nullopt;
            }
            payload.push_back(uint8_t(b));
            sum += b;
        }

        int checksum = hex_byte(line, p);
        if (checksum < 0) {
            error = "line " + std::to_string(line_no) + ": missing checksum";
            return std::nullopt;
        }
        if (((sum + checksum) & 0xFF) != 0) {
            error = "line " + std::to_string(line_no) + ": checksum mismatch";
            return std::nullopt;
        }

        if (type == 0x01) { saw_eof = true; break; }
        if (type != 0x00) {
            error = "line " + std::to_string(line_no) + ": unsupported record type " +
                    std::to_string(type) + " (ardio targets flat <64KB AVR images)";
            return std::nullopt;
        }

        if (addr + payload.size() > img.data.size())
            img.data.resize(addr + payload.size(), 0xFF);
        for (size_t n = 0; n < payload.size(); ++n) img.data[addr + n] = payload[n];
    }

    if (!saw_eof) {
        error = "missing end-of-file record (:00000001FF)";
        return std::nullopt;
    }
    return img;
}

} // namespace ardio
