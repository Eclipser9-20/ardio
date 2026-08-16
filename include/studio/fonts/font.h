// A drawable font: libtruetype for the shapes, an SDL texture atlas for speed.
//
// libtruetype rasterizes a glyph once; this caches that result in a GPU atlas
// keyed by (code point, pixel size) so drawing a string is a handful of
// textured quads, not a re-rasterization. Coverage is stored as the alpha of a
// white glyph, so any color is a texture color-mod away and one atlas serves
// every text color in the UI.
//
// This is where SDL and libtruetype meet; libtruetype itself knows nothing of
// either. One Font wraps one typeface; the app holds however many it themes
// with (a UI face, a mono face).
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "libtruetype/truetype.h"
#include "studio/core/color.h"
#include "studio/gfx/gfx.h"

struct SDL_Renderer;
struct SDL_Texture;

namespace studio {

class Font {
public:
    ~Font();
    // Loads a .ttf from disk and creates its atlas texture on `r`. `dpr` is the
    // device pixels per point: glyphs are rasterized at size*dpr and drawn back
    // at logical size, so text is crisp on a high-density display. Returns false
    // (and leaves ok() false) if the file is missing or not a usable font.
    bool load(SDL_Renderer* r, const std::string& ttf_path, float dpr);
    bool ok() const { return font_.ok() && atlas_ != nullptr; }

    // Whether the font actually has a glyph for this code point (as opposed to
    // falling back to .notdef). Lets the UI skip an icon a font lacks.
    bool has(uint32_t codepoint) const { return font_.find_glyph(codepoint) != 0; }

    // Line box and baseline for a given pixel size.
    float line_height(float px) const;
    float ascent(float px) const;

    // Width in pixels a string would occupy at `px`.
    float measure(std::string_view utf8, float px);

    // Draws `utf8` with its top-left at (x, y_top), in color `c`. Returns the
    // advance width drawn, so callers can lay out following text.
    float draw(Gfx& g, float x, float y_top, std::string_view utf8, float px, Color c);

    // Draws centered within [x, x+width] horizontally; y_top unchanged.
    float draw_centered(Gfx& g, float x, float y_top, float width, std::string_view utf8, float px,
                        Color c);

private:
    struct Glyph {
        int w = 0, h = 0;      // bitmap size in the atlas
        int xoff = 0, yoff = 0;  // placement relative to pen/baseline
        float adv = 0;         // horizontal advance in pixels
        float u = 0, v = 0;    // top-left in the atlas, pixels
    };
    const Glyph& glyph(uint32_t cp, float px);

    ttf::Font font_;
    std::vector<uint8_t> data_;  // owns the font image ttf::Font points into
    SDL_Renderer* r_ = nullptr;
    SDL_Texture* atlas_ = nullptr;
    int atlas_w_ = 0, atlas_h_ = 0;
    int pen_x_ = 0, pen_y_ = 0, row_h_ = 0;  // shelf packer cursor
    float dpr_ = 1.0f;                       // device pixels per point
    std::unordered_map<uint64_t, Glyph> cache_;
};

}  // namespace studio
