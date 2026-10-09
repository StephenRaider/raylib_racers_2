#pragma once
// Small animation helpers for the HUD and menus: easing curves, values that ease towards a
// target, lights that fade up and down, and colour blends. Frame-rate independent: pass the
// frame time (s) each update. Nothing here knows about races or widgets.
#include <algorithm>
#include <cmath>
#include <map>

#include "raylib.h"

namespace anim {

inline float clamp01(float t) { return std::clamp(t, 0.0f, 1.0f); }

// Easing curves on [0, 1].
inline float linear(float t) { return clamp01(t); }
inline float easeOutCubic(float t) { t = 1 - clamp01(t); return 1 - t * t * t; }
inline float easeInOutCubic(float t) {
    t = clamp01(t);
    return t < 0.5f ? 4 * t * t * t : 1 - std::pow(-2 * t + 2, 3.0f) / 2;
}
inline float easeOutBack(float t) {  // overshoots a little, then settles
    t = clamp01(t) - 1;
    const float s = 1.70158f;
    return 1 + t * t * ((s + 1) * t + s);
}

inline float lerp(float a, float b, float t) { return a + (b - a) * t; }

// Moves `cur` towards `target`, covering about 95% of the distance in `time` seconds.
inline float approach(float cur, float target, float dt, float time) {
    if (time <= 0) return target;
    return cur + (target - cur) * (1 - std::exp(-3 * dt / time));
}

// Moves `cur` towards `target` at a constant rate: a full 0..1 swing takes `time` seconds.
inline float ramp(float cur, float target, float dt, float time) {
    if (time <= 0) return target;
    const float step = dt / time;
    return cur < target ? std::min(target, cur + step) : std::max(target, cur - step);
}

// A value that eases from where it is to a new target over a fixed time whenever the target
// changes. The first set() snaps, so a widget that appears starts settled.
struct Tween {
    float from = 0, to = 0, t = 1, duration = 0.4f;
    bool started = false;
    float (*ease)(float) = easeInOutCubic;

    void set(float target, float time) {
        if (!started) { snap(target); return; }
        if (target == to) return;
        from = value();
        to = target;
        t = 0;
        duration = time;
    }
    void snap(float v) { from = to = v; t = 1; started = true; }
    void update(float dt) { t = duration > 0 ? std::min(1.0f, t + dt / duration) : 1; }
    float value() const { return lerp(from, to, ease(t)); }
    bool moving() const { return t < 1; }
};

// A 0..1 light: fades up in `up` seconds when on, down in `down` seconds when off, eased so it
// glows in rather than blinks.
struct Light {
    float level = 0;
    void update(bool on, float dt, float up = 0.2f, float down = 0.35f) {
        level = ramp(level, on ? 1.0f : 0.0f, dt, on ? up : down);
    }
    float value() const { return easeInOutCubic(level); }
};

// The last value of something shown and the one before it, for crossfading a label that changes
// (a driver name, a position). `t` runs 0..1 after each change.
template <class T>
struct Crossfade {
    T current{}, previous{};
    float t = 1;
    bool started = false;
    void set(const T& v) {
        if (!started) { current = previous = v; t = 1; started = true; return; }
        if (v == current) return;
        previous = current;
        current = v;
        t = 0;
    }
    void update(float dt, float time = 0.3f) { t = time > 0 ? std::min(1.0f, t + dt / time) : 1; }
};

// Per-item animation state, created on first use (keyed by e.g. car index).
template <class K, class V>
struct Keyed {
    std::map<K, V> items;
    V& operator[](const K& k) { return items[k]; }
    void clear() { items.clear(); }
};

inline Color mix(Color a, Color b, float t) {
    t = clamp01(t);
    return {(unsigned char)lerp(a.r, b.r, t), (unsigned char)lerp(a.g, b.g, t), (unsigned char)lerp(a.b, b.b, t),
            (unsigned char)lerp(a.a, b.a, t)};
}

inline Color alpha(Color c, float a) { return Fade(c, clamp01(a) * c.a / 255.0f); }

// Perceived brightness 0..1, for picking a dark or light outline against a colour.
inline float luminance(Color c) { return (0.299f * c.r + 0.587f * c.g + 0.114f * c.b) / 255.0f; }

}  // namespace anim
