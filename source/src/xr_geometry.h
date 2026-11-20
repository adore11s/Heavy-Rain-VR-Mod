#pragma once
// Headset view geometry checks for the session log. Player report (0.17.14, Quest 3 over Quest
// Link, Meta's OpenXR runtime): near people and objects doubled, walls and sky fine, each eye
// clean on its own. The log now states what the runtime reports for each eye relative to the head
// and what the game cameras render with, so one log separates eye placement from frustum issues.
// Pure maths, desk-tested in tests/xr_geometry_tests.cpp.
#include <cmath>

#include "posemath.h"

namespace hrvr::xrgeom {

// One eye as the runtime reports it in OpenXR VIEW space (relative to the head), angles in radians.
struct Eye {
    pose::Quat q;
    pose::Vec3 p;
    float angleLeft, angleRight, angleUp, angleDown;
};

enum Flag : unsigned {
    kSwapped = 1u << 0,        // the second eye is not to the right of the first
    kIpdOutOfRange = 1u << 1,  // eye distance outside 45..80 mm
    kVertical = 1u << 2,       // eyes at different heights (> 2 mm)
    kDepth = 1u << 3,          // eyes at different depths (> 2 mm)
    kCanted = 1u << 4,         // the eyes look in different directions (> 0.5 deg)
    kRotated = 1u << 5,        // an eye does not look along the head's forward (> 0.5 deg)
    kFovInvalid = 1u << 6,     // a frustum is empty, inverted, not finite or >= 90 deg on a side
};
constexpr unsigned kFlagCount = 7;

inline const char* FlagText(unsigned bit) {
    switch (bit) {
    case kSwapped: return "the right eye is not to the right of the left eye (eyes swapped)";
    case kIpdOutOfRange: return "eye distance outside 45..80 mm";
    case kVertical: return "the eyes are at different heights";
    case kDepth: return "the eyes are at different depths";
    case kCanted: return "the eyes look in different directions (canted views)";
    case kRotated: return "an eye does not look straight ahead of the head";
    case kFovInvalid: return "an eye frustum is invalid";
    default: return "?";
    }
}

struct Angles {
    float yaw, pitch, roll;  // degrees; yaw + turns left, pitch + looks up, roll + counter-clockwise
};

struct Report {
    float ipdMm = 0.f, dyMm = 0.f, dzMm = 0.f;  // right eye minus left eye, head frame
    float cantDeg = 0.f;
    Angles eye[2]{};
    unsigned flags = 0;
};

inline Angles EyeAngles(pose::Quat q) {
    constexpr float kToDeg = 57.2957795f;
    if (!pose::Normalize(q)) return {0.f, 0.f, 0.f};
    const pose::Mat4 m = pose::FromPose(q, {0.f, 0.f, 0.f});
    // Row-vector matrix: row 0 = image of +X (right), row 1 = +Y (up), row 2 = +Z (back).
    const float fx = -m.m[8], fy = -m.m[9], fz = -m.m[10];
    const float pitch = std::asin(fy < -1.f ? -1.f : fy > 1.f ? 1.f : fy);
    return {std::atan2(-fx, -fz) * kToDeg, pitch * kToDeg, std::atan2(m.m[1], m.m[5]) * kToDeg};
}

inline bool FovValid(const Eye& e) {
    constexpr float kLimit = 1.5707963f;
    const float a[] = {e.angleLeft, e.angleRight, e.angleUp, e.angleDown};
    for (float v : a)
        if (!std::isfinite(v) || std::fabs(v) >= kLimit) return false;
    return e.angleLeft < e.angleRight && e.angleDown < e.angleUp;
}

inline Report Evaluate(const Eye& left, const Eye& right) {
    constexpr float kToDeg = 57.2957795f;
    Report r;
    const float dx = right.p.x - left.p.x, dy = right.p.y - left.p.y, dz = right.p.z - left.p.z;
    r.ipdMm = 1000.f * std::sqrt(dx * dx + dy * dy + dz * dz);
    r.dyMm = 1000.f * dy;
    r.dzMm = 1000.f * dz;
    pose::Quat ql = left.q, qr = right.q;
    const bool qOk = pose::Normalize(ql) && pose::Normalize(qr);
    r.cantDeg = qOk ? pose::AngleBetween(ql, qr) * kToDeg : 0.f;
    r.eye[0] = EyeAngles(left.q);
    r.eye[1] = EyeAngles(right.q);
    if (!(dx > 0.f)) r.flags |= kSwapped;
    if (!(r.ipdMm >= 45.f && r.ipdMm <= 80.f)) r.flags |= kIpdOutOfRange;
    if (std::fabs(r.dyMm) > 2.f) r.flags |= kVertical;
    if (std::fabs(r.dzMm) > 2.f) r.flags |= kDepth;
    if (r.cantDeg > 0.5f) r.flags |= kCanted;
    const pose::Quat head{0.f, 0.f, 0.f, 1.f};
    if (!qOk || pose::AngleBetween(ql, head) * kToDeg > 0.5f || pose::AngleBetween(qr, head) * kToDeg > 0.5f)
        r.flags |= kRotated;
    if (!FovValid(left) || !FovValid(right)) r.flags |= kFovInvalid;
    return r;
}

// ---- 0.17.16: crop the symmetric eye image to the eye's own frustum ---------------------------
// The game renders each eye with a symmetric frustum containing the eye's own (EXP-014: some engine
// passes ignore off-centre projections). Submitting that wider symmetric frustum is valid OpenXR and
// fine on SteamVR, but over Quest Link (Meta's runtime) near objects came out doubled; another mod
// submitting an enlarged symmetric frustum showed the same on Quest 2. Now only the part of the image
// the eye's own frustum covers is copied, and the runtime gets back exactly the frustum it reported.

struct Tans {
    float left, right, up, down;  // tangents: left and down negative
};
inline Tans TansFromAngles(float angleLeft, float angleRight, float angleUp, float angleDown) {
    return {std::tan(angleLeft), std::tan(angleRight), std::tan(angleUp), std::tan(angleDown)};
}

struct UvRect {
    float u0, v0, u1, v1;  // in the rendered image, v down
};

// The part of an image rendered with frustum `rendered` that frustum `own` covers, clamped to the image.
inline UvRect CropToFov(const Tans& rendered, const Tans& own) {
    const auto clamp01 = [](float v) { return v < 0.f ? 0.f : v > 1.f ? 1.f : v; };
    const float w = rendered.right - rendered.left, h = rendered.up - rendered.down;
    if (!(w > 0.f) || !(h > 0.f)) return {0.f, 0.f, 1.f, 1.f};
    return {clamp01((own.left - rendered.left) / w), clamp01((rendered.up - own.up) / h),
            clamp01((own.right - rendered.left) / w), clamp01((rendered.up - own.down) / h)};
}

// ---- 0.23.0: Mono rendering (playbook 07.7) ------------------------------------------------------
// One picture per frame from the CENTRE of the head: the midpoint of the two eye positions with the eyes' common
// orientation (the normalised mean; parallel eyes give either eye's), rendered with a symmetric frustum. Each eye's
// own frustum is expressed in the centre's orientation (eyeFov): on parallel eyes that is the eye's own frustum; on
// canted displays (Pimax, ~5..10 deg per eye) the eye's frustum turned into the centre frame is a quadrilateral on
// the centre's image plane (a rotation maps lines to lines), and eyeFov is its exact bounding box from the four
// corner rays. The picture's symmetric frustum contains both eyeFovs; each eye is cropped out of it to its eyeFov
// (CropToFov) and submitted with the centre pose and eyeFov - a frustum the picture really has from that pose, so
// correct head rotation, no stereo depth, no parallax claimed, and no cant error (0.23.0 owner: "do the known limit").
struct FovAngles {
    float angleLeft, angleRight, angleUp, angleDown;
};
struct MonoView {
    pose::Quat q;
    pose::Vec3 p;
    float angleLeft, angleRight, angleUp, angleDown;  // the picture: symmetric
    FovAngles eyeFov[2];                              // each eye's own frustum in the centre's orientation
};
// The bounding box, in the frame `to`, of the frustum seen from the frame `from` (both orientations in the same
// space). Corner rays turned toward or behind the image plane are clamped to 89 deg.
inline FovAngles FovInFrame(const Eye& e, const pose::Quat& to) {
    const pose::Mat4 rel = pose::Mul(pose::FromPose(e.q, {0.f, 0.f, 0.f}), pose::RigidInverse(pose::FromPose(to, {0.f, 0.f, 0.f})));
    const float tl = std::tan(e.angleLeft), tr = std::tan(e.angleRight), tu = std::tan(e.angleUp), td = std::tan(e.angleDown);
    const float xs[2] = {tl, tr}, ys[2] = {td, tu};
    float l = 1e9f, r = -1e9f, d = 1e9f, u = -1e9f;
    constexpr float kMax = 57.29f;  // tan(89 deg)
    for (float x : xs)
        for (float y : ys) {
            const pose::Vec3 c = pose::TransformPoint({x, y, -1.f}, rel);  // a corner ray (row vector, no translation)
            const float depth = std::fmax(-c.z, 1e-3f);
            const float tx = std::fmin(std::fmax(c.x / depth, -kMax), kMax), ty = std::fmin(std::fmax(c.y / depth, -kMax), kMax);
            l = std::fmin(l, tx);
            r = std::fmax(r, tx);
            d = std::fmin(d, ty);
            u = std::fmax(u, ty);
        }
    return {std::atan(l), std::atan(r), std::atan(u), std::atan(d)};
}
inline MonoView MonoCentre(const Eye& left, const Eye& right) {
    MonoView m;
    pose::Quat a = left.q, b = right.q;
    if (a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w < 0.f) b = {-b.x, -b.y, -b.z, -b.w};  // same hemisphere
    m.q = {a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w};
    if (!pose::Normalize(m.q)) m.q = left.q;
    m.p = {0.5f * (left.p.x + right.p.x), 0.5f * (left.p.y + right.p.y), 0.5f * (left.p.z + right.p.z)};
    m.eyeFov[0] = FovInFrame(left, m.q);
    m.eyeFov[1] = FovInFrame(right, m.q);
    float h = 0.f, v = 0.f;
    for (const FovAngles& f : m.eyeFov) {
        h = std::fmax(h, std::fmax(-f.angleLeft, f.angleRight));
        v = std::fmax(v, std::fmax(f.angleUp, -f.angleDown));
    }
    m.angleLeft = -h;
    m.angleRight = h;
    m.angleUp = v;
    m.angleDown = -v;
    return m;
}

// Right-eye camera position in the left eye's view space (metres), from the two world-to-view
// matrices the game renders with. Parallel eyes give about (+IPD, 0, 0).
inline pose::Vec3 RightEyeInLeftView(const pose::Mat4& viewLeft, const pose::Mat4& viewRight) {
    const pose::Mat4 camRight = pose::RigidInverse(viewRight);
    return pose::TransformPoint({camRight.m[12], camRight.m[13], camRight.m[14]}, viewLeft);
}

// ---- 0.29.0: AFR stale-eye turn (playbook 06.3a, GoW v0.2.9 D-310) -------------------------------
// In AFR the eye not rendered this frame used to be submitted with its old picture AND its old pose. SteamVR reprojects
// each view by its own pose; Meta-family runtimes (Quest Link, Virtual Desktop / VDXR) reproject from the frame's pose
// and ignore per-view poses, so the stale eye showed the head where it was a frame ago - a constant shake as the
// stale eye swaps every frame. Fix: turn the old picture into this frame's pose and frustum (rotation only; one frame's
// head translation is ignored) and submit it with this frame's pose, so both views are one moment on any runtime.
//
// Row-vector convention (posemath.h): a direction d in the NEW eye frame is d_local = d * Rnew; in the OLD eye frame
// d_old = d_local * Rold^T. The warp matrix is M = Rnew * Rold^T (3x3, rows), d_old = d * M.
struct Warp {
    float m[9];     // rows of M
    Tans now, was;  // this frame's frustum (the output) and the old picture's frustum (the input)
};
inline Warp MakeWarp(const pose::Quat& was, const pose::Quat& now, const Tans& wasT, const Tans& nowT) {
    const pose::Mat4 rn = pose::FromPose(now, {0.f, 0.f, 0.f}), ro = pose::FromPose(was, {0.f, 0.f, 0.f});
    Warp w{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            float s = 0.f;
            for (int k = 0; k < 3; ++k) s += rn.m[i * 4 + k] * ro.m[j * 4 + k];  // (Rnew * Rold^T)[i][j]
            w.m[i * 3 + j] = s;
        }
    w.now = nowT;
    w.was = wasT;
    return w;
}
// The old picture's uv (v down) seen through output pixel (u, v) of this frame's frustum; false when the direction is
// behind the old eye. The pixel shader (xr.cpp kWarpHlsl) does the same per pixel; the result may lie outside 0..1
// (the head turned past the old picture's edge: the sampler clamps to the edge).
inline bool WarpUv(const Warp& w, float u, float v, float* ou, float* ov) {
    const float tx = w.now.left + u * (w.now.right - w.now.left);
    const float ty = w.now.up + v * (w.now.down - w.now.up);
    const float d[3] = {tx, ty, -1.f};
    float o[3];
    for (int j = 0; j < 3; ++j) o[j] = d[0] * w.m[j] + d[1] * w.m[3 + j] + d[2] * w.m[6 + j];
    if (!(o[2] < -1e-6f)) return false;
    const float px = o[0] / -o[2], py = o[1] / -o[2];
    *ou = (px - w.was.left) / (w.was.right - w.was.left);
    *ov = (w.was.up - py) / (w.was.up - w.was.down);
    return std::isfinite(*ou) && std::isfinite(*ov);
}
// hrvr.ini afr_reproject: 2 = every runtime except SteamVR (default; SteamVR honours per-view poses), 1 = always,
// 0 = never (the old behaviour).
inline bool AfrReprojectOn(int setting, const char* runtimeName) {
    if (setting <= 0) return false;
    if (setting == 1) return true;
    if (!runtimeName) return true;
    for (const char* s = runtimeName; *s; ++s) {
        const char* a = s;
        const char* b = "steamvr";
        while (*a && *b && (*a | 0x20) == *b) ++a, ++b;
        if (!*b) return false;
    }
    return true;
}

}  // namespace hrvr::xrgeom
