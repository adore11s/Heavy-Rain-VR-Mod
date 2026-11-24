#pragma once
// 1.7.1 (owner 2026-11-24: "when im in f1 and get near a prompt of any kind, it disables my rotations, please find a good
// fix"). 1.6.2 paused right-stick turning while ANY prompt that reads the right stick was on offer (playbook 7E "give the
// stick back to the game") - and Heavy Rain offers right-stick prompts beside most things it lets you touch. But each
// prompt only reacts to ONE kind of push (STATIC, decompiled):
// - MPAR_ELEMENT_AXIS_TRANSLATION (check 0x140459b70, setter 0x1404597f0): ONE axis by mode +0x228 - 0 left X, 1 left Y,
//   2 right X (inputs 0x68 = left part, 0x69 = right part), 3 right Y (0x6A / 0x6B), 6 right Y (0x101 / 0x102), 7..12
//   other inputs; it engages only when that axis moves from +0x22C toward +0x230 (progress (v - from) / (to - from)).
//   So a right-Y prompt never sees a sideways turn, and a right-X prompt only the push toward its own side.
// - MPAR_ELEMENT_AXIS_ROTATION (the semicircle, 0x14045df90, port tests/ari_rotation_port.h): stick +0x220, enters only
//   at its start point (-0.43 .. +0.87 rad along the direction +0x224 from the start angle +0x228; angle 0 = up,
//   +pi/2 = right). A sideways push engages it only if that side lies in that window.
// - button types: the input ids they accept (+0x88 list): 0x68 / 0x69 right stick left / right, 0x6A / 0x6B / 0x101 /
//   0x102 right stick up / down.
// - MPAR_ELEMENT_AXIS_DIAGONAL: not decoded - it still pauses turning both ways.
// So turning is paused only for the side a prompt on offer could take; while any right-stick prompt is on offer a turn
// must be a SIDEWAYS push (|x| >= |y|), so the up / down / arc pushes the prompts want never turn the view. Pure: tests/
// turn_gate_tests.cpp (against the ported semicircle check).
#include <cmath>
#include <cstdint>

namespace hrvr::turngate {

enum : uint8_t { kBlockLeft = 1, kBlockRight = 2, kSideways = 4 };
constexpr float kTurnCone = 0.7853982f;  // a turn push is within 45 deg of sideways (|y| <= |x|)

inline uint8_t ForTranslation(int mode, float from, float to) {
    if (mode == 2) {
        const float d = to - from;
        return uint8_t(kSideways | (d > 0.f ? kBlockRight : d < 0.f ? kBlockLeft : kBlockLeft | kBlockRight));
    }
    if (mode == 3 || mode == 6) return kSideways;
    return 0;  // left stick (walking) or other inputs
}

// The semicircle's entry window holds an angle a (0 up, +pi/2 right)?
inline bool InRotationWindow(float a, float start, int dir) {
    float d = std::remainder(a - start, 6.2831855f);
    if (dir == 1) d = -d;
    return d > -0.43f && d < 0.87f;
}

inline uint8_t ForRotation(int stick, float start, int dir) {
    if (stick != 1) return 0;
    uint8_t m = kSideways;
    for (int side = 0; side < 2; ++side) {  // left -pi/2, right +pi/2, any push within the turn cone
        const float centre = side ? 1.5707963f : -1.5707963f;
        bool hit = false;
        for (int k = -8; k <= 8 && !hit; ++k) hit = InRotationWindow(centre + kTurnCone * float(k) / 8.f, start, dir);
        if (hit) m = uint8_t(m | (side ? kBlockRight : kBlockLeft));
    }
    return m;
}

inline uint8_t ForButtonInput(int id) {
    if (id == 0x68) return kSideways | kBlockLeft;
    if (id == 0x69) return kSideways | kBlockRight;
    if (id == 0x6A || id == 0x6B || id == 0x101 || id == 0x102) return kSideways;
    return 0;
}

constexpr uint8_t kDiagonal = kSideways | kBlockLeft | kBlockRight;

// May the right stick (x, y) turn the view while prompts with these gates are on offer?
inline bool TurnAllowed(uint8_t gates, float x, float y) {
    if (x < 0.f && (gates & kBlockLeft)) return false;
    if (x > 0.f && (gates & kBlockRight)) return false;
    if ((gates & kSideways) && std::fabs(y) > std::fabs(x)) return false;
    return true;
}

}  // namespace hrvr::turngate
