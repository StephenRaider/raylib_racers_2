#include "surface_sound.hpp"

#include <algorithm>
#include <cmath>

void SurfaceSynth::setVoices(const std::vector<Voice>& voices, float master) {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_ = voices;
    if ((int)pending_.size() > kMaxCars) pending_.resize(kMaxCars);
    pendingMaster_ = master;
    dirty_ = true;
}

void SurfaceSynth::render(float* out, int frames) {
    {
        std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
        if (lock.owns_lock() && dirty_) {
            active_ = pending_;
            master_ = pendingMaster_;
            dirty_ = false;
        }
    }
    std::fill(out, out + frames * 2, 0.0f);
    constexpr float dt = 1.0f / kRate, twoPi = 6.2831853f;
    const float ease = 1.0f - std::exp(-dt / 0.04f);  // gains follow in ~40 ms: no clicks
    for (const Voice& v : active_) {
        if (v.car < 0 || v.car >= kMaxCars) continue;
        State& s = st_[v.car];
        if (!s.seen) { s.seen = true; s.speedG = v.speed; s.panG = v.pan; }
        const float kT = std::min(1.0f, v.kerb * 0.5f), grT = std::min(1.0f, v.grass * 0.4f);
        const float gvT = std::min(1.0f, v.gravel * 0.4f), dtT = std::min(1.0f, v.dirt * 0.4f);
        for (int i = 0; i < frames; ++i) {
            s.kerbG += (kT - s.kerbG) * ease;
            s.grassG += (grT - s.grassG) * ease;
            s.gravelG += (gvT - s.gravelG) * ease;
            s.dirtG += (dtT - s.dirtG) * ease;
            s.gainG += (v.gain - s.gainG) * ease;
            s.speedG += (v.speed - s.speedG) * (ease * 0.5f);
            s.panG += (v.pan - s.panG) * ease;
            const float sp = s.speedG, spN = std::min(sp / 60.0f, 1.0f);
            const float n = noise();
            float y = 0;

            if (s.kerbG > 0.001f) {
                // a pulse per stripe pair; a carrier whose pitch climbs with speed
                s.kerbPhase += sp / 2.5f * dt;
                s.kerbPhase -= std::floor(s.kerbPhase);
                s.carrierPhase += (50.0f + sp * 1.4f) * dt;
                s.carrierPhase -= std::floor(s.carrierPhase);
                const float p = std::max(0.0f, std::sin(twoPi * s.kerbPhase));
                const float pulse = p * p * 0.8f + 0.2f;
                s.lpA += (n - s.lpA) * 0.12f;
                const float buzz = std::sin(twoPi * s.carrierPhase) + 0.5f * std::sin(twoPi * 2.0f * s.carrierPhase) + 1.4f * s.lpA;
                y += s.kerbG * pulse * buzz * (0.25f + 0.35f * spN);
            }
            if (s.grassG + s.dirtG > 0.001f) {
                // swish: low-passed noise that opens up with speed
                const float a = 0.02f + 0.1f * spN;
                s.lpB += (n - s.lpB) * a;
                y += (s.grassG * 0.8f + s.dirtG * 1.0f) * s.lpB * (0.3f + 1.4f * spN);
            }
            if (s.gravelG + s.dirtG > 0.001f) {
                // crackle: stones popping at a rate that follows the speed, over a hiss
                if (rnd01() < (0.002f + 0.01f * spN)) s.crackle = 0.4f + 0.6f * rnd01();
                s.crackle *= 0.995f;
                s.lpC += (n - s.lpC) * 0.35f;
                s.bp = s.lpC - s.lpB * 0.5f;
                y += (s.gravelG + 0.4f * s.dirtG) * (s.crackle * n * 0.9f + s.bp * 0.25f * (0.2f + spN));
            }
            y *= s.gainG * master_;
            const float pan = std::clamp(s.panG, -1.0f, 1.0f);
            out[2 * i] += y * (1.0f - std::max(0.0f, pan) * 0.6f);
            out[2 * i + 1] += y * (1.0f + std::min(0.0f, pan) * 0.6f);
        }
    }
    for (int i = 0; i < frames * 2; ++i) out[i] = std::clamp(out[i], -1.0f, 1.0f);
}
