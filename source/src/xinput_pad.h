#pragma once
// 0.34.0 (playbook 10.1, the Beyond route of 0.16.7): the VR controllers as an XInput pad, for store builds that read
// pads through XInput instead of Steam Input (Heavy Rain on Epic: no steam_api64.dll import, XINPUT1_3 by ordinal 2 / 3 -
// reverse_engineering/legacy_epic_2019/imports_exports.json). Pure: the mapping and the merge rules, desk-tested in
// tests/xinput_tests.cpp; the hooks are in vrinput.cpp.
//
// Mapping = the virtual Steam pad's (vrinput.cpp Button(): the same 16 button cases), so both stores play the same.
// Merge with a real pad (Beyond): buttons OR, each trigger the larger value, each stick whichever source is pushed further.
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace hrvr::xpad {

// XINPUT_GAMEPAD's layout (Xinput.h), so a hook can copy it straight into XINPUT_STATE.Gamepad.
struct Pad {
    uint16_t buttons = 0;
    uint8_t leftTrigger = 0, rightTrigger = 0;
    int16_t lx = 0, ly = 0, rx = 0, ry = 0;
};
static_assert(sizeof(Pad) == 12);

// XInput button bits per B2S button case (vrinput.cpp): 0 start, 1 back, 2..5 dpad up / down / left / right, 6 A, 7 B,
// 8 X, 9 Y, 10 left bumper, 11 left trigger (analog), 12 left stick click, 13 right bumper, 14 right trigger (analog),
// 15 right stick click.
constexpr uint16_t kBits[16] = {0x0010, 0x0020, 0x0001, 0x0002, 0x0004, 0x0008, 0x1000, 0x2000,
                                0x4000, 0x8000, 0x0100, 0x0000, 0x0040, 0x0200, 0x0000, 0x0080};
constexpr uint16_t kA = 0x1000;

inline int16_t Axis(float v) {
    if (!std::isfinite(v)) return 0;
    return int16_t(std::lround(std::clamp(v, -1.f, 1.f) * 32767.f));
}
inline uint8_t Trigger(float v, bool pressed) {
    const float t = std::isfinite(v) ? std::clamp(v, 0.f, 1.f) : 0.f;
    return pressed ? uint8_t(255) : uint8_t(std::lround(t * 255.f));
}

// pressed[case]: the VR controllers' button cases; sticks in -1..1 (y up), triggers 0..1.
inline Pad FromVr(const bool pressed[16], float moveX, float moveY, float viewX, float viewY, float lt, float rt) {
    Pad p;
    for (int i = 0; i < 16; ++i)
        if (pressed[i]) p.buttons = uint16_t(p.buttons | kBits[i]);
    p.leftTrigger = Trigger(lt, pressed[11]);
    p.rightTrigger = Trigger(rt, pressed[14]);
    p.lx = Axis(moveX);
    p.ly = Axis(moveY);
    p.rx = Axis(viewX);
    p.ry = Axis(viewY);
    return p;
}

inline bool Empty(const Pad& p) {
    return !p.buttons && !p.leftTrigger && !p.rightTrigger && !p.lx && !p.ly && !p.rx && !p.ry;
}

inline Pad Merge(const Pad& real, const Pad& vr) {
    Pad o;
    o.buttons = uint16_t(real.buttons | vr.buttons);
    o.leftTrigger = std::max(real.leftTrigger, vr.leftTrigger);
    o.rightTrigger = std::max(real.rightTrigger, vr.rightTrigger);
    const auto len2 = [](int16_t x, int16_t y) { return int64_t(x) * x + int64_t(y) * y; };
    if (len2(vr.lx, vr.ly) > len2(real.lx, real.ly)) o.lx = vr.lx, o.ly = vr.ly;
    else o.lx = real.lx, o.ly = real.ly;
    if (len2(vr.rx, vr.ry) > len2(real.rx, real.ry)) o.rx = vr.rx, o.ry = vr.ry;
    else o.rx = real.rx, o.ry = real.ry;
    return o;
}

// The game's rumble (XInputSetState: two 16-bit motors) as one VR haptic level 0..1: the stronger motor.
inline float RumbleLevel(uint16_t left, uint16_t right) { return float(std::max(left, right)) / 65535.f; }

}  // namespace hrvr::xpad
