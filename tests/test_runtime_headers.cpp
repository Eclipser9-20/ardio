// The runtime headers must compile with ardio's own compiler.
//
// These headers describe the API a sketch programs against, so "plausible" is
// not good enough: each one is read from disk, handed to compile_avr() with a
// trivial sketch body appended, and required to come back without an error.
// compile_avr() does not process #include, so the header text is concatenated
// directly rather than included.
//
// A missing file is skipped rather than failed, so that the suite still runs in
// a build tree that does not carry the runtime headers alongside it.

#include "harness.h"

#include "ardio/avr/compiler.h"

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

// The tests may run from the source root or from a build directory beside it.
const std::vector<std::string>& search_paths() {
    static const std::vector<std::string> paths = {
        "runtime/include/",
        "../runtime/include/",
        "../../runtime/include/",
        "../../../runtime/include/",
    };
    return paths;
}

// Reads one runtime header. Returns false when the file cannot be found
// anywhere, which callers treat as "skip", not "fail".
bool read_header(const std::string& name, std::string& out) {
    for (const std::string& prefix : search_paths()) {
        std::ifstream in(prefix + name);
        if (!in) continue;
        std::ostringstream buffer;
        buffer << in.rdbuf();
        out = buffer.str();
        return true;
    }
    return false;
}

// A sketch is only a translation unit once it has an entry point, so every
// header is compiled with the smallest possible one appended.
const char* kEntryPoint = "\nvoid setup() {}\nvoid loop() {}\n";

// Compiles one header and reports the compiler's own message on failure.
void check_header_compiles(const char* name) {
    std::string text;
    if (!read_header(name, text)) {
        std::printf("  skip %s (not found)\n", name);
        return;
    }
    ardio::CompileResult result = ardio::compile_avr(text + kEntryPoint);
    if (!result.ok)
        ::ardio_test::fail(__FILE__, __LINE__,
                           std::string(name) + " did not compile: " + result.error);
    CHECK(!result.assembly.empty());
}

} // namespace

TEST(arduino_header_compiles) {
    check_header_compiles("Arduino.h");
}

TEST(servo_header_compiles) {
    check_header_compiles("Servo.h");
}

TEST(stepper_header_compiles) {
    check_header_compiles("Stepper.h");
}

TEST(wire_header_compiles) {
    check_header_compiles("Wire.h");
}

TEST(liquidcrystal_i2c_header_compiles) {
    check_header_compiles("LiquidCrystal_I2C.h");
}

TEST(ezbutton_header_compiles) {
    check_header_compiles("ezButton.h");
}

// A real sketch pulls in several headers at once, so the whole set has to agree
// on its global and function names as well as compile one at a time.
TEST(runtime_headers_compile_together) {
    const char* names[] = {"Arduino.h", "Servo.h",             "Stepper.h",
                           "Wire.h",    "LiquidCrystal_I2C.h", "ezButton.h"};
    std::string combined;
    for (const char* name : names) {
        std::string text;
        if (!read_header(name, text)) {
            std::printf("  skip combined build (%s not found)\n", name);
            return;
        }
        combined += text;
        combined += '\n';
    }
    ardio::CompileResult result = ardio::compile_avr(combined + kEntryPoint);
    if (!result.ok)
        ::ardio_test::fail(__FILE__, __LINE__,
                           "the runtime headers do not compile together: " + result.error);
}

// The declarations are only worth having if a sketch can call them, so a small
// sketch is compiled against the core header.
TEST(sketch_calls_core_api_from_header) {
    std::string text;
    if (!read_header("Arduino.h", text)) {
        std::printf("  skip sketch build (Arduino.h not found)\n");
        return;
    }
    const char* sketch =
        "\n"
        "void setup() {\n"
        "    pinMode(LED_BUILTIN, OUTPUT);\n"
        "}\n"
        "void loop() {\n"
        "    digitalWrite(LED_BUILTIN, HIGH);\n"
        "    delay(500);\n"
        "    digitalWrite(LED_BUILTIN, LOW);\n"
        "    delay(500);\n"
        "    int level = analogRead(A0);\n"
        "    int scaled = constrain(level, 0, 255);\n"
        "    analogWrite(D9, scaled);\n"
        "}\n";
    ardio::CompileResult result = ardio::compile_avr(text + sketch);
    if (!result.ok)
        ::ardio_test::fail(__FILE__, __LINE__,
                           std::string("a sketch using Arduino.h did not compile: ") +
                               result.error);
}
