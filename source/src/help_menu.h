#pragma once
// 1.2 (owner 2026-11-22: "make prompts in first person appear in front of you, ... "At any time, you can listen to what
// your character thinks by pressing and holding down *button*" and a controller / keyboard / mouse png").
//
// DATA (offline, the game's own files): the exact English text is UTF-16 in the Text_Tuto menu data (and the shared
// text banks Pack_Design_1..7 / InGame_Pause / SceneIntro / MainMenu_V2): "At any time, you can listen to\r\n" "what
// your character thinks by\r\n" "pressing and holding down " U+0186 "." (U+0186 = the font's button glyph), key
// R2_Info. The 0201 zones _Var_ZTutoThinkA / B / C (and 43 more Text_Tuto scripts in 0201, 0401, 1200 ...) show it:
// MENUS_MASTER_CONTROLLER SetMenusMasterData("Text_Tuto"), SetMenusMasterComParameter(VarMenuMasterParam),
// VarMenuMasterParam:SetIntVariable("iInitText", n), ExecuteTriggerMMAction, SetAddress(<room locator, e.g.
// ETHANHOUSEBEFORE_Var.AdFromTutoText26a>), Set2DScale. Its package carries the pictures PAD_Info_R2 / SteamPad_info_R2
// / XBOXPad_info_R2 / X360Pad_info_R2 / HEAVY_RAIN_PC_MOUSE_* / Gyro_CC_KeyBoard and MC_TUTO_INFO1..21.
// STATIC: SetAddress (0x1402c3460 -> 0x1402c1760) writes the locator's world matrix into the param object +0x80 and
// sets +0x57; SetScriptEntity (0x1402c17e0) sets +0xC8 / +0x57. The menu draw IHM_MENUS::Display3D 0x14087b8f0 (from
// the viewport pass, every eye render) walks its entries (menu-data handle, param block = object +0x50) and copies the
// block's +0x30 matrix (0x140174bb0) - so the text floats at a fixed place in the ROOM, never in front of a first-person
// view, and never passes MPAR::UpdateElement (no USER_ACTION) - the 0.36.12 prompt route could not see it.
// Menu data ids (class 0x32c9, unique in GameData_WIN / BigFile_WIN .pdbg): the tutorial / help family below. Other
// menus placed with SetAddress (main menu scene, SceneIntro chapter titles, ARI glasses, GPS, SubInfo) stay put.
#include <cmath>
#include <cstdint>

namespace hrvr::helpmenu {

constexpr uint32_t kMenuDataClass = 0x32c9;

// The tutorial / help menus brought in front of a first-person view; nullptr for every other menu.
inline const char* Name(uint32_t cls, uint32_t id) {
    if (cls != kMenuDataClass) return nullptr;
    switch (id) {
        case 0x0a: return "Text_Tuto";   // tutorial texts at room locators (the thoughts text, R2_Info)
        case 0x19: return "GyroHelp";    // motion help on the character
        case 0x21: return "HeadHelp";    // help at the head (MC_TUTO_INFO pictures)
        case 0x12a: return "R1Help";
        case 0x12f: return "R2Help";     // the thoughts button help
        default: return nullptr;
    }
}

// 1.3 (owner 2026-11-22: "some toughts that should alreaydy appear in front of me like when pressing L2 "SHOWER"").
// DATA: "SHOWER" is the thought HR_0201_THOUGHTS_SHOWER (class 0x3f8, package _0201_PROLOGUE_BIRTHDAY_Var_ZShower; ENG
// line "I'd better take a shower and get dressed before I go downstairs..." + word "SHOWER", UTF-16 big-endian in its
// LOCALIZ_ chunk). STATIC: the THOUGHTS animator (0x1404ff340, ring 0x140501250: up to 4 words in angle slots, a spring
// toward each slot, the slot frame from camera matrices) writes every word's menu block, and the words are drawn by
// IHM_MENUS::Display3D as menu Think_Main (dialogue choices: Speak_Main). The mod moves only the ANCHOR in front (0.7.0),
// so the words orbit a point 0.9 m ahead - the near side of the ring reaches the eye, the far side and the flanks leave
// the view. Think words near a moved player anchor are laid flat in front of it (Flatten).
inline bool IsThoughtWords(uint32_t cls, uint32_t id) { return cls == kMenuDataClass && (id == 0x17 || id == 0x18); }

// A word's offset from its anchor in the view's frame (side = left of the view, up, depth = away from the eye) laid
// into a shallow slab facing the eye: 70 % of the spread, at most 35 cm across / 22 cm up or down, depth 15 % (max 8 cm,
// which keeps front and back words apart in stereo).
struct Flat {
    float side, up, depth;
};
inline Flat Flatten(float side, float up, float depth) {
    auto clampf = [](float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; };
    return {clampf(side * 0.7f, -0.35f, 0.35f), clampf(up * 0.7f, -0.22f, 0.22f), clampf(depth * 0.15f, -0.08f, 0.08f)};
}

// Size factor for a menu moved to `target` m from the eye: the size it had on the game's own screen (seen from the
// game camera at `designDist` m) is kept, never larger than the game's own, never under 15 %.
inline float Scale(float designDist, float target) {
    if (!(designDist > 0.f) || !std::isfinite(designDist) || !(target > 0.f)) return 1.f;
    const float s = target / designDist;
    return s < 0.15f ? 0.15f : s > 1.f ? 1.f : s;
}

}  // namespace hrvr::helpmenu
