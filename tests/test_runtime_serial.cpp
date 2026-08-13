// Checks that ardio's own USART0 runtime (runtime/serial.S) is accepted by
// ardio's own assembler. The serial runtime is only useful if the toolchain in
// this repository can actually build it, so this test assembles the real file
// from disk rather than a copy pasted into the test.

#include "harness.h"
#include "runtime_source.h"

#include "ardio/avr/assembler.h"

#include <fstream>
#include <sstream>
#include <string>

namespace {

// The test binary may be started from the build directory or from the source
// root, so try the handful of places the file can be relative to the cwd.
bool read_serial_source(std::string& out) {
    static const char* const paths[] = {
        "runtime/serial.S", "../runtime/serial.S", "../../runtime/serial.S",
        "../../../runtime/serial.S"};
    for (const char* path : paths) {
        std::ifstream in(path, std::ios::binary);
        if (!in) continue;
        std::ostringstream buf;
        buf << in.rdbuf();
        out = buf.str();
        // The runtime is written against the device prelude's AD_* names, so
        // on its own it is not assemblable source.
        out += "\n" + ardio::test::default_prelude();
        return true;
    }
    return false;
}

} // namespace

TEST(runtime_serial_assembles) {
    std::string source;
    if (!read_serial_source(source)) {
        std::printf("  skip runtime/serial.S not found relative to the cwd\n");
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

TEST(runtime_serial_declares_the_expected_entry_points) {
    std::string source;
    if (!read_serial_source(source)) {
        std::printf("  skip runtime/serial.S not found relative to the cwd\n");
        return;
    }

    // The names HardwareSerial.h binds to. A rename on one side without the
    // other would leave sketches failing to link, which is worth catching here.
    static const char* const labels[] = {
        "serial_begin:", "serial_write:", "serial_print:", "serial_print_int:",
        "serial_available:", "serial_read:", "serial_flush:"};
    for (const char* label : labels) {
        if (source.find(label) == std::string::npos)
            std::printf("    missing label: %s\n", label);
        CHECK(source.find(label) != std::string::npos);
    }
}

TEST(runtime_serial_assembles_without_its_fallback_section) {
    std::string source;
    if (!read_serial_source(source)) {
        std::printf("  skip runtime/serial.S not found relative to the cwd\n");
        return;
    }

    // The build drops everything below the marker when linking against a
    // sketch, so the file has to assemble in that form too.
    std::string::size_type marker = source.find("ARDIO_FALLBACK_BEGIN");
    if (marker != std::string::npos) {
        // The prelude was appended by the reader, so it sits below the marker
        // and the trim takes it with it. Put it back afterwards: the point of
        // this test is that the fallback section is droppable, not that the
        // device definitions are.
        std::string trimmed = source.substr(0, source.rfind('\n', marker));
        trimmed += "\n" + ardio::test::default_prelude();
        ardio::AssembleResult result = ardio::assemble(trimmed);
        if (!result.ok) std::printf("    assembler said: %s\n", result.error.c_str());
        CHECK(result.ok);
        CHECK(!result.code.empty());
    }
}
