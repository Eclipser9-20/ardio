// Checks that ardio's own string runtime (runtime/string.S) is accepted by
// ardio's own assembler. The routines behind String are only useful if the
// toolchain in this repository can actually build them, so this test assembles
// the real file from disk rather than a copy pasted into the test.

#include "harness.h"

#include "ardio/avr/assembler.h"

#include <fstream>
#include <sstream>
#include <string>

namespace {

// The test binary may be started from the build directory or from the source
// root, so try the handful of places the file can be relative to the cwd.
bool read_source(const char* relative, std::string& out) {
    static const char* const prefixes[] = {"", "../", "../../", "../../../"};
    for (const char* prefix : prefixes) {
        std::ifstream in(std::string(prefix) + relative, std::ios::binary);
        if (!in) continue;
        std::ostringstream buf;
        buf << in.rdbuf();
        out = buf.str();
        return true;
    }
    return false;
}

} // namespace

TEST(runtime_string_assembles) {
    std::string source;
    if (!read_source("runtime/string.S", source)) {
        std::printf("  skip runtime/string.S not found relative to the cwd\n");
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

TEST(runtime_string_defines_every_primitive) {
    std::string source;
    if (!read_source("runtime/string.S", source)) {
        std::printf("  skip runtime/string.S not found relative to the cwd\n");
        return;
    }

    // WString.h calls exactly these; a missing label would only show up as a
    // link failure inside a sketch, which is a much worse place to find it.
    static const char* const entry_points[] = {
        "str_len:", "str_copy:", "str_append:", "str_compare:",
        "str_from_int:", "str_index_of:", "str_char_at:",
    };
    for (const char* label : entry_points) {
        if (source.find(label) == std::string::npos)
            std::printf("    missing entry point %s\n", label);
        CHECK(source.find(label) != std::string::npos);
    }
}
