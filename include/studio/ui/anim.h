// The animation store for immediate-mode widgets.
//
// Immediate mode keeps no widget objects, so a hover fade or a sliding knob has
// nowhere to remember its progress between frames. Anim is that memory: a value
// per id that each frame springs toward a target, framerate-independently. A
// widget asks for smooth(id, target) and gets the current eased value; it does
// not care that the value is retained here.
//
// The loop drives it: begin_frame(dt) resets the "is anything still moving"
// flag, widgets call smooth() during draw, and active() afterward tells the
// loop whether to render another frame or go back to sleep. That is how the app
// animates smoothly yet still idles at zero CPU once everything has settled.
#pragma once
#include <cmath>
#include <cstdint>
#include <unordered_map>

namespace studio {

class Anim {
public:
    // Start a frame. `dt` is seconds since the last frame.
    void begin_frame(float dt) {
        dt_ = dt;
        active_ = false;
    }

    // Spring the stored value for `id` toward `target`. `rate` sets snappiness
    // (higher = faster); the motion is exponential, so it is smooth regardless
    // of framerate. Returns the current value. Snaps exactly to target once
    // close enough, so animations actually finish and the loop can sleep.
    float smooth(uint64_t id, float target, float rate = 18.0f) {
        auto it = store_.find(id);
        float v = (it == store_.end()) ? target : it->second;  // new ids start settled
        float k = 1.0f - std::exp(-rate * dt_);
        v += (target - v) * k;
        if (std::fabs(target - v) > 0.0015f)
            active_ = true;
        else
            v = target;
        store_[id] = v;
        return v;
    }

    // Whether any value was still in motion this frame.
    bool active() const { return active_; }

private:
    std::unordered_map<uint64_t, float> store_;
    float dt_ = 1.0f / 60.0f;
    bool active_ = false;
};

// A stable id from a widget's position. Two widgets are never at the same spot
// in the same frame, so hashing the top-left is enough to key its animation.
inline uint64_t anim_id(float x, float y) {
    uint32_t xi = static_cast<uint32_t>(x * 4.0f);
    uint32_t yi = static_cast<uint32_t>(y * 4.0f);
    return (static_cast<uint64_t>(xi) << 32) ^ (yi * 2654435761u);
}

}  // namespace studio
