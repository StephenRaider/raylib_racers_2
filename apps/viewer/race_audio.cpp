#include "race_audio.hpp"

#include <algorithm>
#include <cmath>

#include "raymath.h"

namespace {

EngineSynth* gSynth = nullptr;

void audioCallback(void* buffer, unsigned int frames) {
    if (gSynth) gSynth->render(static_cast<float*>(buffer), (int)frames);
}

Vector3 carPosAt(const rr::Car& c, const std::function<float(const rr::Car&)>* heightOf) {
    return {c.state.pos.x, heightOf && *heightOf ? (*heightOf)(c) : 0.6f, -c.state.pos.y};
}
Vector3 carVel(const rr::Car& c) {
    rr::Vec2 v = c.state.velWorld();
    return {v.x, 0, -v.y};
}

}  // namespace

bool RaceAudio::init() {
    InitAudioDevice();
    if (!IsAudioDeviceReady()) return false;
    SetAudioStreamBufferSizeDefault(1024);  // ~23 ms: rpm changes stay in step with the picture
    stream_ = LoadAudioStream(EngineSynth::kRate, 32, 2);
    if (!IsAudioStreamValid(stream_)) {
        CloseAudioDevice();
        return false;
    }
    gSynth = &synth_;
    SetAudioStreamCallback(stream_, audioCallback);
    PlayAudioStream(stream_);
    ready_ = true;
    return true;
}

void RaceAudio::shutdown() {
    if (!ready_) return;
    StopAudioStream(stream_);
    UnloadAudioStream(stream_);
    gSynth = nullptr;
    CloseAudioDevice();
    ready_ = false;
}

std::vector<EngineSynth::Voice> RaceAudio::listen(const rr::Race& race, Vector3 listener, Vector3 listenerVel,
                                                  Vector3 right, int focus, EngineSynth::Engine engine,
                                                  const std::function<float(const rr::Car&)>* heightOf) {
    struct Near { int car; float dist; };
    std::vector<Near> near;
    const auto& cars = race.cars();
    for (int i = 0; i < (int)cars.size(); ++i) {
        if (cars[i].dnf) continue;
        near.push_back({i, Vector3Distance(listener, carPosAt(cars[i], heightOf))});
    }
    // the focused car always gets a voice; then the nearest others
    std::sort(near.begin(), near.end(), [&](const Near& a, const Near& b) {
        if ((a.car == focus) != (b.car == focus)) return a.car == focus;
        return a.dist < b.dist;
    });
    if ((int)near.size() > EngineSynth::kMaxVoices) near.resize(EngineSynth::kMaxVoices);

    std::vector<EngineSynth::Voice> voices;
    for (const Near& n : near) {
        const rr::Car& c = cars[n.car];
        EngineSynth::Voice v;
        v.car = n.car;
        v.rpm = c.state.rpm;
        v.engine = engine;
        v.gear = c.state.gear;
        v.throttle = c.state.fuel > 0 ? std::clamp(c.control.accel, 0.0f, 1.0f) : 0.0f;
        v.speed = rr::length(c.state.velWorld());
        v.maxRpm = c.phys.maxRpm;
        if (engine == EngineSynth::V8) {
            // the sim's 4000..19000 rpm onto a 2013 V8's 4000..18000
            const float idle = c.phys.idleRpm, top = c.phys.maxRpm;
            v.rpm = idle + std::max(0.0f, c.state.rpm - idle) * (18000.0f - idle) / (top - idle);
            v.maxRpm = 18000.0f;
        }
        v.gain = std::min(1.0f, 10.0f / (n.dist + 2.0f));
        if (n.car != focus) v.gain *= 0.8f;
        Vector3 to = Vector3Subtract(carPosAt(c, heightOf), listener);
        Vector3 dir = n.dist > 0.1f ? Vector3Scale(to, 1.0f / n.dist) : Vector3{0, 0, 1};
        v.pan = std::clamp(Vector3DotProduct(dir, right), -1.0f, 1.0f) * 0.8f;
        // Doppler: a source moving away (positive radial speed) sounds lower
        const float radial = Vector3DotProduct(Vector3Subtract(carVel(c), listenerVel), dir);
        v.pitch = std::clamp(343.0f / (343.0f + radial), 0.7f, 1.4f);
        voices.push_back(v);
    }
    return voices;
}

void RaceAudio::update(const rr::Race& race, const Camera3D& camera, int focus, bool active, float dt) {
    if (!ready_) return;
    Vector3 vel{};
    if (havePos_ && dt > 0) vel = Vector3Scale(Vector3Subtract(camera.position, lastPos_), 1.0f / dt);
    // camera cuts would read as huge speeds
    if (Vector3Length(vel) > 150.0f) vel = {};
    lastPos_ = camera.position;
    havePos_ = true;
    Vector3 fwd = Vector3Normalize(Vector3Subtract(camera.target, camera.position));
    Vector3 right = Vector3Normalize(Vector3CrossProduct(fwd, camera.up));
    if (Vector3Length(right) < 0.5f) right = {1, 0, 0};
    synth_.setVoices(active ? listen(race, camera.position, vel, right, focus, engine, heightOf ? &heightOf : nullptr)
                            : std::vector<EngineSynth::Voice>{},
                     active ? 1.1f * master_ : 0.0f);
}
