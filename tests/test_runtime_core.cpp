// Checks that ardio's own AVR core runtime (runtime/core.S) is accepted by
// ardio's own assembler. The runtime is only useful if the toolchain in this
// repository can actually build it, so this test assembles the real file from
// disk rather than a copy pasted into the test.

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

} // namespace

TEST(runtime_core_assembles) {
    std::string source;
    if (!read_source("runtime/core.S", source)) {
        std::printf("  skip runtime/core.S not found relative to the cwd\n");
        return;
    }
    CHECK(!source.empty());

    ardio::AssembleResult result = ardio::assemble(source);
    if (!result.ok) std::printf("    assembler said: %s\n", result.error.c_str());
    CHECK(result.ok);
    CHECK(result.error.empty());
    CHECK(!result.code.empty());
    CHECK_EQ(result.code.size() % 2, size_t(0));   // whole 16-bit words
}

TEST(runtime_core_starts_with_reset_entry) {
    std::string source;
    if (!read_source("runtime/core.S", source)) {
        std::printf("  skip runtime/core.S not found relative to the cwd\n");
        return;
    }

    ardio::AssembleResult result = ardio::assemble(source);
    CHECK(result.ok);
    if (!result.ok || result.code.size() < 2) return;

    // Word 0 is the reset entry, which must be `clr r1` -- encoded as
    // `eor r1, r1` = 0x2411, stored little-endian.
    CHECK_EQ(int(result.code[0]), 0x11);
    CHECK_EQ(int(result.code[1]), 0x24);
}
