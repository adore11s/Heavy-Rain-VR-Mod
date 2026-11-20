#pragma once
// Head-tracked sound listener math (audio.cpp; playbook chapter 17), header-only for the desk tests.
//
// Engine convention: row vectors, rows X Y Z T of a world matrix; front = -Z, up = +Y (the listener task
// viewport_1404ddb10 rotates (0,0,-1) and (0,1,0) by the camera node world and passes its +0xC0 row).
#include <cmath>

#include "posemath.h"

namespace hrvr::audio {

using pose::Mat4;

// The head the eyes were drawn with this pair. absolute: first person, the head is placed in the world
// (fpcam base) and does not depend on the game camera; otherwise the head rides on the game camera.
struct HeadPublish {
    Mat4 head = pose::Identity();      // head world (eye midpoint), engine units
    Mat4 authored = pose::Identity();  // the game camera world of the same pair
    bool absolute = false;
    bool valid = false;
    unsigned long long ms = 0;
};

inline float Dot3(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// The camera world from the listener inputs; false when front / up are degenerate or parallel.
inline bool FrameFromVectors(const float* front, const float* up, const float* pos, Mat4* out) {
    float z[3] = {-front[0], -front[1], -front[2]};
    const float zl = std::sqrt(Dot3(z, z));
    if (!(zl > 1e-4f) || !std::isfinite(zl)) return false;
    for (float& c : z) c /= zl;
    const float d = Dot3(up, z);
    float y[3] = {up[0] - d * z[0], up[1] - d * z[1], up[2] - d * z[2]};
    const float yl = std::sqrt(Dot3(y, y));
    if (!(yl > 1e-3f) || !std::isfinite(yl)) return false;
    for (float& c : y) c /= yl;
    const float x[3] = {y[1] * z[2] - y[2] * z[1], y[2] * z[0] - y[0] * z[2], y[0] * z[1] - y[1] * z[0]};
    Mat4 m = pose::Identity();
    for (int k = 0; k < 3; ++k) {
        m.m[k] = x[k];
        m.m[4 + k] = y[k];
        m.m[8 + k] = z[k];
        m.m[12 + k] = pos[k];
    }
    if (!std::isfinite(pos[0]) || !std::isfinite(pos[1]) || !std::isfinite(pos[2])) return false;
    *out = m;
    return true;
}

// The listener world for the game camera `cam` (this frame's): the head itself in first person; otherwise
// the head's offset from the camera it was drawn with, carried onto the current camera (the listener task can
// run a frame before or after the eye passes).
inline Mat4 ListenerWorld(const HeadPublish& p, const Mat4& cam) {
    if (p.absolute) return p.head;
    const Mat4 offset = pose::Mul(p.head, pose::RigidInverse(p.authored));  // head = offset * authored
    return pose::Mul(offset, cam);
}

// The published head applies to a listener only while it is fresh and that listener follows the same camera
// (split screens: one listener per viewport; a cut: the old pair's camera is elsewhere).
inline bool Applies(const HeadPublish& p, const float* gamePos, unsigned long long now, float maxMetres) {
    if (!p.valid || now < p.ms || now - p.ms > 250) return false;
    const float d[3] = {gamePos[0] - p.authored.m[12], gamePos[1] - p.authored.m[13], gamePos[2] - p.authored.m[14]};
    return Dot3(d, d) <= maxMetres * maxMetres;
}

inline void FrontUpOf(const Mat4& m, float* front, float* up) {
    for (int k = 0; k < 3; ++k) {
        front[k] = -m.m[8 + k];
        up[k] = m.m[4 + k];
    }
}

}  // namespace hrvr::audio
