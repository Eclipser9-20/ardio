// A color, and the few operations the UI needs from one. Kept tiny and header-
// only: colors are passed by value everywhere and this must inline away to
// nothing. Channels are 0..255; alpha compositing is left to the renderer.
#pragma once
#include <cstdint>

namespace studio {

struct Color {
    uint8_t r = 0, g = 0, b = 0, a = 255;

    // A copy with a different opacity. The common case in chrome drawing is one
    // base color used at several alphas (a stroke at 0.55, bright at 0.95), so
    // this reads better at the call site than rebuilding the struct.
    constexpr Color with_alpha(float alpha) const {
        return {r, g, b, static_cast<uint8_t>(alpha < 0 ? 0 : alpha > 1 ? 255 : alpha * 255.0f + 0.5f)};
    }

    // From 0xRRGGBB. Alpha defaults to opaque; pass it separately if needed.
    static constexpr Color hex(uint32_t rgb, uint8_t alpha = 255) {
        return {static_cast<uint8_t>((rgb >> 16) & 0xff),
                static_cast<uint8_t>((rgb >> 8) & 0xff),
                static_cast<uint8_t>(rgb & 0xff), alpha};
    }

    // Linear blend toward `other`, t in 0..1. Used for hover/press states so a
    // theme need only name a base color, not every interaction variant.
    constexpr Color mix(Color other, float t) const {
        auto lerp = [t](uint8_t x, uint8_t y) {
            return static_cast<uint8_t>(x + (y - x) * (t < 0 ? 0 : t > 1 ? 1 : t) + 0.5f);
        };
        return {lerp(r, other.r), lerp(g, other.g), lerp(b, other.b), lerp(a, other.a)};
    }
};

}  // namespace studio
