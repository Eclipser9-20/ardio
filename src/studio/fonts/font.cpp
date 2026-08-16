#include "studio/fonts/font.h"

#include <SDL3/SDL.h>

#include <cstdio>

namespace studio {
namespace {

constexpr int kAtlasSize = 1024;

// Decode one UTF-8 code point starting at s[i], advancing i past it. Malformed
// bytes decode as U+FFFD and advance one, so a bad string cannot loop forever.
uint32_t next_cp(std::string_view s, size_t& i) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    if (c < 0x80) { ++i; return c; }
    int extra;
    uint32_t cp;
    if ((c & 0xE0) == 0xC0) { extra = 1; cp = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { extra = 2; cp = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { extra = 3; cp = c & 0x07; }
    else { ++i; return 0xFFFD; }
    ++i;
    for (int k = 0; k < extra; ++k) {
        if (i >= s.size() || (static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) return 0xFFFD;
        cp = (cp << 6) | (static_cast<unsigned char>(s[i]) & 0x3F);
        ++i;
    }
    return cp;
}

std::vector<uint8_t> read_file(const std::string& path) {
    std::vector<uint8_t> out;
    SDL_IOStream* io = SDL_IOFromFile(path.c_str(), "rb");
    if (!io) return out;
    Sint64 n = SDL_GetIOSize(io);
    if (n > 0) {
        out.resize(static_cast<size_t>(n));
        SDL_ReadIO(io, out.data(), out.size());
    }
    SDL_CloseIO(io);
    return out;
}

}  // namespace

Font::~Font() {
    if (atlas_) SDL_DestroyTexture(atlas_);
}

bool Font::load(SDL_Renderer* r, const std::string& ttf_path, float dpr) {
    r_ = r;
    dpr_ = dpr > 0 ? dpr : 1.0f;
    data_ = read_file(ttf_path);
    if (data_.empty()) return false;
    if (!font_.load(data_.data(), data_.size())) return false;

    atlas_ = SDL_CreateTexture(r_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, kAtlasSize,
                               kAtlasSize);
    if (!atlas_) return false;
    atlas_w_ = atlas_h_ = kAtlasSize;
    SDL_SetTextureBlendMode(atlas_, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(atlas_, SDL_SCALEMODE_NEAREST);  // glyphs are 1:1
    return true;
}

float Font::line_height(float px) const {
    int a, d, g;
    font_.vmetrics(&a, &d, &g);
    float s = font_.scale_for_pixel_height(px);
    return (a - d + g) * s;
}

float Font::ascent(float px) const {
    int a, d, g;
    font_.vmetrics(&a, &d, &g);
    return a * font_.scale_for_pixel_height(px);
}

const Font::Glyph& Font::glyph(uint32_t cp, float px) {
    // Rasterize at device pixels so the glyph lands on the physical grid; all
    // stored metrics are therefore in device pixels and converted back to points
    // (dividing by dpr) at draw time.
    float device_px = px * dpr_;
    uint64_t key = (static_cast<uint64_t>(cp) << 16) | static_cast<uint32_t>(device_px + 0.5f);
    auto it = cache_.find(key);
    if (it != cache_.end()) return it->second;

    int gi = font_.find_glyph(cp);
    float scale = font_.scale_for_pixel_height(device_px);
    ttf::Bitmap bm = font_.rasterize(gi, scale);

    Glyph gl;
    gl.adv = font_.advance(gi) * scale;
    gl.xoff = bm.xoff;
    gl.yoff = bm.yoff;
    gl.w = bm.w;
    gl.h = bm.h;

    if (bm.w > 0 && bm.h > 0) {
        // Shelf-pack: wrap to a new row when this glyph runs off the right edge.
        if (pen_x_ + bm.w + 1 > atlas_w_) {
            pen_x_ = 0;
            pen_y_ += row_h_ + 1;
            row_h_ = 0;
        }
        if (pen_y_ + bm.h <= atlas_h_) {
            gl.u = static_cast<float>(pen_x_);
            gl.v = static_cast<float>(pen_y_);
            // Expand 8-bit coverage to white-with-alpha RGBA for the atlas.
            std::vector<uint8_t> rgba(static_cast<size_t>(bm.w) * bm.h * 4);
            for (size_t i = 0; i < static_cast<size_t>(bm.w) * bm.h; ++i) {
                rgba[i * 4 + 0] = 255;
                rgba[i * 4 + 1] = 255;
                rgba[i * 4 + 2] = 255;
                rgba[i * 4 + 3] = bm.cov[i];
            }
            SDL_Rect dst{pen_x_, pen_y_, bm.w, bm.h};
            SDL_UpdateTexture(atlas_, &dst, rgba.data(), bm.w * 4);
            pen_x_ += bm.w + 1;
            if (bm.h > row_h_) row_h_ = bm.h;
        } else {
            gl.w = gl.h = 0;  // atlas full: draw nothing rather than corrupt it
        }
    }

    auto res = cache_.emplace(key, gl);
    return res.first->second;
}

float Font::measure(std::string_view s, float px) {
    float x = 0;  // device pixels
    for (size_t i = 0; i < s.size();) x += glyph(next_cp(s, i), px).adv;
    return x / dpr_;  // report in points
}

float Font::draw(Gfx& g, float x, float y_top, std::string_view s, float px, Color c) {
    (void)g;
    // Glyph metrics are device pixels; the UI draws in points, so every device
    // measure is divided by dpr to place and size the quad. The render scale
    // then maps points back to device pixels 1:1, keeping the glyph crisp.
    const float inv = 1.0f / dpr_;
    float baseline = y_top + ascent(px);
    float startx = x;
    SDL_SetTextureColorMod(atlas_, c.r, c.g, c.b);
    SDL_SetTextureAlphaMod(atlas_, c.a);
    for (size_t i = 0; i < s.size();) {
        const Glyph& gl = glyph(next_cp(s, i), px);
        if (gl.w > 0 && gl.h > 0) {
            SDL_FRect src{gl.u, gl.v, static_cast<float>(gl.w), static_cast<float>(gl.h)};
            SDL_FRect dst{x + gl.xoff * inv, baseline + gl.yoff * inv, gl.w * inv, gl.h * inv};
            SDL_RenderTexture(r_, atlas_, &src, &dst);
        }
        x += gl.adv * inv;
    }
    return x - startx;
}

float Font::draw_centered(Gfx& g, float x, float y_top, float width, std::string_view s, float px,
                          Color c) {
    float w = measure(s, px);
    return draw(g, x + (width - w) * 0.5f, y_top, s, px, c);
}

}  // namespace studio
