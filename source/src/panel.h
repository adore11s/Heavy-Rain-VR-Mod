#pragma once
// VR settings panel, profile GOW_STYLE_NINE_ROW_V6 (playbook chapter 11; GoW vr/panel.cpp, MIT). Drawn with
// GDI into a 1000 x 1434 DIB (redrawn only when a value or the hovered button changes) and shown by xr.cpp
// as its own quad, 0.15 m to the right of the floating game screen, while the game is paused. Point with
// either controller and pull its trigger; every choice is saved in hrvr.ini at once.
#include <cstdint>

#include "panel_math.h"
#include "xr.h"

namespace hrvr::panel {

struct Frame {
    Placement placement;  // seated reference frame
    const uint32_t* pixels = nullptr;  // kWidthPx x kHeightPx, 0xAARRGGBB (sRGB bytes), opaque
    bool dirty = false;                // new pixels since the last frame
    bool cursor = false;               // a controller points at the panel
    pose::Vec3 cursorAt{};             // the cursor quad's centre (1 cm in front of the panel)
    float cursorSize = 0.035f;
};

// The thread that submits the frame's layers: the panel next to the screen centred at `screen` (width
// `screenW`, reference frame) for this input; false = no panel (the game is not paused). Handles pointing.
bool Update(const xr::FrameInput& in, const pose::Vec3& screen, float screenW, Frame* out);
// No panel this frame: pointing state reset.
void Hidden();
// The 64 x 64 cursor dot (0xAARRGGBB, premultiplied, sRGB).
const uint32_t* CursorPixels();
constexpr int kCursorPx = 64;
// The pointer ray of hand 0 / 1 (aim origin and unit direction, reference frame) through the panel's One Euro filter
// (GoW D-263); also used by the welcome windows, which never show together with the panel.
void SmoothAim(int hand, pose::Vec3& p, pose::Vec3& d);
// Hand 0 left / 1 right points at the panel: its trigger belongs to the panel, not the game.
bool CapturesTrigger(int hand);

}  // namespace hrvr::panel
