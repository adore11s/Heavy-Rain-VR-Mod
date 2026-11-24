#pragma once
// 1.6 (owner 2026-11-24): Jayden's ARI glasses by hand in first person (F1) with motion controls. Evidence and design:
// docs/ARI_GESTURE.md; pure logic: ari_gesture.h; hint pictures: ari_draw.h.
//
// The glasses prompts are the semicircle (MPAR_ELEMENT_AXIS_ROTATION) user actions whose movie plays the 0020 put-on /
// take-off clips. A call-through hook on the USER_ACTION activation (0x14023d180) tags such an element by the movie
// name the user action carries (+0x3C8). While F1 + motion controls: the element's stick fields are written by the mod
// right before the game's per-frame semicircle check (0x14045df90; 1.6.1) - the centre, or the arc once the hand gesture is done - so the
// thumbstick no longer answers it; its drawing is moved out of sight (MPAR::UpdateElement, prompts.cpp); the first
// time, a hint shows "Grab AR Glasses" (a line to the chest) then "Put on AR Glasses" (a line up from the hand).
// Anywhere else (F3, motion controls off, ari_gesture=off) the game's prompt is untouched.
#include <cstdint>

#include "posemath.h"
#include "xr.h"

namespace hrvr::ari {

bool Install();
// XR submit thread, every 3D frame: the gesture from the controllers and the headset.
void Frame(const xr::FrameInput& in);
// The hint to draw this frame (seated reference frame): picture 0 / 1 (ari_draw.h), its centre, facing yaw (the quad's
// +Z toward the head) and tilt (radians, top leaning away), size.
struct HintQuad {
    int picture = -1;
    const uint32_t* pixels = nullptr;
    pose::Vec3 center{};
    float yaw = 0.f, pitch = 0.f, widthM = 0.f, heightM = 0.f;
};
bool HintLayer(HintQuad* out);
// MPAR::UpdateElement after the game's update (game thread): true = this record is a glasses prompt handled by the
// gesture, its drawing was moved out of sight (the caller then leaves it alone).
bool HidePrompt(void* record);
// 1.6.3: hand 0 left / 1 right - its grip belongs to the glasses gesture now (pressed in the start zone with a glasses
// prompt on offer, or holding, until let go): vrinput keeps it from the game (L1 / R1).
bool TakesGrip(int hand);
// 1.6.4: the hand (0 / 1) holding the glasses in the gesture right now (F1 + motion controls), else -1; `kind` = put on
// or take off. fpbody draws the glasses object at that controller meanwhile.
int GlassesHand(int* kind);

}  // namespace hrvr::ari
