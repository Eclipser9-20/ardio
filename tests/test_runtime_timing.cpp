// Checks that ardio's own timekeeping and analog-input runtime
// (runtime/timer.S and runtime/adc.S) is accepted by ardio's own assembler,
// and that the entry points a sketch calls are actually there. Both files are
// read from disk rather than pasted into the test, so the thing being checked
// is the runtime that really ships.

#include "harness.h"
#include "runtime_source.h"

#include "ardio/avr/assembler.h"

#include <fstream>
#include <sstream>
#include <string>

namespace {

// The test binary may be started from the build directory or from the source
// root, so try the handful of places the file can be relative to the cwd.
bool read_source(const char* relative, std::string& out) {
    return ardio::test::read_runtime(relative, out);
}

bool defines_label(const std::string& source, const std::string& name) {
    return source.find("\n" + name + ":") != std::string::npos ||
           source.compare(0, name.size() + 1, name + ":") == 0;
}

// Assembles a runtime file, reporting the assembler's own message on failure.
// Returns false when the file is missing, so a caller can skip.
bool assemble_runtime(const char* relative, ardio::AssembleResult& result,
                      std::string& source) {
    if (!read_source(relative, source)) {
        std::printf("  skip %s not found relative to the cwd\n", relative);
        return false;
    }
    CHECK(!source.empty());
    result = ardio::assemble(source);
    if (!result.ok) std::printf("    assembler said: %s\n", result.error.c_str());
    return true;
}

} // namespace

TEST(runtime_timer_assembles) {
    std::string source;
    ardio::AssembleResult result;
    if (!assemble_runtime("runtime/timer.S", result, source)) return;

    CHECK(result.ok);
    CHECK(result.error.empty());
    CHECK(!result.code.empty());
    CHECK_EQ(result.code.size() % 2, size_t(0));   // whole 16-bit words
}

TEST(runtime_timer_defines_its_entry_points) {
    std::string source;
    if (!read_source("runtime/timer.S", source)) {
        std::printf("  skip runtime/timer.S not found relative to the cwd\n");
        return;
    }
    CHECK(defines_label(source, "timer_init"));
    CHECK(defines_label(source, "millis"));
    CHECK(defines_label(source, "micros"));
    CHECK(defines_label(source, "__ardio_timer0_ovf"));
}

TEST(runtime_timer_places_the_vector_table) {
    std::string source;
    ardio::AssembleResult result;
    if (!assemble_runtime("runtime/timer.S", result, source)) return;
    CHECK(result.ok);
    if (!result.ok) return;

    // The table has to cover the reset vector at word 0 and TIMER0_OVF at
    // word 0x20, so the image is at least that long, and both entries are
    // JMP -- opcode 0x940C with the low bit of the target's high bits clear,
    // which for any address under 128K encodes as the word 0x940C.
    CHECK(result.code.size() > 0x20 * 2 + 4);
    if (result.code.size() <= 0x20 * 2 + 4) return;

    auto word_at = [&](size_t word_index) {
        size_t byte = word_index * 2;
        return static_cast<unsigned>(result.code[byte]) |
               (static_cast<unsigned>(result.code[byte + 1]) << 8);
    };
    CHECK_EQ(word_at(0x0000), 0x940Cu);          // reset vector
    CHECK_EQ(word_at(0x0020), 0x940Cu);          // TIMER0_OVF, vector 16

    // Both jump somewhere real, past the end of the 26-vector table.
    CHECK(word_at(0x0001) >= 0x0034u);
    CHECK(word_at(0x0021) >= 0x0034u);
}

TEST(runtime_timer_reads_millis_atomically) {
    std::string source;
    if (!read_source("runtime/timer.S", source)) {
        std::printf("  skip runtime/timer.S not found relative to the cwd\n");
        return;
    }

    // A torn 32-bit read is the whole hazard millis() has to avoid, so the
    // four loads must sit between a cli and a restore of the saved SREG.
    std::string::size_type body = source.find("\nmillis:");
    CHECK(body != std::string::npos);
    if (body == std::string::npos) return;
    std::string::size_type end = source.find("\nmicros:", body);
    CHECK(end != std::string::npos);
    if (end == std::string::npos) return;

    std::string millis_body = source.substr(body, end - body);
    std::string::size_type off = millis_body.find("cli");
    CHECK(off != std::string::npos);
    // Interrupts come back by writing the saved SREG, not by a blind sei --
    // that is what keeps millis() callable from inside an interrupt handler.
    CHECK(millis_body.find("out     SREG_IO") != std::string::npos);
    CHECK(millis_body.find("sei") == std::string::npos);
    if (off == std::string::npos) return;
    CHECK_EQ(millis_body.substr(off).find("lds") != std::string::npos, true);
}

TEST(runtime_adc_assembles) {
    std::string source;
    ardio::AssembleResult result;
    if (!assemble_runtime("runtime/adc.S", result, source)) return;

    CHECK(result.ok);
    CHECK(result.error.empty());
    CHECK(!result.code.empty());
    CHECK_EQ(result.code.size() % 2, size_t(0));
}

TEST(runtime_adc_defines_its_entry_points) {
    std::string source;
    if (!read_source("runtime/adc.S", source)) {
        std::printf("  skip runtime/adc.S not found relative to the cwd\n");
        return;
    }
    CHECK(defines_label(source, "adc_init"));
    CHECK(defines_label(source, "analog_read"));
    CHECK(defines_label(source, "analogRead"));
}

TEST(runtime_adc_reads_the_low_byte_first) {
    std::string source;
    if (!read_source("runtime/adc.S", source)) {
        std::printf("  skip runtime/adc.S not found relative to the cwd\n");
        return;
    }

    // The datasheet requires ADCL before ADCH: reading ADCL locks the pair,
    // reading ADCH releases it. The other order silently mixes two samples.
    std::string::size_type low = source.find("lds     r24, ADCL");
    std::string::size_type high = source.find("lds     r25, ADCH");
    CHECK(low != std::string::npos);
    CHECK(high != std::string::npos);
    if (low == std::string::npos || high == std::string::npos) return;
    CHECK(low < high);
}
