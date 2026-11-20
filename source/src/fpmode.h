#pragma once
// Pure first-person camera logic (ported from the B2S VR mod's fpmode.h, MIT). No engine types:
// desk-tested in tests/fp_tests.cpp. Engine convention: row-major, row-vector, right-handed, Y up
// (Havok gravity (0,-9.81,0), STATIC), cameras look down -Z.
#include <cmath>
#include <cstdint>

#include "posemath.h"

namespace hrvr::fp {

enum class BehaviourClass { Unknown, Gameplay, Cutscene, NativeFirstPerson };
enum class Mode { Cinematic, FirstPerson, NativeFirstPerson };

// F1 enables first person while the player walks the character, F3 in cutscenes; the game's own
// first person (CAMERA_BEHAVIOR_FIRST_PERSON) is never overridden; anything unrecognised stays cinematic.
inline Mode Classify(BehaviourClass c, bool f1, bool f3) {
    switch (c) {
        case BehaviourClass::NativeFirstPerson: return Mode::NativeFirstPerson;
        case BehaviourClass::Gameplay: return f1 ? Mode::FirstPerson : Mode::Cinematic;
        case BehaviourClass::Cutscene: return f3 ? Mode::FirstPerson : Mode::Cinematic;
        default: return Mode::Cinematic;
    }
}

// Moves angle `from` towards `to` by fraction t along the shorter arc.
inline float ApproachAngle(float from, float to, float t) {
    float d = std::remainder(to - from, 6.28318531f);
    return std::remainder(from + d * t, 6.28318531f);
}

// Heading of a world matrix: 0 looking down -Z, positive turning left; pitch and roll ignored.
inline float YawOf(const pose::Mat4& m) {
    const float fx = -m.m[8], fz = -m.m[10];  // row 2 = local +Z axis; forward is -Z
    return std::atan2(-fx, -fz);
}

// Heading of a world direction (0 = -Z, positive turning left).
inline float YawOfDir(float x, float z) { return std::atan2(-x, -z); }

// Camera base for first person: position = head + (eyeFwd forward, eyeUp up) in the view-yaw
// frame; orientation = yaw only, the HMD supplies all head rotation (playbook CAM-001).
inline pose::Mat4 FirstPersonBase(const pose::Mat4& head, float viewYaw, float eyeFwd, float eyeUp) {
    const pose::Quat yq = pose::YawQuat(viewYaw);
    const pose::Mat4 rot = pose::FromPose(yq, {0, 0, 0});
    const pose::Vec3 off = pose::TransformPoint({0.f, eyeUp, -eyeFwd}, rot);
    return pose::FromPose(yq, {head.m[12] + off.x, head.m[13] + off.y, head.m[14] + off.z});
}

// The same base pitched about the eye: Pitch * Yaw at the eye position (F3 follows the head's pitch).
inline pose::Mat4 PitchedBase(const pose::Mat4& base, float viewYaw, float pitch) {
    const pose::Vec3 eye{base.m[12], base.m[13], base.m[14]};
    const pose::Quat pq{std::sin(pitch * 0.5f), 0.f, 0.f, std::cos(pitch * 0.5f)};
    return pose::Mul(pose::FromPose(pq, {0, 0, 0}), pose::FromPose(pose::YawQuat(viewYaw), eye));
}

// Rejects non-finite positions and teleport-sized jumps between frames.
inline bool PlausibleStep(const pose::Vec3& prev, const pose::Vec3& next, float maxMetres) {
    if (!std::isfinite(next.x) || !std::isfinite(next.y) || !std::isfinite(next.z)) return false;
    const float dx = next.x - prev.x, dy = next.y - prev.y, dz = next.z - prev.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz) <= maxMetres;
}

// F3 facing from the face (Heavy Rain rigs). The HumanIK rig names "Head" and the face bone "MNoseUp"
// (camera target alias table 0x1415ba240, STATIC). Head -> nose is a fixed direction of the skull that
// points out of the face, so unlike B2S's head -> eye midpoint it has no front/back ambiguity. Yaw is its
// heading, `elevation` its angle above the horizontal (the rig's "level" value is learned separately).
// False when the vector is too short, or too steep to carry a heading (looking almost straight down/up).
inline bool FaceDirection(pose::Vec3 head, pose::Vec3 nose, float* yaw, float* elevation) {
    const float x = nose.x - head.x, y = nose.y - head.y, z = nose.z - head.z;
    const float flat = std::sqrt(x * x + z * z), len = std::sqrt(x * x + y * y + z * z);
    if (!std::isfinite(len) || len < 0.02f || len > 0.5f || flat < 0.2f * len) return false;
    *yaw = YawOfDir(x, z);
    *elevation = std::atan2(y, flat);
    return true;
}

// A character the game is not animating stands in its bind pose: a T-pose, both arms straight out
// at shoulder height (B2S owner 0.16.11). True when each hand is level with its shoulder (within
// 15 cm) and at least 45 cm out from it, and the hands are at least 1.2 m apart.
inline bool LooksLikeTPose(pose::Vec3 shoulderL, pose::Vec3 handL, pose::Vec3 shoulderR, pose::Vec3 handR) {
    auto out = [](pose::Vec3 s, pose::Vec3 h) {
        const float dx = h.x - s.x, dz = h.z - s.z;
        return std::fabs(h.y - s.y) < 0.15f && dx * dx + dz * dz > 0.45f * 0.45f;
    };
    const float sx = handL.x - handR.x, sy = handL.y - handR.y, sz = handL.z - handR.z;
    return out(shoulderL, handL) && out(shoulderR, handR) && sx * sx + sy * sy + sz * sz > 1.2f * 1.2f;
}

// "At the character's head" for prompts: a column, not a sphere (B2S 7A: a 0.6 m sphere missed a name
// floating above the head). Within 0.5 m horizontally, from 0.7 m below to 1.5 m above the head bone.
inline bool InHeadColumn(pose::Vec3 head, pose::Vec3 p) {
    const float dx = p.x - head.x, dz = p.z - head.z, dy = p.y - head.y;
    return dx * dx + dz * dz <= 0.25f && dy >= -0.7f && dy <= 1.5f;
}

// World-locked placement in front of the view (B2S 7B): placed once, then kept fixed in the world, and
// placed again when it is new (unseen for 0.5 s), when the gaze turns more than ~35 deg away from it
// (dot < 0.82), or when it ends up nearer than 0.30 m / further than 1.6x the wanted distance.
struct WorldLock {
    pose::Vec3 pos{};
    bool placed = false;
    uint64_t lastMs = 0;
    // eye / gaze (unit) of the view, wanted distance, drop below the gaze; returns the locked point.
    pose::Vec3 Update(pose::Vec3 eye, pose::Vec3 gaze, float dist, float drop, uint64_t nowMs, bool* replaced) {
        bool again = !placed || nowMs - lastMs > 500;
        if (!again) {
            const float dx = pos.x - eye.x, dy = pos.y - eye.y, dz = pos.z - eye.z;
            const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
            const float dot = d > 1e-4f ? (dx * gaze.x + dy * gaze.y + dz * gaze.z) / d : 1.f;
            again = dot < 0.82f || d < 0.30f || d > dist * 1.6f;
        }
        if (again) pos = {eye.x + gaze.x * dist, eye.y + gaze.y * dist - drop, eye.z + gaze.z * dist};
        placed = true;
        lastMs = nowMs;
        if (replaced) *replaced = again;
        return pos;
    }
};

// Snap turning for F1 (B2S, playbook ch03): a push past `fire` turns one step, holding it repeats
// every `repeat` seconds, and the stick must come back inside `rearm` before a new push counts.
struct SnapTurn {
    bool armed = true;
    float held = 0.f;
    // -1, 0 or +1 steps, with the sign of the stick.
    int Step(float x, float dt, float fire = 0.7f, float rearm = 0.35f, float repeat = 0.33f) {
        const float a = std::fabs(x);
        if (a < rearm) {
            armed = true;
            held = 0.f;
            return 0;
        }
        if (a < fire) {
            held = 0.f;
            return 0;
        }
        const int dir = x > 0.f ? 1 : -1;
        if (armed) {
            armed = false;
            held = 0.f;
            return dir;
        }
        held += dt;
        if (held < repeat) return 0;
        held -= repeat;
        return dir;
    }
    // The stick belongs to something else (prompts): it has to be centred before the next step.
    void Block() {
        armed = false;
        held = 0.f;
    }
};

// Which way a character's root faces relative to its face (0 or pi), by votes (fpcam 0.7.0): only clear frames vote
// (face and root within 30 deg of agreeing or of opposing); 40 votes with a 4:1 majority lock the answer.
struct FrontVote {
    int same = 0, opposite = 0;
    bool locked = false;
    float front = 0.f;
    float Majority() const { return opposite > same ? 3.14159265f : 0.f; }
    // diff = face yaw - root yaw (radians, -pi..pi). Returns true when this vote locks the answer.
    bool Feed(float diff) {
        if (locked || !std::isfinite(diff)) return false;
        const float a = std::fabs(diff);
        if (a < 0.52f) ++same;
        else if (a > 3.14159265f - 0.52f) ++opposite;
        else return false;
        const int total = same + opposite;
        if (total >= 40 && (same >= 4 * opposite || opposite >= 4 * same)) {
            locked = true;
            front = Majority();
            return true;
        }
        return false;
    }
};

}  // namespace hrvr::fp
