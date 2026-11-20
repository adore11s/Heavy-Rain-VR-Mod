#pragma once
// Projection maths for Heavy Rain's cameras, desk-tested in tests/stereo_math_tests.cpp.
// STATIC (camerasystem_140b54610 / 1401831a0): row-major, row-vector, RH perspective with
// p[11] = -1, p[10] / p[14] the depth terms, p0 = cot(fovH/2) / s, p5 = p0 * aspect, and then
// p0 /= guardX, p5 /= guardY (the render targets carry a guard band around the visible image).
#include <algorithm>
#include <cmath>

namespace hrvr::stereo {

// Writes the frustum with tangents l (<0), r, u, d (<0) into the projection's x/y terms, keeping
// the engine's guard-band scale: a direction (tx, ty, -1) on the frustum edge lands at
// NDC +-1/guard, exactly where the engine puts the edge of its own visible image.
inline void SetFrustum(float* p, float l, float r, float u, float d, float guardX, float guardY) {
    p[0] = 2.f / (r - l) / guardX;
    p[5] = 2.f / (u - d) / guardY;
    p[8] = (r + l) / (r - l) / guardX;
    p[9] = (u + d) / (u - d) / guardY;
}

// Horizontal FOV scalar (+0x104) for a frustum of half tangent h, given s = scale * aspectParam.
inline float EngineFov(float h, float s) { return 2.f * std::atan(h / s); }

// NDC of a view-space point (row-vector p' = v * P, then divide by w).
inline void ProjectNdc(const float* p, const float v[3], float& x, float& y) {
    const float cx = v[0] * p[0] + v[1] * p[4] + v[2] * p[8] + p[12];
    const float cy = v[0] * p[1] + v[1] * p[5] + v[2] * p[9] + p[13];
    const float w = v[0] * p[3] + v[1] * p[7] + v[2] * p[11] + p[15];
    x = cx / w;
    y = cy / w;
}

}  // namespace hrvr::stereo
