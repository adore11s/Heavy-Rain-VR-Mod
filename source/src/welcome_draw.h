#pragma once
// First-start welcome windows (playbook 11.4, REQ-27): the DRAWING - the panel's V6 pattern, the built-in Montserrat,
// the pages' text - shared by the mod (welcome.cpp: when and where the windows show) and tests/welcome_preview.cpp
// (PNGs of every window from this exact code, checked before the headset). Ported from GoW's vr/welcome_draw.cpp
// (D-318 / D-328, MIT). Heavy Rain shows three windows (owner 2026-11-17: "there only needs to be three welcome
// windows" - no GoW FIRST PERSON page), in the owner's Heavy Rain colours (origami tan on rain slate, 2026-10-27).
#include <windows.h>

#include <cstdint>

namespace hrvr::welcome {
constexpr int kWW = 1200, kWH = 820;  // the window's pixels
constexpr int kWelPages = 3;          // WELCOME, KO-FI SUPPORTER, VR CONTROLS
extern uint32_t g_wOut[kWW * kWH];    // the last drawn window, 0xAARRGGBB premultiplied
extern RECT g_wBtn;                   // the CONTINUE / START bubble in g_wOut's pixels
bool MakeD2D();                          // Direct2D / DirectWrite / WIC and the fonts (once); false = unavailable
void WelcomeDraw(int page, bool hover);  // page 0..kWelPages-1 into g_wOut (hover = the bubble pointed at)
const char* PageName(int page);          // "welcome", "kofi", "controls"
}  // namespace hrvr::welcome
