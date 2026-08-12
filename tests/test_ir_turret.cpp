// The IR turret example is the second real-world sketch ardio's own compiler
// is expected to handle end to end. This test reads it off disk and puts it
// through the same two stages `ardio build` does -- sketch preprocessing, then
// the AVR compiler -- so a regression in either shows up here rather than only
// when someone builds the example by hand.
//
// The example is found relative to the working directory, which differs between
// running the tests from the source root and from a build directory. If it
// cannot be found at all the test skips rather than fails, so a partial
// checkout does not look like a compiler bug.

#include "harness.h"
#include "ardio/avr/compiler.h"
#include "ardio/avr/sketch.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

namespace {

const char* kSearchPaths[] = {
    "examples/ir_turret/",
    "../examples/ir_turret/",
    "../../examples/ir_turret/",
    "../../../examples/ir_turret/",
};

// Returns the sketch text, or an empty string when the example is not present.
std::string read_example(const char* name) {
    for (const char* dir : kSearchPaths) {
        std::ifstream in(std::string(dir) + name);
        if (!in) continue;
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }
    return {};
}

bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

} // namespace

TEST(ir_turret_example_compiles) {
    std::string ino = read_example("ir_turret.ino");
    if (ino.empty()) {
        std::printf("  skip ir_turret.ino not found from this directory\n");
        return;
    }

    ardio::SketchResult sketch = ardio::preprocess_sketch(ino);
    CHECK(sketch.ok);
    if (!sketch.ok) {
        std::printf("    sketch error: %s\n", sketch.error.c_str());
        return;
    }

    ardio::CompileResult compiled = ardio::compile_avr(sketch.source);
    CHECK(compiled.ok);
    if (!compiled.ok) {
        std::printf("    compiler error: %s\n", compiled.error.c_str());
        return;
    }
    CHECK(!compiled.assembly.empty());
}

// The sketch declares the runtime entry points itself, because the compiler
// does not process #include yet. If that ever regresses to relying on a header,
// the example stops being a standalone reproduction of the limitation.
TEST(ir_turret_example_declares_its_runtime) {
    std::string ino = read_example("ir_turret.ino");
    if (ino.empty()) {
        std::printf("  skip ir_turret.ino not found from this directory\n");
        return;
    }
    CHECK(contains(ino, "void pinMode(int pin, int mode);"));
    CHECK(contains(ino, "void digitalWrite(int pin, int value);"));
    CHECK(contains(ino, "int digitalRead(int pin);"));
    CHECK(contains(ino, "void delay(int ms);"));
    CHECK(contains(ino, "void delayMicroseconds(int us);"));
}
