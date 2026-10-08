#include "engine_sound.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <complex>
#include <cstdio>

namespace {

constexpr float kTwoPi = 6.28318530718f;

// Engine orders, in multiples of the cycle frequency (rpm / 120). A V10 fires ten
// times per cycle (order 10) and each bank five times (order 5); the crank turns twice
// (order 2). Most of the weight sits on those low orders, so the sound has a body
// instead of a whine; the orders above 10 roll off steeply and only give an edge.
constexpr int kOrders = 48;

struct OrderTable {
    float base[kOrders + 1];    // amplitude off throttle
    float bright[kOrders + 1];  // extra amplitude on throttle
    explicit OrderTable(bool v8) {
        unsigned h = 0x9e3779b9u;
        for (int j = 1; j <= kOrders; ++j) {
            h = h * 1664525u + 1013904223u;
            const float r = (h >> 8) / 16777216.0f;
            float a;
            if (v8) {
                // Flat-plane V8, 2.4 l: eight firings a cycle (order 8) and a second-order crank
                // imbalance (order 4) that gives the hard rasp; each bank fires four times (order 4).
                // Heavy low orders for the body, the firing order and its harmonics for the scream.
                switch (j) {
                    case 1: a = 0.14f; break;
                    case 2: a = 0.45f; break;   // crank
                    case 3: a = 0.28f; break;
                    case 4: a = 0.90f; break;   // bank firing and second-order: the growl
                    case 6: a = 0.60f; break;
                    case 8: a = 1.10f; break;   // firing
                    case 12: a = 0.62f; break;
                    case 16: a = 0.48f; break;  // the scream on top
                    case 24: a = 0.20f; break;
                    case 32: a = 0.06f; break;
                    default:
                        if (j < 8) a = 0.26f + 0.18f * r;
                        else a = (0.12f + 0.10f * r) * std::pow(8.0f / j, 1.5f);
                }
                base[j] = a * (j <= 4 ? 0.34f : j <= 8 ? 0.60f : 0.78f);
                bright[j] = a * (j <= 8 ? 0.95f : 0.65f);
                continue;
            }
            switch (j) {
                case 1: a = 0.55f; break;    // cycle: cylinder-to-cylinder lumpiness
                case 2: a = 0.85f; break;    // crank
                case 3: a = 0.35f; break;
                case 4: a = 0.50f; break;
                case 5: a = 1.00f; break;    // bank firing, the growl
                case 10: a = 0.80f; break;   // firing
                case 15: a = 0.30f; break;
                case 20: a = 0.20f; break;   // a little scream on top
                case 30: a = 0.08f; break;
                default:
                    if (j < 10) a = 0.22f + 0.18f * r;
                    else a = (0.10f + 0.10f * r) * std::pow(10.0f / j, 1.6f);
            }
            // Off throttle the low orders fall away and it goes thin; on throttle they
            // come back hard together with the firing harmonics.
            base[j] = a * (j <= 5 ? 0.30f : j <= 10 ? 0.55f : 0.75f);
            bright[j] = a * (j <= 10 ? 0.85f : 0.6f);
        }
        base[0] = bright[0] = 0;
    }
};
const OrderTable kTable10(false), kTable8(true);

// Exhaust pipe resonances: fixed formants the harmonics sweep through as it revs.
struct Formant { float hz, q, gain; };
constexpr Formant kFormants10[3] = {{165, 2.2f, 0.9f}, {420, 2.8f, 1.0f}, {1050, 3.0f, 0.35f}};
// a V8's shorter, fatter pipes: lower and heavier
constexpr Formant kFormants8[3] = {{300, 2.0f, 0.55f}, {1250, 2.4f, 1.0f}, {2800, 3.2f, 0.8f}};

// Asymmetric drive: tanh around an offset, so it adds even harmonics (grit) too.
float drive(float x, float k) {
    constexpr float b = 0.35f;
    return (std::tanh(k * x + b) - std::tanh(b)) / k;
}

float softClip(float x) { return std::tanh(x); }

// Topology-preserving state-variable band-pass coefficients for the formants.
struct SvfCoef { float a1, a2, a3; };
struct SvfTable {
    SvfCoef c[3];
    explicit SvfTable(const Formant* f) {
        for (int k = 0; k < 3; ++k) {
            const float g = std::tan(3.14159265f * f[k].hz / EngineSynth::kRate), r = 1.0f / f[k].q;
            c[k].a1 = 1.0f / (1.0f + g * (g + r));
            c[k].a2 = g * c[k].a1;
            c[k].a3 = g * c[k].a2;
        }
    }
};
const SvfTable kSvf10(kFormants10), kSvf8(kFormants8);

constexpr float kOutGain = 2.4f;

}  // namespace

// ---------------------------------------------------------------- exhaust convolution

namespace {

// In-place radix-2 complex FFT; sign -1 forward, +1 inverse (unscaled).
void fft(std::vector<std::complex<float>>& a, int sign) {
    const int n = (int)a.size();
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (int len = 2; len <= n; len <<= 1) {
        const float ang = sign * kTwoPi / len;
        const std::complex<float> wl(std::cos(ang), std::sin(ang));
        for (int i = 0; i < n; i += len) {
            std::complex<float> w(1, 0);
            for (int k = 0; k < len / 2; ++k) {
                const std::complex<float> u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

constexpr int kBlock = 256;  // convolution block: 5.8 ms of latency

}  // namespace

// The impulse response cut into kBlock-sample partitions and transformed once.
struct ExhaustIr {
    int parts = 0;
    std::vector<std::vector<std::complex<float>>> spec;  // [part][2 * kBlock]
};

// One voice's streaming convolution: uniform-partition overlap-save.
struct IrConvolver {
    std::shared_ptr<ExhaustIr> ir;
    std::vector<std::vector<std::complex<float>>> history;  // input spectra (ring, newest at head)
    std::vector<std::complex<float>> frame, acc;
    float prev[kBlock] = {}, cur[kBlock] = {}, out[kBlock] = {};
    int pos = 0, head = 0;

    explicit IrConvolver(std::shared_ptr<ExhaustIr> r) : ir(std::move(r)) {
        history.assign(ir->parts, std::vector<std::complex<float>>(2 * kBlock));
        frame.resize(2 * kBlock);
        acc.resize(2 * kBlock);
    }
    float process(float x) {
        const float y = out[pos];
        cur[pos] = x;
        if (++pos == kBlock) {
            pos = 0;
            for (int i = 0; i < kBlock; ++i) frame[i] = prev[i], frame[kBlock + i] = cur[i];
            fft(frame, -1);
            head = (head + ir->parts - 1) % ir->parts;
            history[head] = frame;
            std::fill(acc.begin(), acc.end(), std::complex<float>(0, 0));
            for (int p = 0; p < ir->parts; ++p) {
                const auto& h = history[(head + p) % ir->parts];
                const auto& s = ir->spec[p];
                for (int k = 0; k < 2 * kBlock; ++k) acc[k] += h[k] * s[k];
            }
            fft(acc, +1);
            for (int i = 0; i < kBlock; ++i) {
                out[i] = acc[kBlock + i].real() / (2 * kBlock);
                prev[i] = cur[i];
            }
        }
        return y;
    }
};

void EngineSynth::setExhaustImpulse(const std::vector<float>& ir, float gain) {
    if (ir.empty()) {
        ir_.reset();
        return;
    }
    auto r = std::make_shared<ExhaustIr>();
    r->parts = ((int)ir.size() + kBlock - 1) / kBlock;
    r->spec.assign(r->parts, std::vector<std::complex<float>>(2 * kBlock));
    for (int p = 0; p < r->parts; ++p) {
        std::vector<std::complex<float>>& s = r->spec[p];
        for (int i = 0; i < kBlock; ++i) {
            const int idx = p * kBlock + i;
            s[i] = idx < (int)ir.size() ? ir[idx] : 0.0f;
        }
        fft(s, -1);
    }
    ir_ = std::move(r);
    irGain_ = gain;
}

float EngineSynth::noise() {
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 17;
    rng_ ^= rng_ << 5;
    return (rng_ >> 8) / 8388608.0f - 1.0f;
}

void EngineSynth::setVoices(const std::vector<Voice>& voices, float masterGain) {
    std::lock_guard<std::mutex> lock(mutex_);
    pendingCount_ = std::min((int)voices.size(), kMaxVoices);
    for (int i = 0; i < pendingCount_; ++i) pending_[i] = voices[i];
    pendingMaster_ = masterGain;
    dirty_ = true;
}

void EngineSynth::render(float* out, int frames) {
    {
        std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
        if (lock.owns_lock() && dirty_) {
            targetCount_ = pendingCount_;
            for (int i = 0; i < targetCount_; ++i) target_[i] = pending_[i];
            masterTarget_ = pendingMaster_;
            dirty_ = false;
        }
    }
    // Which cars are audible; the others fade out.
    bool wanted[kMaxCars] = {};
    for (int i = 0; i < targetCount_; ++i) {
        const Voice& v = target_[i];
        if (v.car < 0 || v.car >= kMaxCars) continue;
        CarState& c = cars_[v.car];
        wanted[v.car] = true;
        if (!c.live) {  // start a new voice at its current state, silent
            c = CarState{};
            c.rpm = v.rpm;
            c.throttle = v.throttle;
            c.pan = v.pan;
            c.pitch = v.pitch;
            c.speed = v.speed;
            c.live = true;
        }
    }

    const float dt = 1.0f / kRate;
    // per-sample smoothing: rpm ~8 ms (seamless shifts stay quick), throttle ~25 ms, gain ~40 ms
    const float kRpm = 1 - std::exp(-dt / 0.008f), kThr = 1 - std::exp(-dt / 0.025f), kGain = 1 - std::exp(-dt / 0.04f);
    const float kMaster = 1 - std::exp(-dt / 0.15f);
    const float kPulseDecay = std::exp(-dt / 0.0012f);    // firing pulse ~1.2 ms
    const float kRough = 1 - std::exp(-dt / 0.0004f);
    const float kBody = 1 - std::exp(-kTwoPi * 140.0f * dt);  // body rumble below ~140 Hz

    for (int f = 0; f < frames; ++f) {
        master_ += (masterTarget_ - master_) * kMaster;
        float left = 0, right = 0;
        for (int ci = 0; ci < kMaxCars; ++ci) {
            CarState& c = cars_[ci];
            if (!c.live) continue;
            const Voice* v = nullptr;
            if (wanted[ci])
                for (int i = 0; i < targetCount_; ++i)
                    if (target_[i].car == ci) { v = &target_[i]; break; }
            const float tgtGain = v ? v->gain : 0.0f;
            if (v) {
                c.rpm += (v->rpm - c.rpm) * kRpm;
                c.throttle += (v->throttle - c.throttle) * kThr;
                c.pan += (v->pan - c.pan) * kGain;
                c.pitch += (v->pitch - c.pitch) * kGain;
                c.speed += (v->speed - c.speed) * kGain;
                c.maxRpm = v->maxRpm;
                c.engine = v->engine;
                if (v->engine == V8 && v->gear > 0) {
                    // gear changes: a short ignition cut on the way up (a tiny crack as it comes
                    // back) and a throttle blip on the way down. Kept subtle.
                    if (c.gearPrev > 0 && v->gear != c.gearPrev) {
                        if (v->gear > c.gearPrev) {
                            c.cutLeft = 0.045f;
                            if (c.bang < 0.1f && noise() > -0.2f) {
                                c.bang = 0.12f;
                                c.bangHz = 95.0f;
                            }
                        } else {
                            c.blipLeft = 0.14f;
                        }
                    }
                    c.gearPrev = v->gear;
                }
            }
            const bool v8 = c.engine == V8;
            const OrderTable& table = v8 ? kTable8 : kTable10;
            const Formant* formantDefs = v8 ? kFormants8 : kFormants10;
            const SvfTable& svf = v8 ? kSvf8 : kSvf10;
            const int pulses = v8 ? 8 : 10;
            c.gain += (tgtGain - c.gain) * kGain;
            if (!v && c.gain < 1e-4f) { c.live = false; continue; }

            // --- engine orders
            // (a V8's revs wander a few tenths of a percent: nothing is perfectly steady)
            if (v8) c.jit += (noise() - c.jit) * 0.004f;
            const float cycleHz = std::max(10.0f, c.rpm / 120.0f * c.pitch * (v8 ? 1.0f + 0.12f * c.jit : 1.0f));
            c.phase += cycleHz * dt;
            c.phase -= std::floor(c.phase);
            const float th = kTwoPi * (float)c.phase;
            c.cutLeft = std::max(0.0f, c.cutLeft - dt);
            c.blipLeft = std::max(0.0f, c.blipLeft - dt);
            const float thr = c.blipLeft > 0 ? std::max(c.throttle, 0.45f) : c.throttle;
            const float revs = std::min(1.0f, c.rpm / c.maxRpm);

            // Firing pulses: ten per cycle, each a little stronger or weaker than the last,
            // which roughens the tone the way a real engine's combustion does.
            const int firing = (int)(c.phase * pulses);
            if (firing != c.firing) {
                c.firing = firing;
                c.pulseGain = 1.0f + (0.18f + 0.22f * thr) * (1.0f - 0.5f * revs) * noise();
                if (v8) {
                    // each cylinder has its own strength, drifting; off the throttle they fire
                    // unevenly, some barely and some hard: the burble
                    float& cy = c.cyl[firing & 7];
                    cy = std::clamp(cy + 0.06f * noise(), 0.7f, 1.3f);
                    c.pulseGain *= cy;
                    if (thr < 0.15f && c.rpm > 0.3f * c.maxRpm) {
                        const float r = 0.5f + 0.5f * noise();
                        c.pulseGain *= r < 0.45f ? 0.35f : 1.1f + 1.0f * r;  // a gentle burble
                        // a hard, unburnt firing sometimes goes off in the pipe
                        if (r > 0.99f && c.bang < 0.2f) {
                            c.bang = 0.15f + 0.15f * (0.5f + 0.5f * noise());
                            c.bangHz = 80.0f + 50.0f * (0.5f + 0.5f * noise());
                        }
                    }
                }
                if (v8) {
                    // (after engine-sim's exhaust model: the pulse is the cylinder's blowdown, and it
                    // arrives after that cylinder's header length, 0.3-1.4 ms of sound travel here)
                    static const float kDelayMs[8] = {0.0f, 0.55f, 1.15f, 0.30f, 0.85f, 1.40f, 0.20f, 0.95f};
                    c.pendT[firing & 7] = kDelayMs[firing & 7] * 1e-3f;
                    c.pendG[firing & 7] = c.pulseGain;
                } else {
                    c.pulse += c.pulseGain;
                }
            }
            if (v8) {
                for (int k = 0; k < 8; ++k)
                    if (c.pendG[k] != 0 && (c.pendT[k] -= dt) <= 0) {
                        c.pulse += c.pendG[k];
                        c.pulseSlow += c.pendG[k];
                        c.pendG[k] = 0;
                    }
                c.pulseSlow *= std::exp(-dt / 0.006f);
            }
            c.pulse *= kPulseDecay;
            c.rough += (c.pulseGain - c.rough) * kRough;

            // Chebyshev recurrence: sin(j th) from sin((j-1) th) and sin((j-2) th)
            const float twoCos = 2.0f * std::cos(th);
            float s2 = 0, s1 = std::sin(th);
            const int maxOrder = std::min(kOrders, (int)(12000.0f / cycleHz));
            float eng = 0;
            for (int j = 1; j <= maxOrder; ++j) {
                eng += s1 * (table.base[j] + thr * table.bright[j]);
                const float s0 = twoCos * s1 - s2;
                s2 = s1;
                s1 = s0;
            }
            eng *= 0.16f * c.rough;

            // intake and mechanical noise, band-passed, louder on throttle and at high revs
            const float n = noise();
            c.noiseLp += (n - c.noiseLp) * 0.25f;
            c.noiseHp += (c.noiseLp - c.noiseHp) * 0.02f;

            // overrun crackle: off throttle at high revs, random pops that bang through the pipes
            if (thr < 0.1f && c.rpm > 0.4f * c.maxRpm && noise() > 0.9992f) c.pop = 1.0f;
            if (v8) {
                // lifting off at speed starts a volley of backfires over the next half second
                c.thrSlow += (thr - c.thrSlow) * (dt / 0.25f);
                if (!c.lifted && c.thrSlow - thr > 0.5f && c.rpm > 0.45f * c.maxRpm) {
                    c.lifted = true;
                    c.volley = noise() > 0.2f ? 0 : 1;  // most lifts are clean; one in a few gets a single bang
                    c.volleyClock = 0.02f;
                }
                if (thr > 0.3f) c.lifted = false;
                if (c.volley > 0) {
                    c.volleyClock -= dt;
                    if (c.volleyClock <= 0) {
                        c.bang = 0.22f + 0.15f * (0.5f + 0.5f * noise());
                        c.bangHz = 75.0f + 55.0f * (0.5f + 0.5f * noise());
                        c.volley--;
                        c.volleyClock = 0.06f + 0.2f * (0.5f + 0.5f * noise());
                    }
                }
                c.bangPh += kTwoPi * c.bangHz * dt;
                c.bang *= std::exp(-dt / 0.035f);
                if (c.bang < 1e-3f) c.bang = 0;
            }
            const float popSig = c.pop * noise() + (v8 ? c.bang * noise() * 1.8f : 0.0f);
            c.pop *= 0.9965f;

            // exhaust: firing pulses, harmonics and pops ring the pipe resonances
            // (the pulse train's mean level is the pulse rate times its decay time)
            const float pulseMean = v8 ? cycleHz * pulses * (0.0012f - 0.18f * 0.006f) : 1.6f;
            const float exc = eng + ((v8 ? c.pulse - 0.18f * c.pulseSlow : c.pulse) - pulseMean) * (0.10f + 0.22f * thr) * (v8 ? 2.6f : 1.0f) + popSig * 0.8f;
            float formants = 0;
            for (int k = 0; k < 3; ++k) {
                const SvfCoef& q = svf.c[k];
                const float v3 = exc - c.fz2[k];
                const float v1 = q.a1 * c.fz1[k] + q.a2 * v3;
                const float v2 = c.fz2[k] + q.a2 * c.fz1[k] + q.a3 * v3;
                c.fz1[k] = 2 * v1 - c.fz1[k];
                c.fz2[k] = 2 * v2 - c.fz2[k];
                // (the V8's top resonance is its scream: it opens up with throttle and revs)
                formants += formantDefs[k].gain * (v8 && k == 2 ? 0.12f + 1.7f * thr * revs * revs : 1.0f) * v1;
            }

            // the exhaust's recorded response: the pulses ring through it
            float convOut = 0;
            if (v8 && ir_) {
                if (!c.conv) c.conv = std::make_shared<IrConvolver>(ir_);
                convOut = c.conv->process(exc) * irGain_;
                // the recording is heavy below 100 Hz: take that out, the body comes from elsewhere
                const float a = 1 - std::exp(-kTwoPi * 230.0f * dt);
                float hp = convOut;
                for (int k = 0; k < 3; ++k) {  // three cascaded one-pole high-passes
                    c.irLp[k] += (hp - c.irLp[k]) * a;
                    hp -= c.irLp[k];
                }
                convOut = hp;  // (the recording is bass-heavy: -30 dB at 60 Hz)
            }

            // tailpipe low-pass: opens up a little with throttle and revs, never to a whine
            const float fc = 900.0f + 4200.0f * thr * revs;
            const float a = 1 - std::exp(-kTwoPi * fc * dt);
            c.lp1 += (exc - c.lp1) * a;
            c.lp2 += (c.lp1 - c.lp2) * a;

            // body: the low thump under it all, strongest pulling away on throttle
            c.body += (exc - c.body) * kBody;
            c.body2 += (c.body - c.body2) * kBody;
            const float bodyAmt = thr * (1.25f - 0.85f * revs) * (v8 ? 0.5f : 1.0f);

            float sig = 0.55f * c.lp2 + 0.12f * eng + 0.75f * formants + bodyAmt * c.body2;
            if (v8 && ir_) sig = 0.30f * c.lp2 + 0.14f * eng + 0.60f * formants + bodyAmt * c.body2 + convOut;
            sig += (c.noiseLp - c.noiseHp) * (0.03f + 0.10f * thr) * revs;
            sig += popSig * 0.25f;  // a bit of raw crackle on top

            // rev limiter: the ignition cuts in and out at ~25 Hz when pinned against it
            if (c.rpm > 0.985f * c.maxRpm && thr > 0.5f) {
                c.limiterClock += dt * 25.0f;
                c.limiter = (c.limiterClock - std::floor(c.limiterClock)) < 0.5f ? 0.35f : 1.0f;
            } else {
                c.limiter = 1.0f;
            }

            const float level = (0.3f + 0.7f * thr) * (0.5f + 0.5f * revs) * c.limiter * (c.cutLeft > 0 ? 0.45f : 1.0f);
            // asymmetric saturation: dirtier the harder it is driven
            // (the V8 is driven much less: hard saturation reads as a blown speaker)
            float s = v8 ? drive(sig * 0.9f * level, 0.25f + 0.25f * thr) : drive(sig * 2.0f * level, 1.5f + 2.5f * thr);
            // remove the DC the asymmetric drive leaves behind
            c.dcOut = s - c.dcIn + 0.9985f * c.dcOut;
            c.dcIn = s;
            s = v8 ? softClip(c.dcOut * 1.3f) * 0.75f : softClip(c.dcOut * kOutGain) * 0.6f;

            // wind and tyres: low-passed noise rising with speed
            c.windLp += (n - c.windLp) * 0.04f;
            s += c.windLp * std::min(1.0f, c.speed * c.speed / 9000.0f) * 0.5f;

            // the thump of a backfire, below everything the pipes colour
            if (v8 && c.bang > 0) s += c.bang * std::sin(c.bangPh) * (0.55f + 0.2f * std::sin(c.bangPh * 0.5f));
            s *= c.gain;
            const float pl = std::sqrt(0.5f * (1 - c.pan)), pr = std::sqrt(0.5f * (1 + c.pan));
            left += s * pl;
            right += s * pr;
        }
        if (reverbMix_ > 0) {
            // four damped combs on the mono mix, tapped with different signs for each ear
            static const int len[4] = {1327, 1699, 2111, 2543};
            if (comb_[0].empty())
                for (int k = 0; k < 4; ++k) comb_[k].assign(len[k], 0.0f);
            const float in = 0.5f * (left + right);
            float o[4];
            for (int k = 0; k < 4; ++k) {
                o[k] = comb_[k][combIdx_[k]];
                combLp_[k] += (o[k] - combLp_[k]) * 0.45f;
                comb_[k][combIdx_[k]] = in + combLp_[k] * 0.66f;
                if (++combIdx_[k] >= len[k]) combIdx_[k] = 0;
            }
            left += reverbMix_ * 0.25f * (o[0] - o[1] + o[2] - o[3]);
            right += reverbMix_ * 0.25f * (o[0] + o[1] - o[2] - o[3]);
        }
        out[2 * f] = softClip(left * master_);
        out[2 * f + 1] = softClip(right * master_);
    }
}

bool writeWav(const char* path, const std::vector<float>& stereo, int rate) {
    FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    const uint32_t dataBytes = (uint32_t)(stereo.size() * 2);
    auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    std::fwrite("RIFF", 1, 4, f);
    u32(36 + dataBytes);
    std::fwrite("WAVEfmt ", 1, 8, f);
    u32(16); u16(1); u16(2); u32(rate); u32(rate * 4); u16(4); u16(16);
    std::fwrite("data", 1, 4, f);
    u32(dataBytes);
    for (float s : stereo) {
        int16_t v = (int16_t)std::lround(std::clamp(s, -1.0f, 1.0f) * 32767.0f);
        std::fwrite(&v, 2, 1, f);
    }
    return std::fclose(f) == 0;
}
