#include "harness.h"
#include "ardio/avr/sketch.h"

#include <string>

using ardio::preprocess_sketch;
using ardio::SketchResult;

namespace {

bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

// Position of `needle`, or a very large number so that ordering checks fail
// loudly rather than silently passing on a missing string.
size_t at(const std::string& hay, const std::string& needle) {
    size_t p = hay.find(needle);
    return p == std::string::npos ? std::string::npos : p;
}

} // namespace

TEST(sketch_prepends_arduino_header) {
    SketchResult r = preprocess_sketch("void setup() {}\nvoid loop() {}\n");
    CHECK(r.ok);
    CHECK(r.source.rfind("#include <Arduino.h>", 0) == 0);
}

TEST(sketch_does_not_duplicate_arduino_header) {
    SketchResult r = preprocess_sketch("#include <Arduino.h>\nvoid setup() {}\n");
    CHECK(r.ok);
    CHECK(at(r.source, "Arduino.h") == r.source.find("Arduino.h"));
    CHECK(r.source.find("Arduino.h", r.source.find("Arduino.h") + 1) == std::string::npos);
}

TEST(sketch_declares_setup_and_loop) {
    SketchResult r = preprocess_sketch("void setup() {\n}\nvoid loop() {\n}\n");
    CHECK(r.ok);
    CHECK(contains(r.source, "void setup();"));
    CHECK(contains(r.source, "void loop();"));
}

TEST(sketch_allows_call_before_definition) {
    const char* ino =
        "void setup() {\n"
        "  blink(3);\n"
        "}\n"
        "void blink(int times) {\n"
        "  for (int i = 0; i < times; i++) { digitalWrite(13, HIGH); }\n"
        "}\n";
    SketchResult r = preprocess_sketch(ino);
    CHECK(r.ok);
    // The declaration must land before the body that calls it.
    CHECK(at(r.source, "void blink(int times);") < at(r.source, "blink(3);"));
}

TEST(sketch_declaration_goes_after_includes) {
    const char* ino =
        "#include <Wire.h>\n"
        "#include <SPI.h>\n"
        "void setup() {}\n";
    SketchResult r = preprocess_sketch(ino);
    CHECK(r.ok);
    CHECK(at(r.source, "<SPI.h>") < at(r.source, "void setup();"));
    CHECK(at(r.source, "void setup();") < at(r.source, "void setup() {}"));
}

TEST(sketch_preserves_original_lines) {
    const char* ino = "int counter = 0;\nvoid setup() {\n  counter++;\n}\n";
    SketchResult r = preprocess_sketch(ino);
    CHECK(r.ok);
    CHECK(contains(r.source, "int counter = 0;"));
    CHECK(at(r.source, "int counter = 0;") < at(r.source, "void setup() {"));
    CHECK(contains(r.source, "  counter++;"));
}

// ------------------------------------------------------------ return types --

TEST(sketch_multiword_return_type) {
    SketchResult r = preprocess_sketch("unsigned long uptime() {\n  return millis();\n}\n");
    CHECK(r.ok);
    CHECK(contains(r.source, "unsigned long uptime();"));
}

TEST(sketch_pointer_return_type) {
    SketchResult r = preprocess_sketch("const char* name() {\n  return \"ardio\";\n}\n");
    CHECK(r.ok);
    CHECK(contains(r.source, "const char* name();"));
}

TEST(sketch_class_return_type) {
    SketchResult r = preprocess_sketch("String greeting() {\n  return String(\"hi\");\n}\n");
    CHECK(r.ok);
    CHECK(contains(r.source, "String greeting();"));
}

TEST(sketch_reference_return_type) {
    SketchResult r = preprocess_sketch("int& slot(int i) {\n  static int v; return v;\n}\n");
    CHECK(r.ok);
    CHECK(contains(r.source, "int& slot(int i);"));
}

TEST(sketch_multiline_signature_is_collapsed) {
    const char* ino =
        "int add(int a,\n"
        "        int b)\n"
        "{\n"
        "  return a + b;\n"
        "}\n";
    SketchResult r = preprocess_sketch(ino);
    CHECK(r.ok);
    CHECK(contains(r.source, "int add(int a, int b);"));
}

TEST(sketch_keeps_string_default_argument) {
    SketchResult r = preprocess_sketch("void say(const char* s = \"hi\") {\n}\n");
    CHECK(r.ok);
    CHECK(contains(r.source, "void say(const char* s = \"hi\");"));
}

// --------------------------------------------------------------- literals ---

TEST(sketch_brace_inside_string_does_not_confuse_depth) {
    const char* ino =
        "void setup() {\n"
        "  Serial.println(\"}\");\n"
        "}\n"
        "void loop() {\n"
        "  Serial.println(\"{ not a block\");\n"
        "}\n";
    SketchResult r = preprocess_sketch(ino);
    CHECK(r.ok);
    CHECK(contains(r.source, "void setup();"));
    CHECK(contains(r.source, "void loop();"));
}

TEST(sketch_paren_inside_string_does_not_confuse_scan) {
    const char* ino =
        "void report() {\n"
        "  Serial.println(\"f(int a) {\");\n"
        "}\n";
    SketchResult r = preprocess_sketch(ino);
    CHECK(r.ok);
    CHECK(contains(r.source, "void report();"));
    CHECK(!contains(r.source, "f(int a);"));
}

TEST(sketch_char_literal_braces_ignored) {
    const char* ino =
        "void a() {\n"
        "  char c = '{';\n"
        "  char d = '}';\n"
        "  char e = '\\'';\n"
        "}\n"
        "void b() {}\n";
    SketchResult r = preprocess_sketch(ino);
    CHECK(r.ok);
    CHECK(contains(r.source, "void a();"));
    CHECK(contains(r.source, "void b();"));
}

TEST(sketch_escaped_quote_in_string) {
    const char* ino =
        "void a() {\n"
        "  Serial.println(\"he said \\\"} {\\\" loudly\");\n"
        "}\n"
        "void b() {}\n";
    SketchResult r = preprocess_sketch(ino);
    CHECK(r.ok);
    CHECK(contains(r.source, "void a();"));
    CHECK(contains(r.source, "void b();"));
}

TEST(sketch_unterminated_string_is_an_error) {
    SketchResult r = preprocess_sketch("void a() {\n  Serial.println(\"oops);\n}\n");
    CHECK(!r.ok);
    CHECK(!r.error.empty());
}

// --------------------------------------------------------------- comments ---

TEST(sketch_line_comment_containing_code) {
    const char* ino =
        "// void ghost() {\n"
        "void real() {\n"
        "}\n";
    SketchResult r = preprocess_sketch(ino);
    CHECK(r.ok);
    CHECK(contains(r.source, "void real();"));
    CHECK(!contains(r.source, "void ghost();"));
}

TEST(sketch_block_comment_containing_code) {
    const char* ino =
        "/* void ghost() {\n"
        "     nothing here\n"
        "   } */\n"
        "void real() {\n"
        "}\n";
    SketchResult r = preprocess_sketch(ino);
    CHECK(r.ok);
    CHECK(contains(r.source, "void real();"));
    CHECK(!contains(r.source, "void ghost();"));
}

TEST(sketch_comment_between_type_and_name) {
    SketchResult r = preprocess_sketch("int /* the answer */ answer() {\n  return 42;\n}\n");
    CHECK(r.ok);
    CHECK(contains(r.source, "int answer();"));
}

TEST(sketch_comment_markers_inside_string_are_not_comments) {
    const char* ino =
        "void a() {\n"
        "  Serial.println(\"// not a comment\");\n"
        "  Serial.println(\"/* nor this\");\n"
        "}\n"
        "void b() {}\n";
    SketchResult r = preprocess_sketch(ino);
    CHECK(r.ok);
    CHECK(contains(r.source, "void a();"));
    CHECK(contains(r.source, "void b();"));
}

TEST(sketch_unterminated_block_comment_is_an_error) {
    SketchResult r = preprocess_sketch("/* forever\nvoid a() {}\n");
    CHECK(!r.ok);
    CHECK(!r.error.empty());
}

TEST(sketch_declaration_lands_before_leading_comment_free_body) {
    const char* ino =
        "// blink helper\n"
        "void blink() {}\n";
    SketchResult r = preprocess_sketch(ino);
    CHECK(r.ok);
    // The comment stays where the user wrote it, above the definition.
    CHECK(at(r.source, "// blink helper") < at(r.source, "void blink() {}"));
    CHECK(contains(r.source, "void blink();"));
}

// ---------------------------------------------------------- preprocessor ----

TEST(sketch_directive_with_braces_is_skipped) {
    const char* ino =
        "#define BLOCK { 1, 2, 3 }\n"
        "#define CALL(x) doThing(x)\n"
        "void setup() {}\n";
    SketchResult r = preprocess_sketch(ino);
    CHECK(r.ok);
    CHECK(contains(r.source, "void setup();"));
    CHECK(!contains(r.source, "CALL(x);"));
    CHECK(contains(r.source, "#define BLOCK { 1, 2, 3 }"));
}

TEST(sketch_directive_does_not_leak_into_declaration) {
    SketchResult r = preprocess_sketch("#define PIN 13\nvoid setup() {}\n");
    CHECK(r.ok);
    CHECK(contains(r.source, "void setup();"));
    CHECK(!contains(r.source, "#define PIN 13 void setup();"));
}

TEST(sketch_multiline_directive) {
    const char* ino =
        "#define LOOP(n) \\\n"
        "  for (int i = 0; i < n; i++) { }\n"
        "void setup() {}\n";
    SketchResult r = preprocess_sketch(ino);
    CHECK(r.ok);
    CHECK(contains(r.source, "void setup();"));
}

// ------------------------------------------------------- nested functions ---

TEST(sketch_class_methods_get_no_file_scope_declaration) {
    const char* ino =
        "class Blinker {\n"
        "public:\n"
        "  void begin() { pinMode(13, OUTPUT); }\n"
        "  int period() { return 500; }\n"
        "};\n"
        "void setup() {}\n";
    SketchResult r = preprocess_sketch(ino);
    CHECK(r.ok);
    CHECK(contains(r.source, "void setup();"));
    CHECK(!contains(r.source, "void begin();"));
    CHECK(!contains(r.source, "int period();"));
}

TEST(sketch_struct_methods_get_no_declaration) {
    const char* ino =
        "struct Point {\n"
        "  int x, y;\n"
        "  int sum() { return x + y; }\n"
        "};\n"
        "void loop() {}\n";
    SketchResult r = preprocess_sketch(ino);
    CHECK(r.ok);
    CHECK(contains(r.source, "void loop();"));
    CHECK(!contains(r.source, "int sum();"));
}

TEST(sketch_out_of_class_method_is_not_redeclared) {
    const char* ino =
        "class Timer {\n"
        "public:\n"
        "  void start();\n"
        "};\n"
        "void Timer::start() {\n"
        "}\n"
        "void setup() {}\n";
    SketchResult r = preprocess_sketch(ino);
    CHECK(r.ok);
    CHECK(contains(r.source, "void setup();"));
    CHECK(!contains(r.source, "void Timer::start();\nvoid"));
}

TEST(sketch_nested_blocks_do_not_produce_declarations) {
    const char* ino =
        "void setup() {\n"
        "  if (digitalRead(2)) {\n"
        "    while (true) { break; }\n"
        "  }\n"
        "}\n"
        "void loop() {}\n";
    SketchResult r = preprocess_sketch(ino);
    CHECK(r.ok);
    CHECK(contains(r.source, "void setup();"));
    CHECK(contains(r.source, "void loop();"));
    CHECK(!contains(r.source, "while (true);"));
    CHECK(!contains(r.source, "if (digitalRead(2));"));
}

// ----------------------------------------------------------- non-functions --

TEST(sketch_array_initialiser_is_not_a_function) {
    const char* ino = "int pins[] = {2, 3, 4};\nvoid setup() {}\n";
    SketchResult r = preprocess_sketch(ino);
    CHECK(r.ok);
    CHECK(contains(r.source, "void setup();"));
    CHECK(!contains(r.source, "pins[];"));
    CHECK(contains(r.source, "int pins[] = {2, 3, 4};"));
}

TEST(sketch_global_object_with_constructor_args) {
    const char* ino = "Servo arm(9);\nvoid setup() {}\n";
    SketchResult r = preprocess_sketch(ino);
    CHECK(r.ok);
    CHECK(contains(r.source, "void setup();"));
    CHECK(!contains(r.source, "Servo arm(9);\nServo"));
}

TEST(sketch_prototype_only_needs_no_extra_work) {
    const char* ino = "void helper(int x);\nvoid setup() { helper(1); }\n";
    SketchResult r = preprocess_sketch(ino);
    CHECK(r.ok);
    CHECK(contains(r.source, "void setup();"));
}

TEST(sketch_empty_input) {
    SketchResult r = preprocess_sketch("");
    CHECK(r.ok);
    CHECK(r.source == "#include <Arduino.h>\n");
}

TEST(sketch_unbalanced_braces_reported) {
    SketchResult r = preprocess_sketch("void setup() {\n");
    CHECK(!r.ok);
    CHECK(!r.error.empty());
}

TEST(sketch_realistic_blink) {
    const char* ino =
        "// Blink, the hello world of Arduino.\n"
        "#include <Wire.h>\n"
        "\n"
        "const int LED = 13;\n"
        "unsigned long lastToggle = 0;\n"
        "\n"
        "void setup() {\n"
        "  pinMode(LED, OUTPUT);\n"
        "  Serial.begin(9600);\n"
        "  Serial.println(\"ready {}\");\n"
        "}\n"
        "\n"
        "void loop() {\n"
        "  if (elapsed(500)) { toggle(); }\n"
        "}\n"
        "\n"
        "bool elapsed(unsigned long ms) {\n"
        "  return millis() - lastToggle > ms;\n"
        "}\n"
        "\n"
        "void toggle() {\n"
        "  lastToggle = millis();\n"
        "  digitalWrite(LED, !digitalRead(LED));\n"
        "}\n";
    SketchResult r = preprocess_sketch(ino);
    CHECK(r.ok);
    CHECK(contains(r.source, "void setup();"));
    CHECK(contains(r.source, "void loop();"));
    CHECK(contains(r.source, "bool elapsed(unsigned long ms);"));
    CHECK(contains(r.source, "void toggle();"));
    // Every declaration precedes the first definition and the calls in loop().
    CHECK(at(r.source, "void toggle();") < at(r.source, "void setup() {"));
    CHECK(at(r.source, "bool elapsed(unsigned long ms);") < at(r.source, "elapsed(500)"));
    // Globals and includes stay above the declarations.
    CHECK(at(r.source, "#include <Wire.h>") < at(r.source, "void setup();"));
    CHECK(at(r.source, "const int LED = 13;") < at(r.source, "void setup();"));
}
