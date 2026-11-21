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
    // Playbook r37 L2: pts = n model-bone positions (world), pts[0..2] = the root; a skinned prop's part in the fingers
    // can be far from its root (MovieCarry::FeedPoints).
    // fore[s]: that hand's animated ForeArm bone (the elbow), or nullptr: an item hanging on the forearm stays there.
    void SamplePoints(bool link, const float* pts, int n, const float* const clean[2], const float* head,
                      const float* const* fore = nullptr) {
        hand = carry.FeedPoints(link, pts, n, clean, wt, head, fore);
    }
    bool Held() const { return wt[0] > 0.f || wt[1] > 0.f; }
    // The drawn world (anim x clean^-1 x drawn per hand, blended by the weights); false = draw it as animated.
    bool Draw(const float* anim, const float* const clean[2], const float* const drawn[2], float* out) const {
        if (!Held()) return false;
        rigmap::BlendCarry(anim, clean, drawn, wt, out);
        return true;
    }
};

// Owner 2026-11-20 "Only the held part": an object made of several scene nodes (shoebox + lid + origami inside, phone
// base + handset, a tray of tools, a bundle of letters - 17 such node trees in Gate 1, tools/norig_tree.py) is carried
// node by node. Each node below the tracked one has its own State; it is linked only while it moves against its parent
// (PartMotion), and it is drawn in a hand when it holds itself OR rides a parent held there (Ride). Gate 2 replays exactly
// this (tools/rig_gate2.py part_free / ride, docs/items/GATE2.md: 0 open).

// Per hand the larger of the part's own weight and its parent's; the two hands' sum kept <= 1 (no step when the part's
// own carry ends inside a carried parent).
inline void Ride(const float own[2], const float up[2], float out[2]) {
    out[0] = std::fmax(own[0], up[0]);
    out[1] = std::fmax(own[1], up[1]);
    const float s = out[0] + out[1];
    if (s > 1.f) out[0] /= s, out[1] /= s;
}

// The part's local matrix (against its parent node) changed by >= 1 mm or >= 0.5 deg since the last call; Free() = it
// did within the last 1 s (Gate 2: 30 frames at the clips' 30 Hz). A part that never moved is never free.
struct PartMotion {
    float last[16] = {};
    bool have = false;
    unsigned long long movedMs = 0;
    bool moved = false;
    void Feed(const float* local, unsigned long long nowMs) {
        if (have) {
            const float dx = local[12] - last[12], dy = local[13] - last[13], dz = local[14] - last[14];
            // rotation change: trace of R_now x R_last^T (rows 0..2, columns 0..2)
            float tr = 0.f;
            for (int i = 0; i < 3; ++i)
                tr += local[i * 4] * last[i * 4] + local[i * 4 + 1] * last[i * 4 + 1] + local[i * 4 + 2] * last[i * 4 + 2];
            const float c = std::fmin(1.f, std::fmax(-1.f, 0.5f * (tr - 1.f)));
            if (dx * dx + dy * dy + dz * dz >= 1e-6f || c <= 0.99996192f) {  // 1 mm, cos(0.5 deg)
                movedMs = nowMs;
                moved = true;
            }
        }
        for (int i = 0; i < 16; ++i) last[i] = local[i];
        have = true;
    }
    bool Free(unsigned long long nowMs) const { return moved && nowMs - movedMs <= 1000; }
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
