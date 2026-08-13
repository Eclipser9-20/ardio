// The runtime .S files are assembled by ardio itself, so the thing worth
// testing is exactly that: each one goes through ardio::assemble() cleanly and
// produces real code. If the assembler ever loses a mnemonic one of these
// relies on, these tests say which file and which line.
//
// The tests run from whatever directory ctest happens to pick, so each file is
// looked up against a few plausible roots and the test skips (rather than
// fails) when the runtime tree is not next to it.

#include "harness.h"
#include "runtime_source.h"
#include "ardio/avr/assembler.h"

#include <cstdio>
#include <string>

namespace {

// Reads a whole file, or returns false if it is not there.
bool read_file(const std::string& path, std::string& out) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    out.clear();
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    std::fclose(f);
    return true;
}

// Tries the usual places a build directory can sit relative to the source.
bool find_runtime_file(const char* name, std::string& out) {
    const char* roots[] = {"runtime/", "../runtime/", "../../runtime/",
                           "../../../runtime/"};
    for (const char* root : roots) {
        std::string path = std::string(root) + name;
        if (read_file(path, out)) {
            // The runtime is written against the device prelude's AD_* names,
            // so on its own it is not assemblable source.
            out += "\n" + ardio::test::default_prelude();
            return true;
        }
    }
    return false;
}

// Assembles one runtime file; skips quietly if it is not on disk.
void check_assembles(const char* name) {
    std::string source;
    if (!find_runtime_file(name, source)) {
        std::printf("    (skipping %s -- not found from this directory)\n", name);
        return;
    }
    CHECK(!source.empty());

    auto r = ardio::assemble(source);
    if (!r.ok) std::printf("    (%s: %s)\n", name, r.error.c_str());
    CHECK(r.ok);
    CHECK(!r.code.empty());
    // AVR instructions are whole 16-bit words, so the image is always even.
    CHECK_EQ(int(r.code.size() % 2), 0);
}

} // namespace

TEST(runtime_wire_assembles) {
    check_assembles("wire.S");
}

TEST(runtime_lcd_assembles) {
    check_assembles("lcd.S");
}

TEST(runtime_button_assembles) {
    check_assembles("button.S");
}

TEST(runtime_files_are_substantial) {
    // A truncated or empty runtime would still "assemble", so check that each
    // file actually carries a body of code rather than a header comment.
    const char* names[] = {"wire.S", "lcd.S", "button.S"};
    for (const char* name : names) {
        std::string source;
        if (!find_runtime_file(name, source)) continue;
        auto r = ardio::assemble(source);
        if (!r.ok) continue;
        CHECK(r.code.size() >= 32);
    }
}
