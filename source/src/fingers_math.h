#pragma once
// Finger poses for first-person motion controls (HRVR 0.22.0, playbook 14.12 / 14.6 / 14.7 / 26.15).
// Pure math on the engine's row-vector world matrices (rows 0..2 the axes, row 3 the position). Used by fpbody.cpp,
// desk-tested by tests/finger_tests.cpp on the four playable rigs read from the game files (tests/finger_fixture.inc),
// which also writes the poses that tools/finger_pictures.py draws.
//
// Heavy Rain's hands (rig pictures, all playable outfits): <Side>Hand -> <Side>HandIndex1 -> 2 -> 3 and Middle1..3
// directly; Ring1..3 and Pinky1..3 under an unnamed cup bone; Thumb1 -> 2 -> 3 under the hand. No tip bones.
// MEASURED (tools/finger_pictures.py --measure, rest poses of Ethan, Madison, Jayden, Shelby, both hands):
//   palm normal n = f x s on the left hand and s x f on the right (f = wrist -> Middle1, s = Pinky1 -> Index1): the
//     thumb and every finger's rest bend lie on +n;
//   long fingers: CORRECTED 0.35.0 - their rest bend axis IS local Z (row 2) of every joint (0.0 deg on every
//     playable outfit, tools/finger_fixture.py); the palm-built bone x n is square to the bone but 0.2..16 deg off
//     that axis, which swung the curling fingers sideways (the old note called it "a true hinge": it was only square);
//   thumb: its rest bend axis is local Y (row 1) of Thumb1 and Thumb2 within 0..5 deg, local X is the bone. Detroit's
//     thumb used local Z: measured per game, never copied.
//   rest shape: Heavy Rain's rest hand is relaxed, NOT flat: knuckles bent +19..45 deg, middle joints +4..37 deg
//     (tools/finger_fixture.py). Detroit's 4 deg + (limit - 4) x amount was added to a flat rest; added to this one the
//     fist went past the palm (finger_tests failed on it first). So for the long fingers the angle is the joint's bend
//     FROM STRAIGHT (measured on the rest-shaped hand, each segment against the one before it; the last joint against
//     its bone's local X, which is the bone on every rig): each joint turns by (wanted - rest bend). The thumb's joints
//     past its base are near straight at rest (0 .. 25 deg): its curl is added to the rest as in Detroit.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "fpbody_math.h"

namespace hrvr::fingers {
using pose::Mat4;
using pose::Vec3;

enum Finger { kThumb = 0, kIndex = 1, kMiddle = 2, kRing = 3, kPinky = 4 };

// Joint indices of one hand: joint[f][0..2] from the knuckle out; -1 = missing.
struct HandJoints {
    int hand = -1;
    int joint[5][3] = {{-1, -1, -1}, {-1, -1, -1}, {-1, -1, -1}, {-1, -1, -1}, {-1, -1, -1}};
    bool Complete() const {
        if (hand < 0) return false;
        for (const auto& f : joint)
            for (int j : f)
                if (j < 0) return false;
        return true;
    }
};

// Curl amounts 0 (open) .. 1 (closed) per finger.
struct Amounts {
    float a[5] = {0.f, 0.f, 0.f, 0.f, 0.f};
};

struct Input {
    float trigger = 0.f, squeeze = 0.f;
    bool touchIndex = false, touchThumb = false, touchKnown = false;
};

inline float Clamp01(float v) { return v < 0.f ? 0.f : v > 1.f ? 1.f : v; }

// 14.12 (Half-Life: Alyx style, from what the controller senses) and 14.10's resting bases without touch sensing.
inline Amounts FromInput(const Input& in) {
    const float grip = Clamp01((in.squeeze - 0.05f) / 0.30f);
    const float trig = Clamp01((in.trigger - 0.05f) / 0.90f);
    Amounts r;
    if (in.touchKnown) {
        const float rest = 0.15f + 0.85f * grip;
        r.a[kMiddle] = r.a[kRing] = r.a[kPinky] = rest;
        r.a[kIndex] = (in.touchIndex || in.trigger > 0.05f) ? 0.35f + 0.65f * trig : 0.f;  // lifted: pointing
        r.a[kThumb] = in.touchThumb ? 0.55f + 0.35f * grip : 0.f;                          // lifted: thumbs up
    } else {
        const float rest = 0.45f + 0.55f * grip;
        r.a[kMiddle] = r.a[kRing] = r.a[kPinky] = rest;
        r.a[kIndex] = 0.25f + 0.75f * trig;
        r.a[kThumb] = std::max(0.35f, 0.8f * std::max(rest, r.a[kIndex]));
    }
    return r;
}

// Every amount moves at most 12 units per second (14.7): a pose change is a quick transition, never a jump.
inline void StepToward(Amounts* cur, const Amounts& target, float dt) {
    const float max = 12.f * std::clamp(dt, 0.f, 0.25f);
    for (int f = 0; f < 5; ++f) cur->a[f] += std::clamp(target.a[f] - cur->a[f], -max, max);
}

// Joint limits (degrees): long fingers 80 / 95 / 65 (GoW, kept in Detroit), thumb 30 / 45 / 20; a 4 deg relaxed bend.
constexpr float kLong[3] = {80.f, 95.f, 65.f}, kThumbLimit[3] = {30.f, 45.f, 20.f}, kRelaxed = 4.f;
inline float JointAngle(int finger, int j, float amount) {
    const float lim = finger == kThumb ? kThumbLimit[j] : kLong[j];
    return (kRelaxed + (lim - kRelaxed) * Clamp01(amount)) * 0.017453293f;
}

inline Mat4 AtW(const float* w, int i) {
    Mat4 m;
    std::memcpy(m.m, w + size_t(i) * 16, sizeof(m.m));
    return m;
}
inline void PutW(float* w, int i, const Mat4& m) { std::memcpy(w + size_t(i) * 16, m.m, sizeof(m.m)); }
inline Vec3 PosW(const float* w, int i) { return {w[size_t(i) * 16 + 12], w[size_t(i) * 16 + 13], w[size_t(i) * 16 + 14]}; }

// A usable rest matrix (14.12): finite, |det| in 0.05 .. 20, w column (0, 0, 0, 1).
inline bool UsableRest(const float* m) {
    for (int k = 0; k < 16; ++k)
        if (!std::isfinite(m[k])) return false;
    if (std::fabs(m[3]) > 1e-3f || std::fabs(m[7]) > 1e-3f || std::fabs(m[11]) > 1e-3f || std::fabs(m[15] - 1.f) > 1e-3f)
        return false;
    const float det = m[0] * (m[5] * m[10] - m[6] * m[9]) - m[1] * (m[4] * m[10] - m[6] * m[8]) +
                      m[2] * (m[4] * m[9] - m[5] * m[8]);
    return std::fabs(det) >= 0.05f && std::fabs(det) <= 20.f;
}

struct Report {
    bool applied = false;
    int restPlaced = 0, animatedKept = 0;  // bones below the hand put at their rest offset / left at the animation's
    bool thumbOwnAxis[3] = {false, false, false};
    bool longOwnAxis[4] = {false, false, false, false};  // index .. pinky: hinged about the knuckle's own local Z
};

// Rotates bone b and everything below it (within `below`, parents before children) about `axis` through `pivot`.
inline void TurnSubtree(float* w, uint32_t count, const int16_t* parent, int b, Vec3 axis, float angle, Vec3 pivot,
                        uint8_t* mark) {
    const Mat4 r = fpbody::AboutPivot(fpbody::AxisAngle(axis, angle), pivot);
    std::memset(mark, 0, count);
    mark[b] = 1;
    PutW(w, b, pose::Mul(AtW(w, b), r));
    for (uint32_t i = uint32_t(b) + 1; i < count; ++i)
        if (parent[i] >= 0 && mark[parent[i]]) {
            mark[i] = 1;
            PutW(w, int(i), pose::Mul(AtW(w, int(i)), r));
        }
}

// The signed angle from segment a to segment b about `axis` (positive = the way a curl turns, once axis is Signed).
// Uses the same rotation as TurnSubtree, so the sign convention cannot differ.
inline float BendAbout(Vec3 a, Vec3 b, Vec3 axis) {
    const Vec3 pa = fp::Sub(a, fp::Mul(axis, fp::Dot(a, axis))), pb = fp::Sub(b, fp::Mul(axis, fp::Dot(b, axis)));
    if (fp::Len(pa) < 1e-7f || fp::Len(pb) < 1e-7f) return 0.f;
    const float ang = std::acos(std::clamp(fp::Dot(fp::Norm(pa), fp::Norm(pb)), -1.f, 1.f));
    const Mat4 r = fpbody::AxisAngle(axis, ang);
    const Vec3 turned = fpbody::Pos(pose::Mul(fpbody::Translation(fp::Norm(pa)), r));
    return fp::Dot(turned, fp::Norm(pb)) > 0.9995f ? ang : -ang;  // +ang takes a onto b: a positive bend
}

// The sign of `axis` that moves `tip` (turning about `pivot`) toward `toward`.
inline Vec3 Signed(Vec3 axis, Vec3 pivot, Vec3 tip, Vec3 toward) {
    const Mat4 r = fpbody::AboutPivot(fpbody::AxisAngle(axis, 0.1f), pivot);
    const Vec3 moved = fpbody::Pos(pose::Mul(fpbody::Translation(tip), r));
    return fp::Dot(fp::Sub(moved, tip), toward) >= 0.f ? axis : fp::Mul(axis, -1.f);
}

// Poses one hand in place in `w` (the hand bone already where it is drawn): every bone below the hand to its rest offset
// from its parent (`rest` = object-space rest worlds; null or restShape false = keep the animation's offsets), then each
// finger curled as a hinge. side 0 = left, 1 = right. `scratch` holds count * 16 floats, `mark` count bytes.
// Playbook 19.8 point 2 (revision 15): a hand that starts or stops holding an item crosses between the animation's own
// hand (`anim`: the pose before Apply, the hand bone where it is drawn) and the finger pose (`w` after Apply) over the
// curl filter's time (StepToward's 12 per second), never a jump. t = 1: the animation's hand, 0: the finger pose.
// 0.36.6 (owner 2026-11-20: "on right hand ... they get crooked from the middle part of finger till the end"): every bone
// below `hand` is blended in its PARENT's frame and the chain recomposed from the hand out, so a part-way blend is still a
// jointed finger (each segment keeps its length, each joint turns about itself). 0.33.0 blended each bone in the hand's
// frame on its own: the far joints' places were mixed in a straight line while their turns were mixed separately, so
// the middle and end segments left the finger's line, more toward the tip. `before` holds count * 16 floats (scratch).
inline void BlendBelowHand(float* w, const float* anim, uint32_t count, const int16_t* parent, int hand, float t,
                           uint8_t* mark, float* before) {
    if (!(t > 0.f) || hand < 0 || uint32_t(hand) >= count) return;
    std::memcpy(before, w, size_t(count) * 64);  // the finger pose; w is rebuilt from the hand out
    std::memset(mark, 0, count);
    mark[hand] = 1;
    for (uint32_t b = uint32_t(hand) + 1; b < count; ++b) {
        const int p = parent[b];
        if (p < 0 || !mark[p]) continue;
        mark[b] = 1;
        Mat4 invA, invF;
        if (!fpbody::AffineInverse(AtW(anim, p), &invA) || !fpbody::AffineInverse(AtW(before, p), &invF)) continue;
        const Mat4 la = pose::Mul(AtW(anim, int(b)), invA), lf = pose::Mul(AtW(before, int(b)), invF);
        PutW(w, int(b), pose::Mul(t >= 1.f ? la : fpbody::BlendAffine(lf, la, t), AtW(w, p)));
    }
}

inline bool Apply(float* w, uint32_t count, const int16_t* parent, const float* rest, const HandJoints& h, int side,
                  const Amounts& am, bool restShape, float* scratch, uint8_t* mark, Report* rep) {
    if (!h.Complete() || uint32_t(h.hand) >= count) return false;
    // 1. the rest shape below the hand (14.12: the fingers start from the rig's rest shape, not this frame's animation)
    std::memcpy(scratch, w, size_t(count) * 64);  // the animated pose, for the bones without a usable rest
    std::memset(mark, 0, count);
    mark[h.hand] = 1;
    for (uint32_t b = uint32_t(h.hand) + 1; b < count; ++b) {
        const int p = parent[b];
        if (p < 0 || !mark[p]) continue;
        mark[b] = 1;
        Mat4 local, inv;
        const bool fromRest = restShape && rest && UsableRest(rest + size_t(b) * 16) && UsableRest(rest + size_t(p) * 16) &&
                              fpbody::AffineInverse(AtW(rest, p), &inv);
        if (fromRest) {
            local = pose::Mul(AtW(rest, int(b)), inv);
            if (rep) ++rep->restPlaced;
        } else {
            if (!fpbody::AffineInverse(AtW(scratch, p), &inv)) continue;
            local = pose::Mul(AtW(scratch, int(b)), inv);
            if (rep) ++rep->animatedKept;
        }
        PutW(w, int(b), pose::Mul(local, AtW(w, p)));
    }
    // 2. the palm frame from the knuckles (measured: n points to the palm side on every playable rig)
    const Vec3 at = PosW(w, h.hand);
    const Vec3 f = fp::Sub(PosW(w, h.joint[kMiddle][0]), at);
    const Vec3 s = fp::Sub(PosW(w, h.joint[kIndex][0]), PosW(w, h.joint[kPinky][0]));
    Vec3 n = side == 0 ? fp::Cross(f, s) : fp::Cross(s, f);
    if (fp::Len(n) < 1e-6f) return false;
    n = fp::Norm(n);
    uint8_t* sub = mark;  // reused by TurnSubtree
    // 3. long fingers: one hinge axis per finger, taken once at the knuckle, never a shortest-arc swing; each joint to
    //    its wanted bend from straight (see the header). 0.35.0 (owner 2026-11-20: "some have weird four fingers but
    //    not thumb"): the axis is the knuckle's OWN local Z (row 2), measured as every long finger's rest bend axis on
    //    every playable outfit (0.0 deg; the palm-built bone x n was 0.2 .. 16 deg off it and swung the fingers up to
    //    12 mm out of their plane, the nails turning sideways - finger_tests); bone x n only when Z is not square to
    //    the bone (|Z . bone| >= 0.2). Signed so that a positive curl closes toward the palm (n).
    for (int fi = kIndex; fi <= kPinky; ++fi) {
        const int* j = h.joint[fi];
        const Vec3 d = fp::Sub(PosW(w, j[1]), PosW(w, j[0]));
        const Mat4 knuckle = AtW(w, j[0]);
        const Vec3 own{knuckle.m[8], knuckle.m[9], knuckle.m[10]};
        const bool usable = fp::Len(own) > 1e-6f && fp::Len(d) > 1e-6f && std::fabs(fp::Dot(fp::Norm(own), fp::Norm(d))) < 0.2f;
        Vec3 axis = usable ? own : fp::Cross(d, n);
        if (fp::Len(axis) < 1e-7f || parent[j[0]] < 0) continue;
        if (rep) rep->longOwnAxis[fi - kIndex] = usable;
        axis = Signed(fp::Norm(axis), PosW(w, j[0]), PosW(w, j[1]), n);
        // 0.36.7 (owner: "the right hand is still bad ... the final tip of fingers except thumb ... like 90 degrees too
        // much"): there is no tip bone, so the end segment's direction is the last joint's local X - but signed. DATA (all
        // playable outfits): X runs ALONG the finger on the left hand (X . bone = +1.00) and BACKWARDS on the right (-1.00,
        // a mirrored rig); 0.22.0 .. 0.36.6 took +X on both, so the right end segment was read as pointing back at the
        // palm and the end joint was turned by ~180 deg minus its real bend. The sign is the middle joint's own X against
        // its bone (X3 . X2 = +0.99 .. 1.00 on every finger).
        const Mat4 mid = AtW(w, j[1]), last = AtW(w, j[2]);
        const Vec3 seg2 = fp::Sub(PosW(w, j[2]), PosW(w, j[1]));
        const float xSign = fp::Dot(Vec3{mid.m[0], mid.m[1], mid.m[2]}, seg2) < 0.f ? -1.f : 1.f;
        const Vec3 seg[4] = {fp::Sub(PosW(w, j[0]), PosW(w, parent[j[0]])), d, seg2,
                             fp::Mul(Vec3{last.m[0], last.m[1], last.m[2]}, xSign)};
        float bend[3];
        for (int k = 0; k < 3; ++k) bend[k] = BendAbout(seg[k], seg[k + 1], axis);
        for (int k = 0; k < 3; ++k)
            TurnSubtree(w, count, parent, j[k], axis, JointAngle(fi, k, am.a[fi]) - bend[k], PosW(w, j[k]), sub);
    }
    // 4. the thumb: about its OWN local Y (measured), used while it is square to the bone (|axis . bone| < 0.2),
    //    otherwise the palm-built axis; the sign fixed once at Thumb1 so a positive curl closes it (toward n - s)
    const int* t = h.joint[kThumb];
    const Vec3 closeDir = fp::Sub(n, fp::Norm(s));
    Vec3 axis0{};
    for (int k = 0; k < 3; ++k) {
        const Mat4 m = AtW(w, t[k]);
        // the end segment: local X, signed like Thumb2's X against its bone (0.36.7, mirrored right hand)
        Vec3 bone = k < 2 ? fp::Sub(PosW(w, t[k + 1]), PosW(w, t[k])) : Vec3{m.m[0], m.m[1], m.m[2]};
        if (k == 2) {
            const Mat4 m1 = AtW(w, t[1]);
            if (fp::Dot(Vec3{m1.m[0], m1.m[1], m1.m[2]}, fp::Sub(PosW(w, t[2]), PosW(w, t[1]))) < 0.f) bone = fp::Mul(bone, -1.f);
        }
        Vec3 own{m.m[4], m.m[5], m.m[6]};
        const bool usable = fp::Len(own) > 1e-6f && fp::Len(bone) > 1e-6f &&
                            std::fabs(fp::Dot(fp::Norm(own), fp::Norm(bone))) < 0.2f;
        Vec3 axis = usable ? fp::Norm(own) : fp::Norm(fp::Cross(bone, n));
        if (rep) rep->thumbOwnAxis[k] = usable;
        if (k == 0) {
            axis = Signed(axis, PosW(w, t[0]), PosW(w, t[1]), closeDir);
            axis0 = axis;
        } else if (fp::Dot(axis, axis0) < 0.f) {
            axis = fp::Mul(axis, -1.f);
        }
        TurnSubtree(w, count, parent, t[k], axis, JointAngle(kThumb, k, am.a[kThumb]), PosW(w, t[k]), sub);
    }
    if (rep) rep->applied = true;
    return true;
}

// HumanIK names -> joints: <Side>Hand<Finger>1 by name, then each next joint as the first child of the previous one.
template <typename NameOf>
inline HandJoints FindJoints(uint32_t count, const int16_t* parent, int hand, int side, NameOf nameIs) {
    static const char* const kNames[5] = {"Thumb", "Index", "Middle", "Ring", "Pinky"};
    HandJoints h;
    h.hand = hand;
    for (int f = 0; f < 5; ++f) {
        char name[48];
        snprintf(name, sizeof(name), "%sHand%s1", side ? "Right" : "Left", kNames[f]);
        int j = -1;
        for (uint32_t i = 0; i < count && j < 0; ++i)
            if (nameIs(int(i), name)) j = int(i);
        for (int k = 0; k < 3 && j >= 0; ++k) {
            h.joint[f][k] = j;
            int next = -1;
            for (uint32_t i = uint32_t(j) + 1; i < count && next < 0; ++i)
                if (parent[i] == j) next = int(i);
            j = next;
        }
    }
    return h;
}

}  // namespace hrvr::fingers
