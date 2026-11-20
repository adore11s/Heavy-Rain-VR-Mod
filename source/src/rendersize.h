#pragma once
// VR picture size (playbook chapter 4 / REQ-06, REQ-07), STATIC 2026-10-27.
//
// Heavy Rain draws the scene at g_RenderWidth x g_RenderHeight into the swap chain's back buffer
// (SWAP_CHAIN+0x18; +0x28 when g_Force16x9). The sizes come from three functions the game itself runs when
// the window or the resolution changes: ComputeBackbufferSize (configured size within the window) ->
// ComputeRenderSize -> SwapChainResize (ResizeBuffers at g_SwapWidth x g_SwapHeight, then every render target,
// the deferred buffers and the menu display at the render size). The configured resolution
// (0x141a1e7f0, saved in user_setting.ini) is never written.
//
// While the headset shows 3D frames, the three report a square N x N instead (N fixed, or Auto = the
// runtime's largest recommended eye size); 2D-only screens (videos, loading, the main menu) and VR off go back
// to the game's own size, so a 16:9 video stays 16:9 (REQ-08). 0.25.0 (playbook 08 / 08.4): for the whole VR session
// the desktop window is the mod's - HD by default (desktop_window = its client height, 1280 x 720), resizable by the
// player and always 16:9, never moved or resized by the mod on loads, menus or videos; the game never sees its size
// (its WM_SIZE is answered by DefWindowProc meanwhile) and the player's own window comes back when VR ends.
#include <cstdint>

#include "rendersize_math.h"

namespace hrvr::rendersize {

bool Install();
// Game thread at Present, before the game's Present: applies a pending size at a frame boundary.
// scene3D: the headset showed stereo 3D frames recently.
// video: a full-screen video is on the UI screen (the game's own 16:9 size at once).
void Frame(bool xrRunning, bool scene3D, bool video);

int Setting();           // kGame, kAuto or a size
void SetSetting(int v);  // saved to hrvr.ini (render_size, render_size_auto)
uint32_t AutoSize();     // 0 until the runtime reported its eye size
uint32_t Applied();      // the square size in use now, 0 = the game's own
// Render thread, right before the game's Present: the desktop window's client size while it is the mod's (and the player
// is not dragging it), and whether the square 3D view may be cropped to fill it (desktop_crop); false = draw nothing.
bool DesktopPicture(int* clientW, int* clientH, bool* crop);
// Command file verb: rendersize game|auto|<N>|status.
bool Command(const char* verb, const char* arg);

}  // namespace hrvr::rendersize
