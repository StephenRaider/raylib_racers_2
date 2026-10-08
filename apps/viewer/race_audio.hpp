#pragma once
#include <functional>
#include <vector>

#include "engine_sound.hpp"
#include "race.hpp"
#include "raylib.h"

// Engine sound for the viewer: picks the cars nearest the camera, works out how
// loud, where and how Doppler-shifted each one is, and streams EngineSynth to raylib.
class RaceAudio {
public:
    bool init();  // false when there is no audio device (the viewer runs silent)
    void shutdown();
    // Call once per frame. `active` is false in the menu, while paused or when muted.
    void update(const rr::Race& race, const Camera3D& camera, int focus, bool active, float dt);
    bool ready() const { return ready_; }
    void setMaster(float m) { master_ = m; }  // viewer v2: volume, 1 = normal

    // The voices heard from `listener` (moving at `listenerVel`, `right` = its right
    // ear direction), in world coordinates. Shared with the offline WAV renderer.
    static std::vector<EngineSynth::Voice> listen(const rr::Race& race, Vector3 listener, Vector3 listenerVel,
                                                  Vector3 right, int focus, EngineSynth::Engine engine = EngineSynth::V10,
                                                  const std::function<float(const rr::Car&)>* heightOf = nullptr);

    // Viewer v2: a 2013 V8. The sim's engine still runs the old 4000-19000 rpm map; its
    // revs are scaled onto the V8's 4000-18000 for the sound.
    EngineSynth::Engine engine = EngineSynth::V10;
    // The car's real height above sea level (tracks with hills); default is a flat world.
    std::function<float(const rr::Car&)> heightOf;

private:
    EngineSynth synth_;
    AudioStream stream_{};
    bool ready_ = false;
    Vector3 lastPos_{};
    bool havePos_ = false;
    float master_ = 1.0f;
};
