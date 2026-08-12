// The infrared receive runtime (runtime/ir.S) is written in AVR assembly and
// assembled by ardio itself, so the thing worth testing here is exactly that:
// the file goes through ardio::assemble() cleanly and produces real code. If
// the assembler ever loses a mnemonic ir.S relies on, or a branch in it drifts
// out of range, these tests say so.
//
// The tests run from whatever directory ctest happens to pick, so the file is
// looked up against a few plausible roots and the test skips — rather than
// fails — when the runtime tree is not next to it.

#include "harness.h"
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
        if (read_file(path, out)) return true;
    }
    return false;
}

// Confirms a name is present as a label, so an entry point cannot be renamed
// or dropped without a test noticing.
bool has_label(const std::string& source, const std::string& label) {
    return source.find("\n" + label + ":") != std::string::npos ||
           source.rfind(label + ":", 0) == 0;
}

} // namespace

TEST(runtime_ir_assembles) {
    std::string source;
    if (!find_runtime_file("ir.S", source)) {
        std::printf("    (skipping ir.S -- not found from this directory)\n");
        return;
    }
    CHECK(!source.empty());

    auto r = ardio::assemble(source);
    if (!r.ok) std::printf("    (ir.S: %s)\n", r.error.c_str());
    CHECK(r.ok);
    CHECK(r.error.empty());
    CHECK(!r.code.empty());
    // AVR instructions are whole 16-bit words, so the image is always even.
    CHECK_EQ(int(r.code.size() % 2), 0);
    // A header comment alone would still "assemble"; the decoder is a real
    // body of code, so insist on one.
    CHECK(r.code.size() >= 64);
}

TEST(runtime_ir_exports_its_entry_points) {
    std::string source;
    if (!find_runtime_file("ir.S", source)) {
        std::printf("    (skipping ir.S -- not found from this directory)\n");
        return;
    }
    const char* labels[] = {"ir_begin", "ir_available", "ir_read_command",
                            "ir_read_address", "ir_decode", "ir_resume"};
    for (const char* label : labels) CHECK(has_label(source, label));
}
