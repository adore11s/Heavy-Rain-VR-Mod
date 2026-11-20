#pragma once
// 0.33.0: one object a playing movie hands the player (playbook 19.8 point 11; Gate 2 rule rigmap::MovieCarry, Gate 3
// wiring). fpbody keeps one State per object node, samples it at the clips' 30 Hz and draws it with Draw. Pure: the desk
// test (tests/movie_item_tests.cpp) runs this exact code on the game's 0403 clips.
#include "rigmap.h"

#include <cmath>

namespace hrvr::movieitem {

struct State {
    rigmap::MovieCarry carry;
    float wt[2] = {0.f, 0.f};
    int hand = -1;

    // The object was just in that hand's socket (the bone route answered it there < 1 s ago): a put-back clip starts in
    // the drawn hand at full weight (Gate 2: 875 held runs start at frame 0).
    void Seed(int h) {
        carry.Seed(h);
        wt[0] = h == 0 ? 1.f : 0.f;
        wt[1] = h == 1 ? 1.f : 0.f;
        hand = h;
    }
    // link: in a playing movie with the player and of class "held" (item_class.h); anim: the object's animated world
    // (16 floats, row vectors); clean[s]: that hand's animated wrist; head: the animated Head bone (or nullptr).
    void Sample(bool link, const float* anim, const float* const clean[2], const float* head) {
        hand = carry.Feed(link, anim + 12, clean, wt, head);
    }
    bool Held() const { return wt[0] > 0.f || wt[1] > 0.f; }
    // The drawn world (anim x clean^-1 x drawn per hand, blended by the weights); false = draw it as animated.
    bool Draw(const float* anim, const float* const clean[2], const float* const drawn[2], float* out) const {
        if (!Held()) return false;
        rigmap::BlendCarry(anim, clean, drawn, wt, out);
        return true;
    }
};

// Playbook 19.8 (Gate 3 row 8): how far the drawn object is from its place in the drawn hand - its place in the animated
// hand (anim x clean^-1) against its drawn place in the drawn hand (item x drawn^-1). 0 at full weight by construction;
// a ramp (pick-up / release / pass) shows here.
inline void Drift(const float* anim, const float* item, const float* clean, const float* drawn, float* cm, float* deg) {
    float ic[16], id[16], ra[16], rd[16];
    *cm = 0.f;
    *deg = 0.f;
    if (!rigmap::InvAffine(clean, ic) || !rigmap::InvAffine(drawn, id)) return;
    rigmap::MulAffine(anim, ic, ra);
    rigmap::MulAffine(item, id, rd);
    const float dx = ra[12] - rd[12], dy = ra[13] - rd[13], dz = ra[14] - rd[14];
    *cm = 100.f * std::sqrt(dx * dx + dy * dy + dz * dz);
    // the turn between the two: rotation rows of ra x rd^T, angle from the quaternion's vector part and w (atan2 keeps
    // its precision near 0, where acos of a float dot product cannot resolve 0.05 deg)
    float rt[16] = {}, q[4];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) rt[i * 4 + j] = ra[i * 4] * rd[j * 4] + ra[i * 4 + 1] * rd[j * 4 + 1] + ra[i * 4 + 2] * rd[j * 4 + 2];
    rt[15] = 1.f;
    rigmap::QuatOf(rt, q);
    const float v = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]);
    *deg = 2.f * std::atan2(v, std::fabs(q[3])) * 57.29578f;
}

}  // namespace hrvr::movieitem
