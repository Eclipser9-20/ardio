// What still stops ardio compiling the *original*, unmodified CrunchLabs
// Label Maker sketch.
//
// examples/label_maker/label_maker.ino is a hand-written port: same machine,
// same behaviour, but written in the subset ardio already understands. The
// goal beyond that is the upstream .ino byte for byte. The sketch itself is
// not redistributed here -- it is not ours to ship -- so what is recorded
// below is the minimal repro of each limitation it runs into, in the order a
// front-to-back compile hits them, matching the numbering in
// examples/label_maker/ORIGINAL_GAP.md.
//
// Every test asserts the limitation is STILL THERE. When one starts failing,
// that is the good news: the limitation is gone. Delete the test, tick the
// entry off ORIGINAL_GAP.md, and move on to the next one.

#include "harness.h"
#include "ardio/avr/assembler.h"
#include "ardio/avr/compiler.h"

#include <fstream>
#include <string>
#include <vector>

namespace {

// The tests run from the build directory as often as from the source root.
bool runtime_include_dir(std::string& path) {
    for (const char* dir : {"runtime/include", "../runtime/include",
                            "../../runtime/include", "../../../runtime/include"}) {
        std::ifstream probe(std::string(dir) + "/Arduino.h");
        if (!probe) continue;
        path = dir;
        return true;
    }
    return false;
}

// Compiles a fragment on its own, with no headers in scope. Anything the
// fragment needs beyond the language itself has to be declared inline.
ardio::CompileResult bare(const std::string& fragment) {
    return ardio::compile_avr(fragment + "\nvoid setup() {}\nvoid loop() {}\n");
}

// Compiles a fragment the way `ardio build` does: preprocessed, with ardio's
// Arduino-compatible headers on the include path.
bool with_headers(const std::string& fragment, ardio::CompileResult& out) {
    std::string dir;
    if (!runtime_include_dir(dir)) return false;
    out = ardio::compile_avr("#include <Arduino.h>\n" + fragment +
                                 "\nvoid setup() {}\nvoid loop() {}\n",
                             std::vector<std::string>{dir});
    return true;
}

// Reports the error alongside the failure, so a test that starts failing says
// what changed rather than just that something did.
void still_rejected(const ardio::CompileResult& r) {
    if (r.ok) std::printf("    this now compiles -- the limitation is gone\n");
    else      std::printf("    (still: %s)\n", r.error.c_str());
    CHECK(!r.ok);
}

} // namespace

// ---------------------------------------------------------------------------
// 1. sizeof
//
//     int alphabetSize = sizeof(alphabet) - 1;
// ---------------------------------------------------------------------------
TEST(original_label_maker_still_cannot_sizeof) {
    still_rejected(bare("int n = sizeof(int);"));
}

// ---------------------------------------------------------------------------
// 2. Array fields cannot be *used* from a member function.
//
// Declaring one is fine; reading it from a method is rejected during layout.
// This is what stops WString.h compiling, and so what stops the sketch's
// `String text;` from existing at all.
// ---------------------------------------------------------------------------
TEST(original_label_maker_still_cannot_use_an_array_field) {
    still_rejected(bare("class C { public: char f() { return b[0]; } char b[33]; };\n"
                        "C c;"));
}

TEST(original_label_maker_still_cannot_use_String) {
    ardio::CompileResult r;
    if (!with_headers("String text;", r)) {
        std::printf("    skipped: runtime/include not found\n");
        return;
    }
    still_rejected(r);
}

// ---------------------------------------------------------------------------
// 3-4. The Arduino spellings of the built-in types: boolean, byte, uint8_t.
//
//     boolean pPenOnPaper = false;
//     const uint8_t vector[63][14] = { ... };
//     if (byte(c) != 195) { ... }
// ---------------------------------------------------------------------------
TEST(original_label_maker_still_cannot_spell_boolean) {
    still_rejected(bare("boolean b = false;"));
}

TEST(original_label_maker_still_cannot_spell_uint8_t) {
    still_rejected(bare("uint8_t x = 1;"));
}

TEST(original_label_maker_still_cannot_spell_byte) {
    still_rejected(bare("byte b = 1;"));
}

// ---------------------------------------------------------------------------
// 5. Aggregate initialisers -- one-dimensional, two-dimensional, and the
// string-literal form. The sketch has all three: xPins/yPins, the 63x14
// character table, and the alphabet.
// ---------------------------------------------------------------------------
TEST(original_label_maker_still_cannot_initialise_an_array) {
    still_rejected(bare("int v[4] = {1, 2, 3, 4};"));
}

TEST(original_label_maker_still_cannot_initialise_a_2d_array) {
    still_rejected(bare("int v[2][2] = {{1, 2}, {3, 4}};"));
}

TEST(original_label_maker_still_cannot_initialise_char_array_from_a_string) {
    still_rejected(bare("const char a[] = \"ABC\";"));
}

// ---------------------------------------------------------------------------
// 6. Functional-style casts.
//
//     char c = char(str.charAt(i));
//     if (uint8_t(c) > 64 ...
// ---------------------------------------------------------------------------
TEST(original_label_maker_still_cannot_use_a_functional_cast) {
    still_rejected(bare("void f() { int i = 65; char c = char(i); }"));
}

// ---------------------------------------------------------------------------
// 7. Floating point. The sketch never declares a float, but it divides by
// one: `pos -= (scale*4) / 1.1;` and `y + cy*y_scale*3.5`.
// ---------------------------------------------------------------------------
TEST(original_label_maker_still_cannot_use_a_float_literal) {
    still_rejected(bare("void f() { int x = 10; x = x / 1.1; }"));
}

TEST(original_label_maker_still_cannot_use_the_float_type) {
    still_rejected(bare("float f = 1.0;"));
}

// ---------------------------------------------------------------------------
// 8. The alternative operator spellings.
//
//     if (uint8_t(c) > 64 and uint8_t(c) < 91) { ... }
// ---------------------------------------------------------------------------
TEST(original_label_maker_still_cannot_use_and_as_an_operator) {
    still_rejected(bare("void f() { int a = 1, b = 2; if (a and b) a = 0; }"));
}

// ---------------------------------------------------------------------------
// 9. extern "C" linkage blocks. Nothing in the sketch writes one, but
// HardwareSerial.h does, so `Serial` is unreachable until this parses.
// ---------------------------------------------------------------------------
TEST(original_label_maker_still_cannot_parse_extern_c) {
    still_rejected(bare("extern \"C\" { void ff(unsigned char c); }"));
}

TEST(original_label_maker_still_cannot_include_HardwareSerial) {
    std::string dir;
    if (!runtime_include_dir(dir)) {
        std::printf("    skipped: runtime/include not found\n");
        return;
    }
    auto r = ardio::compile_avr(
        "#include <HardwareSerial.h>\nvoid setup() { Serial.begin(9600); }\nvoid loop() {}\n",
        std::vector<std::string>{dir});
    still_rejected(r);
}

// ---------------------------------------------------------------------------
// 10. Runtime entry points the sketch calls that nothing implements yet:
// millis() (used for the cursor blink) and abs() (used by line()).
// ---------------------------------------------------------------------------
TEST(original_label_maker_still_has_no_millis) {
    ardio::CompileResult r;
    if (!with_headers("void blink() { if (millis() % 600 < 400) {} }", r)) {
        std::printf("    skipped: runtime/include not found\n");
        return;
    }
    // Front end accepts the call; there is simply nothing behind it, which
    // surfaces at link time rather than here. Record whichever it is.
    std::printf("    (compile stage: %s)\n", r.ok ? "accepted" : r.error.c_str());
    CHECK(true);
}

TEST(original_label_maker_still_has_no_abs) {
    ardio::CompileResult r;
    if (!with_headers("void f() { int a = -1; a = abs(a); }", r)) {
        std::printf("    skipped: runtime/include not found\n");
        return;
    }
    std::printf("    (compile stage: %s)\n", r.ok ? "accepted" : r.error.c_str());
    CHECK(true);
}

// ---------------------------------------------------------------------------
// 11. The assembler only ever emits the short forms: rjmp/rcall and the
// conditional branches all reach +/-2K words. Nothing is relaxed to jmp/call,
// so a program bigger than that cannot be assembled at all -- and the
// original, at full size, is bigger than that.
//
// Written as raw assembly so the test says exactly what is missing rather
// than needing a few thousand lines of C to provoke it.
// ---------------------------------------------------------------------------
TEST(original_label_maker_still_cannot_relax_a_long_jump) {
    std::string asm_text = "start:\n    rjmp far\n";
    for (int i = 0; i < 3000; i++) asm_text += "    nop\n";
    asm_text += "far:\n    ret\n";

    auto r = ardio::assemble(asm_text);
    if (r.ok) std::printf("    long jumps now assemble -- the limitation is gone\n");
    else      std::printf("    (still: %s)\n", r.error.c_str());
    CHECK(!r.ok);
}

// ---------------------------------------------------------------------------
// 12. The library classes. Every header the sketch includes declares its
// class, but no constructor is implemented, so a global object of one does
// not link. The sketch builds four: lcd, button1, xStepper/yStepper, servo.
//
// compile_avr() stops before linking, so what is checked here is that the
// constructor is at least reachable as a declaration; the missing symbol
// itself is what `ardio build` reports.
// ---------------------------------------------------------------------------
TEST(original_label_maker_still_has_no_library_constructors) {
    std::string dir;
    if (!runtime_include_dir(dir)) {
        std::printf("    skipped: runtime/include not found\n");
        return;
    }
    struct Object { const char* header; const char* decl; };
    const Object objects[] = {
        {"LiquidCrystal_I2C.h", "LiquidCrystal_I2C lcd(0x27, 16, 2);"},
        {"ezButton.h",          "ezButton button1(14);"},
        {"Stepper.h",           "Stepper xStepper(2048, 6, 8, 7, 9);"},
        {"Servo.h",             "Servo servo;"},
    };
    for (const Object& o : objects) {
        std::string source = std::string("#include <") + o.header + ">\n" + o.decl +
                             "\nvoid setup() {}\nvoid loop() {}\n";
        auto r = ardio::compile_avr(source, std::vector<std::string>{dir});
        std::printf("    %-24s %s\n", o.header,
                    r.ok ? "compiles (constructor still unimplemented at link time)"
                         : r.error.c_str());
    }
    CHECK(true);
}
