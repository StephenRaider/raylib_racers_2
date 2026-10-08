#pragma once
#include <memory>
#include <mutex>
#include <vector>

// Synthesised engine sound: a mid-2000s F1 V10 (or a 2013 V8) for each audible car, built from the
// engine orders of its rpm (firing pulses, crank and cam irregularity), intake and
// exhaust noise, overrun crackle, a rev-limiter stutter and some wind and tyre noise.
// It has no raylib dependency, so it can also render offline to a WAV file.
class EngineSynth {
public:
    static constexpr int kMaxCars = 32;
    static constexpr int kRate = 44100;
    enum Engine : int {
        V10,  // 2000s 3.0 l V10: ten firings a cycle, to 19000 rpm
        V8,   // 2013 2.4 l flat-plane V8: eight firings a cycle (a harder, rawer note), to 18000 rpm
    };

    // What one car sounds like right now, as heard from the listener.
    struct Voice {
        int car = -1;          // car index, keeps each car's oscillator state
        float rpm = 0;
        float throttle = 0;    // 0..1
        float speed = 0;       // m/s, for wind and tyre noise
        float maxRpm = 19000;
        Engine engine = V10;
        float gain = 0;        // distance attenuation, 0..1
        float pan = 0;         // -1 left .. 1 right
        float pitch = 1;       // Doppler factor
    };

    // Main thread: replace the audible voices (at most kMaxVoices are used).
    void setVoices(const std::vector<Voice>& voices, float masterGain);
    // Audio thread (or offline): render interleaved stereo float samples.
    void render(float* out, int frames);

    static constexpr int kMaxVoices = 6;
    // An exhaust impulse response (mono, kRate): the V8's firing pulses are convolved with it,
    // as engine-sim does, in place of the built-in pipe resonances. Call before any voice starts.
    void setExhaustImpulse(const std::vector<float>& ir, float gain = 1.0f);
    // A small room/trackside reverb on the mix (0 = dry, the default).
    void setReverb(float mix) { reverbMix_ = mix; }

private:
    struct CarState {
        double phase = 0;      // engine cycle (two crank revolutions), 0..1
        float rpm = 0, throttle = 0, gain = 0, pan = 0, pitch = 1, speed = 0, maxRpm = 19000;
        Engine engine = V10;
        float lp1 = 0, lp2 = 0, noiseLp = 0, noiseHp = 0, windLp = 0;
        float pop = 0, limiter = 1, limiterClock = 0;
        int firing = 0;                       // index of the last firing pulse (10 or 8 per cycle)
        float pulse = 0, pulseGain = 1, rough = 1;  // firing-pulse excitation and per-pulse jitter
        float fz1[3] = {}, fz2[3] = {};       // exhaust formant filters (state-variable)
        float body = 0, body2 = 0, dcIn = 0, dcOut = 0;
        bool live = false;
        // V8: per-cylinder strength, backfire (a bang is a low thump plus a crack that decays),
        // the throttle lifts that start a volley of them, and a slow wobble in the revs
        float cyl[8] = {1, 1, 1, 1, 1, 1, 1, 1};
        float bang = 0, bangPh = 0, bangHz = 100, bangLevel = 1;
        float thrSlow = 0, volleyClock = 0, jit = 0;
        // each cylinder's exhaust blowdown pulse reaches the collector after its own pipe length
        float pendT[8] = {}, pendG[8] = {};
        float pulseSlow = 0;  // the slow, negative scavenging tail of a pulse
        std::shared_ptr<struct IrConvolver> conv;  // this car's running convolution with the exhaust response
        int volley = 0;
        bool lifted = false;
    };

    std::mutex mutex_;
    Voice pending_[kMaxVoices];
    int pendingCount_ = 0;
    float pendingMaster_ = 0;
    bool dirty_ = false;

    // owned by the rendering thread
    Voice target_[kMaxVoices];
    int targetCount_ = 0;
    float master_ = 0, masterTarget_ = 0;
    CarState cars_[kMaxCars];
    unsigned rng_ = 0x1234567u;
    float reverbMix_ = 0;
    std::shared_ptr<struct ExhaustIr> ir_;  // partitioned spectrum of the exhaust response
    float irGain_ = 1.0f;
    std::vector<float> comb_[4];
    int combIdx_[4] = {0, 0, 0, 0};
    float combLp_[4] = {0, 0, 0, 0};

    float noise();
};

// Writes interleaved stereo float samples as a 16-bit WAV file.
bool writeWav(const char* path, const std::vector<float>& stereo, int rate);
