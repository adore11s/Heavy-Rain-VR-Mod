#pragma once
// 1.5 (owner 2026-11-24, from the 3DMigoto "Experimental No PP" mod): the game's "yellow filter" on / off, live.
//
// STATIC (decompiled 2026-11-24, record PORT_HISTORY 1.5 / TARGET_ADAPTER_MAP "Colour pass gamma"): Heavy Rain's final
// colour pass (embedded shader source SIMPLE_FRAGMENT, cbuffer c8 "Gamma //[gammaR, gammaG, gammaB]") ends with
// oColor.rgb = pow(oColor.rgb, Gamma.rgb). The post pass 0x140a7d670 fills c8 with (menu brightness * -0.01 + g_GammaR /
// G / B); those three floats start at R 0.982 / G 1.02 / B 1.13 (blue darkened most = the yellow look) and only the Lua
// SetGammaCorrection(r, g, b) handler 0x1402783c0 writes them. No PP deletes the pow from the shaders; the VR mod gets
// the same picture by writing 1 / 1 / 1 (pow(x, 1) = x) while the filter is Off - the menu brightness slider keeps
// working - and puts the game's own values back when it is switched On.
//
// Pure logic (no game memory): GammaFilter::Apply is called once per camera Build on the game thread, the same thread
// that runs the scripts and the post pass.
namespace hrvr::gamma {

struct GammaFilter {
    static constexpr float kNeutral = 1.f;
    // The game's values, seen before the mod first wrote the neutral ones (the .data defaults until then).
    float game[3] = {0.982f, 1.02f, 1.13f};
    // The three globals hold the mod's neutral values.
    bool written = false;

    static bool IsNeutral(float* const ch[3]) {
        return *ch[0] == kNeutral && *ch[1] == kNeutral && *ch[2] == kNeutral;
    }

    // ch: the three globals (R, G, B). filterOn = the game's look. Returns true when it wrote them.
    bool Apply(float* const ch[3], bool filterOn) {
        if (!filterOn) {
            if (written && IsNeutral(ch)) return false;  // still ours
            // First time, or a script called SetGammaCorrection since: those are the game's values now.
            for (int i = 0; i < 3; ++i) game[i] = *ch[i];
            for (int i = 0; i < 3; ++i) *ch[i] = kNeutral;
            written = true;
            return true;
        }
        if (!written) return false;
        written = false;
        if (!IsNeutral(ch)) return false;  // a script set new values while Off: they are the game's, keep them
        for (int i = 0; i < 3; ++i) *ch[i] = game[i];
        return true;
    }
};

}  // namespace hrvr::gamma
