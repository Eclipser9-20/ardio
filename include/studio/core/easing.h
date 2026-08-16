// Easing curves: pure functions mapping a linear 0..1 progress to an eased
// 0..1. Used for fixed-duration tweens (a screen slide, a panel reveal). For
// the common "spring a value toward a target" case, prefer Anim::smooth, which
// is framerate-independent and needs no start time.
//
// Header-only and constexpr where it can be, so these inline away to nothing.
#pragma once

namespace studio::ease {

constexpr float clamp01(float t) { return t < 0 ? 0 : t > 1 ? 1 : t; }

constexpr float linear(float t) { return clamp01(t); }

constexpr float in_quad(float t) {
    t = clamp01(t);
    return t * t;
}
constexpr float out_quad(float t) {
    t = clamp01(t);
    return 1 - (1 - t) * (1 - t);
}
constexpr float in_out_quad(float t) {
    t = clamp01(t);
    return t < 0.5f ? 2 * t * t : 1 - (-2 * t + 2) * (-2 * t + 2) * 0.5f;
}

constexpr float in_cubic(float t) {
    t = clamp01(t);
    return t * t * t;
}
constexpr float out_cubic(float t) {
    t = clamp01(t);
    float u = 1 - t;
    return 1 - u * u * u;
}
constexpr float in_out_cubic(float t) {
    t = clamp01(t);
    return t < 0.5f ? 4 * t * t * t : 1 - (-2 * t + 2) * (-2 * t + 2) * (-2 * t + 2) * 0.5f;
}

// A gentle overshoot, for things that should feel springy (a popover, a toggle).
constexpr float out_back(float t) {
    t = clamp01(t);
    constexpr float c1 = 1.70158f;
    constexpr float c3 = c1 + 1;
    float u = t - 1;
    return 1 + c3 * u * u * u + c1 * u * u;
}

// Linear interpolation, the other half of every tween.
constexpr float lerp(float a, float b, float t) { return a + (b - a) * t; }

}  // namespace studio::ease
