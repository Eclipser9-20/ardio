#include "harness.h"
#include "ardio/avr/preprocess.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace ardio;

namespace {

std::string squeeze(const std::string& s) {
    std::string out;
    bool space = false;
    for (char c : s) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { space = !out.empty(); continue; }
        if (space) out += ' ';
        space = false;
        out += c;
    }
    return out;
}

std::string pp(const std::string& src, const std::vector<std::string>& paths = {}) {
    PreprocessResult r = preprocess(src, paths);
    if (!r.ok) return "ERROR: " + r.error;
    return squeeze(r.text);
}

// A scratch directory that cleans up after itself.
struct TempDir {
    std::filesystem::path path;
    TempDir() {
        path = std::filesystem::temp_directory_path() / "ardio_pp_test";
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    void write(const std::string& name, const std::string& text) const {
        std::filesystem::path p = path / name;
        std::filesystem::create_directories(p.parent_path());
        std::ofstream f(p);
        f << text;
    }
    std::string dir() const { return path.string(); }
};

} // namespace

TEST(pp_passes_plain_text_through) {
    CHECK(pp("int main() { return 0; }") == "int main() { return 0; }");
}

TEST(pp_strips_line_comments) {
    CHECK(pp("int a; // trailing\nint b;") == "int a; int b;");
}

TEST(pp_strips_block_comments) {
    CHECK(pp("int /* mid */ a;") == "int a;");
    CHECK(pp("a /* spans\nlines */ b;") == "a b;");
}

TEST(pp_keeps_comment_characters_inside_strings) {
    CHECK(pp("const char* s = \"a // b /* c */\";") == "const char* s = \"a // b /* c */\";");
}

TEST(pp_joins_line_continuations) {
    CHECK(pp("int a = 1 + \\\n2;") == "int a = 1 + 2;");
}

TEST(pp_object_like_macro) {
    CHECK(pp("#define LED 13\nint p = LED;") == "int p = 13;");
}

TEST(pp_object_like_macro_expands_recursively) {
    CHECK(pp("#define A B\n#define B 7\nint x = A;") == "int x = 7;");
}

TEST(pp_empty_macro_expands_to_nothing) {
    CHECK(pp("#define EMPTY\nint EMPTY x;") == "int x;");
}

TEST(pp_function_like_macro) {
    CHECK(pp("#define SQ(x) ((x)*(x))\nint y = SQ(3);") == "int y = ((3)*(3));");
}

TEST(pp_function_like_macro_two_params) {
    CHECK(pp("#define MAX(a,b) ((a)>(b)?(a):(b))\nint y = MAX(1, 2);") ==
          "int y = ((1)>(2)?(1):(2));");
}

TEST(pp_function_like_macro_nested_call_arguments) {
    CHECK(pp("#define ID(x) x\nint y = ID(ID(4));") == "int y = 4;");
}

TEST(pp_function_like_name_without_parens_is_left_alone) {
    CHECK(pp("#define F(x) x\nvoid* p = F;") == "void* p = F;");
}

TEST(pp_macro_stringize_and_paste) {
    CHECK(pp("#define STR(x) #x\nconst char* s = STR(hi);") == "const char* s = \"hi\";");
    CHECK(pp("#define CAT(a,b) a##b\nint xy = CAT(x,y);") == "int xy = xy;");
}

TEST(pp_macros_do_not_expand_inside_strings) {
    CHECK(pp("#define N 5\nconst char* s = \"N\";") == "const char* s = \"N\";");
}

TEST(pp_undef_removes_macro) {
    CHECK(pp("#define N 5\n#undef N\nint x = N;") == "int x = N;");
}

TEST(pp_self_referential_macro_terminates) {
    CHECK(pp("#define N N + 1\nint x = N;") == "int x = N + 1;");
}

TEST(pp_mutually_recursive_macros_terminate) {
    CHECK(pp("#define A B\n#define B A\nint x = A;") == "int x = A;");
}

TEST(pp_ifdef_selects_the_defined_branch) {
    CHECK(pp("#define ON\n#ifdef ON\nint a;\n#else\nint b;\n#endif") == "int a;");
}

TEST(pp_ifndef_selects_the_undefined_branch) {
    CHECK(pp("#ifndef ON\nint a;\n#else\nint b;\n#endif") == "int a;");
}

TEST(pp_if_defined_operator) {
    CHECK(pp("#define ON 1\n#if defined(ON)\nint a;\n#endif") == "int a;");
    CHECK(pp("#if defined OFF\nint a;\n#endif") == "");
}

TEST(pp_if_arithmetic) {
    CHECK(pp("#if 2 + 2 == 4\nint a;\n#endif") == "int a;");
    CHECK(pp("#if 1 && 0\nint a;\n#endif") == "");
    CHECK(pp("#if (1 << 3) > 7\nint a;\n#endif") == "int a;");
}

TEST(pp_if_uses_macro_values) {
    CHECK(pp("#define V 3\n#if V > 2\nint a;\n#else\nint b;\n#endif") == "int a;");
}

TEST(pp_undefined_identifier_is_zero_in_if) {
    CHECK(pp("#if NOPE\nint a;\n#else\nint b;\n#endif") == "int b;");
}

TEST(pp_elif_chain) {
    const char* src =
        "#define V 2\n"
        "#if V == 1\nint one;\n"
        "#elif V == 2\nint two;\n"
        "#elif V == 3\nint three;\n"
        "#else\nint other;\n#endif";
    CHECK(pp(src) == "int two;");
}

TEST(pp_nested_conditionals) {
    const char* src =
        "#define A\n"
        "#ifdef A\n"
        "#ifdef B\nint ab;\n#else\nint a_only;\n#endif\n"
        "#else\nint none;\n#endif";
    CHECK(pp(src) == "int a_only;");
}

TEST(pp_inactive_branch_ignores_bad_directives) {
    CHECK(pp("#if 0\n#include \"missing_file_xyz.h\"\n#endif\nint a;") == "int a;");
}

TEST(pp_unterminated_conditional_is_an_error) {
    PreprocessResult r = preprocess("#ifdef A\nint a;\n", {});
    CHECK(!r.ok);
    CHECK(r.error.find("<source>") != std::string::npos);
}

TEST(pp_endif_without_if_is_an_error) {
    PreprocessResult r = preprocess("int a;\n#endif\n", {});
    CHECK(!r.ok);
    CHECK(r.error.find("<source>:2:") != std::string::npos);
}

TEST(pp_unknown_directive_is_an_error) {
    PreprocessResult r = preprocess("#nonsense\n", {});
    CHECK(!r.ok);
    CHECK(r.error.find("<source>:1:") != std::string::npos);
}

TEST(pp_error_directive_reports_its_message) {
    PreprocessResult r = preprocess("\n#error broken build\n", {});
    CHECK(!r.ok);
    CHECK(r.error == "<source>:2: broken build");
}

TEST(pp_include_quoted_from_include_path) {
    TempDir t;
    t.write("pins.h", "#define LED 13\n");
    CHECK(pp("#include \"pins.h\"\nint p = LED;", {t.dir()}) == "int p = 13;");
}

TEST(pp_include_angled_from_include_path) {
    TempDir t;
    t.write("board.h", "int board;\n");
    CHECK(pp("#include <board.h>\n", {t.dir()}) == "int board;");
}

TEST(pp_include_searches_subdirectories_by_name) {
    TempDir t;
    t.write("sub/deep.h", "int deep;\n");
    CHECK(pp("#include <sub/deep.h>\n", {t.dir()}) == "int deep;");
}

TEST(pp_nested_include_resolves_relative_to_its_own_file) {
    TempDir t;
    t.write("sub/outer.h", "#include \"inner.h\"\n");
    t.write("sub/inner.h", "int inner;\n");
    CHECK(pp("#include <sub/outer.h>\n", {t.dir()}) == "int inner;");
}

TEST(pp_missing_include_is_an_error_naming_the_line) {
    PreprocessResult r = preprocess("int a;\n#include \"no_such_header.h\"\n", {});
    CHECK(!r.ok);
    CHECK(r.error.find("<source>:2:") != std::string::npos);
    CHECK(r.error.find("no_such_header.h") != std::string::npos);
}

TEST(pp_error_in_included_file_names_that_file) {
    TempDir t;
    t.write("bad.h", "int ok;\n#error inner failure\n");
    PreprocessResult r = preprocess("#include <bad.h>\n", {t.dir()});
    CHECK(!r.ok);
    CHECK(r.error.find("bad.h:2: inner failure") != std::string::npos);
}

TEST(pp_pragma_once_prevents_double_inclusion) {
    TempDir t;
    t.write("once.h", "#pragma once\nint only;\n");
    CHECK(pp("#include <once.h>\n#include <once.h>\n", {t.dir()}) == "int only;");
}

TEST(pp_include_guards_prevent_double_inclusion) {
    TempDir t;
    t.write("guard.h", "#ifndef GUARD_H\n#define GUARD_H\nint guarded;\n#endif\n");
    CHECK(pp("#include <guard.h>\n#include <guard.h>\n", {t.dir()}) == "int guarded;");
}

TEST(pp_include_cycle_hits_the_depth_limit) {
    TempDir t;
    t.write("a.h", "#include <b.h>\n");
    t.write("b.h", "#include <a.h>\n");
    PreprocessResult r = preprocess("#include <a.h>\n", {t.dir()});
    CHECK(!r.ok);
    CHECK(r.error.find("32") != std::string::npos);
}

TEST(pp_macro_defined_in_header_drives_conditional) {
    TempDir t;
    t.write("cfg.h", "#pragma once\n#define BOARD 2\n");
    const char* src =
        "#include <cfg.h>\n"
        "#if BOARD == 2\nint uno;\n#else\nint mega;\n#endif";
    CHECK(pp(src, {t.dir()}) == "int uno;");
}

TEST(pp_deep_macro_recursion_is_rejected) {
    std::string src = "#define M0 1\n";
    for (int i = 1; i < 200; ++i)
        src += "#define M" + std::to_string(i) + " M" + std::to_string(i - 1) + "\n";
    src += "int x = M199;\n";
    PreprocessResult r = preprocess(src, {});
    CHECK(!r.ok);
    CHECK(r.error.find("deeply") != std::string::npos);
}

TEST(pp_wrong_argument_count_is_an_error) {
    PreprocessResult r = preprocess("#define F(a,b) a+b\nint x = F(1);\n", {});
    CHECK(!r.ok);
    CHECK(r.error.find("<source>:2:") != std::string::npos);
}
