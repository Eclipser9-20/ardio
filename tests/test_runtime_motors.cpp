// The motor runtime is written in AVR assembly and is meant to be built by
// ardio's own assembler, with no external toolchain. These tests hold that
// promise to it: each runtime source is read from disk and run through
// ardio::assemble(), which must accept it and produce a non-empty image.
//
// A missing file is skipped rather than failed, so the suite still runs in a
// checkout where the runtime has not been added yet.

#include "harness.h"
#include "runtime_source.h"

#include "ardio/avr/assembler.h"

#include <cstdio>
#include <string>

namespace {

// Tests may run from the source root or from a build directory beneath it, so
// the file is looked for at a few relative depths.
bool read_runtime(const std::string& name, std::string& out) {
    const char* prefixes[] = {"", "../", "../../", "../../../"};
    for (const char* prefix : prefixes) {
        std::string path = std::string(prefix) + "runtime/" + name;
        std::FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) continue;
        out.clear();
        char buf[4096];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
        std::fclose(f);
        // The runtime is written against the device prelude's AD_* names, so
        // on its own it is not assemblable source.
        out += "\n" + ardio::test::default_prelude();
        return true;
    }
    return false;
}

void check_assembles(const char* name) {
    std::string source;
    if (!read_runtime(name, source)) {
        std::printf("  skip runtime/%s (not present)\n", name);
        return;
    }
    CHECK(!source.empty());

    ardio::AssembleResult result = ardio::assemble(source);
    if (!result.ok) std::printf("    %s: %s\n", name, result.error.c_str());
    CHECK(result.ok);
    CHECK(result.error.empty());
    CHECK(!result.code.empty());
    // Every AVR instruction is a whole number of 16-bit words.
    CHECK_EQ(result.code.size() % 2, size_t(0));
}

// Confirms a name is present as a label, so the entry points cannot be
// renamed or dropped without the test noticing.
bool has_label(const std::string& source, const std::string& label) {
    return source.find("\n" + label + ":") != std::string::npos ||
           source.rfind(label + ":", 0) == 0;
}

void check_entry_points(const char* name, const char* const* labels, size_t count) {
    std::string source;
    if (!read_runtime(name, source)) {
        std::printf("  skip runtime/%s (not present)\n", name);
        return;
    }
    for (size_t i = 0; i < count; ++i) CHECK(has_label(source, labels[i]));
}

} // namespace

TEST(servo_runtime_assembles) {
    check_assembles("servo.S");
}

TEST(stepper_runtime_assembles) {
    check_assembles("stepper.S");
}

TEST(servo_runtime_exports_its_entry_points) {
    const char* labels[] = {"servo_attach", "servo_write", "servo_detach"};
    check_entry_points("servo.S", labels, 3);
}

TEST(stepper_runtime_exports_its_entry_points) {
    const char* labels[] = {"stepper_init", "stepper_set_speed", "stepper_step"};
    check_entry_points("stepper.S", labels, 3);
}
