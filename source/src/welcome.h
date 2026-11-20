#pragma once
// First-start welcome windows (playbook 11.4, REQ-27; GoW D-318 / D-328 / D-331, MIT): WELCOME, KO-FI SUPPORTER,
// VR CONTROLS (Heavy Rain: three, owner 2026-11-17), one at a time, once per welcome version (hrvr.ini welcome_seen).
// They appear ~3 s (welcome_delay_s) into the 3D view outside cutscenes, 1.6 m ahead, follow the view slowly and are
// mandatory: the VR controllers give the game nothing meanwhile. CONTINUE / START with either controller's trigger,
// Enter (the game in front) or a gamepad's A / Cross (any pad: Heavy Rain reads every pad through ISteamController,
// Steam Input included); that key / button is taken from the game while a window is up and until it is released.
// Drawing: welcome_draw.h (shared with tests/welcome_preview.cpp).
#include <cstdint>

#include "posemath.h"
#include "xr.h"

namespace hrvr::welcome {

// Once, at start: the game's imports of GetKeyState / GetKeyboardState (its only keyboard readers - STATIC, see
// welcome.cpp) answer "up" for Enter while TakesEnter().
void Install();

struct Layer {
    pose::Vec3 center{};  // seated reference frame
    float yaw = 0.f;      // the quad's front (+Z) toward the head
    float widthM = 0.f, heightM = 0.f;
    const uint32_t* pixels = nullptr;  // kWW x kWH, 0xAARRGGBB premultiplied
    bool dirty = false;                // new pixels since the last frame
    bool cursor = false;               // a controller points at the window
    pose::Vec3 cursorAt{};             // the pointer dot's centre, 1 cm in front
};
// The thread that submits a stereo (3D) frame's layers: true = a window shows this frame. `paused`: the game is paused
// (the VR panel shows instead) - handled like a flat screen.
bool Frame(const xr::FrameInput& in, bool paused, Layer* out);
// A flat 2D frame (menus, loading, videos): the window hides, the 3 s count starts again, the page reached is kept.
void FlatFrame();
bool Active();
// Enter / the pad's confirm button belong to the window (while one is up, and until released after the last one).
bool TakesEnter();
bool TakesPadConfirm();
// vrinput: a real pad's confirm action (a_button: A / Cross) as the game just queried it.
void NotePadConfirm(bool held);

}  // namespace hrvr::welcome
