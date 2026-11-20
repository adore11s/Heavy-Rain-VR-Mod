#pragma once
// Geometry of the VR settings panel (GoW panel.cpp, playbook chapter 2), in the seated reference frame
// (yaw-only, origin at the recentered head; Y up, -Z forward, metres). Pure: tests/panel_tests.cpp.
#include <cmath>

#include "posemath.h"

namespace hrvr::panel {

// GOW_STYLE_NINE_ROW_V6 (V1 geometry, playbook revision 20 look) plus one row, "Camera closer (third person)" after the
// turning rows (0.36.6, owner 2026-11-20: imported from the Beyond mod's panel, B2S 1.0.3): 1000 x 1562 px (one row
// pitch, 128 px, more than V6's 1434) at 1.2 m wide (1.8744 m high), 0.15 m right of the game screen.
// 0.36.12 (owner): one more row, "Hide head (first person)" after Motion controls: 1000 x 1690 px (2.028 m high), the
// same canvas as Beyond's eleven-row V6.
constexpr int kWidthPx = 1000, kHeightPx = 1690;
// The three-choice row (Rendering method: Native | AFR | Mono): x 40, 360, 680, 280 px wide (gaps 40).
constexpr int kThreeX[3] = {40, 360, 680};
constexpr int kThreeW = 280;
constexpr float kWidthM = 1.2f, kHeightM = kWidthM * float(kHeightPx) / float(kWidthPx);
constexpr float kGapM = 0.15f;

struct Placement {
    pose::Vec3 center{};
    float yaw = 0.f;  // the panel's front (+Z) faces the head: yaw 0 = front towards +Z
};

// To the right of a screen centred at `screen` (facing the reference, i.e. its front towards +Z) of width
// `screenW`, at the same height, turned toward the head (yaw only).
inline Placement Place(const pose::Vec3& screen, float screenW, const pose::Vec3& head) {
    Placement p;
    p.center = {screen.x + 0.5f * screenW + kGapM + 0.5f * kWidthM, screen.y, screen.z};
    const float tx = head.x - p.center.x, tz = head.z - p.center.z;
    p.yaw = std::atan2(tx, tz);
    return p;
}

// A ray (origin, unit direction) against the panel's front: true with canvas pixel coordinates and the
// hit point when it hits within the panel and 0..20 m away (GoW: front only, dot(dir, normal) < -0.001).
// Any quad of widthM x heightM metres drawn from widthPx x heightPx pixels (the panel, the welcome windows).
inline bool HitQuad(const Placement& p, float widthM, float heightM, int widthPx, int heightPx, const pose::Vec3& o,
                    const pose::Vec3& d, float* px, float* py, pose::Vec3* point) {
    const float nx = std::sin(p.yaw), nz = std::cos(p.yaw);  // +Z (front normal)
    const float rx = std::cos(p.yaw), rz = -std::sin(p.yaw);  // +X (panel right)
    const float dn = d.x * nx + d.z * nz;
    if (!(dn < -1e-3f)) return false;
    const float t = ((p.center.x - o.x) * nx + (p.center.z - o.z) * nz) / dn;
    if (!(t > 0.f && t < 20.f)) return false;
    const pose::Vec3 q{o.x + d.x * t, o.y + d.y * t, o.z + d.z * t};
    const float lx = (q.x - p.center.x) * rx + (q.z - p.center.z) * rz, ly = q.y - p.center.y;
    const float u = lx / widthM + 0.5f, v = 0.5f - ly / heightM;
    if (!(u >= 0.f && u <= 1.f && v >= 0.f && v <= 1.f)) return false;
    *px = u * float(widthPx);
    *py = v * float(heightPx);
    *point = q;
    return true;
}
inline bool Hit(const Placement& p, const pose::Vec3& o, const pose::Vec3& d, float* px, float* py, pose::Vec3* point) {
    return HitQuad(p, kWidthM, kHeightM, kWidthPx, kHeightPx, o, d, px, py, point);
}

// Welcome windows (playbook 11.4): 1.6 m ahead of the head along its horizontal facing, 8 cm below eye height,
// turned to the head; the direction eases toward the head's over 0.6 s, the height over 0.4 s; straight ahead at
// once when a window appears. Head yaw: 0 = looking along -Z, + = turned left (atan2(-fwd.x, -fwd.z)).
struct WelcomeFollow {
    static constexpr float kDist = 1.6f, kBelow = 0.08f;
    bool placed = false;
    float yaw = 0.f, y = 0.f;
    Placement Step(const pose::Vec3& head, float headYaw, float dt) {
        if (!placed) {
            yaw = headYaw;
            y = head.y - kBelow;
            placed = true;
        } else {
            yaw += std::remainder(headYaw - yaw, 6.2831853f) * (1.f - std::exp(-dt / 0.6f));
            y += (head.y - kBelow - y) * (1.f - std::exp(-dt / 0.4f));
        }
        Placement p;
        p.center = {head.x - std::sin(yaw) * kDist, y, head.z - std::cos(yaw) * kDist};
        p.yaw = std::atan2(head.x - p.center.x, head.z - p.center.z);
        return p;
    }
};

// The trigger (GoW): a pull past 0.75 clicks once; it re-arms below 0.35. Holding never repeats.
struct TriggerClick {
    bool armed = true;
    bool Feed(float trigger) {
        if (trigger < 0.35f) armed = true;
        if (armed && trigger > 0.75f) {
            armed = false;
            return true;
        }
        return false;
    }
};

}  // namespace hrvr::panel
