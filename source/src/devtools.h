#pragma once
// Developer tools of the mod's maintainer (ported from the Beyond: Two Souls VR mod, where the owner said "those are
// just mine"): the F7 chapter teleporter, the F8 cutscene tour and the save sandbox that keeps both out of the real
// save. They are compiled only into the developer build (CMake target hrvr_dev, sources in src/devtools/). The
// release build links devtools_off.cpp instead: every function below does nothing, installs no hook, reads no key
// and never touches the player's files or saves.
#include <cstdint>

#include "xr.h"

namespace hrvr::devtools {

// False in the release build.
bool Available();

// Once, at startup (hooks of the developer tools).
void Install();

// Once per Present (their hotkeys).
void Frame();

// Wherever controller input reaches the game: the input the game should get.
xr::FrameInput Filter(const xr::FrameInput& in);

// An overlay image in front of the head (any thread): false while none is shown. `version` changes with every redraw.
constexpr int kOverlayWidthPx = 1000, kOverlayHeightPx = 1300;
constexpr float kOverlayWidthM = 1.0f, kOverlayHeightM = kOverlayWidthM * float(kOverlayHeightPx) / float(kOverlayWidthPx);
struct Overlay {
    pose::Vec3 center{};  // seated reference frame (yaw-only, recentered head at the origin)
    float yaw = 0.f;
    uint64_t version = 0;
};
bool GetOverlay(Overlay* out);
// Copies the current image (kOverlayWidthPx x kOverlayHeightPx, B8G8R8A8, opaque) into dst; returns its version.
uint64_t CopyPixels(uint32_t* dst);

// Dev verbs (the mod's command file); false when the verb is not theirs.
bool Command(const char* verb, const char* arg);

}  // namespace hrvr::devtools
