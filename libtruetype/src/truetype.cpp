#include "libtruetype/truetype.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ttf {
namespace {
// Supersampling factor per axis for the rasterizer. Four means sixteen samples
// a pixel: enough to make UI text read as smooth without the cost mattering,
// since a glyph is rasterized once into a cache.
constexpr int kSS = 4;
// Segments a quadratic curve is flattened into. Curves in a font are gentle at
// text sizes, so a fixed subdivision is indistinguishable from an adaptive one
// here and far simpler.
constexpr int kCurveSegs = 8;
}  // namespace

uint16_t Font::u16(size_t o) const {
    return static_cast<uint16_t>((data_[o] << 8) | data_[o + 1]);
}
int16_t Font::s16(size_t o) const { return static_cast<int16_t>(u16(o)); }
uint32_t Font::u32(size_t o) const {
    return (static_cast<uint32_t>(data_[o]) << 24) | (static_cast<uint32_t>(data_[o + 1]) << 16) |
           (static_cast<uint32_t>(data_[o + 2]) << 8) | data_[o + 3];
}

size_t Font::table(const char tag[4]) const {
    uint16_t n = u16(4);
    size_t dir = 12;
    for (uint16_t i = 0; i < n; ++i) {
        size_t rec = dir + i * 16;
        if (std::memcmp(data_ + rec, tag, 4) == 0) return u32(rec + 8);
    }
    return 0;
}

bool Font::load(const uint8_t* data, size_t len) {
    data_ = nullptr;
    if (!data || len < 12) return false;
    data_ = data;
    len_ = len;

    uint32_t ver = u32(0);
    // TrueType outlines only: 0x00010000 or 'true'. 'OTTO' (CFF) has no glyf.
    if (ver != 0x00010000 && ver != 0x74727565 /*true*/) {
        // Some TrueType-flavored OpenType still uses 0x00010000; anything else
        // we cannot read glyph outlines from.
        if (!table("glyf")) {
            data_ = nullptr;
            return false;
        }
    }

    size_t head = table("head"), maxp = table("maxp"), hhea = table("hhea");
    glyf_ = table("glyf");
    loca_ = table("loca");
    hmtx_ = table("hmtx");
    cmap_ = table("cmap");
    if (!head || !maxp || !hhea || !glyf_ || !loca_ || !hmtx_ || !cmap_) {
        data_ = nullptr;
        return false;
    }

    units_per_em_ = u16(head + 18);
    loca_long_ = s16(head + 50);
    num_glyphs_ = u16(maxp + 4);
    ascent_ = s16(hhea + 4);
    descent_ = s16(hhea + 6);
    line_gap_ = s16(hhea + 8);
    num_hmetrics_ = u16(hhea + 34);

    // Pick the most capable Unicode cmap subtable: full-range (3,10)/(0,6/4)
    // before BMP (3,1)/(0,3), before symbol (3,0).
    uint16_t n = u16(cmap_ + 2);
    size_t best = 0;
    int best_score = -1;
    for (uint16_t i = 0; i < n; ++i) {
        size_t rec = cmap_ + 4 + i * 8;
        uint16_t plat = u16(rec), enc = u16(rec + 2);
        size_t sub = cmap_ + u32(rec + 4);
        int score = -1;
        if (plat == 3 && enc == 10) score = 5;
        else if (plat == 0 && (enc == 4 || enc == 6)) score = 4;
        else if (plat == 3 && enc == 1) score = 3;
        else if (plat == 0) score = 2;
        else if (plat == 3 && enc == 0) score = 1;
        if (score > best_score) { best_score = score; best = sub; }
    }
    cmap_sub_ = best;
    if (!cmap_sub_) { data_ = nullptr; return false; }
    return true;
}

void Font::vmetrics(int* ascent, int* descent, int* line_gap) const {
    if (ascent) *ascent = ascent_;
    if (descent) *descent = descent_;
    if (line_gap) *line_gap = line_gap_;
}

float Font::scale_for_pixel_height(float px) const {
    int h = ascent_ - descent_;
    return h > 0 ? px / static_cast<float>(h) : 0.0f;
}

int Font::advance(int glyph) const {
    if (glyph < 0) return 0;
    if (glyph < num_hmetrics_) return u16(hmtx_ + glyph * 4);
    // Past the last long metric, advance is the last one recorded.
    return u16(hmtx_ + (num_hmetrics_ - 1) * 4);
}

int Font::find_glyph(uint32_t cp) const {
    size_t s = cmap_sub_;
    uint16_t fmt = u16(s);
    if (fmt == 0) {
        if (cp < 256) return data_[s + 6 + cp];
        return 0;
    }
    if (fmt == 6) {
        uint16_t first = u16(s + 6), count = u16(s + 8);
        if (cp >= first && cp < static_cast<uint32_t>(first + count))
            return u16(s + 10 + (cp - first) * 2);
        return 0;
    }
    if (fmt == 4) {
        uint16_t segx2 = u16(s + 6);
        uint16_t segs = segx2 / 2;
        size_t end = s + 14;
        size_t start = end + segx2 + 2;
        size_t delta = start + segx2;
        size_t range = delta + segx2;
        for (uint16_t i = 0; i < segs; ++i) {
            if (cp <= u16(end + i * 2)) {
                uint16_t startc = u16(start + i * 2);
                if (cp < startc) return 0;
                uint16_t ro = u16(range + i * 2);
                if (ro == 0) return static_cast<uint16_t>(cp + s16(delta + i * 2));
                // idRangeOffset indexes into the glyphIdArray that follows.
                size_t gi = range + i * 2 + ro + (cp - startc) * 2;
                uint16_t g = u16(gi);
                if (g == 0) return 0;
                return static_cast<uint16_t>(g + s16(delta + i * 2));
            }
        }
        return 0;
    }
    if (fmt == 12) {
        uint32_t groups = u32(s + 12);
        size_t g0 = s + 16;
        for (uint32_t i = 0; i < groups; ++i) {
            size_t g = g0 + i * 12;
            uint32_t sc = u32(g), ec = u32(g + 4);
            if (cp >= sc && cp <= ec) return static_cast<int>(u32(g + 8) + (cp - sc));
        }
        return 0;
    }
    return 0;
}

size_t Font::glyph_offset(int glyph, size_t* end) const {
    if (glyph < 0 || glyph >= num_glyphs_) return 0;
    size_t o, e;
    if (loca_long_) {
        o = u32(loca_ + glyph * 4);
        e = u32(loca_ + glyph * 4 + 4);
    } else {
        o = static_cast<size_t>(u16(loca_ + glyph * 2)) * 2;
        e = static_cast<size_t>(u16(loca_ + glyph * 2 + 2)) * 2;
    }
    if (end) *end = glyf_ + e;
    if (o == e) return 0;  // empty glyph (e.g. space)
    return glyf_ + o;
}

void Font::collect_contours(int glyph, float ox, float oy, float sx, float sy,
                            std::vector<std::vector<std::pair<float, float>>>& out,
                            int depth) const {
    if (depth > 5) return;
    size_t g = glyph_offset(glyph, nullptr);
    if (!g) return;
    int nc = s16(g);

    if (nc < 0) {
        // Composite: walk components, each placed by an offset and an optional
        // scale. Shear/rotation two-by-two forms are uncommon in text glyphs
        // and are approximated by their diagonal here.
        size_t p = g + 10;
        for (;;) {
            uint16_t flags = u16(p);
            uint16_t comp = u16(p + 2);
            p += 4;
            float dx, dy;
            if (flags & 0x0001) {  // ARG_1_AND_2_ARE_WORDS
                dx = s16(p);
                dy = s16(p + 2);
                p += 4;
            } else {
                dx = static_cast<int8_t>(data_[p]);
                dy = static_cast<int8_t>(data_[p + 1]);
                p += 2;
            }
            float csx = 1, csy = 1;
            if (flags & 0x0008) {  // WE_HAVE_A_SCALE
                csx = csy = s16(p) / 16384.0f;
                p += 2;
            } else if (flags & 0x0040) {  // X_AND_Y_SCALE
                csx = s16(p) / 16384.0f;
                csy = s16(p + 2) / 16384.0f;
                p += 4;
            } else if (flags & 0x0080) {  // TWO_BY_TWO
                csx = s16(p) / 16384.0f;
                csy = s16(p + 6) / 16384.0f;
                p += 8;
            }
            // Compose parent transform with the component's (ARGS are xy values
            // when 0x0002 is set, which it is for text composites).
            collect_contours(comp, ox + dx * sx, oy + dy * sy, sx * csx, sy * csy, out, depth + 1);
            if (!(flags & 0x0020)) break;  // MORE_COMPONENTS
        }
        return;
    }

    // Simple glyph.
    size_t p = g + 10;
    std::vector<uint16_t> ends(nc);
    for (int i = 0; i < nc; ++i) ends[i] = u16(p + i * 2);
    p += nc * 2;
    int npts = nc ? ends[nc - 1] + 1 : 0;
    uint16_t ilen = u16(p);
    p += 2 + ilen;  // skip hinting instructions

    std::vector<uint8_t> flags(npts);
    for (int i = 0; i < npts;) {
        uint8_t f = data_[p++];
        flags[i++] = f;
        if (f & 0x08) {  // repeat
            uint8_t r = data_[p++];
            while (r-- && i < npts) flags[i++] = f;
        }
    }
    std::vector<float> xs(npts), ys(npts);
    int x = 0;
    for (int i = 0; i < npts; ++i) {
        uint8_t f = flags[i];
        if (f & 0x02) {  // x-short
            uint8_t d = data_[p++];
            x += (f & 0x10) ? d : -d;
        } else if (!(f & 0x10)) {
            x += s16(p);
            p += 2;
        }
        xs[i] = static_cast<float>(x);
    }
    int y = 0;
    for (int i = 0; i < npts; ++i) {
        uint8_t f = flags[i];
        if (f & 0x04) {  // y-short
            uint8_t d = data_[p++];
            y += (f & 0x20) ? d : -d;
        } else if (!(f & 0x20)) {
            y += s16(p);
            p += 2;
        }
        ys[i] = static_cast<float>(y);
    }

    // Walk each contour, turning on/off-curve points into flattened lines.
    int start = 0;
    for (int c = 0; c < nc; ++c) {
        int last = ends[c];
        int n = last - start + 1;
        if (n <= 0) { start = last + 1; continue; }

        auto onc = [&](int i) { return (flags[start + ((i % n) + n) % n] & 0x01) != 0; };
        auto px = [&](int i) { return xs[start + ((i % n) + n) % n]; };
        auto py = [&](int i) { return ys[start + ((i % n) + n) % n]; };
        auto tx = [&](float vx) { return ox + vx * sx; };
        auto ty = [&](float vy) { return oy + vy * sy; };

        // Find a starting on-curve point; synthesize one from two off-curve
        // neighbors if the contour begins off-curve.
        float cx, cy;
        int i0 = 0;
        bool found = false;
        for (int i = 0; i < n; ++i) {
            if (onc(i)) { cx = px(i); cy = py(i); i0 = i; found = true; break; }
        }
        if (!found) {  // all off-curve: begin at the midpoint of 0 and 1
            cx = (px(0) + px(1)) * 0.5f;
            cy = (py(0) + py(1)) * 0.5f;
            i0 = 0;
        }

        std::vector<std::pair<float, float>> poly;
        poly.emplace_back(tx(cx), ty(cy));

        float curx = cx, cury = cy;
        int i = i0 + 1;
        for (int step = 0; step < n; ++step, ++i) {
            if (onc(i)) {
                curx = px(i);
                cury = py(i);
                poly.emplace_back(tx(curx), ty(cury));
            } else {
                // Off-curve control point; the on-curve end is the next point,
                // or the implied midpoint if that is also off-curve.
                float qx = px(i), qy = py(i);
                float ex, ey;
                if (onc(i + 1)) { ex = px(i + 1); ey = py(i + 1); ++i; ++step; }
                else { ex = (qx + px(i + 1)) * 0.5f; ey = (qy + py(i + 1)) * 0.5f; }
                for (int s = 1; s <= kCurveSegs; ++s) {
                    float t = static_cast<float>(s) / kCurveSegs, mt = 1 - t;
                    float bx = mt * mt * curx + 2 * mt * t * qx + t * t * ex;
                    float by = mt * mt * cury + 2 * mt * t * qy + t * t * ey;
                    poly.emplace_back(tx(bx), ty(by));
                }
                curx = ex;
                cury = ey;
            }
        }
        out.push_back(std::move(poly));
        start = last + 1;
    }
}

Bitmap Font::rasterize(int glyph, float scale) const {
    Bitmap bm;
    std::vector<std::vector<std::pair<float, float>>> contours;
    collect_contours(glyph, 0, 0, 1, 1, contours, 0);
    if (contours.empty()) return bm;  // space / empty glyph

    float minx = 1e30f, miny = 1e30f, maxx = -1e30f, maxy = -1e30f;
    for (auto& c : contours)
        for (auto& pt : c) {
            minx = std::min(minx, pt.first);
            miny = std::min(miny, pt.second);
            maxx = std::max(maxx, pt.first);
            maxy = std::max(maxy, pt.second);
        }

    // Font units, y up -> bitmap pixels, y down.
    int ix0 = static_cast<int>(std::floor(minx * scale));
    int ix1 = static_cast<int>(std::ceil(maxx * scale));
    int iy0 = static_cast<int>(std::floor(-maxy * scale));
    int iy1 = static_cast<int>(std::ceil(-miny * scale));
    int w = ix1 - ix0, h = iy1 - iy0;
    if (w <= 0 || h <= 0) return bm;

    // Build edges in bitmap-pixel space.
    struct Edge { float x0, y0, x1, y1; };
    std::vector<Edge> edges;
    for (auto& c : contours) {
        size_t m = c.size();
        for (size_t i = 0; i < m; ++i) {
            auto a = c[i];
            auto b = c[(i + 1) % m];
            Edge e{a.first * scale - ix0, -a.second * scale - iy0, b.first * scale - ix0,
                   -b.second * scale - iy0};
            if (e.y0 != e.y1) edges.push_back(e);  // horizontals never cross
        }
    }

    std::vector<uint16_t> acc(static_cast<size_t>(w) * h, 0);
    std::vector<float> xs;
    std::vector<int> dir;
    for (int syy = 0; syy < h * kSS; ++syy) {
        float yc = (syy + 0.5f) / kSS;
        xs.clear();
        dir.clear();
        for (auto& e : edges) {
            float y0 = e.y0, y1 = e.y1;
            bool cross = (y0 <= yc && yc < y1) || (y1 <= yc && yc < y0);
            if (!cross) continue;
            float t = (yc - y0) / (y1 - y0);
            xs.push_back(e.x0 + t * (e.x1 - e.x0));
            dir.push_back(y1 > y0 ? 1 : -1);
        }
        if (xs.size() < 2) continue;
        // Sort crossings (carrying their winding direction) by x.
        std::vector<int> order(xs.size());
        for (size_t i = 0; i < order.size(); ++i) order[i] = static_cast<int>(i);
        std::sort(order.begin(), order.end(), [&](int a, int b) { return xs[a] < xs[b]; });

        int py = syy / kSS;
        int winding = 0;
        size_t ci = 0;
        for (int sxx = 0; sxx < w * kSS; ++sxx) {
            float xc = (sxx + 0.5f) / kSS;
            while (ci < order.size() && xs[order[ci]] <= xc) winding += dir[order[ci++]];
            if (winding != 0) ++acc[static_cast<size_t>(py) * w + sxx / kSS];
        }
    }

    bm.w = w;
    bm.h = h;
    bm.xoff = ix0;
    bm.yoff = iy0;
    bm.cov.resize(static_cast<size_t>(w) * h);
    const float norm = 255.0f / (kSS * kSS);
    for (size_t i = 0; i < bm.cov.size(); ++i) {
        int v = static_cast<int>(acc[i] * norm + 0.5f);
        bm.cov[i] = static_cast<uint8_t>(v > 255 ? 255 : v);
    }
    return bm;
}

}  // namespace ttf
