#pragma once
// 1.7 (owner 2026-11-24: "i never see the glasses, i always want to see the open glasses whenever im taking them out or
// on or off just like create a seperate copy of the object. Verify that it will show right in hand via visualisations").
// LIVE 1.6.5 (13:33): the 1.6.4 route looked for the game's glasses among the objects of the movies playing with the
// player - 0 at every grab: the put-on / take-off movie starts only after the prompt is answered. And the game folds
// the glasses' arms ~105 deg in the hand (DATA, tools/ari_copy.py). So the mod draws its OWN copy of
// HR_PROP_ARIGLASSES - its modelled pose, arms open - in the controller hand while the gesture holds them, in every eye
// image (xr.cpp), on top of the game's picture.
// Placement (pure, tests/ari_copy_tests.cpp): the hand's anatomical frame from the controller exactly as fpbody turns
// the drawn hand (fp::GripHandBasis rows forward / thumb side / up, wrist = grip - forward x fp_wrist_back), then the
// hold measured in the game's own clips (kHoldOn / kHoldOff: put on = HR_0020_ARI_STANDING_GN_ARIGLASSES_PUTON f100,
// about to put them on; take off = _TAKEOFF f32, just taken off). Pictures: docs/items/ari_copy/.
#include "fp_math.h"
#include "posemath.h"

// The model is GAME DATA and is never published (owner 2026-11-24, v1.3: "it shoudlnt ship with it"): the DLL reads it
// at run time from the player's own archives (ari_mesh_load.h, started by ari_copy_draw.h StartLoad). The two holds
// below are the mod's own measurements (tools/ari_copy.py, from the clips; tests/ari_mesh_load_tests.cpp checks them
// against the tool's local ari_glasses_mesh.inc).
namespace hrvr::aricopy {

// The frame part against the right hand's anatomical frame (fp::HandBasis rows forward / thumb side / up, origin the
// wrist), row-major, row vectors: put on = PUTON f100 (frame part 22.0 cm ahead of the wrist), take off = TAKEOFF f32.
inline constexpr float kHoldOn[16] = {0.462969f, -0.399749f, -0.791114f, 0.000000f, 0.832291f, -0.110939f,
                                      0.543124f, 0.000000f,  -0.304878f, -0.909887f, 0.281346f, 0.000000f,
                                      0.162802f, 0.017375f,  -0.147357f, 1.000000f};
inline constexpr float kHoldOff[16] = {0.027099f, -0.807714f, -0.588951f, 0.000000f, 0.950177f, -0.162209f,
                                       0.266180f, 0.000000f,  -0.310531f, -0.566821f, 0.763076f, 0.000000f,
                                       0.163497f, 0.013649f,  -0.132075f, 1.000000f};

// The hand's anatomical frame (rows forward, thumb side, up; origin the wrist) from the controller's grip pose.
inline bool HandAnatomy(const pose::Mat4& grip, float wristPitch, float wristBack, pose::Mat4* out) {
    pose::Mat4 b;
    if (!fp::GripHandBasis(grip, wristPitch, &b)) return false;
    b.m[12] = grip.m[12] - b.m[0] * wristBack;
    b.m[13] = grip.m[13] - b.m[1] * wristBack;
    b.m[14] = grip.m[14] - b.m[2] * wristBack;
    *out = b;
    return true;
}

// The glasses (frame part space) against the hand's anatomical frame. Measured on Jayden's RIGHT hand; the left hand is
// its mirror image: mirrored, the anatomical frame keeps forward and thumb side and flips up (u = f x s), so the hold's
// up column is negated, and the glasses - symmetric about their own x = 0 - are mirrored back about x (row 0): a proper
// rotation again.
inline pose::Mat4 Hold(bool putOn, bool leftHand) {
    pose::Mat4 h = pose::Identity();
    const float* src = putOn ? kHoldOn : kHoldOff;
    for (int i = 0; i < 16; ++i) h.m[i] = src[i];
    if (leftHand) {
        for (int c = 0; c < 4; ++c) h.m[c] = -h.m[c];
        for (int r = 0; r < 4; ++r) h.m[r * 4 + 2] = -h.m[r * 4 + 2];
    }
    return h;
}

// Where the copy is drawn (frame part space -> the grip's space), false when the grip is degenerate.
inline bool CopyWorld(const pose::Mat4& grip, float wristPitch, float wristBack, bool putOn, bool leftHand,
                      pose::Mat4* out) {
    pose::Mat4 a;
    if (!HandAnatomy(grip, wristPitch, wristBack, &a)) return false;
    *out = pose::Mul(Hold(putOn, leftHand), a);
    return true;
}

}  // namespace hrvr::aricopy
