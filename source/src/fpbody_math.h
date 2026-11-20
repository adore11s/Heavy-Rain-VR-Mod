#pragma once
// Pure math for the render-only body edits (fpbody.cpp). Row-vector convention as the engine:
// world = local * parentWorld, a matrix's rows 0..2 are its axes in the world, row 3 its position.
// Desk-tested in tests/fpbody_tests.cpp.
#include <cctype>
#include <cmath>
#include <cstring>

#include "fp_math.h"

namespace hrvr::fpbody {
using pose::Mat4;
using pose::Vec3;

inline Vec3 Pos(const Mat4& m) { return {m.m[12], m.m[13], m.m[14]}; }
inline Mat4 Translation(Vec3 t) {
    Mat4 m = pose::Identity();
    m.m[12] = t.x;
    m.m[13] = t.y;
    m.m[14] = t.z;
    return m;
}

// Inverse of an affine matrix (rotation may carry scale/shear from animation blending; B2S: a rigid inverse
// of such a matrix drifts). False when singular.
inline bool AffineInverse(const Mat4& a, Mat4* out) {
    const float* m = a.m;
    const float c00 = m[5] * m[10] - m[6] * m[9], c01 = m[6] * m[8] - m[4] * m[10], c02 = m[4] * m[9] - m[5] * m[8];
    const float det = m[0] * c00 + m[1] * c01 + m[2] * c02;
    if (!std::isfinite(det) || std::fabs(det) < 1e-12f) return false;
    const float id = 1.f / det;
    Mat4 r = pose::Identity();
    r.m[0] = c00 * id;
    r.m[1] = (m[2] * m[9] - m[1] * m[10]) * id;
    r.m[2] = (m[1] * m[6] - m[2] * m[5]) * id;
    r.m[4] = c01 * id;
    r.m[5] = (m[0] * m[10] - m[2] * m[8]) * id;
    r.m[6] = (m[2] * m[4] - m[0] * m[6]) * id;
    r.m[8] = c02 * id;
    r.m[9] = (m[1] * m[8] - m[0] * m[9]) * id;
    r.m[10] = (m[0] * m[5] - m[1] * m[4]) * id;
    const Vec3 t = Pos(a);
    r.m[12] = -(t.x * r.m[0] + t.y * r.m[4] + t.z * r.m[8]);
    r.m[13] = -(t.x * r.m[1] + t.y * r.m[5] + t.z * r.m[9]);
    r.m[14] = -(t.x * r.m[2] + t.y * r.m[6] + t.z * r.m[10]);
    *out = r;
    return true;
}

// A rotation (no translation) about pivot c as a world transform: X' = X * T(-c) * R * T(c).
inline Mat4 AboutPivot(const Mat4& rotation, Vec3 c) {
    return pose::Mul(pose::Mul(Translation({-c.x, -c.y, -c.z}), rotation), Translation(c));
}

// The rotation taking direction a onto direction b (shortest arc), row-vector: v * R.
inline Mat4 RotationBetween(Vec3 a, Vec3 b) {
    a = fp::Norm(a);
    b = fp::Norm(b);
    const Vec3 axis = fp::Cross(a, b);
    const float s = fp::Len(axis), c = fp::Dot(a, b);
    if (s < 1e-6f) {
        if (c > 0.f) return pose::Identity();
        // Opposite: half a turn about any axis perpendicular to a.
        Vec3 p = std::fabs(a.x) < 0.9f ? fp::Cross(a, {1, 0, 0}) : fp::Cross(a, {0, 1, 0});
        p = fp::Norm(p);
        Mat4 r = pose::Identity();
        const float k[3] = {p.x, p.y, p.z};
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) r.m[i * 4 + j] = 2.f * k[i] * k[j] - (i == j ? 1.f : 0.f);
        return r;
    }
    const Vec3 n = fp::Mul(axis, 1.f / s);
    const float ang = std::atan2(s, c);
    // Rodrigues (column form R v), transposed for row vectors.
    const float x = n.x, y = n.y, z = n.z, cs = std::cos(ang), sn = std::sin(ang), t = 1.f - cs;
    const float R[3][3] = {{t * x * x + cs, t * x * y - sn * z, t * x * z + sn * y},
                           {t * x * y + sn * z, t * y * y + cs, t * y * z - sn * x},
                           {t * x * z - sn * y, t * y * z + sn * x, t * z * z + cs}};
    Mat4 r = pose::Identity();
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r.m[i * 4 + j] = R[j][i];
    return r;
}

// Shrinks a bone to (almost) nothing at a point: the skinned vertices it carries collapse there (B2S 6.2).
inline Mat4 Collapsed(const Mat4& m, Vec3 at, float scale = 1e-4f) {
    Mat4 r = m;
    for (int row = 0; row < 3; ++row)
        for (int k = 0; k < 3; ++k) r.m[row * 4 + k] *= scale;
    r.m[12] = at.x;
    r.m[13] = at.y;
    r.m[14] = at.z;
    return r;
}

// One arm re-posed so the wrist reaches `wrist` (B2S 6.3): the upper arm turns about the shoulder so its
// elbow lands on the solved elbow, the forearm then turns about that elbow so the wrist lands on target.
// Returns the two world deltas (apply to the segment and everything riding on it: X' = X * delta).
struct ArmSolve {
    bool valid = false, clamped = false;
    Mat4 upper = pose::Identity();    // for the upper arm (and its helpers)
    Mat4 forearm = pose::Identity();  // for the forearm (and its helpers), includes `upper`
    Vec3 elbow{}, wrist{};
};
inline ArmSolve SolveArm(Vec3 shoulder, Vec3 elbow, Vec3 wrist, Vec3 target, Vec3 pole) {
    ArmSolve s;
    const float upper = fp::Len(fp::Sub(elbow, shoulder)), lower = fp::Len(fp::Sub(wrist, elbow));
    const fp::TwoBoneResult ik = fp::TwoBone(shoulder, target, upper, lower, pole);
    if (!ik.valid) return s;
    s.upper = AboutPivot(RotationBetween(fp::Sub(elbow, shoulder), fp::Sub(ik.elbow, shoulder)), shoulder);
    const Vec3 wrist1 = pose::TransformPoint(wrist, s.upper);
    const Mat4 turn = AboutPivot(RotationBetween(fp::Sub(wrist1, ik.elbow), fp::Sub(ik.wrist, ik.elbow)), ik.elbow);
    s.forearm = pose::Mul(s.upper, turn);
    s.elbow = ik.elbow;
    s.wrist = ik.wrist;
    s.clamped = ik.targetClamped;
    s.valid = true;
    return s;
}

// Running min/max of a point (per axis) to tell rigid attachments from moving ones.
struct Range {
    Vec3 lo{1e9f, 1e9f, 1e9f}, hi{-1e9f, -1e9f, -1e9f};
    int n = 0;
    void Add(Vec3 p) {
        lo = {std::fmin(lo.x, p.x), std::fmin(lo.y, p.y), std::fmin(lo.z, p.z)};
        hi = {std::fmax(hi.x, p.x), std::fmax(hi.y, p.y), std::fmax(hi.z, p.z)};
        ++n;
    }
    float Extent() const { return n ? std::fmax(hi.x - lo.x, std::fmax(hi.y - lo.y, hi.z - lo.z)) : 1e9f; }
};

// A point in a bone's frame (inverse of the bone's world applied to it).
inline Vec3 InFrame(const Mat4& boneInverse, Vec3 p) { return pose::TransformPoint(p, boneInverse); }

// Drawn-hand filter (playbook 08.1, GoW SmoothHand): an adaptive one-pole filter on the 12 affine components,
// in tracking space, with the cutoff rising with speed; then re-orthonormalised keeping each axis' length.
// Only the drawn hands use it; the game's input stays raw.
struct SmoothHand {
    Mat4 f = pose::Identity();
    bool have = false;
    Mat4 Feed(const Mat4& raw, float dt, float cutoffHz = 3.f, float beta = 6.f) {
        if (!have || !(dt > 0.f) || dt > 0.1f) {
            f = raw;
            have = true;
            return f;
        }
        float rot = 0.f;
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) rot += (raw.m[r * 4 + c] - f.m[r * 4 + c]) * (raw.m[r * 4 + c] - f.m[r * 4 + c]);
        rot = std::sqrt(0.5f * rot);
        const float speed = (fp::Len(fp::Sub(Pos(raw), Pos(f))) + 0.15f * rot) / dt;
        const float cutoff = cutoffHz + beta * speed;
        const float tau = 1.f / (6.2831853f * cutoff), alpha = 1.f / (1.f + tau / dt);
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 3; ++c) f.m[r * 4 + c] += alpha * (raw.m[r * 4 + c] - f.m[r * 4 + c]);
        // Gram-Schmidt, keeping the raw pose's axis lengths (a blend of unit axes is shorter than either).
        Vec3 x{f.m[0], f.m[1], f.m[2]}, y{f.m[4], f.m[5], f.m[6]};
        const float lx = fp::Len({raw.m[0], raw.m[1], raw.m[2]}), ly = fp::Len({raw.m[4], raw.m[5], raw.m[6]}),
                    lz = fp::Len({raw.m[8], raw.m[9], raw.m[10]});
        Vec3 z;
        x = fp::Norm(x);
        y = fp::Norm(fp::Sub(y, fp::Mul(x, fp::Dot(x, y))));
        z = fp::Cross(x, y);
        if (fp::Dot(z, Vec3{raw.m[8], raw.m[9], raw.m[10]}) < 0.f) z = fp::Mul(z, -1.f);
        const Vec3 rows[3] = {fp::Mul(x, lx), fp::Mul(y, ly), fp::Mul(z, lz)};
        for (int r = 0; r < 3; ++r) {
            f.m[r * 4] = rows[r].x;
            f.m[r * 4 + 1] = rows[r].y;
            f.m[r * 4 + 2] = rows[r].z;
        }
        return f;
    }
};

// The rotation part with each axis normalised (bones may carry scale).
inline Mat4 UnitRotation(const Mat4& m) {
    Mat4 r = pose::Identity();
    for (int k = 0; k < 3; ++k) {
        const Vec3 a = fp::Norm({m.m[k * 4], m.m[k * 4 + 1], m.m[k * 4 + 2]});
        r.m[k * 4] = a.x;
        r.m[k * 4 + 1] = a.y;
        r.m[k * 4 + 2] = a.z;
    }
    return r;
}

// Rotation by `angle` about the world direction `axis` (row-vector: v * R).
inline Mat4 AxisAngle(Vec3 axis, float angle) {
    axis = fp::Norm(axis);
    const float s = std::sin(angle * 0.5f);
    return pose::FromPose({axis.x * s, axis.y * s, axis.z * s, std::cos(angle * 0.5f)}, {0, 0, 0});
}

// A world rotation R split as R = Twist(about axis) * Swing (the swing takes `axis` to axis * R).
// Returns the twist angle (signed, about axis) and the swing angle and axis.
struct SwingTwist {
    float twist = 0.f, swing = 0.f;
    Vec3 swingAxis{0, 1, 0};
};
inline SwingTwist SplitSwingTwist(const Mat4& rotation, Vec3 axis) {
    SwingTwist out;
    axis = fp::Norm(axis);
    const Vec3 moved = fp::Norm(pose::TransformPoint(axis, rotation));
    out.swing = std::acos(std::fmax(-1.f, std::fmin(1.f, fp::Dot(axis, moved))));
    const Vec3 sa = fp::Cross(axis, moved);
    if (fp::Len(sa) > 1e-6f) out.swingAxis = fp::Norm(sa);
    const Mat4 twist = pose::Mul(rotation, pose::RigidInverse(RotationBetween(axis, moved)));
    // Any direction across the axis shows the twist angle.
    Vec3 u = fp::Cross(axis, std::fabs(axis.y) < 0.9f ? Vec3{0, 1, 0} : Vec3{1, 0, 0});
    u = fp::Norm(u);
    const Vec3 tu = pose::TransformPoint(u, twist);
    out.twist = std::atan2(fp::Dot(fp::Cross(u, tu), axis), fp::Dot(u, tu));
    return out;
}

// A wrist that bends and rolls like one (owner 2026-10-27: "I can rotate them like a balloon"). `neutral` is the hand
// rotation the animation's wrist would give on the solved forearm, `wanted` the controller's; both unit rotations,
// `axis` the forearm direction (elbow -> wrist). The forearm takes `rollShare` of the roll (B2S: half, at most 50 deg)
// so the wrist skin turns with the hand; the hand keeps at most maxTwist of roll and maxSwing of bend beyond that.
struct WristResult {
    Mat4 hand = pose::Identity();  // the limited hand rotation
    float forearmRoll = 0.f;       // radians about `axis` for the forearm (and what rides on it)
    float twist = 0.f;             // the whole roll the hand gets about `axis` (forearmRoll included)
    bool limited = false;
};
inline WristResult LimitWrist(const Mat4& neutral, const Mat4& wanted, Vec3 axis, float maxSwing = 1.22f,
                              float maxTwist = 1.05f, float rollShare = 0.5f, float maxRoll = 0.873f) {
    WristResult r;
    axis = fp::Norm(axis);
    const Mat4 delta = pose::Mul(pose::RigidInverse(neutral), wanted);  // wanted = neutral * delta (world frame)
    const SwingTwist st = SplitSwingTwist(delta, axis);
    r.forearmRoll = std::fmax(-maxRoll, std::fmin(maxRoll, st.twist * rollShare));
    const float twist = r.forearmRoll + std::fmax(-maxTwist, std::fmin(maxTwist, st.twist - r.forearmRoll));
    const float swing = std::fmin(st.swing, maxSwing);
    r.twist = twist;
    r.limited = std::fabs(twist - st.twist) > 1e-3f || swing < st.swing - 1e-3f;
    r.hand = pose::Mul(pose::Mul(neutral, AxisAngle(axis, twist)), AxisAngle(st.swingAxis, swing));
    return r;
}

// Row-wise blend of two affine bone matrices, re-orthonormalised with each row's blended length (B2S idle hold).
inline Mat4 BlendAffine(const Mat4& a, const Mat4& b, float t) {
    Mat4 m = a;
    Vec3 row[3];
    float len[3];
    for (int k = 0; k < 3; ++k) {
        const Vec3 ra{a.m[k * 4], a.m[k * 4 + 1], a.m[k * 4 + 2]}, rb{b.m[k * 4], b.m[k * 4 + 1], b.m[k * 4 + 2]};
        row[k] = fp::Add(fp::Mul(ra, 1.f - t), fp::Mul(rb, t));
        len[k] = fp::Len(ra) * (1.f - t) + fp::Len(rb) * t;
    }
    const Vec3 x = fp::Norm(row[0]);
    const Vec3 y = fp::Norm(fp::Sub(row[1], fp::Mul(x, fp::Dot(row[1], x))));
    Vec3 z = fp::Cross(x, y);
    if (fp::Dot(z, row[2]) < 0.f) z = fp::Mul(z, -1.f);
    if (!(fp::Len(x) > 0.5f) || !(fp::Len(y) > 0.5f)) return t < 0.5f ? a : b;
    const Vec3 axes[3] = {x, y, z};
    for (int k = 0; k < 3; ++k) {
        m.m[k * 4] = axes[k].x * len[k];
        m.m[k * 4 + 1] = axes[k].y * len[k];
        m.m[k * 4 + 2] = axes[k].z * len[k];
    }
    const Vec3 p = fp::Add(fp::Mul(Pos(a), 1.f - t), fp::Mul(Pos(b), t));
    m.m[12] = p.x;
    m.m[13] = p.y;
    m.m[14] = p.z;
    return m;
}

// Idle hold (B2S fpbody IdleHold, owner 2026-10-27: "Jason moves his body too much" standing in F1): while the
// character stands still the drawn body keeps a snapshot of its pose relative to the root (the character node)
// instead of the idle animation. Standing = the root within 3 cm (10 cm with the stick released) for 0.4 s; a real
// movement (the root 8 cm away with the stick pushed, 40 cm without; bone 0 or the head 25 cm from the snapshot:
// sitting, kneeling, bending) releases it at once. 0.15 s blends both ways. Pure state machine: the caller keeps
// the snapshot bones.
struct IdleHoldState {
    bool holding = false, anchored = false;
    float blend = 0.f, still = 0.f;
    Vec3 anchor{};
    // Returns true when a new snapshot is to be taken this frame.
    bool Update(Vec3 root, bool moveStick, float coreOffset, float dt) {
        auto flat = [](Vec3 a, Vec3 b) { return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.z - b.z) * (a.z - b.z)); };
        bool take = false;
        if (holding) {
            if (flat(root, anchor) > (moveStick ? 0.08f : 0.40f) || coreOffset > 0.25f) {
                holding = false;
                anchored = false;
                still = 0.f;
            }
        } else {
            if (!anchored || flat(root, anchor) > (moveStick ? 0.03f : 0.10f)) {
                anchor = root;
                anchored = true;
                still = 0.f;
            } else {
                still += dt;
            }
            if (still >= 0.4f) {
                holding = true;
                take = true;
            }
        }
        const float target = holding ? 1.f : 0.f;
        blend += (target - blend) * (1.f - std::exp(-dt / 0.15f));
        if (!holding && blend < 0.01f) blend = 0.f;
        return take;
    }
};

// Headset family from the OpenXR system name (playbook 08.3): 0 other, 1 PlayStation VR2, 2 Quest / Meta.
inline int HeadsetFamily(const char* systemName) {
    char low[128] = {};
    for (int i = 0; systemName && systemName[i] && i < 127; ++i) low[i] = char(std::tolower(static_cast<unsigned char>(systemName[i])));
    for (const char* k : {"playstation", "psvr", "sony", "vr2"})
        if (std::strstr(low, k)) return 1;
    for (const char* k : {"quest", "oculus", "meta"})
        if (std::strstr(low, k)) return 2;
    return 0;
}

}  // namespace hrvr::fpbody
