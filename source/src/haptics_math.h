#pragma once
// The game's pad rumble on the VR controllers (playbook 02.7 / REQ-05, GoW D-111), pure logic (tests/haptics_tests.cpp).
//
// STATIC (decompiled 2026-11-19, docs/TARGET_ADAPTER_MAP.md "Rumble"): Heavy Rain's rumble comes from the script effect
// QDT::SINT::PAD_VIBRATION_FX. Its update (vfunc 0x1403c5350) sets a level every frame through 0x1403c5ab0 -> the active
// input device's vfunc +0x20 (only while the game's own vibration option, *(DAT_141a33a20 + 0x38) == 1, is on), and its
// stop / destructor sets 0. The Steam pad (PC_CONTROLLER_STEAM slot 13, 0x140b4de70) then calls
// ISteamController::TriggerVibration(active pad +0xE8, m, m) with m = max(left, right byte) * 255 for BOTH motors. So a
// pad motor is a level that holds until the game changes it; OpenXR vibrations have a duration. The XR thread renews a
// short vibration every frame while the level is up and stops it when the level drops (GoW's RumbleTick).
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace hrvr::haptics {

constexpr float kMinAmplitude = 0.02f;     // below this a motor counts as off (GoW)
constexpr uint64_t kWatchdogMs = 10000;    // a level the game has not touched for 10 s is dropped (the effect updates
                                           // every frame; a lost "0" must not buzz forever)

// Steam's TriggerVibration speeds (0..65535; Heavy Rain sends up to 255 * 255) -> 0..1.
inline float FromSteamSpeed(uint16_t left, uint16_t right) {
    return float(std::max(left, right)) / 65535.f;
}

// The pad's level and the one-shot pulses (head switch confirmation), per hand.
struct Rumble {
    float level = 0.f;          // the game's motor level 0..1 (both motors are equal in Heavy Rain)
    uint64_t levelMs = 0;       // when the game last set it
    float strength = 1.f;       // hrvr.ini rumble_strength (0 = the game's rumble off)
    struct Pulse { float amplitude = 0.f; uint64_t untilMs = 0; } pulse[2];
    bool active[2] = {false, false};  // a vibration was applied on this hand and not yet stopped

    void Set(float value, uint64_t nowMs) {
        level = std::isfinite(value) ? std::clamp(value, 0.f, 1.f) : 0.f;
        levelMs = nowMs;
    }
    void AddPulse(int hand, float amplitude, uint64_t nowMs, uint32_t ms) {
        if (hand < 0 || hand > 1 || !std::isfinite(amplitude)) return;
        pulse[hand] = {std::clamp(amplitude, 0.f, 1.f), nowMs + ms};
    }
    void Reset() { *this = Rumble{strength}; }
    explicit Rumble(float s = 1.f) : strength(s) {}

    // The amplitude each hand should vibrate with now (0 = off).
    float Amplitude(int hand, uint64_t nowMs) const {
        float a = 0.f;
        if (level > 0.f && nowMs >= levelMs && nowMs - levelMs <= kWatchdogMs) a = std::min(1.f, level * strength);
        if (nowMs < pulse[hand].untilMs) a = std::max(a, pulse[hand].amplitude);
        return a >= kMinAmplitude ? a : 0.f;
    }

    // Playbook 22.4: no vibration while the session is not focused (headset off the face, focus lost). True when the
    // hand was vibrating and must be stopped now; the game's level is kept and plays again once focus returns.
    bool Unfocus(int hand) {
        const bool was = active[hand];
        active[hand] = false;
        return was;
    }

    // What the XR thread does for a hand this frame.
    enum class Action { None, Apply, Stop };
    Action Step(int hand, uint64_t nowMs, float* amplitude) {
        const float a = Amplitude(hand, nowMs);
        if (amplitude) *amplitude = a;
        if (a > 0.f) {
            active[hand] = true;
            return Action::Apply;
        }
        if (active[hand]) {
            active[hand] = false;
            return Action::Stop;
        }
        return Action::None;
    }
};

}  // namespace hrvr::haptics
