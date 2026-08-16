// Standalone self-test for libtruetype. It loads a real font, checks the table
// parsing and metrics, rasterizes a few glyphs, and prints an ASCII rendering
// of them so a human can eyeball that the outlines actually came out as letters.
// Exits non-zero on the first failed check.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "libtruetype/truetype.h"

#ifndef TTF_TEST_FONT
#error "TTF_TEST_FONT must be defined to the path of a .ttf"
#endif

static int failures = 0;
#define CHECK(cond, msg)                                             \
    do {                                                             \
        if (!(cond)) {                                               \
            std::printf("FAIL: %s\n", msg);                          \
            ++failures;                                              \
        }                                                            \
    } while (0)

static std::vector<uint8_t> read_file(const char* path) {
    std::vector<uint8_t> out;
    FILE* f = std::fopen(path, "rb");
    if (!f) return out;
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    out.resize(n > 0 ? static_cast<size_t>(n) : 0);
    if (!out.empty()) { size_t got = std::fread(out.data(), 1, out.size(), f); (void)got; }
    std::fclose(f);
    return out;
}

// Print a glyph's coverage as ASCII so its shape is visible in the terminal.
static void show(ttf::Font& font, uint32_t cp, float px) {
    int g = font.find_glyph(cp);
    float scale = font.scale_for_pixel_height(px);
    ttf::Bitmap bm = font.rasterize(g, scale);
    std::printf("  '%c'  glyph=%d  %dx%d  advance=%.1f\n", (char)cp, g, bm.w, bm.h,
                font.advance(g) * scale);
    const char* ramp = " .:-=+*#%@";
    for (int y = 0; y < bm.h; ++y) {
        std::printf("    ");
        for (int x = 0; x < bm.w; ++x) {
            int v = bm.cov[y * bm.w + x];
            std::putchar(ramp[v * 9 / 255]);
        }
        std::putchar('\n');
    }
}

int main() {
    std::vector<uint8_t> data = read_file(TTF_TEST_FONT);
    CHECK(!data.empty(), "font file could not be read");
    if (data.empty()) return 1;

    ttf::Font font;
    CHECK(font.load(data.data(), data.size()), "font failed to load");
    CHECK(font.ok(), "font not ok after load");
    CHECK(font.units_per_em() > 0, "units_per_em is zero");

    int asc = 0, desc = 0, gap = 0;
    font.vmetrics(&asc, &desc, &gap);
    CHECK(asc > 0, "ascent not positive");
    CHECK(desc < 0, "descent not negative");

    CHECK(font.find_glyph('A') != 0, "no glyph for 'A'");
    CHECK(font.find_glyph('a') != 0, "no glyph for 'a'");
    CHECK(font.find_glyph('0') != 0, "no glyph for '0'");
    CHECK(font.advance(font.find_glyph('A')) > 0, "'A' has no advance");

    // 'A' must produce ink, with at least one near-fully-covered pixel.
    float scale = font.scale_for_pixel_height(48.0f);
    ttf::Bitmap A = font.rasterize(font.find_glyph('A'), scale);
    CHECK(A.w > 0 && A.h > 0, "'A' rasterized empty");
    long sum = 0;
    int peak = 0;
    for (uint8_t v : A.cov) { sum += v; peak = v > peak ? v : peak; }
    CHECK(sum > 0, "'A' has no coverage");
    CHECK(peak > 200, "'A' never reaches near-full coverage (AA looks wrong)");

    // A space carries an advance but no contours -> empty bitmap.
    ttf::Bitmap sp = font.rasterize(font.find_glyph(' '), scale);
    CHECK(sp.w == 0 && sp.h == 0, "space should rasterize empty");
    CHECK(font.advance(font.find_glyph(' ')) > 0, "space should still advance");

    std::printf("\nlibtruetype -- rasterized samples:\n");
    show(font, 'A', 28);
    show(font, 'g', 28);
    show(font, '@', 28);

    if (failures == 0) std::printf("\nlibtruetype: all checks passed.\n");
    else std::printf("\nlibtruetype: %d checks FAILED.\n", failures);
    return failures ? 1 : 0;
}
