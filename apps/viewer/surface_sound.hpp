#pragma once
#include <mutex>
#include <vector>

// Synthesised sounds of the surfaces under the wheels: the rumble of the kerbs (a buzz whose
// pitch and pulse rate follow the speed, one stripe every 2.5 m), grass swish, gravel crackle
// and dirt. All generated from noise and a few oscillators, so there is no sample to credit.
// No raylib dependency.
class SurfaceSynth {
public:
    static constexpr int kMaxCars = 32;
    static constexpr int kRate = 44100;

    struct Voice {
        int car = -1;
        float speed = 0;  // m/s
        float kerb = 0, grass = 0, gravel = 0, dirt = 0;  // wheels (0..4) on each surface
        float gain = 0;   // distance attenuation, 0..1
        float pan = 0;    // -1 left .. 1 right
    };

    void setVoices(const std::vector<Voice>& voices, float master);
    void render(float* out, int frames);  // interleaved stereo float

private:
    struct State {
        float kerbPhase = 0, carrierPhase = 0;
        float kerbG = 0, grassG = 0, gravelG = 0, dirtG = 0, gainG = 0, speedG = 0, panG = 0;
        float lpA = 0, lpB = 0, lpC = 0, bp = 0, crackle = 0;
        bool seen = false;
    };
    std::mutex mutex_;
    std::vector<Voice> pending_, active_;
    float pendingMaster_ = 0, master_ = 0;
    bool dirty_ = false;
    State st_[kMaxCars];
    unsigned rng_ = 12345u;
    float noise() {
        rng_ ^= rng_ << 13; rng_ ^= rng_ >> 17; rng_ ^= rng_ << 5;
        return (float)(int)rng_ * (1.0f / 2147483648.0f);
    }
    float rnd01() { return noise() * 0.5f + 0.5f; }
};
