#pragma once
// VR picture size: the pure parts (rendersize.cpp), header-only for the desk tests.
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace hrvr::rendersize {

constexpr int kGame = 0;   // setting: the game's own resolution
constexpr int kAuto = -1;  // setting: the runtime's recommendation
constexpr int kPresets[] = {1440, 1728, 2048, 2304, 2560, 2816};
constexpr int kPresetCount = int(sizeof(kPresets) / sizeof(kPresets[0]));

// GoW Auto: the largest recommended eye extent, clamped and rounded up to a multiple of 8.
inline uint32_t AutoFrom(uint32_t w, uint32_t h) {
    if (!w || !h) return 0;
    const uint32_t n = std::clamp<uint32_t>(std::max(w, h), 512u, 8192u);
    return (n + 7u) & ~7u;
}

// A fixed size from the ini: 0 when not a size, else clamped and rounded down to a multiple of 8.
inline uint32_t ValidFixed(int v) {
    if (v < 256) return 0;
    return uint32_t(std::clamp(v, 512, 8192)) & ~7u;
}

// The panel stepper: Game < 1440 < ... < 2816 < Auto. '+' at Auto and '-' at Game stay.
inline int Step(int v, int dir) {
    int i;  // position in Game(0), presets(1..n), Auto(n+1)
    if (v == kGame) i = 0;
    else if (v == kAuto) i = kPresetCount + 1;
    else {
        i = 1;
        for (int k = 0; k < kPresetCount; ++k)
            if (v >= kPresets[k]) i = k + 1;
    }
    i = std::clamp(i + (dir > 0 ? 1 : -1), 0, kPresetCount + 1);
    return i == 0 ? kGame : i == kPresetCount + 1 ? kAuto : kPresets[i - 1];
}

// When to change the render size: square only after 0.5 s of 3D frames, back to the game's own after 1.5 s
// without (a loading screen), at most once every 2 s; immediately the game's own when VR stops. A full-screen video
// (xr video mode) counts as no 3D at once: Heavy Rain keeps rendering its 3D viewport behind the intro video
// (LIVE 0.6.0: the video stayed square), so 3D frames alone do not mean a 3D screen.
struct SwitchPolicy {
    uint64_t since3D = 0, last3D = 0, lastChange = 0;

    uint32_t Want(bool xrRunning, bool scene3D, uint32_t target, uint32_t applied, uint64_t now, bool video = false) {
        if (video) {
            since3D = last3D = 0;
            if (!xrRunning || !target || !applied) return 0;
            return lastChange && now - lastChange < 2000 ? applied : 0;
        }
        if (scene3D) {
            if (!since3D) since3D = now;
            last3D = now;
        } else {
            since3D = 0;
        }
        if (!xrRunning || !target) return 0;
        uint32_t want = applied;
        if (!applied) {
            if (since3D && now - since3D >= 500) want = target;
        } else {
            want = last3D && now - last3D < 1500 ? target : 0;
        }
        if (want != applied && lastChange && now - lastChange < 2000) return applied;
        return want;
    }
};

// ---- 0.25.0: the resizable 16:9 desktop window (playbook 08.4, GoW D-329) ----
constexpr int kMinClientPx = 160;  // the smallest client height (284 x 160)
// Every client size the mod sets or keeps is 16:9, from its height.
inline int W169(int h) { return (h * 16 + 4) / 9; }
inline int H169(int w) { return (w * 9 + 8) / 16; }

// WM_SIZING edges (WMSZ_*): 1 left, 2 right, 3 top, 4 topleft, 5 topright, 6 bottom, 7 bottomleft, 8 bottomright.
// The dragged outer rectangle (l, t, r, b) kept 16:9 in its client: a side edge sets the height from the width, a top
// or bottom edge the width from the height, a corner whichever side moved further; the opposite edge / corner stays.
// frameW / frameH: the window's frame (outer minus client); maxClientH: the largest 16:9 client height on the
// monitor (0 = unknown).
inline void Sizing169(int edge, int frameW, int frameH, int maxClientH, int& l, int& t, int& r, int& b) {
    int cw = (r - l) - frameW, ch = (b - t) - frameH;
    const bool corner = edge == 4 || edge == 5 || edge == 7 || edge == 8;
    const bool byWidth = edge == 1 || edge == 2 || (corner && cw * 9 > ch * 16);
    if (byWidth) ch = H169(cw);
    else cw = W169(ch);
    if (ch < kMinClientPx) {
        ch = kMinClientPx;
        cw = W169(ch);
    }
    if (maxClientH > 0 && ch > maxClientH) {
        ch = maxClientH;
        cw = W169(ch);
    }
    const int ww = cw + frameW, wh = ch + frameH;
    const bool left = edge == 1 || edge == 4 || edge == 7;
    const bool top = edge == 3 || edge == 4 || edge == 5;
    if (left) l = r - ww;
    else r = l + ww;
    if (top) t = b - wh;
    else b = t + wh;
}

// The client size at the next VR start: 16:9 at the player's height (desktop_window_h, else desktop_window), never
// above the largest 16:9 client on the monitor.
inline void TargetClient(int userH, int defaultH, int maxClientH, int& cw, int& ch) {
    ch = userH >= kMinClientPx ? userH : defaultH;
    if (maxClientH > 0 && ch > maxClientH) ch = maxClientH;
    cw = W169(ch);
}

// The picture on the desktop window (08.4 step 4): DXGI stretches the whole backbuffer onto the client area, so a
// picture of another shape is drawn back into its own backbuffer first.
//   crop (the square 3D view): the middle band of the window's shape fills the backbuffer - no black bars;
//   fit (flat screens: menus, videos, loading - their edges hold text): the whole picture in the centred rectangle
//   whose stretch gives it its own shape, black around it.
struct DesktopFitPlan {
    bool draw = false;               // false: the shapes agree within 1 %, nothing to do
    bool crop = false;
    float u0 = 0, v0 = 0, u1 = 1, v1 = 1;  // the source part (crop)
    float x = 0, y = 0, w = 0, h = 0;      // the target rectangle in backbuffer pixels (fit)
};
inline DesktopFitPlan PlanDesktopFit(int bw, int bh, int winW, int winH, bool crop) {
    DesktopFitPlan p;
    if (bw <= 0 || bh <= 0 || winW <= 0 || winH <= 0) return p;
    const float pa = float(bw) / float(bh), wa = float(winW) / float(winH);
    if (std::abs(pa / wa - 1.f) < 0.01f) return p;
    p.draw = true;
    p.crop = crop;
    p.w = float(bw);
    p.h = float(bh);
    if (crop) {
        if (wa > pa) {  // the window is wider: the middle band of the picture's height
            const float f = pa / wa;
            p.v0 = 0.5f - 0.5f * f;
            p.v1 = 0.5f + 0.5f * f;
        } else {        // taller: the middle band of its width
            const float f = wa / pa;
            p.u0 = 0.5f - 0.5f * f;
            p.u1 = 0.5f + 0.5f * f;
        }
    } else if (wa > pa) {  // bars at the sides
        p.w = float(bw) * pa / wa;
        p.x = 0.5f * (float(bw) - p.w);
    } else {               // bars above and below
        p.h = float(bh) * wa / pa;
        p.y = 0.5f * (float(bh) - p.h);
    }
    return p;
}

}  // namespace hrvr::rendersize
