#include "fpcam.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#include "bones.h"
#include "camdirector.h"
#include "fingers_math.h"
#include "fp_math.h"
#include "fpbody.h"
#include "fpbody_math.h"
#include "fpmode.h"
#include "game.h"
#include "hotkeys.h"
#include "log.h"
#include "player.h"
#include "prompts.h"
#include "vrinput.h"

namespace hrvr::fpcam {
namespace {

constexpr float kPi = 3.14159265f, kTwoPi = 6.28318531f;

std::recursive_mutex g_mutex;  // Prepare (render thread), Frame (Present), commands
bool g_ready = false;
bool g_f1 = false, g_f3 = false;
hotkeys::Edge g_f1Key, g_f3Key;

// Settings (hrvr.ini [hrvr]).
// Eye point from the Head bone (HumanIK "Head" is the top of the neck): B2S measured 9 cm forward and
// 7 cm up on its rig (6 cm / 0 put the view at the back of the skull). fp_eye_fwd / fp_eye_up.
float g_eyeForward = 0.17f, g_eyeUp = 0.07f;  // 0.7.0: 8 cm further forward (owner)
float g_f3EyeForward = 0.f;  // 0.36.6: F3 view between the eye bones, moved this far along the view (fp_f3_eye_fwd)
bool g_f3FromEyes = false;   // the last F3 view came from the eye bones
bool g_snapTurn = false;
float g_snapStep = 0.78539816f;  // 45 deg
float g_turnSpeed = 1.f;         // smooth turning: fraction of 120 deg/s (panel: 20 % .. 100 %)
bool g_autoWalk = true;          // F1: hold R2 (walk) while the left stick is pushed
float g_near = 0.05f;            // m; the game's near plane is set for cameras metres away
// 0.13.0 (owner: "the head hiding is bad - make the head never disappear, but a radius around the head that removes it
// from the first-person view"): fp_hidehead=clip (default) keeps the head in the pose - mirrors, reflections and
// shadows show it as the game draws it - and the first-person eye passes do not draw anything nearer than
// fp_head_clip (0.20 m since 0.36.0) in front of the eyes. The eye sits 17 cm in front of the head bone, so the head is behind
// it or inside that radius. =collapse is the old head collapse into the neck, =off shows everything.
// 0.36.0 (owner 2026-11-20: "make the near VR headset radius of disappearing things bigger in first person (F1 and
// F3)"): 0.20 m (was 0.12).
float g_headClip = 0.20f;
int g_headMode = 2;  // 0 off, 1 clip, 2 collapse (= the panel's Hide head On; default since 1.0.2, owner)
// Render-only body (fpbody.h): head hidden (F1 and F3), body turned to the view and arms on the controllers (F1).
bool g_hideHead = true, g_turnBody = true, g_arms = true, g_idleHold = true;
bool g_bodyFollow = true;  // 0.36.12: fp_body_follow - the drawn body under the headset (F1)
// 0.14.0 (owner: "sometimes when I get near a prompt it disables the motion controls"): the 3 s arm handover after a
// performed action is optional and off by default (fp_arms_handover=on brings it back).
// 0.35.0 (owner 2026-11-20: "make it so that motion controls cant start when F3 is on ... when I move the left joystick
// it activates motion controls during F3"): 0.14.0's fp_arms_scenes (tracked arms in F3 scripted scenes while the
// movement stick was used) is removed - in F3 the arms are always the game's. A leftover key in hrvr.ini is ignored.
bool g_armsHandover = false;
// The game animates the hands while it performs an action the player started (a gesture or a button while action
// prompts are on offer): the arms are the game's until 3 s after the last such input.
uint64_t g_interactUntil = 0;
const char* g_armsWhy = "off";
float g_wristPitch = 0.f, g_wristBack = 0.07f;  // radians / metres (fp_wrist_pitch degrees, fp_wrist_back)
float g_drawnBodyYaw = 0.f;                     // smoothed heading of the drawn body (B2S 3.7)
bool g_handSmoothing = true;                    // hand_smoothing (playbook 08.1), drawn hands only
fpbody::SmoothHand g_handFilter[2];
// 0.22.0 (playbook 14.12): finger poses from the controllers' grip, trigger and touch sensing.
bool g_fingers = true, g_fingerRest = true;  // fp_fingers, fp_fingers_rest_shape
fingers::Amounts g_curl[2];
bool g_touchSeen[2] = {false, false};
bool g_wristPitchSet = false;                   // fp_wrist_pitch given: it wins over the headset family keys
int g_family = -1;                              // 0 other, 1 PlayStation VR2, 2 Quest (playbook 08.3)

// Per-skeleton rig facts (reset when the skeleton changes: new character, outfit or scene).
struct Rig {
    void* skeleton = nullptr;
    int head = -1, nose = -1, lArm = -1, rArm = -1, lHand = -1, rHand = -1;
    int lEye = -1, rEye = -1;  // 0.36.6: LeftEye / RightEye (every playable rig and all 56 character rigs with a nose bone)
    // 0.36.6: the face's level = head -> nose elevation in the rig's BIND pose (fpbody::RestElevation), read once per
    // rig. STATIC + DATA: 22 .. 44 deg over the game's character rigs; Shelby 37.1 = his walking value in the live dump.
    float bindLevel = 0.f;
    bool bindLevelKnown = false, bindLevelLogged = false;
    float rootFront = 0.f;  // face heading - root heading, learned while walking (0 or pi)
    bool rootFrontKnown = false;
    fp::FrontVote frontVote;  // 0.7.0: votes, then locked for this rig
} g_rig;
fp::StableEye g_eye;
int g_implausibleEyeFrames = 0;
constexpr int kLevelSamples = 600;  // 5 per second, the last 2 minutes
float g_levelSamples[kLevelSamples] = {};
int g_levelCount = 0, g_levelNext = 0;
uint64_t g_levelAt = 0;

// View state.
void* g_cameraNode = nullptr;
pose::Mat4 g_base = pose::Identity();
bool g_baseValid = false;
float g_viewYaw = 0.f, g_viewPitch = 0.f;
fp::SnapTurn g_snap;
fp::Mode g_lastMode = fp::Mode::Cinematic;
fp::BehaviourClass g_lastKind = fp::BehaviourClass::Unknown;
uint64_t g_lastAnchor = 0;
LARGE_INTEGER g_qpcFreq{}, g_qpcLast{};

std::wstring IniPath() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring s(exe);
    return s.substr(0, s.find_last_of(L"\\/")) + L"\\hrvr.ini";
}

void ReadSettings() {
    const std::wstring ini = IniPath();
    wchar_t v[32];
    auto text = [&](const wchar_t* key, const wchar_t* def) {
        GetPrivateProfileStringW(L"hrvr", key, def, v, 32, ini.c_str());
        return std::wstring(v);
    };
    auto on = [&](const wchar_t* key, bool def) {
        const std::wstring s = text(key, def ? L"on" : L"off");
        return _wcsicmp(s.c_str(), L"on") == 0 || _wcsicmp(s.c_str(), L"true") == 0 || s == L"1";
    };
    auto number = [&](const wchar_t* key, float fallback, float lo, float hi) {
        const std::wstring s = text(key, L"");
        if (s.empty()) return fallback;
        const float x = float(_wtof(s.c_str()));
        return std::isfinite(x) && x >= lo && x <= hi ? x : fallback;
    };
    g_f1 = on(L"firstperson", false);
    g_f3 = on(L"fpcutscenes", false);
    g_eyeForward = number(L"fp_eye_fwd", g_eyeForward, -0.3f, 0.4f);
    g_f3EyeForward = number(L"fp_f3_eye_fwd", g_f3EyeForward, -0.1f, 0.2f);
    g_eyeUp = number(L"fp_eye_up", g_eyeUp, -0.3f, 0.4f);
    g_snapTurn = _wcsicmp(text(L"turn", L"smooth").c_str(), L"snap") == 0;
    g_snapStep = number(L"snap_turn_degrees", 45.f, 10.f, 90.f) * 0.017453293f;
    g_turnSpeed = number(L"turn_speed", 1.f, 0.2f, 1.f);
    g_autoWalk = on(L"fp_autowalk", true);
    g_near = number(L"fp_near", g_near, 0.f, 1.f);
    fpbody::SetEnabled(on(L"fp_body", true));
    {
        const std::wstring m = text(L"fp_hidehead", L"collapse");  // 1.0.2: head not drawn by default (owner)
        g_headMode = _wcsicmp(m.c_str(), L"collapse") == 0 ? 2
                     : (_wcsicmp(m.c_str(), L"off") == 0 || _wcsicmp(m.c_str(), L"false") == 0 || m == L"0") ? 0
                                                                                                           : 1;
        g_hideHead = g_headMode != 0;
        g_headClip = number(L"fp_head_clip", g_headClip, 0.02f, 0.4f);
    }
    g_turnBody = on(L"fp_turnbody", true);
    g_bodyFollow = on(L"fp_body_follow", true);
    g_arms = on(L"fparms", true);
    g_armsHandover = on(L"fp_arms_handover", false);
    if (!text(L"fp_arms_scenes", L"").empty())
        Log("FP fp_arms_scenes in hrvr.ini is no longer used (0.35.0): in F3 the arms are always the game's");
    g_idleHold = on(L"fp_idlehold", true);
    fpbody::SetMirrorHead(on(L"fp_mirror_head", true));
    fpbody::SetForearmTwistHelpers(_wcsicmp(text(L"fp_forearm_twist", L"helpers").c_str(), L"forearm") != 0);
    fpbody::SetItems(on(L"fp_items", true));
    fpbody::SetLooseItems(on(L"fp_items_loose", true));
    fpbody::SetItemSeam(on(L"fp_items_seam", true));
    fpbody::SetCloth(on(L"fp_cloth", true), on(L"fp_cloth_reset", true), on(L"fp_cloth_hair", true));
    g_wristPitchSet = !text(L"fp_wrist_pitch", L"").empty();
    g_wristPitch = number(L"fp_wrist_pitch", 0.f, -90.f, 90.f) * 0.017453293f;
    g_handSmoothing = on(L"hand_smoothing", true);
    g_fingers = on(L"fp_fingers", true);
    g_fingerRest = on(L"fp_fingers_rest_shape", true);
    g_wristBack = number(L"fp_wrist_back", g_wristBack, 0.f, 0.2f);
    prompts::SetEnabled(on(L"fp_prompts", true));
    prompts::SetAllFront(_wcsicmp(text(L"fp_prompts_front", L"all").c_str(), L"player") != 0);
}

void Save(const wchar_t* key, const wchar_t* value) { WritePrivateProfileStringW(L"hrvr", key, value, IniPath().c_str()); }
void SaveNumber(const wchar_t* key, float value) {
    wchar_t v[32];
    swprintf_s(v, L"%g", value);
    Save(key, v);
}

// Every F1 / F3 switch (key, head button, panel, command) is saved, so the panel and hrvr.ini agree.
void SetF1(bool on, const char* why) {
    g_f1 = on;
    Save(L"firstperson", on ? L"on" : L"off");
    Log("FP F1=%d (%s)", int(on), why);
}
void SetF3(bool on, const char* why) {
    g_f3 = on;
    Save(L"fpcutscenes", on ? L"on" : L"off");
    Log("FP F3=%d (%s)", int(on), why);
}

bool BoneAt(int index, pose::Mat4* out) { return g_rig.skeleton && index >= 0 && bones::BoneWorld(g_rig.skeleton, index, out); }
pose::Vec3 Pos(const pose::Mat4& m) { return {m.m[12], m.m[13], m.m[14]}; }

void NewRig(void* skeleton) {
    g_rig = Rig{};
    g_rig.skeleton = skeleton;
    g_rig.head = bones::BoneIndex(skeleton, bones::Crc32("Head"));
    g_rig.nose = bones::BoneIndex(skeleton, bones::Crc32("MNoseUp"));
    g_rig.lEye = bones::BoneIndex(skeleton, bones::Crc32("LeftEye"));
    g_rig.rEye = bones::BoneIndex(skeleton, bones::Crc32("RightEye"));
    g_rig.bindLevelKnown = g_rig.bindLevelLogged = false;
    g_rig.lArm = bones::BoneIndex(skeleton, bones::Crc32("LeftArm"));
    g_rig.rArm = bones::BoneIndex(skeleton, bones::Crc32("RightArm"));
    g_rig.lHand = bones::BoneIndex(skeleton, bones::Crc32("LeftHand"));
    g_rig.rHand = bones::BoneIndex(skeleton, bones::Crc32("RightHand"));
    g_eye.Reset(reinterpret_cast<uintptr_t>(skeleton));
    g_implausibleEyeFrames = 0;
    g_levelCount = g_levelNext = 0;
    g_levelAt = 0;
    bones::Describe(skeleton);
}

// Face elevation samples (head -> nose above the horizontal): their median is the rig's "level" (a face
// looks about level most of the time; B2S 0.16.40). Pitch = elevation - level.
// 0.36.6 (owner 2026-11-20: "sometimes in f3 the view goes bit too much high"; LIVE 0.36.5: F3 "level" -45 / -27 / -33
// deg on a rig whose level is +37, pitch up to +55 / -72 deg): the median also took the cutscenes' frames, where faces
// look down for minutes. Now the rig's bind-pose level is used (all rigs); the median is the fallback, fed only while
// the player WALKS the character (`learn`), and 0 until it has a sample.
float Level(float elevation, uint64_t now, bool learn) {
    if (!g_rig.bindLevelKnown && g_rig.head >= 0 && g_rig.nose >= 0) {
        float lv = 0.f;
        if (fpbody::RestElevation(g_rig.skeleton, g_rig.head, g_rig.nose, &lv) && lv > -0.35f && lv < 1.05f) {
            g_rig.bindLevel = lv;
            g_rig.bindLevelKnown = true;
        }
    }
    if (g_rig.bindLevelKnown) {
        if (!g_rig.bindLevelLogged) {
            g_rig.bindLevelLogged = true;
            Log("FP face level %.1f deg from the rig's bind pose (head -> nose); F3 pitch = face elevation - this",
                g_rig.bindLevel * 57.29578f);
        }
        return g_rig.bindLevel;
    }
    if (!learn && g_levelCount == 0) return elevation;  // nothing learned while walking yet: no pitch from the face
    if (learn && (g_levelCount == 0 || now - g_levelAt >= 200)) {
        g_levelAt = now;
        g_levelSamples[g_levelNext] = elevation;
        g_levelNext = (g_levelNext + 1) % kLevelSamples;
        g_levelCount = std::min(g_levelCount + 1, kLevelSamples);
    }
    float sorted[kLevelSamples];
    std::copy(g_levelSamples, g_levelSamples + g_levelCount, sorted);
    std::nth_element(sorted, sorted + g_levelCount / 2, sorted + g_levelCount);
    return sorted[g_levelCount / 2];
}

void Suspend() {
    g_baseValid = false;
    g_cameraNode = nullptr;
    g_lastMode = fp::Mode::Cinematic;
    vrinput::SetFirstPersonMove({});
    prompts::PublishView(false, {}, {0, 0, -1}, {});
    fpbody::Suspend();
}

// Right hand at the head + one tap of B / O (GoW D-252; vrinput HeadSwitch; playbook 10.3 revision 17: at any time,
// prompts or not): while the player walks the character it switches F1, otherwise (a cutscene, a scripted scene) F3.
// Polled before anything else, so it also turns first person ON (0.5.0 polled the both-thumbsticks combo after the
// "both modes off" exit: it could never switch first person on).
void HeadButtonSwitch() {
    if (!vrinput::ConsumeHeadSwitch()) return;
    const player::State st = player::Poll();
    if (st.control == player::Control::Walking) SetF1(!g_f1, "right hand at the head + B / O (walking)");
    else SetF3(!g_f3, "right hand at the head + B / O (cutscene)");
}

const char* ModeName(fp::Mode m) {
    return m == fp::Mode::FirstPerson ? "first person" : m == fp::Mode::NativeFirstPerson ? "the game's own first person" : "game camera";
}

}  // namespace

void Install() {
    std::lock_guard lock(g_mutex);
    ReadSettings();
    const bool director = camdirector::Install();
    fpbody::Install();
    prompts::Install();
    g_ready = game::Va(game::vt::Character) && game::Va(game::vt::ThirdPersonGameplay) &&
              game::Addr(game::Anchor::NodeUpdateWorld);
    QueryPerformanceFrequency(&g_qpcFreq);
    Log("FPCAM install: %s (director=%d) F1=%d F3=%d eye fwd=%.3f up=%.3f turn=%s %.0f autowalk=%d near=%.3f", g_ready ? "ready" : "UNAVAILABLE",
        int(director), int(g_f1), int(g_f3), g_eyeForward, g_eyeUp, g_snapTurn ? "snap" : "smooth", g_snapStep * 57.29578f,
        int(g_autoWalk), g_near);
    Log("FPCAM head: %s (clip radius %.3f m around the eyes in the first-person views)",
        g_headMode == 1 ? "kept, clipped from the view" : g_headMode == 2 ? "collapsed into the neck" : "shown", g_headClip);
}

void Frame() {
    std::lock_guard lock(g_mutex);
    if (g_f1Key.Feed(VK_F1)) SetF1(!g_f1, "key");
    if (g_f3Key.Feed(VK_F3)) SetF3(!g_f3, "key");
    // Playbook 17.3 / 11.3 (GoW F5 / F6): the drawn hands' wrist pitch in 5 degree steps, saved for this headset family
    // (wristpitch_psvr2 / _quest / _other), or in fp_wrist_pitch when that override is set. F5 = fingers up (-5),
    // F6 = fingers down (+5): positive pitch turns the fingers away from the thumb side (fp_math.h GripHandBasis).
    // HRVR has pitch only: no wrist yaw / roll keys (owner 2026-11-20, playbook revision 32).
    static hotkeys::Edge f5, f6;
    const bool up = f5.Feed(VK_F5), down = f6.Feed(VK_F6);
    if ((up || down) && !g_wristPitchSet && g_family < 0) {
        Log("FP wrist pitch: %s ignored - the headset family is not known yet (VR not started)", up ? "F5" : "F6");
    } else if (up || down) {
        const float deg = std::clamp(std::round(g_wristPitch * 57.29578f) + (down ? 5.f : -5.f), -90.f, 90.f);
        g_wristPitch = deg * 0.017453293f;
        const wchar_t* key = g_wristPitchSet ? L"fp_wrist_pitch"
                             : g_family == 1 ? L"wristpitch_psvr2" : g_family == 2 ? L"wristpitch_quest" : L"wristpitch_other";
        SaveNumber(key, deg);
        Log("FP wrist pitch %.0f deg (%s; saved as %ls): the drawn fingers turn %s", deg, up ? "F5" : "F6", key,
            up ? "up" : "down");
    }
}

void Prepare(void* cameraNode, const pose::Mat4& authored, const xr::FrameInput& input, bool eligible) {
    std::lock_guard lock(g_mutex);
    const uint64_t now = GetTickCount64();
    HeadButtonSwitch();
    // Both modes off: nothing to find (no heap scans for players who never use first person).
    if (!g_ready || !cameraNode || !fp::ValidMatrix(authored) || (!g_f1 && !g_f3)) {
        Suspend();
        return;
    }
    // Headset not focused (SteamVR dashboard): keep the last first-person view instead of dropping out
    // and back in (B2S: re-entering came back flipped).
    if (!xr::FrameInputUsable(input, now)) {
        if (!(g_baseValid && cameraNode == g_cameraNode)) Suspend();
        return;
    }
    player::SetCamera(authored.m[12], authored.m[13], authored.m[14]);  // 0.36.8: the character choice prefers nearby ones
    const player::State st = player::Poll();
    void* behaviour = camdirector::Active();
    fp::BehaviourClass kind = st.control == player::Control::Walking    ? fp::BehaviourClass::Gameplay
                              : st.control == player::Control::Cutscene ? fp::BehaviourClass::Cutscene
                                                                        : fp::BehaviourClass::Unknown;
    if (camdirector::Classify(behaviour) == fp::BehaviourClass::NativeFirstPerson) kind = fp::BehaviourClass::NativeFirstPerson;
    else if (!eligible) kind = fp::BehaviourClass::Unknown;
    const fp::Mode mode = fp::Classify(kind, g_f1, g_f3);
    {
        static int lastState = -1;
        const int state = int(kind) * 4 + int(mode) + (st.character ? 0 : 64) + (eligible ? 0 : 128);
        if (state != lastState) {
            lastState = state;
            Log("FP state: control=%s camera=%s F1=%d F3=%d%s -> %s%s", player::Name(st.control), camdirector::Name(behaviour),
                int(g_f1), int(g_f3), eligible ? "" : " (no single full-screen view)", ModeName(mode),
                st.character ? "" : " (player character not found yet)");
        }
    }
    if (mode != fp::Mode::FirstPerson || !st.character) {
        Suspend();
        return;
    }
    void* node = bones::CharacterNode(st.character);
    void* skeleton = bones::Skeleton(node);
    if (!node || !skeleton) {
        Suspend();
        return;
    }
    const bool newRig = skeleton != g_rig.skeleton;
    if (newRig) NewRig(skeleton);
    pose::Mat4 head{}, root{}, nose{};
    const bool haveHead = BoneAt(g_rig.head, &head);
    const bool haveRoot = bones::NodeWorld(node, &root);
    const bool haveNose = BoneAt(g_rig.nose, &nose);
    // Cutscenes cut between characters: the player's character may not be in this shot at all (another
    // place, or parked far away). F3 only takes over while the shot is near that character's head.
    if (kind == fp::BehaviourClass::Cutscene && haveHead) {
        const float dx = head.m[12] - authored.m[12], dy = head.m[13] - authored.m[13], dz = head.m[14] - authored.m[14];
        const bool distant = dx * dx + dy * dy + dz * dz > 20.f * 20.f;
        static bool reported = false;
        if (distant != reported) {
            reported = distant;
            Log(distant ? "FP F3: the shot is more than 20 m from the player's character: game camera"
                    : "FP F3: the shot is near the player's character again");
        }
        if (distant) {
            Suspend();
            return;
        }
    }
    // A character the game is not animating (bind pose at a scene change): game camera until it moves.
    {
        pose::Mat4 la{}, ra{}, lh{}, rh{};
        static bool reported = false;
        const bool tposed = BoneAt(g_rig.lArm, &la) && BoneAt(g_rig.rArm, &ra) && BoneAt(g_rig.lHand, &lh) &&
                            BoneAt(g_rig.rHand, &rh) && fp::LooksLikeTPose(Pos(la), Pos(lh), Pos(ra), Pos(rh));
        if (tposed != reported) {
            reported = tposed;
            Log(tposed ? "FP the character is in its bind pose (T-pose): game camera until it is animated"
                       : "FP the character is animated again: first person resumes");
        }
        if (tposed) {
            Suspend();
            return;
        }
    }
    const bool entering = newRig || g_lastMode != mode || g_lastKind != kind || !g_baseValid;
    g_lastKind = kind;
    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);
    const float dt = !entering && g_qpcLast.QuadPart && g_qpcFreq.QuadPart
                         ? std::min(float(double(qpc.QuadPart - g_qpcLast.QuadPart) / double(g_qpcFreq.QuadPart)), 0.1f)
                         : 0.f;
    g_qpcLast = qpc;

    // The face: heading and elevation of head -> nose (no front/back ambiguity, fpmode.h FaceDirection).
    float faceYaw = 0.f, faceElev = 0.f;
    const bool face = haveHead && haveNose && fp::FaceDirection(Pos(head), Pos(nose), &faceYaw, &faceElev);
    const float level = face ? Level(faceElev, now, kind == fp::BehaviourClass::Gameplay) : 0.f;
    // Which way the root faces relative to the face (models can face root -Z or +Z). 0.6.0 decided it again on every
    // frame where face and root differed by more than 90 deg (LIVE: 68 flips in one session: a fast turn, the
    // turn-around when walking backwards or the head looking aside turned the whole drawn body round, "the whole body
    // flips"). Now only frames where face and root clearly agree (within 30 deg) or clearly oppose (within 30 deg of
    // 180) vote, while the character walks; 40 votes with a 4:1 majority lock it for this rig.
    if (face && haveRoot && kind == fp::BehaviourClass::Gameplay && !g_rig.frontVote.locked) {
        const float diff = std::remainder(faceYaw - fp::YawOf(root), kTwoPi);
        if (g_rig.frontVote.Feed(diff)) {
            g_rig.rootFront = g_rig.frontVote.front;
            g_rig.rootFrontKnown = true;
            Log("FP the character's front is its root %s (locked: %d votes same, %d opposite)",
                g_rig.rootFront != 0.f ? "+Z" : "-Z", g_rig.frontVote.same, g_rig.frontVote.opposite);
        } else if (!g_rig.rootFrontKnown && g_rig.frontVote.same + g_rig.frontVote.opposite >= 5) {
            g_rig.rootFront = g_rig.frontVote.Majority();  // provisional until locked
        }
    }
    const float bodyYaw = haveRoot ? std::remainder(fp::YawOf(root) + g_rig.rootFront, kTwoPi) : faceYaw;

    const bool promptsShowing = prompts::Showing();
    if (entering) {
        // Where the character faces (Heavy Rain's authored cameras often look at the character from the
        // front, so B2S's "start along the game camera" would start F1 facing backwards).
        g_viewYaw = face ? faceYaw : haveRoot ? bodyYaw : fp::YawOf(authored);
        g_viewPitch = 0.f;
        g_snap = {};
        Log("FP entering %s: view yaw %.0f deg (game camera %.0f, face %s %.0f, elevation %.0f level %.0f)",
            kind == fp::BehaviourClass::Gameplay ? "F1" : "F3", g_viewYaw * 57.29578f, fp::YawOf(authored) * 57.29578f,
            face ? "read" : "unread", faceYaw * 57.29578f, faceElev * 57.29578f, level * 57.29578f);
    }

    if (haveHead && haveRoot) {
        if (kind == fp::BehaviourClass::Gameplay) {
            // F1: the right stick turns the view, except while action prompts are on offer (the stick does
            // the game's gestures then, B2S 7E); snap must be re-armed after them.
            const auto& right = input.hand[1];
            if (right.active && !promptsShowing) {
                if (!g_snapTurn) {
                    if (std::fabs(right.stick[0]) > 0.25f) g_viewYaw -= right.stick[0] * 2.0943951f * g_turnSpeed * dt;  // 120 deg/s
                } else if (const int step = g_snap.Step(right.stick[0], dt)) {
                    g_viewYaw -= float(step) * g_snapStep;
                }
                g_viewYaw = std::remainder(g_viewYaw, kTwoPi);
            } else {
                g_snap.Block();
            }
            // A stable eye (B2S 3.2): the head sampled in the root's yaw frame on plausible standing frames,
            // averaged; after ~2.5 s without one (a long crouch) take what there is.
            const bool plausible = fp::PlausibleStandingHead(head, root) || g_implausibleEyeFrames > 225;
            if (!plausible && !g_eye.Ready()) ++g_implausibleEyeFrames;
            g_eye.Sample(reinterpret_cast<uintptr_t>(skeleton), head, root, plausible);
            if (!g_eye.HasSample()) {
                Suspend();
                return;
            }
            pose::Mat4 next = g_eye.Base(root, bodyYaw, g_viewYaw, g_eyeForward, g_eyeUp);
            if (g_baseValid && !fp::PlausibleStep(Pos(g_base), Pos(next), 3.f)) {  // a teleport: start again
                g_eye.Reset(reinterpret_cast<uintptr_t>(skeleton));
                g_implausibleEyeFrames = 0;
                g_eye.Sample(reinterpret_cast<uintptr_t>(skeleton), head, root, fp::PlausibleStandingHead(head, root));
                if (!g_eye.HasSample()) {
                    Suspend();
                    return;
                }
                next = g_eye.Base(root, bodyYaw, g_viewYaw, g_eyeForward, g_eyeUp);
            }
            g_base = next;
        } else {
            // F3: the view on the head, facing where the face points, pitched by the face against the rig's
            // level, both smoothed (tau 0.15 s) so animation twitches do not shake the view.
            float yaw = face ? faceYaw : bodyYaw;
            float pitch = face ? std::clamp(faceElev - level, -1.25f, 1.25f) : 0.f;
            const float t = 1.f - std::exp(-dt / 0.15f);
            g_viewYaw = entering ? yaw : fp::ApproachAngle(g_viewYaw, yaw, t);
            g_viewPitch = entering ? pitch : g_viewPitch + (pitch - g_viewPitch) * t;
            // 0.36.6 (owner: "the view goes bit too much high ... not real from ... eyes ... on face"; offline on the
            // live dumps: head + 7 cm world-up + 17 cm ahead sat up to 9 cm ABOVE and ~8 cm in front of the real eyes
            // whenever the head tipped forward, because both offsets ignore the head's own tilt): the view sits between
            // the rig's two eye bones, which move with the skull - every rig the player can be has them. Rigs without
            // them keep the old offsets. fp_f3_eye_fwd (default 0) moves it along the view.
            pose::Mat4 le{}, re{};
            if (BoneAt(g_rig.lEye, &le) && BoneAt(g_rig.rEye, &re)) {
                pose::Mat4 mid = head;
                for (int i = 12; i < 15; ++i) mid.m[i] = 0.5f * (le.m[i] + re.m[i]);
                g_base = fp::PitchedBase(fp::FirstPersonBase(mid, g_viewYaw, g_f3EyeForward, 0.f), g_viewYaw, g_viewPitch);
                g_f3FromEyes = true;
            } else {
                g_base = fp::PitchedBase(fp::FirstPersonBase(head, g_viewYaw, g_eyeForward, g_eyeUp), g_viewYaw, g_viewPitch);
                g_f3FromEyes = false;
            }
        }
        g_lastAnchor = now;
    } else if (!g_baseValid || !g_lastAnchor || now - g_lastAnchor > 500) {
        Suspend();
        return;
    }
    g_baseValid = true;
    g_cameraNode = cameraNode;
    g_lastMode = mode;

    const pose::Mat4 headWorld = pose::Mul(input.headRel, g_base);
    const pose::Vec3 gaze = fp::Norm(pose::Vec3{-headWorld.m[8], -headWorld.m[9], -headWorld.m[10]});
    const pose::Vec3 gameCamera{authored.m[12], authored.m[13], authored.m[14]};
    prompts::PublishView(true, Pos(headWorld), gaze, Pos(head), &gameCamera);
    // The UI screen follows the gaze lazily in first person (xr.h SetUiFollow). 0.11.0: also while the game is paused
    // with the 3D scene still drawn - Heavy Rain pauses itself under its tutorial texts ("For actions requiring timing
    // and precision ...", LIVE 0.10.0: paused=1 with 3D frames, the screen stayed put). Prepare only runs on 3D frames,
    // so the pause menu (2D only) still lets the screen ease back beside the settings panel.
    xr::SetUiFollow();
    // 0.36.12: the game paused itself under a tutorial text (3D still drawn, so this runs): the screen comes in front
    if (const auto* paused = game::Global<uint8_t>(game::g::GamePaused); paused && *paused) xr::SetUiFocus();
    if (kind == fp::BehaviourClass::Gameplay) {
        // The game turns the stick by its camera (CtrlMode 0 CAMERA_RELATIVE) or by the character
        // (1 ROOT_CHARACTER_RELATIVE, 2 CHARACTER_RELATIVE): rotate it from the headset's heading into that frame.
        const float reference = st.ctrlMode == 0 ? fp::YawOf(authored) : bodyYaw;
        vrinput::SetFirstPersonMove({true, std::remainder(fp::YawOf(headWorld) - reference, kTwoPi), g_autoWalk && !promptsShowing});
    } else {
        vrinput::SetFirstPersonMove({});
    }
    // The render-only body plan for this frame (fpbody.h).
    {
        fpbody::Plan plan;
        plan.active = true;
        plan.skeleton = skeleton;
        plan.node = node;
        {
            void* others[2] = {nullptr, nullptr};
            const int n = player::Partners(st.character, others, 2);
            for (int k = 0; k < n; ++k) plan.partner[k] = bones::Skeleton(bones::CharacterNode(others[k]));
        }
        plan.hideHead = g_hideHead && g_headMode == 2;  // clip mode keeps the head in the pose
        plan.facingYaw = bodyYaw;
        plan.pivot = Pos(g_base);
        const bool walking = kind == fp::BehaviourClass::Gameplay;
        // The drawn body follows the view while walking (tau 0.12 s); standing, the head turns freely within
        // +-35 deg and beyond that the body catches up (tau 0.2 s). In cutscenes it keeps the game's heading.
        const float want = walking ? fp::YawOf(headWorld) : bodyYaw;
        const bool moving = std::hypot(input.hand[0].stick[0], input.hand[0].stick[1]) > 0.2f;
        const float off = std::remainder(want - g_drawnBodyYaw, kTwoPi);
        if (entering || !walking) g_drawnBodyYaw = want;
        else if (moving || std::fabs(off) > 0.61f)
            g_drawnBodyYaw = fp::ApproachAngle(g_drawnBodyYaw, want, 1.f - std::exp(-dt / (moving ? 0.12f : 0.2f)));
        plan.turnBody = walking && g_turnBody;
        plan.bodyYaw = plan.turnBody ? g_drawnBodyYaw : bodyYaw;
        // 0.36.12 (owner: "make the body follow my vr headset like it does in beyond two souls"): the drawn body is
        // moved so its head is under the headset's eye (fpbody Edit, B2S EXP-036 / 0.16.12)
        plan.followHead = plan.turnBody && g_bodyFollow;
        plan.headEye = Pos(headWorld);
        plan.eyeForward = g_eyeForward;
        plan.eyeUp = g_eyeUp;
        // 0.6.0 (owner: "the motion controls work only sometimes ... during walking it becomes non motion controlled"):
        // Heavy Rain shows object prompts all the time while walking past things, and 0.5.0 gave the arms to the
        // game whenever any prompt was on offer. Now only an action the player performs (the right stick pushed or a
        // face button while prompts are on offer) gives them to the game, until 3 s after that input.
        // 0.8.0 (owner: "sometimes the motion controls turn off when I walk near prompts"): 0.6.0 took any right-stick
        // push or face button while prompts were on offer as an action, so turning while walking past objects gave
        // the arms away. Now an action counts only when it completes: input while prompts are on offer, then the
        // prompts go within 1 s, with the character not being walked (movement stick released for 0.5 s).
        const xr::HandInput& rh = input.hand[1];
        const bool acting = std::hypot(rh.stick[0], rh.stick[1]) > 0.5f || rh.a || rh.b || input.hand[0].a || input.hand[0].b;
        static uint64_t lastActing = 0, lastWalked = 0;
        static bool wasShowing = false;
        if (promptsShowing && acting) lastActing = now;
        if (std::hypot(input.hand[0].stick[0], input.hand[0].stick[1]) > 0.2f) lastWalked = now;
        if (g_armsHandover && wasShowing && !promptsShowing && lastActing && now - lastActing < 1000 && now - lastWalked > 500) {
            g_interactUntil = now + 3000;
            Log("FP an action prompt was done: the game animates the hands for 3 s (fp_arms_handover)");
        }
        wasShowing = promptsShowing;
        // The panel's "Motion controls (first person)" switches the tracked arms too (0.7.0, owner: "it's always motion
        // controlled even when you turn it off").
        const bool motion = vrinput::MotionControls();
        // Tracked arms only in F1 (the game's gameplay object walks the character). 0.35.0 (owner): never in F3 -
        // scripted scenes and cutscenes keep the game's arms even while the movement stick is used.
        const bool armed = walking;
        plan.arms = armed && g_arms && motion && now >= g_interactUntil;
        g_armsWhy = !armed     ? "not walking (F3)"
                    : !motion  ? "motion controls off"
                    : !g_arms  ? "fparms off"
                    : !plan.arms ? "the game performs an action"
                               : "on";
        // Idle hold (B2S): the drawn body holds still while the character stands.
        plan.idleHold = g_idleHold;
        plan.haveRoot = haveRoot;
        plan.root = root;
        plan.moveStick = moving;
        // Headset family (playbook 08.3): per-family wrist pitch (wristpitch_psvr2 / _quest / _other, degrees,
        // Beyond's grip-basis convention, default 0) unless fp_wrist_pitch is set.
        if (g_family < 0 && xr::SystemName()[0]) {
            g_family = fpbody::HeadsetFamily(xr::SystemName());
            const wchar_t* key = g_family == 1 ? L"wristpitch_psvr2" : g_family == 2 ? L"wristpitch_quest" : L"wristpitch_other";
            wchar_t v[32] = {};
            GetPrivateProfileStringW(L"hrvr", key, L"", v, 32, IniPath().c_str());
            if (!g_wristPitchSet && v[0]) g_wristPitch = std::clamp(float(_wtof(v)), -90.f, 90.f) * 0.017453293f;
            Log("FP headset family %s (system \"%s\"): wrist pitch %.0f deg", g_family == 1 ? "PlayStation VR2" :
                g_family == 2 ? "Quest" : "other", xr::SystemName(), g_wristPitch * 57.29578f);
        }
        // Playbook 17.1.2: hand_smoothing is read live - once a second, when hrvr.ini changed on disk. Off resets the
        // filters so the hands are raw at once; On starts from the current raw sample (a reset filter starts there).
        {
            static uint64_t lastCheck = 0;
            static FILETIME lastWrite{};
            if (now - lastCheck >= 1000) {
                lastCheck = now;
                WIN32_FILE_ATTRIBUTE_DATA fa{};
                if (GetFileAttributesExW(IniPath().c_str(), GetFileExInfoStandard, &fa) &&
                    CompareFileTime(&fa.ftLastWriteTime, &lastWrite) != 0) {
                    const bool first = lastWrite.dwLowDateTime == 0 && lastWrite.dwHighDateTime == 0;
                    lastWrite = fa.ftLastWriteTime;
                    wchar_t v[16] = {};
                    GetPrivateProfileStringW(L"hrvr", L"hand_smoothing", L"on", v, 16, IniPath().c_str());
                    const bool smooth = _wcsicmp(v, L"on") == 0 || _wcsicmp(v, L"true") == 0 || wcscmp(v, L"1") == 0;
                    if (!first && smooth != g_handSmoothing) {
                        g_handSmoothing = smooth;
                        g_handFilter[0] = g_handFilter[1] = {};
                        Log("FP hand_smoothing %s (hrvr.ini changed): drawn hands %s", smooth ? "on" : "off",
                            smooth ? "filtered from the current sample" : "raw at once");
                    }
                }
            }
        }
        for (int i = 0; i < 2; ++i) {
            plan.handValid[i] = input.hand[i].active && input.hand[i].gripValid;
            // Drawn hands are filtered in tracking space, before they are placed on the body (08.1).
            const pose::Mat4 grip = g_handSmoothing && plan.handValid[i] ? g_handFilter[i].Feed(input.hand[i].gripRel, dt)
                                                                          : input.hand[i].gripRel;
            if (!plan.handValid[i]) g_handFilter[i] = {};
            plan.grip[i] = pose::Mul(grip, g_base);
            // 14.12: finger amounts from what the controller senses, at most 12 units per second
            const xr::HandInput& hi = input.hand[i];
            fingers::Input fin;
            fin.trigger = hi.trigger;
            fin.squeeze = hi.squeeze;
            fin.touchIndex = hi.touchIndex;
            fin.touchThumb = hi.touchThumb;
            fin.touchKnown = hi.touchKnown;
            g_touchSeen[i] = hi.touchKnown;
            fingers::StepToward(&g_curl[i], fingers::FromInput(fin), dt);
            for (int f = 0; f < 5; ++f) plan.curl[i][f] = g_curl[i].a[f];
        }
        plan.fingers = g_fingers;
        plan.fingerRest = g_fingerRest;
        plan.wristPitch = g_wristPitch;
        plan.wristBack = g_wristBack;
        plan.ms = now;
        fpbody::Publish(plan);
    }
    static uint64_t lastDiag = 0;
    if (now - lastDiag >= 10000) {
        lastDiag = now;
        // 14.12: say every 10 s which path the fingers took, so a run tells it
        Log("FPFINGER fingers %s", !g_fingers ? "off (fp_fingers=off)"
                                  : (g_touchSeen[0] || g_touchSeen[1])
                                      ? "on (from finger touch: point, fist; thumb from grip)"
                                      : "on (no touch reported by the controllers: grip and trigger only)");
        Log("FPDIAG %s view yaw %.0f pitch %.0f eye (%.2f %.2f %.2f) head-root %.2f m ctrlMode %d face %s level %.0f (%s) "
            "prompts %d arms %s%s",
            kind == fp::BehaviourClass::Gameplay ? "F1" : "F3", g_viewYaw * 57.29578f, g_viewPitch * 57.29578f, g_base.m[12],
            g_base.m[13], g_base.m[14], haveRoot ? head.m[13] - root.m[13] : 0.f, st.ctrlMode, face ? "read" : "unread",
            level * 57.29578f, g_rig.bindLevelKnown ? "bind pose" : "learned while walking", int(promptsShowing), g_armsWhy,
            kind == fp::BehaviourClass::Gameplay ? "" : g_f3FromEyes ? "; F3 eye between the eye bones" : "; F3 eye from the head offsets");
    }
}

bool Base(void* cameraNode, pose::Mat4* out) {
    std::lock_guard lock(g_mutex);
    if (!out || !g_baseValid || cameraNode != g_cameraNode) return false;
    *out = g_base;
    return true;
}

float NearPlane() {
    std::lock_guard lock(g_mutex);
    if (!g_baseValid) return 0.f;
    return g_hideHead && g_headMode == 1 ? std::max(g_near, g_headClip) : g_near;
}

void DescribeForCrash(char* out, size_t cap) {
    // Unlocked on purpose: the crashing thread may hold the mutex; a torn read only mislabels.
    snprintf(out, cap, " F1 %s, F3 %s, now %s", g_f1 ? "on" : "off", g_f3 ? "on" : "off", g_baseValid ? "first person" : "game camera");
}

bool Command(const char* verb, const char* arg) {
    std::lock_guard lock(g_mutex);
    const bool argOn = arg && (std::strcmp(arg, "on") == 0 || std::strcmp(arg, "1") == 0);
    if (std::strcmp(verb, "fp") == 0) {
        SetF1(argOn, "command");
        return true;
    }
    if (std::strcmp(verb, "fpcutscenes") == 0) {
        SetF3(argOn, "command");
        return true;
    }
    if (std::strcmp(verb, "fpstatus") == 0) {
        const player::State st = player::Poll();
        Log("FPSTATUS ready=%d F1=%d F3=%d base=%d mode=%s control=%s character=%p skeleton=%p bones head=%d nose=%d "
            "eyeSamples=%d viewYaw=%.1f pitch=%.1f camera=%s",
            int(g_ready), int(g_f1), int(g_f3), int(g_baseValid), ModeName(g_lastMode), player::Name(st.control),
            st.character, g_rig.skeleton, g_rig.head, g_rig.nose, g_eye.Samples(), g_viewYaw * 57.29578f,
            g_viewPitch * 57.29578f, camdirector::Name(camdirector::Active()));
        vrinput::LogStatus();
        fpbody::LogStatus();
        return true;
    }
    if (std::strcmp(verb, "fpplayer") == 0) {
        player::Dump();
        return true;
    }
    if (std::strcmp(verb, "fpprompts") == 0) {
        prompts::SetEnabled(!arg || std::strcmp(arg, "off") != 0);
        return true;
    }
    if (std::strcmp(verb, "fpautowalk") == 0) {
        g_autoWalk = argOn;
        Log("FP auto-walk (R2 held while the left stick is pushed) %s", g_autoWalk ? "on" : "off");
        return true;
    }
    if (std::strcmp(verb, "fpeye") == 0 && arg) {
        g_eyeForward = std::clamp(float(std::atof(arg)), -0.3f, 0.4f);
        Log("FP eye forward=%.3f up=%.3f", g_eyeForward, g_eyeUp);
        return true;
    }
    if (std::strcmp(verb, "fpeyeup") == 0 && arg) {
        g_eyeUp = std::clamp(float(std::atof(arg)), -0.3f, 0.4f);
        Log("FP eye forward=%.3f up=%.3f", g_eyeForward, g_eyeUp);
        return true;
    }
    if (std::strcmp(verb, "fpbody") == 0) {
        fpbody::SetEnabled(!arg || std::strcmp(arg, "off") != 0);
        return true;
    }
    if (std::strcmp(verb, "fparms") == 0 || std::strcmp(verb, "fphead") == 0 || std::strcmp(verb, "fpturnbody") == 0 ||
        std::strcmp(verb, "fpidle") == 0) {
        bool* b = std::strcmp(verb, "fparms") == 0  ? &g_arms
                  : std::strcmp(verb, "fphead") == 0 ? &g_hideHead
                  : std::strcmp(verb, "fpidle") == 0 ? &g_idleHold
                                                     : &g_turnBody;
        *b = argOn;
        Log("FP %s %s", verb, argOn ? "on" : "off");
        return true;
    }
    if (std::strcmp(verb, "fpskel") == 0) {
        fpbody::RequestDump();
        return true;
    }
    if (std::strcmp(verb, "fpwrist") == 0 && arg) {
        g_wristPitch = std::clamp(float(std::atof(arg)), -90.f, 90.f) * 0.017453293f;
        Log("FP wrist pitch %.0f deg", g_wristPitch * 57.29578f);
        return true;
    }
    if (std::strcmp(verb, "fpheadclip") == 0 && arg) {
        g_headClip = std::clamp(float(std::atof(arg)), 0.02f, 0.4f);
        Log("FP head clip radius %.3f m (fp_hidehead=%s)", g_headClip,
            g_headMode == 1 ? "clip" : g_headMode == 2 ? "collapse" : "off");
        return true;
    }
    if (std::strcmp(verb, "fpnear") == 0 && arg) {
        g_near = std::clamp(float(std::atof(arg)), 0.f, 1.f);
        Log("FP near plane %.3f m (0 = the game's)", g_near);
        return true;
    }
    if (std::strcmp(verb, "turn") == 0 && arg) {
        // "turn smooth" / "turn snap [degrees]"
        g_snapTurn = std::strncmp(arg, "snap", 4) == 0;
        if (g_snapTurn && arg[4] == ' ') g_snapStep = std::clamp(float(std::atof(arg + 5)), 10.f, 90.f) * 0.017453293f;
        g_snap = {};
        Log("FP turning in F1: %s (%.0f deg steps)", g_snapTurn ? "snap" : "smooth", g_snapStep * 57.29578f);
        return true;
    }
    return false;
}

bool GameplayFP() {
    std::lock_guard lock(g_mutex);
    return g_f1;
}
void SetGameplayFP(bool on) {
    std::lock_guard lock(g_mutex);
    SetF1(on, "panel");
}
bool CutsceneFP() {
    std::lock_guard lock(g_mutex);
    return g_f3;
}
// 0.36.12 / 0.36.13 (owner: "Add a option in vr panel Hide head On/Off ... make it a live switch"; then "make default
// for mod to not hide head, and i meant head hiding i didnt mean the 20cm but i meant literally not showing the head at
// all ... hide all the accesories on head, hair, teeth, eyeballs ... Dont forget about simulated things"): On = the head
// not drawn at all - fp_hidehead=collapse: the Head subtree and the helpers resting on it collapse into the neck in the
// drawn palette only (mirrors still get the head, 0.10.0), and simulated hair (Madison) is simulated out of sight
// (fp_cloth_hair). Checked on every playable outfit by tools/rig_viz.py --checks: eyes (M1) and teeth / tongue / mouth
// (M2) collapse on all, the face bones (K2) are all in the set, hair (M3) except the scalp's edge on the neck weighted
// to Neck (Ethan 53, Jayden 79, Shelby 9, Madison 2 vertices, at most 8 cm from the neck joint, behind and below the
// eye). Off = the head drawn (fp_near 5 cm still keeps the eyelids and lashes out of the eyes). Live.
bool HideHead() {
    std::lock_guard lock(g_mutex);
    return g_hideHead && g_headMode == 2;
}
void SetHideHead(bool on) {
    std::lock_guard lock(g_mutex);
    g_headMode = on ? 2 : 0;
    g_hideHead = on;
    Save(L"fp_hidehead", on ? L"collapse" : L"off");
    Log("FP hide head %s (panel; fp_hidehead=%s)", on ? "on - not drawn (collapse)" : "off - drawn", on ? "collapse" : "off");
}
void SetCutsceneFP(bool on) {
    std::lock_guard lock(g_mutex);
    SetF3(on, "panel");
}
bool SnapTurn() {
    std::lock_guard lock(g_mutex);
    return g_snapTurn;
}
void SetSnapTurn(bool on) {
    std::lock_guard lock(g_mutex);
    g_snapTurn = on;
    g_snap = {};
    Save(L"turn", on ? L"snap" : L"smooth");
}
float SnapDegrees() {
    std::lock_guard lock(g_mutex);
    return g_snapStep * 57.29578f;
}
void SetSnapDegrees(float degrees) {
    std::lock_guard lock(g_mutex);
    g_snapStep = std::clamp(degrees, 10.f, 90.f) * 0.017453293f;
    SaveNumber(L"snap_turn_degrees", std::clamp(degrees, 10.f, 90.f));
}
float TurnSpeed() {
    std::lock_guard lock(g_mutex);
    return g_turnSpeed;
}
void SetTurnSpeed(float fraction) {
    std::lock_guard lock(g_mutex);
    g_turnSpeed = std::clamp(fraction, 0.2f, 1.f);
    SaveNumber(L"turn_speed", g_turnSpeed);
}

}  // namespace hrvr::fpcam
