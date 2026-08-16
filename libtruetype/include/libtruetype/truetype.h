// libtruetype -- a small, dependency-free TrueType rasterizer.
//
// It reads the tables a text renderer actually needs (glyph shapes, the
// character map, and horizontal metrics) and turns a glyph into an anti-aliased
// 8-bit coverage bitmap. It does no caching, no atlas building, and no layout:
// those belong to whoever uses it, because a font library that owned an atlas
// would impose its idea of a cache on every caller. This owns exactly one thing
// -- how to read a font file and draw a glyph from it -- which is the whole of
// what "TrueType" means and no more.
//
// The design mirrors the well-worn shape of a single-header font rasterizer,
// but every line here is written for this library: tables are parsed directly,
// outlines are flattened to line segments, and coverage is accumulated by a
// supersampled non-zero-winding scan. It is not the fastest rasterizer in
// existence; it is meant to be correct, small, and easy to read, and to run
// once per glyph into a cache the caller keeps.
#ifndef LIBTRUETYPE_TRUETYPE_H
#define LIBTRUETYPE_TRUETYPE_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ttf {

// An anti-aliased glyph, in coverage form: cov[y*w + x] is 0..255 how much of
// that pixel the glyph covers. (xoff, yoff) place the top-left of the bitmap
// relative to the pen's baseline origin, y growing downward, so a caller blits
// at (pen_x + xoff, baseline_y + yoff).
struct Bitmap {
    int w = 0, h = 0;
    int xoff = 0, yoff = 0;
    std::vector<uint8_t> cov;
};

class Font {
public:
    // Points `this` at a font image in memory. The data is NOT copied: it must
    // stay alive as long as the Font is used. Returns false if the image is not
    // a TrueType/OpenType font with the tables we require.
    bool load(const uint8_t* data, size_t len);

    bool ok() const { return data_ != nullptr; }

    // Design units per em -- the coordinate space glyph outlines live in.
    int units_per_em() const { return units_per_em_; }

    // Vertical metrics in font units: how far the tallest glyph rises above the
    // baseline, how far the lowest descends (negative), and the designer's gap
    // between lines. Multiply by scale_for_pixel_height() for pixels.
    void vmetrics(int* ascent, int* descent, int* line_gap) const;

    // The scale that makes a line `px` pixels tall (ascent to descent).
    float scale_for_pixel_height(float px) const;

    // Glyph index for a Unicode code point, or 0 (the .notdef glyph) if the
    // font has no glyph for it.
    int find_glyph(uint32_t codepoint) const;

    // Horizontal advance for a glyph, in font units. Multiply by scale.
    int advance(int glyph) const;

    // Rasterize a glyph at `scale` (font units -> pixels) into a coverage
    // bitmap. A glyph with no contours (a space) returns an empty bitmap with
    // w == h == 0; its advance still applies.
    Bitmap rasterize(int glyph, float scale) const;

private:
    // A big-endian view helper set drawn from the raw image.
    uint16_t u16(size_t off) const;
    int16_t s16(size_t off) const;
    uint32_t u32(size_t off) const;
    size_t table(const char tag[4]) const;  // offset of a table, or 0 if absent

    size_t glyph_offset(int glyph, size_t* end) const;  // into glyf, or 0/empty
    // Appends this glyph's contours as flattened polylines (in font units) to
    // `contours`. Handles composite glyphs by recursion. `depth` guards cycles.
    void collect_contours(int glyph, float ox, float oy, float sx, float sy,
                          std::vector<std::vector<std::pair<float, float>>>& contours,
                          int depth) const;

    const uint8_t* data_ = nullptr;
    size_t len_ = 0;
    size_t loca_ = 0, glyf_ = 0, hmtx_ = 0, cmap_ = 0, cmap_sub_ = 0;
    int units_per_em_ = 0;
    int num_glyphs_ = 0;
    int num_hmetrics_ = 0;
    int ascent_ = 0, descent_ = 0, line_gap_ = 0;
    int loca_long_ = 0;  // indexToLocFormat: 0 = 16-bit offsets, 1 = 32-bit
};

}  // namespace ttf

#endif  // LIBTRUETYPE_TRUETYPE_H
