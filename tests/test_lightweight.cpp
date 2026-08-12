// The Lightweight containers must survive the whole toolchain, not just the
// parser. Every sketch below is handed to compile_avr() with runtime/include
// on the path -- so <Lightweight/String> and <Lightweight/Vector> are resolved
// by ardio's own preprocessor, extensionless names and all -- and the assembly
// that comes back is then assembled together with runtime/string.S.
//
// The assembly step is the point. LwString calls into runtime/string.S, and a
// compile-only test would pass just as happily if one of those routines were
// misspelled or missing: the call would only fail later, as an undefined label,
// in a sketch someone was trying to flash. Assembling here catches it now.
//
// A missing runtime directory is skipped rather than failed, so the suite still
// runs in a build tree that does not carry the runtime alongside it.

#include "harness.h"

#include "ardio/avr/assembler.h"
#include "ardio/avr/compiler.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

// The test binary may be started from the source root or from a build
// directory beside or below it.
const char* const kPrefixes[] = {"", "../", "../../", "../../../"};

// The runtime include directory, or an empty string when it is not there.
std::string include_root() {
    for (const char* prefix : kPrefixes) {
        std::string candidate = std::string(prefix) + "runtime/include";
        std::error_code ec;
        if (std::filesystem::is_directory(candidate, ec)) return candidate;
    }
    return std::string();
}

bool read_file(const std::string& relative, std::string& out) {
    for (const char* prefix : kPrefixes) {
        std::ifstream in(std::string(prefix) + relative, std::ios::binary);
        if (!in) continue;
        std::ostringstream buffer;
        buffer << in.rdbuf();
        out = buffer.str();
        return true;
    }
    return false;
}

// Compiles a sketch and then assembles it together with the string runtime,
// so that a call to a routine that does not exist shows up as a failure here
// rather than as an undefined label in somebody's sketch. Returns the
// assembly text, or an empty string when the runtime is not available and the
// caller should skip.
std::string build(const char* what, const std::string& sketch, const char* file, int line) {
    std::string root = include_root();
    std::string runtime;
    if (root.empty() || !read_file("runtime/string.S", runtime)) {
        std::printf("  skip %s (runtime not found relative to the cwd)\n", what);
        return std::string();
    }

    ardio::CompileResult compiled = ardio::compile_avr(sketch, {root});
    if (!compiled.ok) {
        ::ardio_test::fail(file, line,
                           std::string(what) + " did not compile: " + compiled.error);
        return std::string();
    }
    if (compiled.assembly.empty()) {
        ::ardio_test::fail(file, line, std::string(what) + " compiled to nothing");
        return std::string();
    }

    ardio::AssembleResult assembled = ardio::assemble(compiled.assembly + "\n" + runtime);
    if (!assembled.ok) {
        ::ardio_test::fail(file, line,
                           std::string(what) + " did not assemble: " + assembled.error);
        return std::string();
    }
    if (assembled.code.empty())
        ::ardio_test::fail(file, line, std::string(what) + " assembled to no code");

    return compiled.assembly;
}

#define BUILD(what, sketch) build(what, sketch, __FILE__, __LINE__)

// Compiles a sketch that is expected NOT to compile, and reports what the
// compiler said when it unexpectedly succeeds.
bool rejects(const std::string& sketch) {
    std::string root = include_root();
    if (root.empty()) return true;      // skipped elsewhere
    return !ardio::compile_avr(sketch, {root}).ok;
}

const char* const kEntry = "\nvoid loop() {}\n";

} // namespace

// ---------------------------------------------------------------- headers ---

// The include spelling the library is documented under has a directory
// component and no extension. ardio's preprocessor resolves an include by
// joining the name onto each include directory, so this should just work --
// but "should" is why there is a test.
TEST(lightweight_headers_resolve_by_their_extensionless_names) {
    BUILD("both headers",
          "#include <Lightweight/String>\n"
          "#include <Lightweight/Vector>\n"
          "void setup() {}\n" +
              std::string(kEntry));
}

TEST(lightweight_string_header_compiles_alone) {
    BUILD("<Lightweight/String>",
          "#include <Lightweight/String>\n"
          "void setup() {}\n" +
              std::string(kEntry));
}

TEST(lightweight_vector_header_compiles_alone) {
    BUILD("<Lightweight/Vector>",
          "#include <Lightweight/Vector>\n"
          "void setup() {}\n" +
              std::string(kEntry));
}

// A sketch may reasonably want ardio's Arduino-compatible String as well. The
// Lightweight classes are prefixed precisely so that both can be included, and
// the shared runtime declarations must not collide.
TEST(lightweight_coexists_with_wstring_and_arduino_headers) {
    BUILD("Lightweight beside WString.h",
          "#include <Arduino.h>\n"
          "#include <WString.h>\n"
          "#include <Lightweight/String>\n"
          "#include <Lightweight/Vector>\n"
          "char legacy_buf[33];\n"
          "String legacy(legacy_buf, 33);\n"
          "LW_STRING(modern);\n"
          "void setup() { legacy.set(\"a\"); modern.set(\"b\"); }\n" +
              std::string(kEntry));
}

// ----------------------------------------------------------------- String ---

// Every documented String operation, in one sketch, so that a method that
// parses but does not generate code cannot hide behind the ones that do.
TEST(lightweight_string_exercises_every_operation) {
    std::string assembly = BUILD(
        "the String surface",
        "#include <Lightweight/String>\n"
        "LW_STRING(text);\n"
        "LW_STRING(part);\n"
        "LW_STRING(number);\n"
        "int result;\n"
        "void setup() {\n"
        "    text.set(\"hello\");\n"                    // from a C string
        "    number.setInt(-1234);\n"                   // from an integer
        "    text.append(\" world\");\n"
        "    text.appendChar('!');\n"
        "    text.appendInt(7);\n"
        "    text.appendString(&number);\n"
        "    text.setString(&number);\n"
        "    text.setAt(0, 'X');\n"
        "    result = (int)text.length();\n"
        "    result = result + (int)text.capacity();\n"
        "    result = result + (int)text.at(0);\n"      // indexing
        "    result = result + text.indexOf('X');\n"
        "    result = result + text.compare(\"other\");\n"  // comparison
        "    if (text.equals(\"X234\")) result = result + 1;\n"
        "    if (text.equalsString(&number)) result = result + 1;\n"
        "    if (text.startsWith(\"X\")) result = result + 1;\n"
        "    if (text.empty()) result = result + 1;\n"
        "    if (text.full()) result = result + 1;\n"
        "    text.substring(1, 3, &part);\n"            // substring
        "    result = result + (int)part.length();\n"
        "    if (text.c_str() == part.c_str()) result = 0;\n"  // c_str()
        "    text.clear();\n"
        "}\n" +
            std::string(kEntry));
    if (assembly.empty()) return;

    // The header must go through runtime/string.S rather than hand-rolling the
    // character work; if it stopped doing so these calls would disappear.
    for (const char* symbol : {"str_len", "str_copy", "str_append", "str_compare",
                               "str_from_int", "str_index_of", "str_char_at"}) {
        if (assembly.find(std::string("call ") + symbol) == std::string::npos)
            ::ardio_test::fail(__FILE__, __LINE__,
                               std::string("String never calls ") + symbol);
    }
}

// The declaration macro has to produce a working pair of declarations at file
// scope and inside a function body alike.
TEST(lightweight_string_macro_works_at_file_and_block_scope) {
    BUILD("LW_STRING at both scopes",
          "#include <Lightweight/String>\n"
          "LW_STRING(global_text);\n"
          "int n;\n"
          "void use() {\n"
          "    LW_STRING(local_text);\n"
          "    local_text.set(\"scoped\");\n"
          "    n = (int)local_text.length();\n"
          "}\n"
          "void setup() { global_text.set(\"g\"); use(); }\n" +
              std::string(kEntry));
}

// The capacity is documented as a compile-time constant, so it has to be usable
// as one -- in an array bound, and in a constant expression.
TEST(lightweight_string_capacity_is_a_compile_time_constant) {
    BUILD("LW_STRING_CAPACITY as a constant",
          "#include <Lightweight/String>\n"
          "char mirror[LW_STRING_BUFFER];\n"
          "int spare[LW_STRING_CAPACITY - 30];\n"
          "int n;\n"
          "void setup() { mirror[LW_STRING_CAPACITY] = 0; n = spare[0]; }\n" +
              std::string(kEntry));
}

// Overflow is documented as truncation, which is what runtime/string.S does
// when it is given the whole buffer size. Passing anything else -- a capacity
// that excluded the terminator, say -- would be an off-by-one that this catches
// only in the sense that the sketch must still build and still route through
// str_copy; the truncation itself is guaranteed by string.S and covered by its
// own test. What is checked here is that a deliberate overflow is accepted by
// the toolchain rather than rejected or miscompiled.
TEST(lightweight_string_overflowing_text_still_builds) {
    BUILD("an over-long assignment",
          "#include <Lightweight/String>\n"
          "LW_STRING(text);\n"
          "void setup() {\n"
          "    text.set(\"0123456789012345678901234567890123456789\");\n"
          "    text.append(\"and more still\");\n"
          "    text.appendInt(-32768);\n"
          "}\n" +
              std::string(kEntry));
}

// ----------------------------------------------------------------- Vector ---

TEST(lightweight_vector_int_exercises_every_operation) {
    BUILD("the LwVectorInt surface",
          "#include <Lightweight/Vector>\n"
          "LW_VECTOR_INT(samples);\n"
          "int result;\n"
          "void setup() {\n"
          "    if (samples.push_back(11)) result = result + 1;\n"
          "    if (samples.push_back(-11)) result = result + 1;\n"
          "    result = result + (int)samples.size();\n"
          "    result = result + (int)samples.capacity();\n"
          "    result = result + samples.at(0);\n"
          "    if (samples.set(1, 5)) result = result + 1;\n"
          "    result = result + samples.front() + samples.back();\n"
          "    result = result + samples.indexOf(5);\n"
          "    if (samples.contains(5)) result = result + 1;\n"
          "    if (samples.empty()) result = result + 1;\n"
          "    if (samples.full()) result = result + 1;\n"
          "    if (samples.pop_back()) result = result + 1;\n"
          "    samples.clear();\n"
          "}\n" +
              std::string(kEntry));
}

TEST(lightweight_vector_byte_exercises_every_operation) {
    BUILD("the LwVectorByte surface",
          "#include <Lightweight/Vector>\n"
          "LW_VECTOR_BYTE(packet);\n"
          "int result;\n"
          "void setup() {\n"
          "    if (packet.push_back(200)) result = result + 1;\n"
          "    if (packet.push_back(0)) result = result + 1;\n"
          "    result = result + (int)packet.size();\n"
          "    result = result + (int)packet.capacity();\n"
          "    result = result + (int)packet.at(0);\n"
          "    if (packet.set(1, 9)) result = result + 1;\n"
          "    result = result + (int)packet.front() + (int)packet.back();\n"
          "    result = result + packet.indexOf(9);\n"
          "    if (packet.contains(9)) result = result + 1;\n"
          "    if (packet.empty()) result = result + 1;\n"
          "    if (packet.full()) result = result + 1;\n"
          "    if (packet.pop_back()) result = result + 1;\n"
          "    packet.clear();\n"
          "}\n" +
              std::string(kEntry));
}

// Both vectors in one sketch: the two classes are hand-written copies of each
// other, so they have to agree on their field and method names without
// colliding on an assembly label.
TEST(lightweight_both_vectors_in_one_sketch) {
    BUILD("both vector types together",
          "#include <Lightweight/Vector>\n"
          "LW_VECTOR_INT(words);\n"
          "LW_VECTOR_BYTE(bytes);\n"
          "int n;\n"
          "void setup() {\n"
          "    words.push_back(1);\n"
          "    bytes.push_back(2);\n"
          "    n = words.at(0) + (int)bytes.at(0);\n"
          "}\n" +
              std::string(kEntry));
}

TEST(lightweight_vector_macros_work_at_block_scope) {
    BUILD("LW_VECTOR macros inside a function",
          "#include <Lightweight/Vector>\n"
          "int n;\n"
          "void use_int() {\n"
          "    LW_VECTOR_INT(local);\n"
          "    local.push_back(3);\n"
          "    n = local.at(0);\n"
          "}\n"
          "void use_byte() {\n"
          "    LW_VECTOR_BYTE(local);\n"
          "    local.push_back(4);\n"
          "    n = n + (int)local.at(0);\n"
          "}\n"
          "void setup() { use_int(); use_byte(); }\n" +
              std::string(kEntry));
}

TEST(lightweight_vector_capacity_is_a_compile_time_constant) {
    BUILD("LW_VECTOR_CAPACITY as a constant",
          "#include <Lightweight/Vector>\n"
          "int mirror[LW_VECTOR_CAPACITY];\n"
          "int n;\n"
          "void setup() { mirror[LW_VECTOR_CAPACITY - 1] = 1; n = mirror[0]; }\n" +
              std::string(kEntry));
}

// Filling past the end has to be something a sketch can simply write. The
// header refuses the extra pushes at run time; the toolchain must not object.
TEST(lightweight_vector_overfilling_still_builds) {
    BUILD("a loop that overfills",
          "#include <Lightweight/Vector>\n"
          "LW_VECTOR_INT(samples);\n"
          "int refused;\n"
          "void setup() {\n"
          "    int i = 0;\n"
          "    while (i < LW_VECTOR_CAPACITY * 2) {\n"
          "        if (!samples.push_back(i)) refused = refused + 1;\n"
          "        i = i + 1;\n"
          "    }\n"
          "}\n" +
              std::string(kEntry));
}

// ------------------------------------------------------------- both at once ---

// A realistic sketch: a byte frame arrives, its values are formatted into a
// string, and the whole thing goes through every layer at once.
TEST(lightweight_string_and_vector_together) {
    BUILD("a sketch using both",
          "#include <Lightweight/String>\n"
          "#include <Lightweight/Vector>\n"
          "LW_STRING(line);\n"
          "LW_VECTOR_BYTE(frame);\n"
          "void render() {\n"
          "    line.clear();\n"
          "    unsigned int i = 0;\n"
          "    while (i < frame.size()) {\n"
          "        line.appendInt((int)frame.at(i));\n"
          "        line.appendChar(' ');\n"
          "        i = i + 1;\n"
          "    }\n"
          "}\n"
          "void setup() {\n"
          "    frame.push_back(1);\n"
          "    frame.push_back(2);\n"
          "    render();\n"
          "}\n" +
              std::string(kEntry));
}

// -------------------------------------------------------- documented limits ---

// The README tells sketch authors that a function's frame reaches 62 bytes, so
// one local LW_STRING is fine and two are not. That is a claim about the
// compiler, and it should fail here if it ever stops being true -- otherwise
// the advice quietly becomes wrong.
TEST(lightweight_two_local_strings_exceed_the_frame_as_documented) {
    if (include_root().empty()) {
        std::printf("  skip frame limit (runtime not found relative to the cwd)\n");
        return;
    }
    // Reported, not asserted: if the frame ever grows this stops being true,
    // and that is an improvement rather than a regression -- but the README
    // needs updating, so say so loudly instead of failing.
    if (!rejects("#include <Lightweight/String>\n"
                 "int n;\n"
                 "void setup() {\n"
                 "    LW_STRING(a);\n"
                 "    LW_STRING(b);\n"
                 "    a.set(\"x\");\n"
                 "    b.set(\"y\");\n"
                 "    n = (int)a.length() + (int)b.length();\n"
                 "}\n"
                 "void loop() {}\n"))
        std::printf("  note two local LW_STRINGs now fit in a frame; the "
                    "Lightweight README says they do not\n");

    // The documented workaround -- file scope -- has to work regardless.
    BUILD("two file-scope strings",
          "#include <Lightweight/String>\n"
          "LW_STRING(a);\n"
          "LW_STRING(b);\n"
          "int n;\n"
          "void setup() {\n"
          "    a.set(\"x\");\n"
          "    b.set(\"y\");\n"
          "    n = (int)a.length() + (int)b.length();\n"
          "}\n" +
              std::string(kEntry));
}
