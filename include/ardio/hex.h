#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ardio {

struct HexImage {
    std::vector<uint8_t> data;
    uint32_t base_address = 0;
};

// Parses Intel HEX. Gaps between records are filled with 0xFF (erased flash).
// Returns nullopt and sets `error` on malformed input.
std::optional<HexImage> parse_intel_hex(std::string_view text, std::string& error);

} // namespace ardio
