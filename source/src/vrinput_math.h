#pragma once

#include <cmath>
#include <cstdint>
#include "xr.h"

namespace hrvr::vrinput {

struct Sample {
    bool focused = false;
    bool fresh = false;
    float headYaw = 0.f;
    float leftX = 0.f, leftY = 0.f;
    float rightX = 0.f, rightY = 0.f;
    float leftTrigger = 0.f, rightTrigger = 0.f;
    bool leftA = false, leftB = false, rightA = false, rightB = false;
    bool leftMenu = false, rightMenu = false;
    bool leftGrip = false, rightGrip = false;
    bool leftSqueeze = false, rightSqueeze = false;
    bool leftStickClick = false, rightStickClick = false;
};

struct LegacyState {
    float moveX = 0.f, moveY = 0.f;
    float viewX = 0.f, viewY = 0.f;
    float leftTrigger = 0.f, rightTrigger = 0.f;
    uint32_t buttons = 0;
};

constexpr uint32_t kLeftA = 1u << 0;
constexpr uint32_t kLeftB = 1u << 1;
constexpr uint32_t kRightA = 1u << 2;
constexpr uint32_t kRightB = 1u << 3;
constexpr uint32_t kLeftMenu = 1u << 4;
constexpr uint32_t kRightMenu = 1u << 5;
constexpr uint32_t kLeftGrip = 1u << 6;
constexpr uint32_t kRightGrip = 1u << 7;
constexpr uint32_t kLeftSqueeze = 1u << 8;
constexpr uint32_t kRightSqueeze = 1u << 9;
constexpr uint32_t kLeftStickClick = 1u << 10;
constexpr uint32_t kRightStickClick = 1u << 11;
constexpr uint32_t kLeftTriggerButton = 1u << 12;
constexpr uint32_t kRightTriggerButton = 1u << 13;
constexpr uint32_t kDpadUp = 1u << 14;
constexpr uint32_t kDpadDown = 1u << 15;
constexpr uint32_t kDpadLeft = 1u << 16;
constexpr uint32_t kDpadRight = 1u << 17;

inline float Clamp(float v, float lo = -1.f, float hi = 1.f) {
    if (!std::isfinite(v)) return 0.f;
    return v < lo ? lo : (v > hi ? hi : v);
}

// Converts the left stick from the seated reference into game-world movement.
// X is right and Y is forward; headYaw rotates the local vector into world space.
inline void HeadRelativeMove(float localX, float localY, float headYaw,
                             float& worldX, float& worldZ) {
    const float c = std::cos(headYaw), s = std::sin(headYaw);
    worldX = localX * c - localY * s;
    worldZ = localX * s + localY * c;
}

// The right stick always reaches the game: QTE prompts are mostly
// right-stick gestures (owner, 2026-10-08: prompts could not be done in F1/F3 while the right
// stick was kept back for the view). In first-person gameplay it also turns the view (fpcam);
// the left stick is rotated to where the player looks.
inline LegacyState Map(const Sample& in, bool gameplay) {
    LegacyState out{};
    if (!in.focused || !in.fresh || !std::isfinite(in.headYaw)) return out;
    if (gameplay) {
        HeadRelativeMove(Clamp(in.leftX), Clamp(in.leftY), in.headYaw,
                         out.moveX, out.moveY);
    } else {
        out.moveX = Clamp(in.leftX);
        out.moveY = Clamp(in.leftY);
    }
    out.viewX = Clamp(in.rightX);
    out.viewY = Clamp(in.rightY);
    out.leftTrigger = Clamp(in.leftTrigger, 0.f, 1.f);
    out.rightTrigger = Clamp(in.rightTrigger, 0.f, 1.f);
    if (in.leftA) out.buttons |= kLeftA;
    if (in.leftB) out.buttons |= kLeftB;
    if (in.rightA) out.buttons |= kRightA;
    if (in.rightB) out.buttons |= kRightB;
    if (in.leftMenu) out.buttons |= kLeftMenu;
    if (in.rightMenu) out.buttons |= kRightMenu;
    if (in.leftGrip) out.buttons |= kLeftGrip;
    if (in.rightGrip) out.buttons |= kRightGrip;
    if (in.leftSqueeze) out.buttons |= kLeftSqueeze;
    if (in.rightSqueeze) out.buttons |= kRightSqueeze;
    if (in.leftStickClick) out.buttons |= kLeftStickClick;
    if (in.rightStickClick) out.buttons |= kRightStickClick;
    if (out.leftTrigger >= .5f) out.buttons |= kLeftTriggerButton;
    if (out.rightTrigger >= .5f) out.buttons |= kRightTriggerButton;
    // Left grip is the explicit D-pad modifier; locomotion alone never
    // generates menu directions or double-drives the game's input lane.
    constexpr float kDpadThreshold = .5f;
    if ((out.buttons & kLeftSqueeze) != 0) {
        if (out.moveY > kDpadThreshold) out.buttons |= kDpadUp;
        if (out.moveY < -kDpadThreshold) out.buttons |= kDpadDown;
        if (out.moveX < -kDpadThreshold) out.buttons |= kDpadLeft;
        if (out.moveX > kDpadThreshold) out.buttons |= kDpadRight;
    }
    return out;
}

// First person (fpcam, F1 walking): the left stick is turned from the headset's heading into the
// game's movement reference (yaw = headset world yaw - reference yaw), so forward walks where you look.
// Heavy Rain walks while R2 is held (THIRD_PERSON_GAMEPLAY steers with the stick); autoWalk holds R2
// for the player while the stick is pushed. A held left grip is the D-pad modifier: the stick stays raw.
struct FirstPersonMove {
    bool headRelative = false;
    float yaw = 0.f;
    bool autoWalk = false;
};
inline LegacyState MapFirstPerson(Sample in, const FirstPersonMove& fp) {
    const bool turn = fp.headRelative && std::isfinite(fp.yaw) && !in.leftSqueeze;
    if (turn) in.headYaw = fp.yaw;
    LegacyState out = Map(in, turn);
    if (turn && fp.autoWalk && std::sqrt(out.moveX * out.moveX + out.moveY * out.moveY) > 0.3f) {
        out.rightTrigger = 1.f;
        out.buttons |= kRightTriggerButton;
    }
    return out;
}

// All four face buttons = the settings (GoW D-162 / D-237, owner 2026-10-27: "make 4 face buttons open settings"):
// the PS VR2 Sense and Touch-style controllers have no Options button on the right hand. Holding A B X Y together
// (cross circle square triangle) and letting go sends Start for 150 ms (the pause screen, where the VR settings
// panel is). From the moment all four are down until all four are up no face button reaches the game.
struct FaceChord {
    bool on = false;
    uint64_t pulseUntil = 0;
    // Edits the sample's face buttons; returns true while Start is to be sent.
    bool Feed(Sample& s, uint64_t now) {
        const bool all4 = s.leftA && s.leftB && s.rightA && s.rightB;
        const bool any = s.leftA || s.leftB || s.rightA || s.rightB;
        if (all4) on = true;
        if (on) {
            if (!any) {
                on = false;
                pulseUntil = now + 150;
            }
            s.leftA = s.leftB = s.rightA = s.rightB = false;
        }
        return now < pulseUntil;
    }
};

// Right hand at the head + its upper face button (B / O) switches first person (GoW D-252, owner 2026-10-27: "the
// first person shortcut doesn't work, make it like the GoW mod"): within `zone` (0.25 m) of the eyes, not more than
// 0.10 m behind the face plane, between 0.25 m below and 0.10 m above the eyes. Positions in the seated reference.
// Where the hand is against the head, in the tracking reference (metres): its distance from the eye midpoint, how far
// it is ahead of the face plane (the head's horizontal forward, so looking up or down does not move the zone) and its
// height against the eyes. valid = false when the head has no horizontal forward.
struct HeadZone {
    bool valid = false;
    float dist = 0.f, ahead = 0.f, up = 0.f;
};
inline HeadZone HeadZoneOf(const pose::Mat4& head, const pose::Mat4& grip) {
    HeadZone z;
    const float d[3] = {grip.m[12] - head.m[12], grip.m[13] - head.m[13], grip.m[14] - head.m[14]};
    float fx = -head.m[8], fz = -head.m[10];
    const float fn = std::sqrt(fx * fx + fz * fz);
    if (!(fn > 1e-3f)) return z;
    fx /= fn;
    fz /= fn;
    z.valid = true;
    z.dist = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    z.ahead = d[0] * fx + d[2] * fz;
    z.up = d[1];
    return z;
}
// GoW D-252's zone: within `zone` (0.25 m) of the eyes, not more than 0.10 m behind the face plane, 0.25 m below to
// 0.10 m above the eyes.
inline bool InHeadZone(const HeadZone& z, float zone = 0.25f) {
    return z.valid && z.dist < zone && -z.ahead < 0.10f && z.up > -0.25f && z.up < 0.10f;
}
inline bool HandAtHead(const pose::Mat4& head, const pose::Mat4& grip, float zone = 0.25f) {
    return InHeadZone(HeadZoneOf(head, grip), zone);
}
// The first-person switch (playbook 10.3, the owner's rule of revision 17 / 26.13, 0.26.0): ONE TAP of the right
// upper face button (B / O) with the right hand at the head switches at once, at any time - walking, cutscenes and
// while the game shows prompts. No hold, no prompt block. The press that switches never reaches the game until it is
// released; a press with the hand away from the head is the game's as it is. (0.14.0's 600 ms hold blocked near
// prompts is gone: the owner rejected that form in the Detroit port, "it's supposed to be a tap ... at any time".)
struct HeadSwitch {
    bool down = false, consumed = false;
    // Returns true once, on the press, when the hand is at the head; that press is kept from the game until released.
    bool Feed(Sample& s, bool atHead) {
        const bool b = s.rightB;
        bool fire = false;
        if (b && !down) fire = consumed = atHead;
        if (!b) consumed = false;
        down = b;
        if (consumed) s.rightB = false;
        return fire;
    }
};

inline Sample SampleFromFrame(const xr::FrameInput& frame, uint64_t now) {
    Sample out{};
    if (!xr::FrameInputUsable(frame, now)) return out;
    auto finiteMatrix = [](const pose::Mat4& m) {
        for (float v : m.m) if (!std::isfinite(v)) return false;
        return true;
    };
    if (!finiteMatrix(frame.headRel)) return out;
    out.focused = out.fresh = true;
    out.headYaw = std::atan2(frame.headRel.m[8], frame.headRel.m[10]);
    if (!std::isfinite(out.headYaw)) return Sample{};
    const auto& l = frame.hand[0]; const auto& r = frame.hand[1];
    out.leftX = Clamp(l.stick[0]); out.leftY = Clamp(l.stick[1]);
    out.rightX = Clamp(r.stick[0]); out.rightY = Clamp(r.stick[1]);
    out.leftTrigger = Clamp(l.trigger, 0.f, 1.f); out.rightTrigger = Clamp(r.trigger, 0.f, 1.f);
    out.leftA=l.a; out.leftB=l.b; out.rightA=r.a; out.rightB=r.b;
    out.leftMenu=l.menu; out.rightMenu=r.menu;
    // Pose validity controls arm rendering only.  Bumpers/D-pad use the
    // physical squeeze action so tracked hands are not reported as held.
    out.leftGrip=l.active && l.squeeze >= .5f; out.rightGrip=r.active && r.squeeze >= .5f;
    out.leftSqueeze=l.active && l.squeeze >= .5f; out.rightSqueeze=r.active && r.squeeze >= .5f;
    out.leftStickClick=l.stickClick; out.rightStickClick=r.stickClick;
    return out;
}

} // namespace hrvr::vrinput
