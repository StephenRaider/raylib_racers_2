#include "effects.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdio>

#include "raymath.h"
#include "rlgl.h"

namespace {
const size_t kSkidCap = 6000, kTrackCap = 4000, kPartCap = 3000;
const float kWheelX = 1.65f, kWheelY = 0.78f;  // half wheelbase, half track (m)
Vector3 W(rr::Vec2 p, float h = 0) { return {p.x, h, -p.y}; }

void wheelPos(const rr::Car& c, Vector3 out[4], float h) {
    const float cy = std::cos(c.state.yaw), sy = std::sin(c.state.yaw);
    const float lx[4] = {kWheelX, kWheelX, -kWheelX, -kWheelX}, ly[4] = {kWheelY, -kWheelY, kWheelY, -kWheelY};
    for (int w = 0; w < 4; ++w)
        out[w] = W(c.state.pos + rr::Vec2{lx[w] * cy - ly[w] * sy, lx[w] * sy + ly[w] * cy}, h);
}
}  // namespace

void Effects::reset(const rr::Race& race) {
    race_ = &race;
    skids_.clear(); tracks_.clear(); parts_.clear();
    ++marksVersion_;
    skidHead_ = trackHead_ = 0;
    last_.assign(race.cars().size(), Wheels{});
    for (size_t i = 0; i < race.cars().size(); ++i) last_[i].collisions = race.cars()[i].collisions;
}

void Effects::addMark(std::vector<Mark>& ring, size_t& head, size_t cap, Vector3 a, Vector3 b, float w, unsigned char alpha, Color col) {
    ++marksVersion_;
    Mark m{a, b, w, alpha, col};
    if (ring.size() < cap) ring.push_back(m);
    else ring[head] = m;
    head = (head + 1) % cap;
}

void Effects::emit(int kind, Vector3 p, Vector3 v, int count, Color col) {
    auto rnd = [&]() { rng_ = rng_ * 1664525u + 1013904223u; return (rng_ >> 8) / 16777216.0f; };
    for (int k = 0; k < count && parts_.size() < kPartCap; ++k) {
        Particle q{};
        q.p = p;
        q.kind = kind;
        if (kind == 0) {  // tyre smoke: drifts up, grows, fades
            q.v = Vector3Add(Vector3Scale(v, 0.25f), {rnd() - 0.5f, 0.6f + rnd() * 0.6f, rnd() - 0.5f});
            q.life = 1.4f + rnd(); q.size = 0.4f; q.col = {230, 230, 232, 55};
        } else if (kind == 1) {  // dust and grass thrown up
            q.v = Vector3Add(Vector3Scale(v, 0.35f), {2 * rnd() - 1, 1.0f + rnd() * 1.5f, 2 * rnd() - 1});
            q.life = 1.0f + 0.8f * rnd(); q.size = 0.3f + 0.15f * rnd(); q.col = col;
        } else {  // sparks: fast, short, fall
            q.v = Vector3Add(Vector3Scale(v, 0.6f), {6 * rnd() - 3, 1.5f + rnd() * 3, 6 * rnd() - 3});
            q.life = 0.35f + 0.3f * rnd(); q.size = 0.06f; q.col = {255, 190, 70, 255};
        }
        parts_.push_back(q);
    }
}

void Effects::update(const rr::Race& race, float dt) {
    if (race_ != &race || race.time() < lastTime_ || last_.size() != race.cars().size()) reset(race);
    const bool moved = race.time() > lastTime_;
    lastTime_ = race.time();
    // particles live in real time, so they still drift while paused
    for (auto& q : parts_) {
        q.age += dt;
        if (q.kind == 2) q.v.y -= 9.8f * dt;
        else q.v = Vector3Scale(q.v, 1.0f - std::min(1.0f, 1.2f * dt));
        q.p = Vector3Add(q.p, Vector3Scale(q.v, dt));
        if (q.kind == 0 || q.kind == 1) q.size += dt * (q.kind == 0 ? 1.6f : 0.7f);
    }
    parts_.erase(std::remove_if(parts_.begin(), parts_.end(), [](const Particle& q) { return q.age >= q.life; }), parts_.end());
    if (level_ <= 0 || !moved) return;

    for (size_t i = 0; i < race.cars().size(); ++i) {
        const rr::Car& c = race.cars()[i];
        Wheels& L = last_[i];
        Vector3 now[4];
        wheelPos(c, now, 0.0f);
        const float speed = std::hypot(c.state.vx, c.state.vy);
        if (c.dnf || c.pitState != RR_PIT_NONE) { L.valid = false; continue; }
        const float jump = L.valid ? Vector3Distance(now[2], L.p[2]) : 0.0f;
        const bool join = L.valid && jump < 25.0f;  // a fast-forward or a reset leaves a gap
        const bool sliding = c.state.slipAngle[0] > 1.15f || c.state.slipAngle[1] > 1.15f;
        const bool locking = c.control.brake > 0.85f && speed > 15 && c.state.ax < -30;
        const bool spinning = std::fabs(c.state.wheelSpin) > 0.25f;
        // the road's height under each tyre (RR2's tracks have hills), and what it runs on
        const rr::Track& tr = race.track();
        const float lift = 0.012f;
        for (int w = 0; w < 4; ++w) {
            if (heights_) now[w].y = tr.heightAt(c.trackS, c.wheelLat[w]) + lift;
            const int sf = c.wheelSurf[w];
            L.loose[w] = sf == RR_SURF_GRASS || sf == RR_SURF_GRAVEL || sf == RR_SURF_DIRT;
        }
        if (join && c.onTrack) {
            if (sliding || locking || spinning) {
                const int from = locking ? 0 : (spinning ? 2 : 0);
                for (int w = from; w < 4; ++w) {
                    if (L.loose[w]) continue;  // the loose stuff leaves tracks and dust, below
                    Vector3 a = L.p[w], b = now[w];
                    if (!heights_) a.y = b.y = 0.009f;
                    addMark(skids_, skidHead_, kSkidCap, a, b, 0.30f, 150, Color{18, 18, 18, 255});
                }
            }
        }
        if (join && speed > 3) {
            // tyres on grass, gravel or dirt: dark tracks in the surface and dust and clods thrown up
            const rr::Vec2 vel = c.state.velWorld();
            Vector3 v = W(vel);
            for (int w = 0; w < 4; ++w) {
                if (!L.loose[w]) continue;
                const int sf = c.wheelSurf[w];
                Vector3 a = L.p[w], b = now[w];
                if (!heights_) a.y = b.y = 0.009f;
                const Color tone = sf == RR_SURF_GRAVEL ? Color{60, 52, 42, 255} : (sf == RR_SURF_DIRT ? Color{70, 52, 36, 255} : Color{58, 66, 34, 255});
                if (std::fabs(a.x - b.x) + std::fabs(a.z - b.z) < 3.0f) addMark(tracks_, trackHead_, kTrackCap, a, b, 0.28f, 105, tone);
                if (level_ >= 2 && speed > 6) {
                    const Color dust = sf == RR_SURF_GRAVEL ? Color{186, 166, 130, 95} : (sf == RR_SURF_DIRT ? Color{160, 124, 88, 105} : Color{132, 128, 88, 70});
                    emit(1, now[w], v, speed > 25 ? 2 : 1, dust);
                }
            }
        }
        if (level_ >= 2 && c.collisions > L.collisions) {
            Vector3 v = W(c.state.velWorld());
            Vector3 p = W(c.state.pos, 0.3f);
            emit(2, p, v, 40, Color{255, 190, 70, 255});
        }
        L.collisions = c.collisions;
        for (int w = 0; w < 4; ++w) L.p[w] = now[w];
        L.valid = true;
    }
}

void Effects::draw(const rr::Race& race, Vector3 camPos) const {
    // Only for the race update() last saw: a race swapped since (leaving a test session,
    // a restart) leaves race_ pointing at freed memory until the next update() resets.
    if (level_ <= 0 || race_ != &race) return;
    auto quad = [](const std::vector<Mark>& ring) {
        size_t n = 0;
        rlBegin(RL_TRIANGLES);
        for (const Mark& m : ring) {
            if (++n % 1000 == 0) { rlEnd(); rlDrawRenderBatchActive(); rlBegin(RL_TRIANGLES); }
            Vector3 d = Vector3Subtract(m.b, m.a);
            float len = std::sqrt(d.x * d.x + d.z * d.z);
            if (len < 1e-3f) continue;
            Vector3 n = {-d.z / len * m.width * 0.5f, 0, d.x / len * m.width * 0.5f};
            Vector3 a0 = Vector3Add(m.a, n), a1 = Vector3Subtract(m.a, n);
            Vector3 b0 = Vector3Add(m.b, n), b1 = Vector3Subtract(m.b, n);
            rlColor4ub(m.col.r, m.col.g, m.col.b, m.alpha);
            rlVertex3f(a0.x, a0.y, a0.z); rlVertex3f(b1.x, b1.y, b1.z); rlVertex3f(b0.x, b0.y, b0.z);
            rlVertex3f(a0.x, a0.y, a0.z); rlVertex3f(a1.x, a1.y, a1.z); rlVertex3f(b1.x, b1.y, b1.z);
        }
        rlEnd();
    };
    rlDisableBackfaceCulling();
    rlDisableDepthMask();
    // the rubber on the racing line: the sim's own map (it also gives the grip), drawn near the camera
    if (const rr::TrackRubber* rub = race.rubber()) {
        const rr::Track& tr = race.track();
        size_t n = 0;
        rlBegin(RL_TRIANGLES);
        for (int col = 0; col < rub->cols(); ++col) {
            const float s0 = col * rr::TrackRubber::kCellS;
            const Vector3 mid = W(tr.pointAt(s0, 0));
            if (std::fabs(mid.x - camPos.x) > 220 || std::fabs(mid.z - camPos.z) > 220) continue;
            for (int lane = 0; lane < rr::TrackRubber::kLanes; ++lane) {
                const float r = rub->cell(col, lane);
                if (r < 0.03f) continue;
                if (++n % 1000 == 0) { rlEnd(); rlDrawRenderBatchActive(); rlBegin(RL_TRIANGLES); }
                const float l0 = (lane - rr::TrackRubber::kLanes / 2) * rr::TrackRubber::kCellL;
                const float s1 = s0 + rr::TrackRubber::kCellS, l1 = l0 + rr::TrackRubber::kCellL;
                auto pt = [&](float s, float l) { return W(tr.pointAt(s, l), heights_ ? tr.heightAt(s, l) + 0.006f : 0.006f); };
                Vector3 p00 = pt(s0, l0), p10 = pt(s1, l0), p01 = pt(s0, l1), p11 = pt(s1, l1);
                rlColor4ub(18, 18, 20, (unsigned char)(r * 140));
                rlVertex3f(p00.x, p00.y, p00.z); rlVertex3f(p11.x, p11.y, p11.z); rlVertex3f(p10.x, p10.y, p10.z);
                rlVertex3f(p00.x, p00.y, p00.z); rlVertex3f(p01.x, p01.y, p01.z); rlVertex3f(p11.x, p11.y, p11.z);
            }
        }
        rlEnd();
    }
    quad(tracks_);
    quad(skids_);
    if (level_ >= 2) {
        for (const Particle& q : parts_) {
            float t = q.age / q.life;
            Color c = q.col;
            c.a = (unsigned char)(c.a * (1.0f - t));
            if (q.kind == 2) DrawCubeV(q.p, {q.size, q.size, q.size}, c);
            else if (Vector3Distance(q.p, camPos) < 250) DrawSphereEx(q.p, q.size, 4, 6, c);
        }
    }
    rlDrawRenderBatchActive();
    rlEnableDepthMask();
    rlEnableBackfaceCulling();
}
