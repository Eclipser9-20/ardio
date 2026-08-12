// The label maker example is the biggest sketch in the tree and the one that
// pushes hardest on the compiler: a four-state menu, a stroke font, Bresenham
// lines, a bit-banged I2C display and about 1300 lines of it. If it stops
// compiling, something in the front end or the code generator has regressed.
//
// The sketch is read from disk rather than embedded here so that the example
// and the test can never drift apart. Note that compile_avr() does no
// preprocessing, so the sketch has to be self-contained -- which it is, by
// design.

#include "harness.h"
#include "ardio/avr/compiler.h"

#include <fstream>
#include <sstream>
#include <string>

namespace {

// The tests are run from the build directory as often as from the source root,
// so the example is looked for along the way up.
bool read_example(std::string& source, std::string& where) {
    for (const char* dir : {"examples/label_maker/",
                            "../examples/label_maker/",
                            "../../examples/label_maker/",
                            "../../../examples/label_maker/"}) {
        std::string path = std::string(dir) + "label_maker.ino";
        std::ifstream in(path);
        if (!in) continue;
        std::ostringstream text;
        text << in.rdbuf();
        source = text.str();
        where = path;
        return true;
    }
    return false;
}

} // namespace

TEST(label_maker_example_compiles) {
    std::string source, where;
    if (!read_example(source, where)) {
        std::printf("    skipped: examples/label_maker/label_maker.ino not found\n");
        return;
    }

    auto compiled = ardio::compile_avr(source);
    if (!compiled.ok) std::printf("    %s: %s\n", where.c_str(), compiled.error.c_str());
    CHECK(compiled.ok);
    if (!compiled.ok) return;
    CHECK(!compiled.assembly.empty());

    // The sketch calls into the runtime, so the assembly on its own does not
    // link; assembling it here would only fail on pinMode and friends. What
    // can be checked without the runtime is that the entry points came out and
    // that the code generator actually emitted the state machine.
    CHECK(compiled.assembly.find("setup:") != std::string::npos);
    CHECK(compiled.assembly.find("loop:") != std::string::npos);
    CHECK(compiled.assembly.find("plot_character:") != std::string::npos);

    // The font is one flat table of stroke bytes rather than a function per
    // glyph. That is what keeps the sketch inside a Nano, so it is worth
    // pinning down: the table is in the source, the per-glyph routines are
    // not, and no label for one of them is emitted.
    CHECK(source.find("const char stroke[") != std::string::npos);
    CHECK(source.find("const char glyph_start[41]") != std::string::npos);
    CHECK(source.find("int glyph_0(") == std::string::npos);
    CHECK(compiled.assembly.find("glyph_0:") == std::string::npos);
}
