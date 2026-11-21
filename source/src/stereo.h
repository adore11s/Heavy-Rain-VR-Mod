#pragma once
#include <cstddef>
#include <cstdint>

// Native stereo for Heavy Rain (derived from the B2S VR mod's "R1 variant B", MIT).
//
// Heavy Rain renders immediately on its render thread, straight into the swapchain backbuffer:
// DISPLAY_WINDOW::Display -> VIEWPORT_LIST::Display (3D, per viewport, each pass ending with the
// viewport's 2D layer) -> the window's 2D layer. With OpenXR running, VIEWPORT_LIST::Display is
// run once per eye with the eye pose composed onto every viewport camera's node (+0x90) and the
// headset frustum written into the camera projections; the backbuffer is copied after each eye.
// The 2D layers are held back during the eye passes and drawn once afterwards over transparent
// black, so the UI lands on a floating panel (xr::OnUiBegin / OnUiEnd) and the flat mirror shows
// the last eye with the UI composited on top.
namespace hrvr::stereo {

// Installs every render hook (MinHook must be initialised). Returns false when a core hook fails.
bool Install();
// VR on / off as F9 does (the VR settings panel's "Off"; F9 turns it back on).
void RequestVr(bool on);
// 0.36.6 (Beyond 1.0.3 "Camera closer (third person)"): the game's camera moved this many metres toward the character
// along its horizontal view while no first-person view drives it; 0 = off, max 5, default 2 (0.36.8). hrvr.ini
// camera_forward, saved.
float CameraPush();
void SetCameraPush(float metres);
// The size of each eye's image (the render size: square with a VR picture size), false before the first pair.
bool EyeImageSize(uint32_t* width, uint32_t* height);
// Rendering method (stereo.cpp): Native, AFR (one eye per frame), AER (one eye per frame, both eyes of a pair
// from the same head pose; hrvr.ini only) or Mono (playbook 07.7: one render per frame from the centre of the
// head, the same picture to both eyes); saved to hrvr.ini (rendering_method). Native is the default (1.0.2, owner).
enum { kNative = 0, kAfr = 1, kAer = 2, kMono = 3 };
int Method();
void SetMethod(int method);
// For a crash report, without locks: the rendering method and the render phase (an eye pass or not).
void DescribeForCrash(char* out, size_t cap);
inline const char* MethodName(int m) {
    return m == kNative ? "Native" : m == kAfr ? "AFR" : m == kAer ? "AER" : m == kMono ? "Mono" : "?";
}

}  // namespace hrvr::stereo
