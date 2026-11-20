#include "fpbody.h"

#include <windows.h>

#include <MinHook.h>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <vector>

#include "bones.h"
#include "engineobj.h"
#include "fingers_math.h"
#include "fpbody_math.h"
#include "item_class.h"
#include "item_seam.h"
#include "movie_item.h"
#include "movielink.h"
#include "rigmap.h"
#include "game.h"
#include "log.h"

namespace hrvr::fpbody {
namespace {

constexpr int kMaxBones = 512;
constexpr size_t kRecWorlds = 0x58, kRecCount = 0x60;  // skinned render record (type 3)
constexpr size_t kSkelWorldsA = 0x08, kSkelWorlds = 0x10, kSkelHashes = 0x210;
constexpr int kDumpSamples = 32;

enum Group : uint8_t { kNone, kCore, kHead, kHandL, kHandR, kForeL, kForeR, kUpperL, kUpperR };
const char* GroupName(uint8_t g) {
    static const char* n[] = {"-", "core", "head", "handL", "handR", "foreL", "foreR", "upperL", "upperR"};
    return g < 9 ? n[g] : "?";
}

using PaletteFn = void (*)(void* record);
PaletteFn g_orig = nullptr;
std::atomic<bool> g_installed{false}, g_enabled{true}, g_active{false};
std::atomic<void*> g_skeleton{nullptr};  // the player's skeleton: a cheap test before any lock
std::atomic<void*> g_node{nullptr};      // the player's scene node (objects on the body node)
std::atomic<uint64_t> g_edits{0}, g_calls{0}, g_wristLimited{0}, g_idleHolds{0};
std::atomic<uint64_t> g_sleeveOnRoll{0};  // 0.35.0: skin nodes placed from a forearm roll helper (rigmap::ArmCarry)
std::atomic<bool> g_dumpRequest{false};
std::atomic<bool> g_twistHelpers{true};  // fp_forearm_twist=helpers (0.16.0) | forearm (0.6.0 .. 0.15.1)

std::mutex g_planMutex;
Plan g_plan;

// ---- the rig: learned from the clean pose ----
struct Rig {
    void* skeleton = nullptr;
    uint32_t count = 0;
    int head = -1, neck = -1, chest = -1, hips = -1;
    int arm[2] = {-1, -1}, fore[2] = {-1, -1}, hand[2] = {-1, -1};
    int finger[2][5] = {{-1, -1, -1, -1, -1}, {-1, -1, -1, -1, -1}};  // HumanIK Thumb1 Index1 Middle1 Ring1 Pinky1
    // 0.16.0 (rig pictures): the forearm's roll helpers, <Side>ForeArmRoll / Roll1 / Roll2 / Roll3, children of ForeArm.
    int roll[2][4] = {{-1, -1, -1, -1}, {-1, -1, -1, -1}};
    int rolls[2] = {0, 0};
    uint8_t group[kMaxBones] = {};
    bool core[kMaxBones] = {};
    Range inHead[kMaxBones], inChest[kMaxBones], inFore[2][kMaxBones], inUpper[2][kMaxBones], inHand[2][kMaxBones];
    bool namedHand[2][kMaxBones] = {};  // finger joints found by their HumanIK names (RightHandIndex1..4, ...)
    bool armReady[2] = {false, false};  // the hand has moved enough against the chest to tell its bones apart
    // 0.9.0: the game's own hierarchy (bones.h Parents, skeleton +0x20, verified local x parent world = world).
    int16_t parent[kMaxBones] = {};
    bool haveParents = false;
    float maxHeadDist[kMaxBones] = {}, maxHandDist[2][kMaxBones] = {}, maxForeDist[2][kMaxBones] = {},
          maxUpperDist[2][kMaxBones] = {};
    bool beyondWrist[2][kMaxBones] = {};
    int samples = 0;
    uint64_t lastSampleMs = 0;
    bool anatValid[2] = {false, false};
    Mat4 anat[2] = {pose::Identity(), pose::Identity()};  // anatomical hand frame in the hand bone's frame
    Mat4 anatSum[2] = {};                                 // its samples summed (row-wise), for the average
    int anatSamples[2] = {0, 0};
    std::vector<float> dump;  // kDumpSamples x count x 16
    int dumpSamples = 0;
    bool dumped = false;
    int headCount = 0, handCount[2] = {0, 0};
    // Finger chains (REQ-15), inferred once from the pose samples: chain bones from the knuckle out, which
    // finger it is (0 thumb .. 4 little), and the palm side in the hand bone's frame.
    static constexpr int kChains = 6, kChainLen = 5;
    int chain[2][kChains][kChainLen] = {};
    int chainLen[2][kChains] = {};
    int chainFinger[2][kChains] = {};
    int chains[2] = {0, 0};
    Vec3 palmLocal[2] = {};
    bool fingersInferred = false;
    // 0.22.0 (playbook 14.12): the rig's rest shape in object space = inverse(skin bind), read from a palette record
    std::vector<float> rest;
    bool haveRest = false;
    int restUsable = 0;
};
std::mutex g_rigMutex;
Rig* g_rig = nullptr;  // allocated once (large)

// What an edit needs from the rig, copied under the lock.
struct RigView {
    uint32_t count = 0;
    bool haveParents = false;
    int16_t parent[kMaxBones];
    int head = -1, neck = -1, arm[2] = {-1, -1}, fore[2] = {-1, -1}, hand[2] = {-1, -1};
    int roll[2][4] = {{-1, -1, -1, -1}, {-1, -1, -1, -1}};
    int rolls[2] = {0, 0};
    uint8_t group[kMaxBones];
    bool anatValid[2] = {false, false};
    bool armReady[2] = {false, false};
    Mat4 anat[2];
    int chain[2][Rig::kChains][Rig::kChainLen] = {};
    int chainLen[2][Rig::kChains] = {};
    int chainFinger[2][Rig::kChains] = {};
    int chains[2] = {0, 0};
    Vec3 palmLocal[2] = {};
    fingers::HandJoints fj[2];  // 0.22.0: five three-joint chains per hand (HumanIK knuckles, then first children)
    bool haveRest = false;
};

const char* const kCoreNames[] = {"Hips", "Spine", "Spine1", "Spine2", "Spine3", "Neck", "Neck1", "Head",
                                  "LeftShoulder", "LeftArm", "LeftForeArm", "LeftHand", "RightShoulder", "RightArm",
                                  "RightForeArm", "RightHand", "LeftUpLeg", "LeftLeg", "LeftFoot", "LeftToe",
                                  "RightUpLeg", "RightLeg", "RightFoot", "RightToe"};
const char* const kFingerNames[5] = {"Thumb1", "Index1", "Middle1", "Ring1", "Pinky1"};

bool Finite(const float* m) {
    for (int i = 0; i < 16; ++i)
        if (!std::isfinite(m[i])) return false;
    return true;
}
Mat4 At(const float* worlds, int i) {
    Mat4 m;
    std::memcpy(m.m, worlds + size_t(i) * 16, sizeof(m.m));
    return m;
}
void Put(float* worlds, int i, const Mat4& m) { std::memcpy(worlds + size_t(i) * 16, m.m, sizeof(m.m)); }
Mat4 RotationOf(const Mat4& m) {
    Mat4 r = m;
    r.m[12] = r.m[13] = r.m[14] = 0.f;
    return r;
}
// The unit rotation `rot` with the axis lengths (bone scale) of `like`.
Mat4 WithScaleOf(const Mat4& rot, const Mat4& like) {
    Mat4 r = rot;
    for (int k = 0; k < 3; ++k) {
        const float len = fp::Len({like.m[k * 4], like.m[k * 4 + 1], like.m[k * 4 + 2]});
        for (int c = 0; c < 3; ++c) r.m[k * 4 + c] *= len;
    }
    r.m[12] = r.m[13] = r.m[14] = 0.f;
    return r;
}
// Gram-Schmidt of the three rows (a summed frame back to a rotation).
Mat4 Orthonormal(const Mat4& m) {
    const Vec3 x = fp::Norm({m.m[0], m.m[1], m.m[2]});
    const Vec3 y = fp::Norm(fp::Sub({m.m[4], m.m[5], m.m[6]}, fp::Mul(x, fp::Dot(x, {m.m[4], m.m[5], m.m[6]}))));
    Vec3 z = fp::Cross(x, y);
    if (fp::Dot(z, {m.m[8], m.m[9], m.m[10]}) < 0.f) z = fp::Mul(z, -1.f);
    Mat4 r = pose::Identity();
    const Vec3 rows[3] = {x, y, z};
    for (int k = 0; k < 3; ++k) {
        r.m[k * 4] = rows[k].x;
        r.m[k * 4 + 1] = rows[k].y;
        r.m[k * 4 + 2] = rows[k].z;
    }
    return r;
}

void ResetRig(Rig& r, void* skeleton, uint32_t count) {
    std::vector<float> keep;
    keep.swap(r.dump);
    r.~Rig();
    new (&r) Rig();
    r.dump.swap(keep);
    r.dump.clear();
    r.skeleton = skeleton;
    r.count = count;
    auto idx = [&](const char* name) { return bones::BoneIndex(skeleton, bones::Crc32(name)); };
    r.head = idx("Head");
    r.neck = idx("Neck");
    r.hips = idx("Hips");
    for (const char* c : {"Spine2", "Spine1", "Spine"})
        if (r.chest < 0) r.chest = idx(c);
    const char* side[2] = {"Left", "Right"};
    for (int s = 0; s < 2; ++s) {
        r.arm[s] = idx((std::string(side[s]) + "Arm").c_str());
        r.fore[s] = idx((std::string(side[s]) + "ForeArm").c_str());
        r.hand[s] = idx((std::string(side[s]) + "Hand").c_str());
        for (int f = 0; f < 5; ++f) r.finger[s][f] = idx((std::string(side[s]) + "Hand" + kFingerNames[f]).c_str());
        for (const char* n : {"ForeArmRoll", "ForeArmRoll1", "ForeArmRoll2", "ForeArmRoll3"}) {
            const int i = idx((std::string(side[s]) + n).c_str());
            if (i >= 0 && i < kMaxBones && r.rolls[s] < 4) r.roll[s][r.rolls[s]++] = i;
        }
        static const char* const kFingers[5] = {"Thumb", "Index", "Middle", "Ring", "Pinky"};
        for (const char* f : kFingers)
            for (int k = 1; k <= 4; ++k) {
                const int i = idx((std::string(side[s]) + "Hand" + f + std::to_string(k)).c_str());
                if (i >= 0 && i < kMaxBones) r.namedHand[s][i] = true;
            }
    }
    for (const char* c : kCoreNames) {
        const int i = idx(c);
        if (i >= 0 && i < kMaxBones) r.core[i] = true;
    }
    for (uint32_t i = 0; i < count && i < uint32_t(kMaxBones); ++i) {
        r.group[i] = r.core[i] ? kCore : kNone;
        for (int s = 0; s < 2; ++s) r.beyondWrist[s][i] = true;
    }
    if (r.head >= 0) r.group[r.head] = kHead;
    {
        // The hierarchy the game composes the pose with (decompiled 2026-10-28, 0x1409fd270), checked on 24 bones.
        static int parents[kMaxBones];
        int checked = 0, good = 0;
        if (count <= uint32_t(kMaxBones) && bones::Parents(skeleton, count, parents)) {
            good = bones::CheckHierarchy(skeleton, count, parents, 24, &checked);
            r.haveParents = checked >= 8 && good * 5 >= checked * 4;
            for (uint32_t i = 0; i < count; ++i) r.parent[i] = int16_t(parents[i]);
        }
        auto parentOf = [&](int b) { return b >= 0 && r.haveParents ? int(r.parent[b]) : -2; };
        Log("FPBODY hierarchy %s: local x parent world = world for %d of %d bones checked; parents Hand L %d R %d, "
            "ForeArm L %d R %d, Head %d",
            r.haveParents ? "read from the game (skeleton +0x20)" : "NOT usable (learned groups instead)", good, checked,
            parentOf(r.hand[0]), parentOf(r.hand[1]), parentOf(r.fore[0]), parentOf(r.fore[1]), parentOf(r.head));
        if (r.haveParents)
            for (int s2 = 0; s2 < 2; ++s2) r.armReady[s2] = r.arm[s2] >= 0 && r.fore[s2] >= 0 && r.hand[s2] >= 0;
        // A roll helper is used only when it really is a child of the ForeArm (all 75 rigs of the game files).
        for (int s2 = 0; s2 < 2; ++s2) {
            int kept = 0;
            for (int k = 0; k < r.rolls[s2]; ++k)
                if (r.haveParents && r.fore[s2] >= 0 && r.parent[r.roll[s2][k]] == r.fore[s2])
                    r.roll[s2][kept++] = r.roll[s2][k];
            r.rolls[s2] = kept;
        }
        Log("FPBODY rig rules (0.16.0, rig pictures): forearm roll helpers L %d R %d (%s); skin helpers resting on the "
            "wrist / elbow / shoulder joint go with that segment, those on the Head joint with the head; 0.35.0: skin nodes "
            "resting on a roll helper turn with it and the sleeve-end bones below them follow them, not the hand",
            r.rolls[0], r.rolls[1],
            g_twistHelpers.load() ? "they share the wrist twist by their place, the forearm bone does not roll"
                                  : "fp_forearm_twist=forearm: the forearm bone rolls half the way");
    }
    int fingers = 0;
    for (auto& side2 : r.finger)
        for (int f : side2) fingers += f >= 0;
    Log("FPBODY rig %p: %u bones; Head=%d Neck=%d chest=%d arms L %d/%d/%d R %d/%d/%d; HumanIK finger names found: %d/10",
        skeleton, count, r.head, r.neck, r.chest, r.arm[0], r.fore[0], r.hand[0], r.arm[1], r.fore[1], r.hand[1], fingers);
}

// The anatomical hand frame (B2S fp::HandBasis: forward = wrist -> knuckles, side = towards the thumb), from
// named fingers when the rig has them, else from the learned finger bones (thumb = the one furthest off the
// wrist -> knuckles line among the near half).
bool HandFrame(const Rig& r, const float* w, int s, Mat4* out) {
    const Vec3 wrist = Pos(At(w, r.hand[s]));
    Vec3 fwd{}, side{};
    if (r.finger[s][2] >= 0 && r.finger[s][1] >= 0 && r.finger[s][4] >= 0) {
        fwd = fp::Sub(Pos(At(w, r.finger[s][2])), wrist);
        side = fp::Sub(Pos(At(w, r.finger[s][1])), Pos(At(w, r.finger[s][4])));
    } else {
        Vec3 sum{};
        int n = 0;
        for (uint32_t i = 0; i < r.count; ++i)
            if (r.group[i] == kHandL + s) {
                sum = fp::Add(sum, Pos(At(w, int(i))));
                ++n;
            }
        if (n < 3) return false;
        fwd = fp::Norm(fp::Sub(fp::Mul(sum, 1.f / float(n)), wrist));
        float maxAlong = 0.f;
        for (uint32_t i = 0; i < r.count; ++i)
            if (r.group[i] == kHandL + s) maxAlong = std::fmax(maxAlong, fp::Dot(fp::Sub(Pos(At(w, int(i))), wrist), fwd));
        float best = 0.f;
        for (uint32_t i = 0; i < r.count; ++i) {
            if (r.group[i] != kHandL + s) continue;
            const Vec3 d = fp::Sub(Pos(At(w, int(i))), wrist);
            const float along = fp::Dot(d, fwd);
            if (along > 0.6f * maxAlong) continue;
            const Vec3 perp = fp::Sub(d, fp::Mul(fwd, along));
            if (fp::Len(perp) > best) {
                best = fp::Len(perp);
                side = perp;
            }
        }
        if (best < 0.01f) return false;
    }
    return fp::HandBasis(fwd, side, out);
}

void Classify(Rig& r, const float* w) {
    const int n = r.samples;
    r.headCount = r.handCount[0] = r.handCount[1] = 0;
    const Vec3 pHead = r.head >= 0 ? Pos(At(w, r.head)) : Vec3{}, pChest = r.chest >= 0 ? Pos(At(w, r.chest)) : pHead;
    for (uint32_t i = 0; i < r.count; ++i) {
        if (r.core[i] || int(i) == r.head) continue;
        uint8_t g = kNone;
        if (n >= 4) {
            // 0.7.0 (owner: "parts of the wrist stretched a few cm down" early on, fine later; LIVE: at 8 samples the
            // right hand held 82 bones, later 20): with the arms hanging still, hip and thigh bones were within the
            // hand's 22 cm and rigid with the forearm, so the arm IK dragged them along. A bone now joins a segment
            // only when it stays with that segment AND has moved at least 10 cm more against the chest than against
            // it (checked on the dump of that session: every finger, wrist and twist bone passes, no hip bone does).
            // Finger joints with HumanIK names belong to their hand from the start.
            // 0.8.0 (owner: the arms "got bad" after the juggling prompt; LIVE 0.7.0): the movement ranges only grow,
            // and the juggling's big hand motions pushed the wrist / twist helpers (which follow hand and forearm
            // partly) past every segment's threshold and out of the groups, so they stayed behind the tracked hand.
            // Now an arm bone keeps the segment it was given (sticky), and goes to the segment it moves with most.
            uint8_t before = r.group[i];
            bool underHead = false;
            if (r.haveParents) {
                const int root = r.neck >= 0 ? r.neck : r.head;
                for (int p = r.parent[i], guard = 0; root >= 0 && p >= 0 && guard < 64; p = r.parent[p], ++guard)
                    if (p == root) underHead = true;
            }
            if (underHead && before >= kHandL && before <= kUpperR) before = kNone;  // never an arm bone
            if (underHead) {
                // left to the head test below
            } else if (before >= kHandL && before <= kUpperR) {
                g = before;
            } else {
                for (int s = 0; s < 2 && g == kNone; ++s) {
                    if (r.hand[s] < 0) continue;
                    if (r.namedHand[s][i]) {
                        g = uint8_t(kHandL + s);
                        break;
                    }
                    const float chest = r.inChest[i].Extent();
                    const float eh = r.inHand[s][i].Extent();
                    const float ef = r.fore[s] >= 0 ? r.inFore[s][i].Extent() : 1e9f;
                    const float eu = r.arm[s] >= 0 ? r.inUpper[s][i].Extent() : 1e9f;
                    const bool handOk = r.maxHandDist[s][i] < 0.22f && r.beyondWrist[s][i] && eh < 0.05f && chest > eh + 0.10f;
                    const bool foreOk = ef < 0.025f && r.maxForeDist[s][i] < 0.35f && chest > ef + 0.10f;
                    const bool upperOk = eu < 0.025f && r.maxUpperDist[s][i] < 0.35f && chest > eu + 0.05f;
                    // Ties go to the hand (bones on the wrist that were as rigid with both in the samples so far).
                    if (handOk && (!foreOk || eh <= ef + 0.005f) && (!upperOk || eh <= eu + 0.005f)) g = uint8_t(kHandL + s);
                    else if (foreOk && (!upperOk || ef <= eu)) g = uint8_t(kForeL + s);
                    else if (upperOk) g = uint8_t(kUpperL + s);
                }
            }
        }
        if (g == kNone && r.head >= 0 && r.maxHeadDist[i] < 0.30f) {
            // Head, face and hair: move with the head rather than with the chest. Before there is motion to
            // tell them apart, only bones clearly closer to the head than to the chest.
            const Vec3 p = Pos(At(w, int(i)));
            const bool provisional = fp::Len(fp::Sub(p, pHead)) < 0.20f &&
                                     fp::Len(fp::Sub(p, pHead)) < 0.6f * fp::Len(fp::Sub(p, pChest));
            if (n < 8 ? provisional : r.inHead[i].Extent() + 0.01f < r.inChest[i].Extent()) g = kHead;
        }
        r.group[i] = g;
        r.headCount += g == kHead;
        r.handCount[0] += g == kHandL;
        r.handCount[1] += g == kHandR;
    }
    // The arm IK waits until the game's own animation has moved this hand 15 cm against the chest (until then its
    // helpers cannot be told from the body's).
    for (int s = 0; s < 2; ++s) {
        if (r.armReady[s] || r.haveParents || r.hand[s] < 0 || n < 8 || r.inChest[r.hand[s]].Extent() < 0.15f) continue;
        r.armReady[s] = true;
        Log("FPBODY %s arm ready after %d samples: %d hand bones, the hand moved %.2f m against the chest",
            s ? "right" : "left", n, r.handCount[s], r.inChest[r.hand[s]].Extent());
    }
    for (int s = 0; s < 2; ++s) {
        if (r.hand[s] < 0 || r.anatSamples[s] >= 16 || n < 8) continue;
        Mat4 basis, inv;
        if (HandFrame(r, w, s, &basis) && AffineInverse(UnitRotation(At(w, r.hand[s])), &inv)) {
            // Anatomical frame in the hand bone's frame, averaged over 16 samples (0.6.0: one sample taken while
            // the animation curled the fingers skewed it, and the hand turned oddly on the controller). A sample
            // whose thumb side is opposite to the average so far is turned 180 deg about the forward axis.
            Mat4 m = pose::Mul(basis, inv);
            if (r.anatSamples[s] > 0 &&
                fp::Dot({m.m[4], m.m[5], m.m[6]}, {r.anatSum[s].m[4], r.anatSum[s].m[5], r.anatSum[s].m[6]}) < 0.f)
                for (int k = 4; k < 12; ++k) m.m[k] = -m.m[k];
            for (int k = 0; k < 12; ++k) r.anatSum[s].m[k] += m.m[k];
            r.anat[s] = Orthonormal(r.anatSum[s]);
            r.anatValid[s] = true;
            ++r.anatSamples[s];
        }
    }
}

void WriteDump(const Rig& r) {
    if (!r.dumpSamples || !r.skeleton) return;
    static int index = 0;
    char name[64];
    sprintf_s(name, "\\skeleton_%u_%d.csv", r.count, index++);
    std::wstring path = std::wstring(LogDir()) + std::wstring(name, name + std::strlen(name));
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"w") || !f) return;
    const void* hashes = obj::Ptr(r.skeleton, kSkelHashes);
    fprintf(f, "# Heavy Rain player skeleton: %u bones, %d clean pose samples (world, metres, Y up, row-vector)\n",
            r.count, r.dumpSamples);
    fprintf(f, "bone,crc,name,group,sample,m0,m1,m2,m3,m4,m5,m6,m7,m8,m9,m10,m11,m12,m13,m14,m15\n");
    for (uint32_t i = 0; i < r.count; ++i) {
        uint32_t h = 0;
        obj::Get(hashes, size_t(i) * 8, h);
        const char* known = "";
        static std::vector<std::pair<uint32_t, std::string>> names;
        if (names.empty()) {
            for (const char* c : kCoreNames) names.push_back({bones::Crc32(c), c});
            for (const char* sd : {"Left", "Right"})
                for (const char* fg : {"Thumb", "Index", "Middle", "Ring", "Pinky"})
                    for (int k = 0; k <= 4; ++k) {
                        std::string nm = std::string(sd) + "Hand" + fg + (k ? std::to_string(k) : "");
                        names.push_back({bones::Crc32(nm.c_str()), nm});
                    }
            names.push_back({bones::Crc32("MNoseUp"), "MNoseUp"});
        }
        for (const auto& kv : names)
            if (kv.first == h) known = kv.second.c_str();
        for (int s = 0; s < r.dumpSamples; ++s) {
            const float* m = r.dump.data() + (size_t(s) * r.count + i) * 16;
            fprintf(f, "%u,%08x,%s,%s,%d", i, h, known, GroupName(r.group[i]), s);
            for (int k = 0; k < 16; ++k) fprintf(f, ",%.5f", m[k]);
            fprintf(f, "\n");
        }
    }
    fclose(f);
    // Raw per-bone data (0.8.0, for the parent hierarchy): the 8-byte name record (CRC32 + its second dword) and the
    // 0x80-byte bone entry (+0x1F8 array), as dwords. The next build reads the parent index from whichever proves it.
    sprintf_s(name, "\\skeleton_%u_raw.csv", r.count);
    path = std::wstring(LogDir()) + std::wstring(name, name + std::strlen(name));
    if (!_wfopen_s(&f, path.c_str(), L"w") && f) {
        const void* entries = obj::Ptr(r.skeleton, 0x1F8);
        int parents[kMaxBones];
        const bool havePar = r.count <= uint32_t(kMaxBones) && bones::Parents(r.skeleton, r.count, parents);
        fprintf(f, "bone,crc,rec_hi,group,parent");
        for (int k = 0; k < 32; ++k) fprintf(f, ",e%02x", k * 4);
        fprintf(f, "\n");
        for (uint32_t i = 0; i < r.count; ++i) {
            uint32_t h = 0, hi = 0;
            obj::Get(hashes, size_t(i) * 8, h);
            obj::Get(hashes, size_t(i) * 8 + 4, hi);
            fprintf(f, "%u,%08x,%08x,%s,%d", i, h, hi, GroupName(r.group[i]), havePar ? parents[i] : -2);
            for (int k = 0; k < 32; ++k) {
                uint32_t e = 0;
                if (entries) obj::Get(entries, size_t(i) * 0x80 + size_t(k) * 4, e);
                fprintf(f, ",%08x", e);
            }
            fprintf(f, "\n");
        }
        fclose(f);
    }
    Log("FPBODY skeleton dump written: hrvr_logs%s (%u bones x %d samples; tools/plot_skeleton.py draws it) + raw records",
        name, r.count, r.dumpSamples);
}

// Finger chains from the stored pose samples (the plotter's rule, tools/plot_skeleton.py): a finger bone's parent is
// the hand bone or another finger bone nearer the wrist in whose frame it keeps the most constant offset; ties
// (rigid siblings) go to the nearest ancestor. Fingers are then ordered across the knuckles from the thumb, and
// the palm side is where the fingertips go when the game's own animation curls them (nearer the wrist).
void InferFingers(Rig& r) {
    r.fingersInferred = true;
    const int S = r.dumpSamples;
    auto M = [&](int s, int i) { return At(r.dump.data() + size_t(s) * r.count * 16, i); };
    for (int side = 0; side < 2; ++side) {
        if (r.hand[side] < 0 || !r.anatValid[side]) continue;
        std::vector<int> mem;
        for (uint32_t i = 0; i < r.count; ++i)
            if (r.group[i] == kHandL + side) mem.push_back(int(i));
        if (mem.size() < 3 || mem.size() > 64) continue;
        const int hand = r.hand[side];
        auto depthOf = [&](int i) {
            float d = 0.f;
            for (int s = 0; s < S; ++s) d += fp::Len(fp::Sub(Pos(M(s, i)), Pos(M(s, hand))));
            return d / float(S);
        };
        std::vector<float> depth(mem.size());
        for (size_t k = 0; k < mem.size(); ++k) depth[k] = depthOf(mem[k]);
        std::vector<int> parent(mem.size(), -1);  // index into mem, -1 = the hand
        for (size_t k = 0; k < mem.size(); ++k) {
            float best = 1e9f, bestDepth = -1.f;
            int pick = -1;
            for (int c = -1; c < int(mem.size()); ++c) {
                const int p = c < 0 ? hand : mem[size_t(c)];
                const float pd = c < 0 ? 0.f : depth[size_t(c)];
                if (c == int(k) || pd >= depth[k]) continue;
                Vec3 mean{}, sq{};
                for (int s = 0; s < S; ++s) {
                    Mat4 inv;
                    if (!AffineInverse(M(s, p), &inv)) continue;
                    const Vec3 l = InFrame(inv, Pos(M(s, mem[k])));
                    mean = fp::Add(mean, l);
                    sq = fp::Add(sq, {l.x * l.x, l.y * l.y, l.z * l.z});
                }
                mean = fp::Mul(mean, 1.f / float(S));
                sq = fp::Mul(sq, 1.f / float(S));
                const float spread = std::sqrt(std::fmax(0.f, std::fmax(sq.x - mean.x * mean.x,
                                                std::fmax(sq.y - mean.y * mean.y, sq.z - mean.z * mean.z))));
                if (spread < best - 0.002f || (spread <= best + 0.002f && pd > bestDepth)) {
                    if (spread < best) best = spread;
                    bestDepth = pd;
                    pick = c;
                }
            }
            parent[k] = pick;
        }
        // Chains: from each knuckle (parent = the hand) follow the deepest child.
        const Mat4 anatW = pose::Mul(r.anat[side], RotationOf(M(S - 1, hand)));
        const Vec3 fwd{anatW.m[0], anatW.m[1], anatW.m[2]}, sideAx{anatW.m[4], anatW.m[5], anatW.m[6]};
        struct C { int bones[Rig::kChainLen]; int len; float across, offAxis; };
        std::vector<C> cs;
        for (size_t k = 0; k < mem.size() && cs.size() < size_t(Rig::kChains); ++k) {
            if (parent[k] != -1) continue;
            C c{};
            int cur = int(k);
            while (cur >= 0 && c.len < Rig::kChainLen) {
                c.bones[c.len++] = mem[size_t(cur)];
                int next = -1;
                for (size_t j = 0; j < mem.size(); ++j)
                    if (parent[j] == cur && (next < 0 || depth[j] > depth[size_t(next)])) next = int(j);
                cur = next;
            }
            const Vec3 d = fp::Sub(Pos(M(S - 1, c.bones[0])), Pos(M(S - 1, hand)));
            c.across = fp::Dot(d, sideAx);
            c.offAxis = fp::Len(fp::Sub(d, fp::Mul(fwd, fp::Dot(d, fwd))));
            if (c.len >= 2) cs.push_back(c);
        }
        if (cs.empty()) continue;
        // The thumb sits furthest towards the thumb side; then index .. little across the knuckles.
        std::sort(cs.begin(), cs.end(), [](const C& a, const C& b) { return a.across > b.across; });
        r.chains[side] = int(cs.size());
        for (size_t k = 0; k < cs.size(); ++k) {
            std::memcpy(r.chain[side][k], cs[k].bones, sizeof(cs[k].bones));
            r.chainLen[side][k] = cs[k].len;
            r.chainFinger[side][k] = int(std::min<size_t>(k, 4));
        }
        // Palm side: fingertip offsets (hand frame) that come nearer the wrist also move towards the palm.
        const Vec3 n = fp::Norm(fp::Cross(fwd, sideAx));
        float cov = 0.f, ma = 0.f, mn = 0.f;
        int cnt = 0;
        for (const C& c : cs)
            for (int s = 0; s < S; ++s) {
                const Vec3 d = fp::Sub(Pos(M(s, c.bones[c.len - 1])), Pos(M(s, hand)));
                const Mat4 aw = pose::Mul(r.anat[side], RotationOf(M(s, hand)));
                const Vec3 f{aw.m[0], aw.m[1], aw.m[2]}, sd{aw.m[4], aw.m[5], aw.m[6]};
                const float along = fp::Dot(d, f), nrm = fp::Dot(d, fp::Norm(fp::Cross(f, sd)));
                cov += along * nrm;
                ma += along;
                mn += nrm;
                ++cnt;
            }
        cov = cov / float(cnt) - (ma / float(cnt)) * (mn / float(cnt));
        Mat4 handInv;
        if (AffineInverse(RotationOf(M(S - 1, hand)), &handInv))
            r.palmLocal[side] = pose::TransformPoint(fp::Mul(n, cov < 0.f ? 1.f : -1.f), handInv);
        Log("FPBODY %s hand: %d finger chains (%s), palm side learned (cov %.5f)", side ? "right" : "left", r.chains[side],
            cs.size() >= 5 ? "thumb..little" : "fewer than five", cov);
    }
}

// Samples the clean pose about every 150 ms (the palette builder may run on render jobs: try-lock only).
void Learn(void* skeleton, const float* w, uint32_t count) {
    const uint64_t now = GetTickCount64();
    std::unique_lock lock(g_rigMutex, std::try_to_lock);
    if (!lock.owns_lock()) return;
    if (!g_rig) g_rig = new Rig();
    Rig& r = *g_rig;
    if (r.skeleton != skeleton || r.count != count) ResetRig(r, skeleton, count);
    const bool dumpNow = g_dumpRequest.exchange(false);
    if (!dumpNow && r.samples && now - r.lastSampleMs < 150) return;
    r.lastSampleMs = now;
    ++r.samples;
    Mat4 headInv = pose::Identity(), chestInv = pose::Identity(), foreInv[2], upperInv[2], handInv[2];
    const bool head = r.head >= 0 && AffineInverse(At(w, r.head), &headInv);
    const bool chest = r.chest >= 0 && AffineInverse(At(w, r.chest), &chestInv);
    bool fore[2], upper[2], hand[2];
    for (int s = 0; s < 2; ++s) {
        hand[s] = r.hand[s] >= 0 && AffineInverse(At(w, r.hand[s]), &handInv[s]);
        fore[s] = r.fore[s] >= 0 && AffineInverse(At(w, r.fore[s]), &foreInv[s]);
        upper[s] = r.arm[s] >= 0 && AffineInverse(At(w, r.arm[s]), &upperInv[s]);
    }
    const Vec3 pHead = head ? Pos(At(w, r.head)) : Vec3{};
    for (uint32_t i = 0; i < count; ++i) {
        const Vec3 p = Pos(At(w, int(i)));
        if (head) {
            r.inHead[i].Add(InFrame(headInv, p));
            r.maxHeadDist[i] = std::fmax(r.maxHeadDist[i], fp::Len(fp::Sub(p, pHead)));
        }
        if (chest) r.inChest[i].Add(InFrame(chestInv, p));
        for (int s = 0; s < 2; ++s) {
            if (r.hand[s] >= 0) {
                const Vec3 pHand = Pos(At(w, r.hand[s]));
                r.maxHandDist[s][i] = std::fmax(r.maxHandDist[s][i], fp::Len(fp::Sub(p, pHand)));
                if (r.fore[s] >= 0) {
                    const Vec3 pElbow = Pos(At(w, r.fore[s]));
                    if (fp::Len(fp::Sub(p, pElbow)) < fp::Len(fp::Sub(pHand, pElbow)) - 0.04f) r.beyondWrist[s][i] = false;
                }
            }
            if (hand[s]) r.inHand[s][i].Add(InFrame(handInv[s], p));
            if (fore[s]) {
                r.inFore[s][i].Add(InFrame(foreInv[s], p));
                r.maxForeDist[s][i] = std::fmax(r.maxForeDist[s][i], fp::Len(fp::Sub(p, Pos(At(w, r.fore[s])))));
            }
            if (upper[s]) {
                r.inUpper[s][i].Add(InFrame(upperInv[s], p));
                r.maxUpperDist[s][i] = std::fmax(r.maxUpperDist[s][i], fp::Len(fp::Sub(p, Pos(At(w, r.arm[s])))));
            }
        }
    }
    Classify(r, w);
    if (r.samples == 8 || r.samples == 40)
        Log("FPBODY rig learned from %d samples: head set %d bones, left hand %d, right hand %d, hand frames %d/%d", r.samples,
            r.headCount, r.handCount[0], r.handCount[1], int(r.anatValid[0]), int(r.anatValid[1]));
    if (r.dumpSamples < kDumpSamples && r.samples % 4 == 0) {
        r.dump.resize(size_t(kDumpSamples) * count * 16);
        std::memcpy(r.dump.data() + size_t(r.dumpSamples) * count * 16, w, size_t(count) * 64);
        ++r.dumpSamples;
    }
    // Finger chains are no longer inferred (owner 2026-10-27: "remove finger detection from first person"): the
    // fingers ride on the hand as the game animates them. InferFingers stays for the skeleton dump tooling.
    if (dumpNow || (!r.dumped && r.dumpSamples == kDumpSamples)) {
        r.dumped = true;
        if (!r.fingersInferred && r.dumpSamples >= 16) InferFingers(r);  // for the dump's picture only
        WriteDump(r);
    }
}

bool View(void* skeleton, RigView* v) {
    std::lock_guard lock(g_rigMutex);
    if (!g_rig || g_rig->skeleton != skeleton) return false;
    const Rig& r = *g_rig;
    v->count = r.count;
    v->haveParents = r.haveParents;
    if (r.haveParents) std::memcpy(v->parent, r.parent, sizeof(int16_t) * r.count);
    v->head = r.head;
    v->neck = r.neck;
    for (int s = 0; s < 2; ++s) {
        v->arm[s] = r.arm[s];
        v->fore[s] = r.fore[s];
        v->hand[s] = r.hand[s];
        v->anatValid[s] = r.anatValid[s];
        v->armReady[s] = r.armReady[s];
        v->anat[s] = r.anat[s];
        v->rolls[s] = r.rolls[s];
        for (int k = 0; k < 4; ++k) v->roll[s][k] = r.roll[s][k];
    }
    std::memcpy(v->group, r.group, sizeof(v->group));
    std::memcpy(v->chain, r.chain, sizeof(v->chain));
    std::memcpy(v->chainLen, r.chainLen, sizeof(v->chainLen));
    std::memcpy(v->chainFinger, r.chainFinger, sizeof(v->chainFinger));
    std::memcpy(v->chains, r.chains, sizeof(v->chains));
    std::memcpy(v->palmLocal, r.palmLocal, sizeof(v->palmLocal));
    v->haveRest = r.haveRest;
    for (int s = 0; s < 2; ++s) {
        fingers::HandJoints h;
        h.hand = r.hand[s];
        for (int f = 0; f < 5 && r.haveParents; ++f) {
            int j = r.finger[s][f];
            for (int k = 0; k < 3 && j >= 0; ++k) {
                h.joint[f][k] = j;
                int next = -1;
                for (uint32_t i = uint32_t(j) + 1; i < r.count && next < 0; ++i)
                    if (r.parent[i] == j) next = int(i);
                j = next;
            }
        }
        v->fj[s] = h;
    }
    return true;
}

void ApplyToGroup(float* w, const RigView& v, uint8_t g, int core, const Mat4& delta) {
    if (core >= 0) Put(w, core, pose::Mul(At(w, core), delta));
    for (uint32_t i = 0; i < v.count; ++i)
        if (v.group[i] == g) Put(w, int(i), pose::Mul(At(w, int(i)), delta));
}

// The game's own composition, applied to the edit (0.9.0): every bone below an edited bone keeps its clean pose
// relative to its parent, world = (clean world x inverse(clean parent world)) x new parent world, in the game's order
// (a parent always before its children). Fingers, wrist / sleeve / twist helpers, face and hair follow exactly.
void Propagate(float* w, const float* clean, const RigView& v, const uint8_t* set) {
    alignas(16) static thread_local uint8_t changed[kMaxBones];
    std::memcpy(changed, set, v.count);
    for (uint32_t i = 1; i < v.count; ++i) {
        if (set[i]) continue;
        const int p = v.parent[i];
        if (p < 0 || !changed[p]) continue;
        Mat4 pinv;
        if (!AffineInverse(At(clean, p), &pinv)) continue;
        Put(w, int(i), pose::Mul(pose::Mul(At(clean, int(i)), pinv), At(w, p)));
        changed[i] = 1;
    }
}

// 0.11.0 (owner: the hand "stretches in the middle on the finger side and cuts off at the end of the hand on the
// palm side"; LIVE 0.10.0 dump): five skin bones of each hand - three on the wrist joint (197, 198, 200 on the right
// hand: 0-5 deg from the hand's rotation, 20 deg from the forearm's) and two on the knuckle line 10 cm out (199, 201)
// - are not under the hand in the hierarchy: the game turns them with its bone constraints. Following their
// parents they stayed behind the tracked hand. Bones that follow a segment without being below it are carried with
// it: those sitting on the wrist joint (within 1.5 cm of the hand bone in this pose) and
// those the rig learned in the segment's group; their own children then follow them (Propagate).
bool Below(const RigView& v, int i, int ancestor) { return rigmap::Below(v.parent, i, ancestor); }
// 0.16.0 (rig pictures, all 75 character rigs + the three live dumps): the helper group nodes rest exactly on the
// joint they follow - wrist, elbow, shoulder - and the wrist's group node is a CHILD of the elbow's. So the joint is
// tested first (rigmap::ArmCarry), then "its parent is carried", then the learned groups. Before, only the wrist had
// a joint rule and the parent rule came first: once the elbow's group node had been learned into the forearm group
// the wrist helpers below it followed the forearm instead of the hand, and until then the elbow's skin bones stayed
// at the animated elbow.
void CarryFollowers(float* w, const float* clean, const RigView& v, int s, uint8_t* set) {
    const int seg[3] = {v.hand[s], v.fore[s], v.arm[s]};
    const uint8_t grp[3] = {uint8_t(kHandL + s), uint8_t(kForeL + s), uint8_t(kUpperL + s)};
    Mat4 inv[3];
    for (int k = 0; k < 3; ++k)
        if (!AffineInverse(At(clean, seg[k]), &inv[k])) return;
    const int faceRoot = v.neck >= 0 ? v.neck : v.head;
    alignas(16) static thread_local uint8_t carry[kMaxBones];
    alignas(16) static thread_local int16_t rollOf[kMaxBones];
    rigmap::ArmCarry(clean, v.count, v.parent, v.arm[s], v.fore[s], v.hand[s], faceRoot, carry, v.roll[s], v.rolls[s],
                     rollOf);
    for (uint32_t i = 0; i < v.count; ++i) {
        if (set[i]) continue;
        // 0.13.1 (owner: "the right eye could be moved by my right hand"): nothing under the neck / head follows an arm.
        if (faceRoot >= 0 && (int(i) == faceRoot || Below(v, int(i), faceRoot))) continue;
        // 0.35.0 (rigmap::ArmCarry): a skin node resting on a forearm roll helper turns with it (Shelby's / Jayden's
        // sleeve end); the roll helper is already placed (Edit writes every roll helper before this).
        if ((carry[i] & rigmap::kOnRoll) && rollOf[i] >= 0 && set[rollOf[i]]) {
            Mat4 rinv;
            if (!AffineInverse(At(clean, rollOf[i]), &rinv)) continue;
            Put(w, int(i), pose::Mul(pose::Mul(At(clean, int(i)), rinv), At(w, rollOf[i])));
            set[i] = 1;
            g_sleeveOnRoll.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        int k = -1;
        if (carry[i] & rigmap::kOnJoint) k = (carry[i] & rigmap::kSegMask) - 1;  // rests on that segment's joint
        // A bone whose parent is already carried follows that parent (Propagate).
        if (k < 0 && v.parent[i] >= 0 && set[v.parent[i]]) continue;
        for (int g = 0; g < 3 && k < 0; ++g)
            if (v.group[i] == grp[g] && !Below(v, int(i), seg[g])) k = g;
        if (k < 0) continue;
        Put(w, int(i), pose::Mul(pose::Mul(At(clean, int(i)), inv[k]), At(w, seg[k])));
        set[i] = 1;
    }
}

// ---- fingers (0.22.0, playbook 14.12; src/fingers_math.h, tests/finger_tests.cpp, docs/fingers/*.png) ----
// Owner 2026-10-27 removed the old finger detection; owner 2026-11-17: "do the finger detection chapter". Each tracked
// hand (motion controls) gets the rig's rest shape below the wrist and a curl per finger from the controller's grip,
// trigger and touch sensing (plan.curl, fpcam). A hand that holds an item plays the animation (19.8 point 2).
std::atomic<uint64_t> g_handItemMs[2] = {0, 0};  // last time an item was seen held in that hand (HookParentMatrix)
std::atomic<uint64_t> g_fingerHands{0}, g_fingerHeld{0}, g_fingerRestPlaced{0}, g_fingerAnimKept{0}, g_fingerThumbOwn{0},
    g_fingerThumbPalm{0}, g_fingerLongOwn{0}, g_fingerLongPalm{0};
// Per hand: 0 = the finger pose, 1 = the animation's hand (holding). Stepped by wall time, shared by every thread that
// builds the pose (a race only repeats a step of a few ms).
std::atomic<float> g_holdBlend[2] = {0.f, 0.f};
std::atomic<uint64_t> g_holdStepMs[2] = {0, 0};
float StepHoldBlend(int s, bool holding, uint64_t now) {
    const uint64_t last = g_holdStepMs[s].exchange(now, std::memory_order_relaxed);
    const float dt = last && now > last ? std::fmin(0.25f, float(now - last) / 1000.f) : 0.f;
    float h = g_holdBlend[s].load(std::memory_order_relaxed);
    const float target = holding ? 1.f : 0.f, step = 12.f * dt;
    h += std::fmax(-step, std::fmin(step, target - h));
    g_holdBlend[s].store(h, std::memory_order_relaxed);
    return h;
}
void ApplyFingers(float* w, const Plan& plan, const RigView& v) {
    if (!plan.fingers || !plan.arms || !v.haveParents) return;
    alignas(16) static thread_local float rest[kMaxBones * 16];
    alignas(16) static thread_local float scratch[kMaxBones * 16];
    alignas(16) static thread_local uint8_t mark[kMaxBones];
    bool haveRest = false;
    if (plan.fingerRest && v.haveRest) {
        std::lock_guard lock(g_rigMutex);
        if (g_rig && g_rig->haveRest && g_rig->count == v.count && g_rig->rest.size() == size_t(v.count) * 16) {
            std::memcpy(rest, g_rig->rest.data(), size_t(v.count) * 64);
            haveRest = true;
        }
    }
    const uint64_t now = GetTickCount64();
    alignas(16) static thread_local float anim[kMaxBones * 16];
    for (int s = 0; s < 2; ++s) {
        if (!plan.handValid[s] || !v.armReady[s] || v.hand[s] < 0 || !v.fj[s].Complete()) continue;
        // 0.33.0 (19.8 point 2, revision 15): the holding weight moves at the curl filter's 12 per second, so taking an
        // item and letting it go cross between the finger pose and the animation's hand in 1/12 s, never a jump.
        const float hold = StepHoldBlend(s, now - g_handItemMs[s].load(std::memory_order_relaxed) < 500, now);
        if (hold >= 1.f) {
            g_fingerHeld.fetch_add(1, std::memory_order_relaxed);
            continue;  // the item's holding animation keeps the hand
        }
        if (hold > 0.f) std::memcpy(anim, w, size_t(v.count) * 64);
        fingers::Amounts a;
        for (int f = 0; f < 5; ++f) a.a[f] = plan.curl[s][f];
        fingers::Report rep;
        if (!fingers::Apply(w, v.count, v.parent, haveRest ? rest : nullptr, v.fj[s], s, a, plan.fingerRest, scratch, mark,
                            &rep))
            continue;
        if (hold > 0.f) fingers::BlendBelowHand(w, anim, v.count, v.parent, v.hand[s], hold, mark, scratch);
        g_fingerHands.fetch_add(1, std::memory_order_relaxed);
        g_fingerRestPlaced.fetch_add(uint64_t(rep.restPlaced), std::memory_order_relaxed);
        g_fingerAnimKept.fetch_add(uint64_t(rep.animatedKept), std::memory_order_relaxed);
        (rep.thumbOwnAxis[0] ? g_fingerThumbOwn : g_fingerThumbPalm).fetch_add(1, std::memory_order_relaxed);
        for (bool own : rep.longOwnAxis) (own ? g_fingerLongOwn : g_fingerLongPalm).fetch_add(1, std::memory_order_relaxed);
    }
}

std::atomic<uint64_t> g_bodyFollowShifts{0};  // 0.36.12: drawn poses moved under the headset
void Edit(float* w, const Plan& plan, const RigView& v) {
    // 1. The drawn body turns to the view about the eye (B2S 3.7); the camera does not move.
    if (plan.turnBody) {
        // 0.7.0 (owner: "the whole body flips" on fast turns and forward / backward moves): the heading to correct is
        // the torso's in this pose (square to the shoulder line, B2S 0.16.17), not the root's. Heavy Rain turns the
        // root round at once when the character turns to walk the other way and the turn animation brings the body
        // round afterwards; correcting by the root's heading faced the drawn body away meanwhile.
        float facing = plan.facingYaw;
        if (v.arm[0] >= 0 && v.arm[1] >= 0) {
            const Vec3 across = fp::Sub(Pos(At(w, v.arm[1])), Pos(At(w, v.arm[0])));  // left -> right shoulder
            if (across.x * across.x + across.z * across.z > 0.05f * 0.05f) facing = std::atan2(-across.z, across.x);
        }
        const float d = std::remainder(plan.bodyYaw - facing, 6.28318531f);
        // 0.8.0 (owner: "when I walk backwards I can see the body in front of me"): the turn's pivot is the
        // character's neck. About the eye (17 cm in front of the head) a 180 deg correction - Heavy Rain turns the
        // character round to walk backwards - put the head 34 cm in front of the view.
        Vec3 pivot = plan.pivot;
        if (v.neck >= 0) pivot = Pos(At(w, v.neck));
        else if (v.head >= 0) pivot = Pos(At(w, v.head));
        if (std::fabs(d) > 1e-4f) {
            const Mat4 turn = AboutPivot(pose::FromPose(pose::YawQuat(d), {0, 0, 0}), pivot);
            for (uint32_t i = 0; i < v.count; ++i) Put(w, int(i), pose::Mul(At(w, int(i)), turn));
        }
    }
    // 1b. 0.36.12 (owner: "make the body follow my vr headset like it does in beyond two souls"; B2S EXP-036 / 0.16.12
    // / 0.16.13): the drawn body goes under the headset - its head where the eye (the headset's position) expects it
    // (the eye minus the fp_eye_fwd / fp_eye_up offsets the view is built with): across fully, so leaning or stepping
    // in the room moves the body with the player and the controllers stay within the arms' reach; up / down only by
    // what exceeds 20 cm (at most 60 cm down, 20 cm up), so the feet stay on the floor. Before the arms are solved.
    if (plan.followHead && v.head >= 0 && fp::Finite(plan.headEye)) {
        const Vec3 fwd{-std::sin(plan.bodyYaw), 0.f, -std::cos(plan.bodyYaw)};  // yaw 0 = -Z
        const Vec3 want = fp::Sub(fp::Sub(plan.headEye, fp::Mul(fwd, plan.eyeForward)), Vec3{0.f, plan.eyeUp, 0.f});
        Vec3 shift = fp::Sub(want, Pos(At(w, v.head)));
        shift.y = shift.y > 0.2f ? shift.y - 0.2f : shift.y < -0.2f ? shift.y + 0.2f : 0.f;
        shift.y = std::clamp(shift.y, -0.6f, 0.2f);
        if (fp::Finite(shift) && fp::Len(shift) < 1.5f) {
            for (uint32_t i = 0; i < v.count; ++i) {
                Mat4 m = At(w, int(i));
                m.m[12] += shift.x;
                m.m[13] += shift.y;
                m.m[14] += shift.z;
                Put(w, int(i), m);
            }
            if (g_bodyFollowShifts.fetch_add(1, std::memory_order_relaxed) == 0)
                Log("FPBODY the drawn body follows the headset (fp_body_follow): first shift %.2f %.2f %.2f m", shift.x,
                    shift.y, shift.z);
        }
    }
    // With the game's hierarchy: the clean (turned) pose to carry descendants from, and the bones set explicitly.
    alignas(16) static thread_local float clean[kMaxBones * 16];
    alignas(16) static thread_local uint8_t set[kMaxBones];
    const bool tree = v.haveParents;
    if (tree) {
        std::memcpy(clean, w, size_t(v.count) * 64);
        std::memset(set, 0, v.count);
    }
    // 2. Arms to the controllers (B2S 6.3): two-bone IK, the hand turned by the anatomical frame onto the
    // controller's, upper-arm / forearm helpers and the fingers carried by their segment.
    if (plan.arms) {
        for (int s = 0; s < 2; ++s) {
            if (!plan.handValid[s] || !v.armReady[s] || v.arm[s] < 0 || v.fore[s] < 0 || v.hand[s] < 0) continue;
            Mat4 target;
            if (!fp::GripHandBasis(plan.grip[s], plan.wristPitch, &target)) continue;
            const Vec3 fwd{target.m[0], target.m[1], target.m[2]};
            const Vec3 wristTarget = fp::Sub(Pos(plan.grip[s]), fp::Mul(fwd, plan.wristBack));
            const Mat4 hand0 = At(w, v.hand[s]);
            const ArmSolve sol = SolveArm(Pos(At(w, v.arm[s])), Pos(At(w, v.fore[s])), Pos(hand0), wristTarget,
                                          fp::ArmPole(plan.bodyYaw, s == 1));
            if (!sol.valid) continue;
            Mat4 handNew = RotationOf(pose::Mul(hand0, sol.forearm));  // the animation's wrist on the new forearm
            Mat4 foreDelta = sol.forearm;
            float helperTwist = 0.f;
            if (v.anatValid[s]) {
                // bone = rel * rig (rel constant): the hand frame from the controller, keeping the bone's axes,
                // then limited like a wrist (owner 2026-10-27: "I can rotate them like a balloon"): at most 70 deg
                // of bend and 60 deg of roll beyond the forearm, which itself rolls half the way (at most 50 deg).
                const Mat4 wanted = fp::RetargetRotation(UnitRotation(hand0), pose::Mul(v.anat[s], UnitRotation(hand0)), target);
                const Vec3 axis = fp::Sub(sol.wrist, sol.elbow);
                const WristResult wr = LimitWrist(UnitRotation(handNew), UnitRotation(wanted), axis);
                handNew = WithScaleOf(wr.hand, hand0);
                // 0.16.0: with roll helpers and the hierarchy the forearm bone only swings; each helper takes the
                // wrist's twist by its place along the forearm (below). Otherwise the forearm rolls half the way.
                if (tree && v.rolls[s] > 0 && g_twistHelpers.load(std::memory_order_relaxed))
                    helperTwist = wr.twist;
                else if (std::fabs(wr.forearmRoll) > 1e-4f)
                    foreDelta = pose::Mul(sol.forearm, AboutPivot(AxisAngle(axis, wr.forearmRoll), sol.elbow));
                if (wr.limited) g_wristLimited.fetch_add(1, std::memory_order_relaxed);
            }
            handNew.m[12] = sol.wrist.x;
            handNew.m[13] = sol.wrist.y;
            handNew.m[14] = sol.wrist.z;
            if (tree) {
                Put(w, v.arm[s], pose::Mul(At(clean, v.arm[s]), sol.upper));
                Put(w, v.fore[s], pose::Mul(At(clean, v.fore[s]), foreDelta));
                Put(w, v.hand[s], handNew);
                set[v.arm[s]] = set[v.fore[s]] = set[v.hand[s]] = 1;
                // The roll helpers (children of the forearm): each takes the wrist's twist by its place. 0.35.0: written
                // every frame, also with no twist (then exactly where Propagate would put them), because skin nodes
                // resting on a roll helper are placed from it in CarryFollowers.
                {
                    const Vec3 twistAxis = fp::Sub(sol.wrist, sol.elbow);
                    for (int k = 0; k < v.rolls[s]; ++k) {
                        const int b = v.roll[s][k];
                        const float share = rigmap::RollShare(rigmap::PlaceAlong(clean, v.fore[s], v.hand[s], b));
                        Mat4 drawn = pose::Mul(At(clean, b), foreDelta);
                        if (std::fabs(helperTwist) > 1e-4f)
                            drawn = pose::Mul(drawn, AboutPivot(AxisAngle(twistAxis, helperTwist * share), sol.elbow));
                        Put(w, b, drawn);
                        set[b] = 1;
                    }
                }
                CarryFollowers(w, clean, v, s, set);
                continue;
            }
            Mat4 hand0Inv;
            if (!AffineInverse(hand0, &hand0Inv)) continue;
            const Mat4 handDelta = pose::Mul(hand0Inv, handNew);
            ApplyToGroup(w, v, uint8_t(kUpperL + s), v.arm[s], sol.upper);
            ApplyToGroup(w, v, uint8_t(kForeL + s), v.fore[s], foreDelta);
            ApplyToGroup(w, v, uint8_t(kHandL + s), v.hand[s], handDelta);
        }
    }
    // 3. The head, face and hair collapse into the neck (B2S 6.2); the neck itself stays.
    if (plan.hideHead && v.head >= 0) {
        const Vec3 at = v.neck >= 0 ? Pos(At(w, v.neck)) : Pos(At(w, v.head));
        if (tree) {
            // The head bone collapsed; its whole subtree follows it (Propagate). Learned head bones outside that
            // subtree (hair simulated from the neck, say) are collapsed below as before.
            Put(w, v.head, Collapsed(At(w, v.head), at));
            set[v.head] = 1;
            // 0.16.0 (rig pictures: Madison's fringe, "MecheLeft / RightBase / Mid"): hair helper groups rest on the
            // Head joint without being below it; they and what hangs below them collapse with the head.
            alignas(16) static thread_local uint8_t follow[kMaxBones];
            rigmap::HeadFollowers(clean, v.count, v.parent, v.head, v.neck, follow);
            for (uint32_t i = 0; i < v.count; ++i)
                if (follow[i] && !set[i]) {
                    Put(w, int(i), Collapsed(At(w, int(i)), at));
                    set[i] = 1;
                }
        }
        if (tree) Propagate(w, clean, v, set);
        for (uint32_t i = 0; i < v.count; ++i)
            if (v.group[i] == kHead && !(tree && set[i])) Put(w, int(i), Collapsed(At(w, int(i)), at));
    } else if (tree) {
        Propagate(w, clean, v, set);
    }
    // 4. fingers on the solved hands (0.22.0), after every descendant has its place on the new hand
    if (tree) ApplyFingers(w, plan, v);
}

// ---- idle hold (fpbody_math.h IdleHoldState; B2S fpbody IdleSource) ----
std::mutex g_idleMutex;
struct IdleHold {
    void* skeleton = nullptr;
    IdleHoldState state;
    std::vector<Mat4> rel;  // the snapshot: each bone in the root's frame
    LARGE_INTEGER last{};
    uint64_t lastLog = 0;
} g_idle;

// F1 only: the drawn body keeps the snapshot while the character stands still. Several meshes of the character
// share the skeleton: the state advances once per 5 ms, every mesh gets the same pose.
// 0.36.0 (owner: "the sleeves get bad when I stop moving sometimes"; LIVE 0.35.0 log: "idle hold on" logged again and
// again with no "released" between, and 2,486 .. 6,832 cloth jump resets while standing): the game updates the player's
// skeleton about 3 times a frame and the cloth path read every intermediate pose - those poses advanced (and released)
// the hold. Only the drawn pose (the palette, `advance`) moves the state now; the cloth and item paths only apply it.
void ApplyIdleHold(float* w, uint32_t count, const Plan& plan, int head, bool advance) {
    std::lock_guard lock(g_idleMutex);
    IdleHold& h = g_idle;
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    Mat4 rootInv;
    if (!plan.idleHold || !plan.turnBody || !plan.haveRoot || h.skeleton != plan.skeleton ||
        !AffineInverse(plan.root, &rootInv)) {
        if (!advance) return;  // only the drawn pose decides about the hold
        h.skeleton = plan.skeleton;
        h.state = IdleHoldState{};
        h.rel.clear();
        h.last = now;
        return;
    }
    const float dt = h.last.QuadPart ? float(double(now.QuadPart - h.last.QuadPart) / double(freq.QuadPart)) : 0.f;
    if (advance && (dt >= 0.005f || !h.last.QuadPart)) {
        h.last = now;
        float core = 0.f;  // bone 0 and the head against the snapshot (sitting, kneeling, bending)
        if (h.state.holding && h.rel.size() == count)
            for (int b : {0, head}) {
                if (b < 0 || uint32_t(b) >= count) continue;
                core = std::fmax(core, fp::Len(fp::Sub(Pos(pose::Mul(At(w, b), rootInv)), Pos(h.rel[size_t(b)]))));
            }
        const bool wasHolding = h.state.holding;
        if (h.state.Update(Pos(plan.root), plan.moveStick, core, std::fmin(dt, 0.25f))) {
            h.rel.resize(count);
            for (uint32_t i = 0; i < count; ++i) h.rel[i] = pose::Mul(At(w, int(i)), rootInv);
            g_idleHolds.fetch_add(1, std::memory_order_relaxed);
        }
        const uint64_t ms = GetTickCount64();
        if (wasHolding != h.state.holding && ms - h.lastLog >= 1000) {
            h.lastLog = ms;
            Log("FPBODY idle hold %s", h.state.holding ? "on: standing still, the idle animation is held"
                                                       : "released: the character moves / sits / bends");
        }
    }
    if (h.state.blend <= 0.f || h.rel.size() != count) return;
    for (uint32_t i = 0; i < count; ++i) {
        const Mat4 held = pose::Mul(h.rel[i], plan.root);
        const Mat4 m = h.state.blend > 0.99f ? held : BlendAffine(At(w, int(i)), held, h.state.blend);
        if (Finite(m.m)) Put(w, int(i), m);
    }
}

// ---- the head in mirrors (0.10.0 / 0.12.0, owner: "can you see the head in mirrors in first person?") ----
// STATIC 2026-10-28: a skinned record's palette is built into the SKELETON entity's double buffer: publish 0x14096da50
// toggles SKELETON+0x318, sets record+0x68 = SKELETON+0x320[+0x318] and record+0x70 = SKELETON + 0x598 + 4 * index,
// and *(SKELETON+0x330) is the record. Every pass's skeleton display (0x14096cd90) gives its draw the buffer at
// SKELETON+0x320[+0x318] at that moment (directly at context +0x348, or copied bone by bone to +0x340), so a pass
// draws whatever that buffer holds while it runs.
// Two kinds of pass show the player from outside: the reflection planes (VIEWPORT::DisplayReflections 0x14087e600,
// render context +0x25 = 1) and the render-to-texture cameras - mirrors, screens - (QDT::M3D::CAMERA_TEXTURE vfunc
// 0x1409a9bf0: its viewport pass 0x1408a15e0 into its texture). LIVE 0.10.0: swapping around the reflection planes
// only, the mirror still showed no head - the mirror is a CAMERA_TEXTURE. The player's records get a second palette
// with the head (same edited pose, the head kept); for the duration of either kind of pass the buffer each SKELETON
// draws from holds it, and the headless palette is put back afterwards.
std::atomic<bool> g_mirrorHead{true};
struct MirrorPalette {
    void* record = nullptr;
    uint8_t* skeleton = nullptr;  // the SKELETON entity (record+0x70 points into it)
    float* palette = nullptr;     // the game's buffer this palette was built for (record+0x68 then)
    std::vector<float> withHead, saved;
    uint32_t floats = 0;
    uint64_t ms = 0;
    bool swapped = false;
};
MirrorPalette g_mirror[32];
std::mutex g_mirrorMutex;
std::atomic<uint64_t> g_mirrorSwaps{0}, g_mirrorPasses{0};
using ReflectionsFn = void (*)(void* viewport, void* context, void* a3);
ReflectionsFn g_origReflections = nullptr;
thread_local int g_outsideDepth = 0;  // nested outside passes swap once

bool BuildInto(void* record, const float* worlds, float* palette) {
    auto* wslot = reinterpret_cast<const float**>(static_cast<uint8_t*>(record) + kRecWorlds);
    auto* pslot = reinterpret_cast<float**>(static_cast<uint8_t*>(record) + 0x68);
    const float* savedW = *wslot;
    float* savedP = *pslot;
    *wslot = worlds;
    *pslot = palette;
    __try {
        g_orig(record);
    } __finally {
        *wslot = savedW;
        *pslot = savedP;
    }
    return true;
}

// The SKELETON entity of a record: record+0x70 = SKELETON + 0x598 + 4 * index (index 0 or 1), and SKELETON+0x330 holds
// the record.
uint8_t* SkeletonOfRecord(void* record) {
    auto* p = static_cast<uint8_t*>(obj::Ptr(record, 0x70));
    if (!p) return nullptr;
    for (uintptr_t back : {uintptr_t(0x598), uintptr_t(0x59C)}) {
        uint8_t* e = p - back;
        if (obj::Ptr(e, 0x330) == record) return e;
    }
    return nullptr;
}

void BuildMirrorPalette(void* record, const float* worlds, uint32_t count) {
    float* palette = static_cast<float*>(obj::Ptr(record, 0x68));
    if (!palette) return;
    const uint32_t floats = count * 12;  // 3x4 per bone
    alignas(16) static thread_local float built[kMaxBones * 12];
    BuildInto(record, worlds, built);
    uint8_t* skel = SkeletonOfRecord(record);
    std::lock_guard lock(g_mirrorMutex);
    MirrorPalette* slot = nullptr;
    const uint64_t now = GetTickCount64();
    for (auto& m : g_mirror)
        if (m.palette == palette) slot = &m;
    if (!slot)
        for (auto& m : g_mirror)
            if (!slot && (!m.palette || now - m.ms > 2000)) slot = &m;
    if (!slot || slot->swapped) return;
    slot->record = record;
    slot->skeleton = skel;
    slot->palette = palette;
    slot->floats = floats;
    slot->withHead.assign(built, built + floats);
    slot->ms = now;
}

bool CopyFloats(float* dst, const float* src, uint32_t n) {
    __try {
        std::memcpy(dst, src, size_t(n) * 4);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The buffer a SKELETON's display reads right now: SKELETON+0x320[*(SKELETON+0x318)].
float* DisplayedPalette(uint8_t* skel) {
    uint32_t index = 0;
    if (!skel || !obj::Get(skel, 0x318, index) || index > 1) return nullptr;
    return static_cast<float*>(obj::Ptr(skel, 0x320 + size_t(index) * 8));
}

int SwapIn() {
    if (!g_active.load(std::memory_order_relaxed) || !g_mirrorHead.load(std::memory_order_relaxed)) return 0;
    std::lock_guard lock(g_mirrorMutex);
    const uint64_t now = GetTickCount64();
    int n = 0;
    for (auto& m : g_mirror) {
        if (!m.palette || m.swapped || now - m.ms > 150 || m.withHead.size() != m.floats) continue;
        // Only the buffer the draw will read: the one its SKELETON displays now (unknown SKELETON: the record's).
        float* shown = m.skeleton ? DisplayedPalette(m.skeleton) : static_cast<float*>(obj::Ptr(m.record, 0x68));
        if (shown != m.palette) continue;
        m.saved.resize(m.floats);
        if (!CopyFloats(m.saved.data(), m.palette, m.floats)) continue;
        if (!CopyFloats(m.palette, m.withHead.data(), m.floats)) continue;
        m.swapped = true;
        ++n;
    }
    return n;
}

void SwapOut() {
    std::lock_guard lock(g_mirrorMutex);
    for (auto& m : g_mirror)
        if (m.swapped) {
            CopyFloats(m.palette, m.saved.data(), m.floats);
            m.swapped = false;
        }
}

void NoteSwap(int n, const char* what) {
    if (n <= 0) return;
    g_mirrorSwaps.fetch_add(uint64_t(n), std::memory_order_relaxed);
    const uint64_t k = g_mirrorPasses.fetch_add(1, std::memory_order_relaxed);
    static uint64_t lastLog = 0;
    const uint64_t now = GetTickCount64();
    if (!k || now - lastLog >= 30000) {
        lastLog = now;
        Log("FPBODY mirror: the player's head drawn in %s (%d palette(s); %llu outside passes so far)", what, n,
            static_cast<unsigned long long>(k + 1));
    }
}

void HookReflections(void* viewport, void* context, void* a3) {
    const int n = g_outsideDepth++ == 0 ? SwapIn() : 0;
    g_origReflections(viewport, context, a3);
    if (--g_outsideDepth == 0 && n) SwapOut();
    NoteSwap(n, "the reflection planes");
}


bool CallWithWorlds(void* record, const float* worlds) {
    auto* slot = reinterpret_cast<const float**>(static_cast<uint8_t*>(record) + kRecWorlds);
    const float* saved = *slot;
    *slot = worlds;
    __try {
        g_orig(record);
    } __finally {
        *slot = saved;
    }
    return true;
}

bool DrawProp(void* record, const float* worlds);
bool RootEditOf(const float* worlds, Mat4* edit);  // 0.33.0, below with the item seam
extern std::atomic<uint64_t> g_rootPalettes;
std::atomic<uint64_t> g_propCalls{0}, g_propTicks{0};
std::atomic<int> g_handIdx[2] = {-1, -1};  // 0.31.0: the player's hand bones, for the loose-item distance test

// 0.22.0 (playbook 14.12): the rest shape. STATIC (SKIN::PaletteBuild 0x140a61920): palette[i] = *(record+0x48)[i] x
// world[i] x inverse(node world), so *(record+0x48) holds inverse(bind) per bone and the rest pose in object space is
// its inverse. Read once per rig; a bone whose matrix is unusable keeps the animated offset (fingers_math UsableRest).
void CaptureRest(void* record, void* skeleton, uint32_t count) {
    {
        std::lock_guard lock(g_rigMutex);
        if (!g_rig || g_rig->skeleton != skeleton || g_rig->count != count || g_rig->haveRest) return;
    }
    const float* inv = static_cast<const float*>(obj::Ptr(record, 0x48));
    std::vector<float> bind(size_t(count) * 16), rest(size_t(count) * 16, 0.f);
    if (!inv || !obj::Bytes(inv, 0, bind.data(), bind.size() * sizeof(float))) return;
    int usable = 0;
    for (uint32_t i = 0; i < count; ++i) {
        Mat4 r;
        if (fingers::UsableRest(&bind[size_t(i) * 16]) && AffineInverse(At(bind.data(), int(i)), &r) &&
            fingers::UsableRest(r.m)) {
            Put(rest.data(), int(i), r);
            ++usable;
        }  // else left all zero: UsableRest refuses it, the bone keeps the animated offset
    }
    std::lock_guard lock(g_rigMutex);
    if (!g_rig || g_rig->skeleton != skeleton || g_rig->count != count || g_rig->haveRest) return;
    g_rig->rest.swap(rest);
    g_rig->haveRest = true;
    g_rig->restUsable = usable;
    Log("FPFINGER rest shape read from the skin data (record +0x48 = inverse binds): %d of %u bones usable", usable, count);
}
void HookPalette(void* record) {
    g_calls.fetch_add(1, std::memory_order_relaxed);
    if (!g_active.load(std::memory_order_relaxed) || !g_enabled.load(std::memory_order_relaxed)) {
        g_orig(record);
        return;
    }
    const float* worlds = static_cast<const float*>(obj::Ptr(record, kRecWorlds));
    void* skel = g_skeleton.load(std::memory_order_relaxed);
    if (!worlds || (worlds != obj::Ptr(skel, kSkelWorlds) && worlds != obj::Ptr(skel, kSkelWorldsA))) {
        // 0.18.0: an object attached to the player whose skeleton copies the player's (a coat) goes with the drawn arms
        // 0.20.0: the test is timed (fpstatus): it runs for every skinned record that is not the player's
        bool drawn = false;
        // 0.33.0: an object whose skeleton root the item routes moved (RememberRootEdit) is drawn from its worlds x the
        // edit; the skeleton itself keeps the game's pose (render-only, Gate 3 row 5).
        Mat4 rootEdit;
        uint32_t rootCount = 0;
        if (worlds && RootEditOf(worlds, &rootEdit) && obj::Get(record, kRecCount, rootCount) && rootCount > 0 &&
            rootCount <= uint32_t(kMaxBones)) {
            alignas(16) static thread_local float moved[kMaxBones * 16];
            if (obj::Bytes(worlds, 0, moved, size_t(rootCount) * 64) && Finite(moved)) {
                for (uint32_t i = 0; i < rootCount; ++i) Put(moved, int(i), pose::Mul(At(moved, int(i)), rootEdit));
                g_rootPalettes.fetch_add(1, std::memory_order_relaxed);
                if (!(skel && DrawProp(record, moved))) CallWithWorlds(record, moved);
                return;
            }
        }
        if (worlds && skel) {
            LARGE_INTEGER t0, t1;
            QueryPerformanceCounter(&t0);
            drawn = DrawProp(record, worlds);
            QueryPerformanceCounter(&t1);
            if (!drawn) {  // the palette build itself is not the test's cost
                g_propCalls.fetch_add(1, std::memory_order_relaxed);
                g_propTicks.fetch_add(uint64_t(t1.QuadPart - t0.QuadPart), std::memory_order_relaxed);
            }
        }
        if (!drawn) g_orig(record);  // another character, a prop, the scenery
        return;
    }
    Plan plan;
    {
        std::lock_guard lock(g_planMutex);
        plan = g_plan;
    }
    uint32_t count = 0;
    if (!plan.active || GetTickCount64() - plan.ms > 250 || !worlds || !obj::Get(record, kRecCount, count) || count == 0 ||
        count > uint32_t(kMaxBones) ||
        (worlds != obj::Ptr(plan.skeleton, kSkelWorlds) && worlds != obj::Ptr(plan.skeleton, kSkelWorldsA))) {
        g_orig(record);
        return;
    }
    // The renderer reads the copy with SSE loads: 16-byte aligned (B2S: a misaligned copy crashed with #GP).
    alignas(16) static thread_local float copy[kMaxBones * 16];
    if (!obj::Bytes(worlds, 0, copy, size_t(count) * 64) || !Finite(copy)) {
        g_orig(record);
        return;
    }
    Learn(plan.skeleton, copy, count);  // the clean pose
    CaptureRest(record, plan.skeleton, count);
    RigView view;
    if (!View(plan.skeleton, &view) || view.count != count) {
        g_orig(record);
        return;
    }
    for (int s = 0; s < 2; ++s) g_handIdx[s].store(view.hand[s], std::memory_order_relaxed);  // 0.31.0 loose items
    ApplyIdleHold(copy, count, plan, view.head, /*advance=*/true);  // the drawn pose: the hold's state moves here
    // The mirror's palette (0.10.0): the same edits without hiding the head, built first from the same pose.
    alignas(16) static thread_local float withHead[kMaxBones * 16];
    const bool mirror = plan.hideHead && g_mirrorHead.load(std::memory_order_relaxed);
    if (mirror) std::memcpy(withHead, copy, size_t(count) * 64);
    Edit(copy, plan, view);
    g_edits.fetch_add(1, std::memory_order_relaxed);
    CallWithWorlds(record, copy);
    if (mirror) {
        Plan shown = plan;
        shown.hideHead = false;
        Edit(withHead, shown, view);
        BuildMirrorPalette(record, withHead, count);
    }
}


// ---- items attached to the player (0.17.0 / 0.18.0, playbook 13.8; docs/ITEMS.md) ----
// STATIC 2026-11-12: every way Heavy Rain puts an object on a character ends in one link. The scripts'
// AttachObjectToBone / ...DontKeepMatrix / AttachObjectToBodyNode (handlers 0x140304460 / 0x1403048a0 / 0x1402ffae0)
// and the animation system's own attach (SCRIPT_ENTITY_CONTROLLER_PLAY_ANIMO 0x1404aa800 -> 0x1402f8a90, the bone
// named by the ANIMATION_MODIFIER: index +0xF0, CRC32 +0xF4, offset +0x100, angles +0x110) all call
// SCRIPT_ENTITY_ABC::AttachEntity 0x1402de5d0 -> 0x1408920a0 / 0x140892530: the object's scene NODE becomes a child of
// the character's node with NODE+0x120 = the bone index (-1 = the body node). NODE::UpdateWorld 0x140893860 then
// builds world (+0x90) = local (+0x50) x ParentMatrix(node), and ParentMatrix 0x14017b750 returns the address of the
// parent skeleton's world matrix of that bone (*(skeleton+0x10) + bone * 0x40), or the parent node's world (+0x90)
// for -1. Two more callers copy the same matrix (0x140893700, and 0x1409fd4b0: the root of an attached object's own
// skeleton). That array is the CLEAN animated pose, so with the arms on the controllers an object stayed at the
// animated hand.
//
// The hook answers, for an object on one of the player's bones, with that bone's matrix in the DRAWN pose (the same
// idle hold + Edit as the palette). The object keeps its authored local, so it sits in the drawn hand as it sits in
// the animated one, and is culled where it is drawn (its node world is the drawn one).
// 0.18.0: an object on the player's BODY NODE gets the node's world moved as the drawn Hips are moved (the body
// turn, the idle hold), and the same hand rule as below.
// DATA (all 29,437 ANIMATION_MODIFIER chunks): 45 name a hand bone with a fixed offset; 498 name the Hips with no
// offset - there the object's own animation carries it to the hand. For those the link says nothing about a hand,
// so an object on a bone outside the tracked arms that lies within reach of a tracked hand in the CLEAN pose takes
// that hand's edit (rigmap::HandCarryWeight: all of it within 30 cm of the wrist, none beyond 45 cm). INFERENCE.
// 0.18.0: an attached object with a skeleton of its own is remembered; when its skin palette is built, the bones
// that lie on the player's are drawn from the player's drawn bones (rigmap::CarryCopy; render-only, like the body).
using ParentMatrixFn = const float* (*)(void* node);
ParentMatrixFn g_origParent = nullptr;
std::atomic<bool> g_items{true};
std::atomic<uint64_t> g_itemAnswers{0}, g_itemByHand{0}, g_itemBody{0}, g_propPalettes{0};
std::atomic<uint64_t> g_itemHandBone{0}, g_itemOtherBone{0};  // 0.30.1 census (playbook 19.8, revision 16)
constexpr size_t kNodeLocal = 0x50, kNodeWorld = 0x90, kNodeSkeleton = 0x160;

bool SameMatrix(const Mat4& a, const Mat4& b) {
    for (int k = 0; k < 16; ++k)
        if (std::fabs(a.m[k] - b.m[k]) > 1e-5f) return false;
    return true;
}

// The player's clean and drawn pose of this moment, per thread, shared by what is updated together (1 ms).
struct PlayerPose {
    alignas(16) float clean[kMaxBones * 16];
    alignas(16) float drawn[kMaxBones * 16];
    alignas(16) float base[kMaxBones * 16];  // clean, moved as the drawn Hips are moved: what props were animated against
    RigView view;
    Plan plan;
    void* skeleton = nullptr;
    uint32_t count = 0;
    LARGE_INTEGER stamp{};
    bool usable = false;
    Mat4 body = pose::Identity();  // clean Hips^-1 x drawn Hips
    bool bodyMoved = false;
};
PlayerPose* PoseOfPlayer(void* skeleton) {
    static thread_local PlayerPose* p = nullptr;
    if (!p) p = new PlayerPose();
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    if (p->skeleton == skeleton && (now.QuadPart - p->stamp.QuadPart) * 1000 <= freq.QuadPart) return p->usable ? p : nullptr;
    p->skeleton = skeleton;
    p->stamp = now;
    p->usable = false;
    {
        std::lock_guard lock(g_planMutex);
        p->plan = g_plan;
    }
    const float* base = static_cast<const float*>(obj::Ptr(skeleton, kSkelWorlds));
    uint32_t count = 0;
    if (!base || !obj::Get(skeleton, 0, count) || count == 0 || count > uint32_t(kMaxBones)) return nullptr;
    if (!p->plan.active || p->plan.skeleton != skeleton || GetTickCount64() - p->plan.ms > 250 ||
        !obj::Bytes(base, 0, p->clean, size_t(count) * 64) || !Finite(p->clean) || !View(skeleton, &p->view) ||
        p->view.count != count)
        return nullptr;
    p->count = count;
    std::memcpy(p->drawn, p->clean, size_t(count) * 64);
    ApplyIdleHold(p->drawn, count, p->plan, p->view.head, /*advance=*/false);
    Edit(p->drawn, p->plan, p->view);
    Mat4 inv;
    p->body = pose::Identity();
    if (AffineInverse(At(p->clean, 0), &inv)) p->body = pose::Mul(inv, At(p->drawn, 0));  // bone 0 = Hips on every rig
    p->bodyMoved = !SameMatrix(p->body, pose::Identity());
    for (uint32_t i = 0; i < count; ++i) Put(p->base, int(i), p->bodyMoved ? pose::Mul(At(p->clean, int(i)), p->body) : At(p->clean, int(i)));
    p->usable = true;
    return p;
}

// Objects with a skeleton of their own that may copy the player's: attached ones seen by the ParentMatrix hook (any
// thread), and - 0.19.0 - unattached ones found by the palette hook itself (a skinned object that is not a character
// and has three or more bones named like the player's). Used by the palette hook.
struct CopyProp {
    void* skeleton = nullptr;
    const void* worlds[2] = {nullptr, nullptr};  // its bone arrays (+0x10, +0x08): what a skinned record points at
    uint64_t ms = 0, lastLog = 0;
    bool attached = false;  // its root is one of the player's bones / the body node (moved with the drawn body)
    bool never = false;     // looked at once: a character, or no names in common
    bool logged = false;
};
CopyProp g_props[160];  // a crowd scene has dozens of skinned characters: each is looked at once
std::mutex g_propMutex;
CopyProp* PropSlot(void* skel, uint64_t now, bool create) {  // g_propMutex held
    for (auto& q : g_props)
        if (q.skeleton == skel) return &q;
    if (!create) return nullptr;
    for (auto& q : g_props)
        if (!q.skeleton || now - q.ms > 2000) {
            q = CopyProp{};
            q.skeleton = skel;
            q.worlds[0] = obj::Ptr(skel, kSkelWorlds);
            q.worlds[1] = obj::Ptr(skel, kSkelWorldsA);
            return &q;
        }
    return nullptr;
}
void RememberProp(void* node, void* playerSkeleton) {
    void* skel = obj::Ptr(node, kNodeSkeleton);
    uint32_t n = 0;
    if (!skel || skel == playerSkeleton || !obj::Get(skel, 0, n) || n < 2 || n > uint32_t(kMaxBones)) return;
    const uint64_t now = GetTickCount64();
    std::lock_guard lock(g_propMutex);
    if (CopyProp* slot = PropSlot(skel, now, true)) {
        slot->ms = now;
        slot->attached = true;
        slot->never = false;
    }
}

// The skeleton instance behind a skinned record: record+0x70 -> SKELETON entity (SkeletonOfRecord), entity+0x68 = the
// node's handle, handle+0x50 = the NODE once resolved (flag 0x200000 at +0x30), NODE+0x160 = the instance
// (STATIC: SKELETON slot 105, 0x14096da50, reads exactly this chain to fill the record).
void* InstanceOfRecord(void* record, const float* worlds) {
    uint8_t* entity = SkeletonOfRecord(record);
    if (!entity) return nullptr;
    void* handle = obj::Ptr(entity, 0x68);
    uint32_t flags = 0;
    if (!handle || !obj::Get(handle, 0x30, flags) || !(flags & 0x200000)) return nullptr;
    void* node = obj::Ptr(handle, 0x50);
    void* inst = node ? obj::Ptr(node, kNodeSkeleton) : nullptr;
    if (!inst || (obj::Ptr(inst, kSkelWorlds) != worlds && obj::Ptr(inst, kSkelWorldsA) != worlds)) return nullptr;
    return inst;
}

// Hand rule, measured (fpstatus): the distances at which objects were carried, in millimetres.
std::atomic<int> g_handMinMm{100000}, g_handMaxMm{0};
std::atomic<uint64_t> g_handFading{0};
void NoteHandCarry(float dist, float weight) {
    const int mm = int(dist * 1000.f);
    int v = g_handMinMm.load();
    while (mm < v && !g_handMinMm.compare_exchange_weak(v, mm)) {}
    v = g_handMaxMm.load();
    while (mm > v && !g_handMaxMm.compare_exchange_weak(v, mm)) {}
    if (weight < 0.999f) g_handFading.fetch_add(1, std::memory_order_relaxed);
}

// 0.30.0: is an object on the Hips / body node a prop the animation carries to a hand, or something worn? Measured per
// object from how it moves (rigmap::CarryVote); undecided = worn.
struct ItemMotion {
    void* node = nullptr;
    uint64_t ms = 0;
    Vec3 local{}, rel[2]{};
    rigmap::CarryVote vote;
};
std::mutex g_motionMutex;
ItemMotion g_itemMotion[64];
std::atomic<uint64_t> g_heldVotes{0}, g_wornVotes{0};
bool HeldByMotion(void* node, int bone, const Vec3& local, const Vec3 rel[2], const bool relValid[2]) {
    const uint64_t now = GetTickCount64();
    std::lock_guard lock(g_motionMutex);
    ItemMotion* slot = nullptr;
    for (auto& m : g_itemMotion)
        if (m.node == node) slot = &m;
    if (!slot) {
        for (auto& m : g_itemMotion)
            if (!m.node || now - m.ms > 3000) {
                slot = &m;
                break;
            }
        if (!slot) return false;
        *slot = ItemMotion{};
        slot->node = node;
        slot->ms = now;
        slot->local = local;
        for (int s = 0; s < 2; ++s) slot->rel[s] = rel[s];
        return false;
    }
    if (now - slot->ms < 10) return slot->vote.held;  // several passes of one frame: one sample
    const float hipsStep = fp::Len(fp::Sub(local, slot->local));
    float wristStep = 1e9f;
    for (int s = 0; s < 2; ++s)
        if (relValid[s]) wristStep = std::min(wristStep, fp::Len(fp::Sub(rel[s], slot->rel[s])));
    slot->ms = now;
    slot->local = local;
    for (int s = 0; s < 2; ++s) slot->rel[s] = rel[s];
    if (wristStep < 1e8f && slot->vote.Feed(hipsStep, wristStep)) {
        (slot->vote.held ? g_heldVotes : g_wornVotes).fetch_add(1, std::memory_order_relaxed);
        static std::atomic<int> logs{0};
        if (logs.fetch_add(1, std::memory_order_relaxed) < 40)
            Log("FPITEM node %p on the player's %s: %s (moving %.1f mm against its parent, %.1f mm against the wrist "
                "per sample)", node, bone < 0 ? "body node" : "Hips",
                slot->vote.held ? "carried - it moves with a wrist, the hand rule applies"
                                : "worn - it stays with the hips, drawn with the body",
                slot->vote.hipsEma * 1000.f, slot->vote.wristEma * 1000.f);
    }
    return slot->vote.held;
}

void LogItem(void* node, int bone, int hand, float dist) {
    static std::mutex m;
    static void* seen[48];
    static int n = 0;
    std::lock_guard lock(m);
    for (int i = 0; i < n; ++i)
        if (seen[i] == node) return;
    if (n >= 48) return;
    seen[n++] = node;
    char where[40];
    if (bone < 0) strcpy_s(where, "body node");
    else sprintf_s(where, "bone %d", bone);
    if (hand < 0)
        Log("FPITEM node %p on the player's %s: drawn from the drawn body", node, where);
    else
        Log("FPITEM node %p on the player's %s, %.2f m from the %s wrist: carried with that hand", node, where, dist,
            hand ? "right" : "left");
}

// ---- 0.31.0 .. 0.33.0: objects a scene clip carries with NO parent link to the player (docs/ITEMS.md 4h) ----
// Owner 2026-11-19: "carried objects follow the animated hand ... i just want motion controls". Heavy Rain plays a
// handled object's own clip beside the character's (`_CT_<character>_<verb>` / `_CT_<object>_<verb>`); the object is a
// rigid mesh on its own NODE chain under the room node (LANDING_LIVING_KITCHEN -> PIZZABOX_L -> PIZZABOX), never parented
// to the player, so this hook sees it with the room's world and the answer moves that node and everything below it.
// 0.33.0 (playbook 19.8 point 11 Gates 1 .. 3; 0.31.0's motion vote LooseCarry did not pass Gate 2): which object, and
// when, follow the engine's link and the Gate 2 rule -
//   link  : the node (or its parent node) is an entity of a MOVIE playing with the player (movielink.h), and the class
//           table says "held" or does not know it (item_class.h: worn / stays / person stay where the game puts them;
//           offline replay with exactly this lookup, rig_gate2.py --route entity: 0 open pairs);
//   carry : rigmap::MovieCarry, sampled at the clips' 30 Hz with the clean wrists and the clean Head (movie_item.h);
//   draw  : rigmap::BlendCarry - anim x clean^-1 x drawn per hand, cross-faded by the two hands' weights;
//   hands : g_handItemMs[s] while that hand's weight > 0 - its fingers play the animation (Gate 3 row 3);
//   seed  : an object the bone route had in a hand socket < 1 s ago starts in that hand (put-back clips; desk:
//           tests/movie_item_tests.cpp, TAKE -> socket -> PUT without a step).
// Only within 60 cm of a clean wrist (or already tracked); never a node that owns a skeleton of more than 8 bones (a
// character); never below a node it tracks (the child inherits).
std::atomic<bool> g_looseItems{true}, g_looseOn{false};
std::atomic<int> g_looseLive{0};
std::atomic<uint64_t> g_looseAnswers{0}, g_looseCarried{0}, g_looseLetGo{0}, g_looseSeeded{0}, g_looseInherit{0},
    g_looseOffClass{0}, g_looseHandHolds{0};
struct LooseNode {
    void* node = nullptr;
    const float* world = nullptr;  // node + 0x90: what ParentMatrix returns for the node's children
    LARGE_INTEGER at{};
    uint64_t ms = 0;
    movieitem::State item;
    bool held = false;  // the last logged decision
    bool classLogged = false;
    float driftCm = 0.f, driftDeg = 0.f, driftWeight = 0.f;  // largest in the current 10 s (Gate 3 row 8)
    int driftHand = -1;
    char entity[64] = {};
};
struct InHand {
    void* node = nullptr;
    uint64_t ms = 0;
    int hand = -1;
};
std::mutex g_looseMutex;
LooseNode g_loose[32];
std::atomic<void*> g_looseIds[32];  // g_loose[i].node, readable without the lock (a far node without a slot skips it)
InHand g_inHand[16];

bool LooseTracked(void* node) {
    for (auto& id : g_looseIds)
        if (id.load(std::memory_order_relaxed) == node) return true;
    return false;
}
void LooseClear(int i) {  // g_looseMutex held
    g_loose[i] = LooseNode{};
    g_looseIds[i].store(nullptr, std::memory_order_relaxed);
    g_looseLive.fetch_sub(1, std::memory_order_relaxed);
}

void NoteInHand(void* node, int hand) {  // the bone path answered it on (or below) that hand bone
    const uint64_t now = GetTickCount64();
    std::lock_guard lock(g_looseMutex);
    for (int i = 0; i < 32; ++i)  // in the socket now: a put-back after this starts a fresh, seeded slot
        if (g_loose[i].node == node) LooseClear(i);
    InHand* slot = nullptr;
    for (auto& h : g_inHand)
        if (h.node == node) slot = &h;
    if (!slot)
        for (auto& h : g_inHand)
            if (!slot || h.ms < slot->ms) slot = &h;  // the oldest
    slot->node = node;
    slot->ms = now;
    slot->hand = hand;
}

const float* LooseAnswer(void* node, const float* r, void* skeleton, const float* base, uint32_t count) {
    if (!g_looseItems.load(std::memory_order_relaxed) || !g_looseOn.load(std::memory_order_relaxed) ||
        node == g_node.load(std::memory_order_relaxed))
        return r;
    Mat4 local, par;
    if (!obj::Bytes(node, kNodeLocal, local.m, sizeof(local.m)) || !Finite(local.m) ||
        !obj::Bytes(r, 0, par.m, sizeof(par.m)) || !Finite(par.m))
        return r;
    const Mat4 world = pose::Mul(local, par);  // where the animation has the object
    const Vec3 at = Pos(world);
    bool nearWrist = false;
    for (int s = 0; s < 2 && !nearWrist; ++s) {
        const int h = g_handIdx[s].load(std::memory_order_relaxed);
        float w[3];
        if (h >= 0 && uint32_t(h) < count && obj::Bytes(base + size_t(h) * 16 + 12, 0, w, sizeof(w)))
            nearWrist = fp::Len(fp::Sub(at, Vec3{w[0], w[1], w[2]})) < 0.6f;
    }
    const bool tracked = LooseTracked(node);
    if (!nearWrist && !tracked) return r;  // the common case: no lock
    // The engine's link (Gate 3 row 9): a movie playing this object with the player, and its class.
    movielink::Link link;
    const void* parentNode = reinterpret_cast<const uint8_t*>(r) - kNodeWorld;  // r = the parent node's world (bone -1)
    const bool inMovie = movielink::Find(node, parentNode, g_node.load(std::memory_order_relaxed), &link);
    const bool linked = inMovie && (link.cls == itemclass::kHeld || link.cls == itemclass::kNotKnown);
    if (inMovie && !linked) g_looseOffClass.fetch_add(1, std::memory_order_relaxed);
    if (!linked && !tracked) return r;  // a tracked one is fed link = false below, so it lets go within the ramp
    const uint64_t now = GetTickCount64();
    LARGE_INTEGER qpc, freq;
    QueryPerformanceCounter(&qpc);
    QueryPerformanceFrequency(&freq);
    bool sample = false, seeded = false, logClass = false;
    int seedHand = -1;
    {
        std::lock_guard lock(g_looseMutex);
        LooseNode* slot = nullptr;
        for (int i = 0; i < 32; ++i) {
            LooseNode& q = g_loose[i];
            if (!q.node) continue;
            if (now - q.ms > 3000) {  // not seen for 3 s: the object is gone or far from the hands
                LooseClear(i);
                continue;
            }
            if (q.world == r) {  // below a node this route tracks: it inherits that node's answer
                if (q.item.Held()) g_looseInherit.fetch_add(1, std::memory_order_relaxed);
                return r;
            }
            if (q.node == node) slot = &q;
        }
        if (!slot) {
            if (!linked) return r;
            void* sk = obj::Ptr(node, kNodeSkeleton);
            uint32_t bones = 0;
            if (sk && obj::Get(sk, 0, bones) && bones > 8) return r;  // a character, never an item
            for (int i = 0; i < 32 && !slot; ++i)
                if (!g_loose[i].node) {
                    slot = &g_loose[i];
                    g_looseIds[i].store(node, std::memory_order_relaxed);
                }
            if (!slot) return r;  // 32 objects near the hands at once: the rest stay as animated
            *slot = LooseNode{};
            slot->node = node;
            slot->world = reinterpret_cast<const float*>(static_cast<const uint8_t*>(node) + kNodeWorld);
            g_looseLive.fetch_add(1, std::memory_order_relaxed);
            for (auto& h : g_inHand)
                if (h.node == node && now - h.ms < 1000 && h.hand >= 0) {
                    slot->item.Seed(h.hand);
                    slot->held = true;
                    seeded = true;
                    seedHand = h.hand;
                }
        }
        if (inMovie) std::snprintf(slot->entity, sizeof(slot->entity), "%s", link.entity);
        if (inMovie && !linked && !slot->classLogged) slot->classLogged = logClass = true;
        slot->ms = now;
        sample = !slot->at.QuadPart || (qpc.QuadPart - slot->at.QuadPart) * 30 >= freq.QuadPart;
        if (!sample && !slot->item.Held()) return r;
    }
    if (seeded) {
        g_looseSeeded.fetch_add(1, std::memory_order_relaxed);
        Log("FPITEM node %p left the player's %s hand socket: it stays in the drawn hand while its clip plays", node,
            seedHand ? "right" : "left");
    }
    if (logClass)
        Log("FPITEM node %p \"%s\" in movie \"%s\" is %s: it stays where the game puts it", node, link.entity, link.movie,
            itemclass::Name(link.cls));
    PlayerPose* p = PoseOfPlayer(skeleton);
    if (!p || p->count != count) return r;
    const float* clean[2] = {nullptr, nullptr};
    const float* drawn[2] = {nullptr, nullptr};
    for (int s = 0; s < 2; ++s)
        if (p->view.hand[s] >= 0) {
            clean[s] = p->clean + size_t(p->view.hand[s]) * 16;
            drawn[s] = p->drawn + size_t(p->view.hand[s]) * 16;
        }
    const float* head = p->view.head >= 0 ? p->clean + size_t(p->view.head) * 16 : nullptr;
    movieitem::State item;
    bool changed = false, heldNow = false;
    {
        std::lock_guard lock(g_looseMutex);
        LooseNode* slot = nullptr;
        for (auto& q : g_loose)
            if (q.node == node) slot = &q;
        if (!slot) return r;
        if (sample) {
            slot->item.Sample(linked, world.m, clean, head);
            slot->at = qpc;
            heldNow = slot->item.Held();
            changed = heldNow != slot->held;
            slot->held = heldNow;
        }
        item = slot->item;
    }
    if (changed) {
        (heldNow ? g_looseCarried : g_looseLetGo).fetch_add(1, std::memory_order_relaxed);
        static std::atomic<int> logs{0};
        if (logs.fetch_add(1, std::memory_order_relaxed) < 60)
            Log("FPITEM node %p \"%s\" (movie link): %s", node, link.entity,
                heldNow ? "moves with a wrist - drawn in that controller hand" : "let go - drawn where its clip has it");
    }
    if (!item.Held()) return r;
    Mat4 drawnWorld, invLocal;
    if (!item.Draw(world.m, clean, drawn, drawnWorld.m) || !Finite(drawnWorld.m) || !AffineInverse(local, &invLocal)) return r;
    const Mat4 answer = pose::Mul(invLocal, drawnWorld);  // local x answer = the drawn world
    if (!Finite(answer.m) || SameMatrix(answer, par)) return r;
    // Gate 3 row 3: the hand that holds it plays the animation's fingers (fingers off while < 500 ms old).
    for (int s = 0; s < 2; ++s)
        if (item.wt[s] > 0.f) {
            const uint64_t t = GetTickCount64();
            g_looseHandHolds.fetch_add(1, std::memory_order_relaxed);
            if (t - g_handItemMs[s].exchange(t) > 5000)
                Log("FPFINGER the %s hand holds node %p \"%s\" (movie route): its fingers play the animation",
                    s ? "right" : "left", node, link.entity);
        }
    // Gate 3 row 8: its place in the drawn hand against its place in the animated hand (0 at full weight).
    const int h = item.wt[1] > item.wt[0] ? 1 : 0;
    if (clean[h] && drawn[h]) {
        float cm = 0.f, deg = 0.f;
        movieitem::Drift(world.m, drawnWorld.m, clean[h], drawn[h], &cm, &deg);
        std::lock_guard lock(g_looseMutex);
        for (auto& q : g_loose)
            if (q.node == node) {
                q.driftCm = std::fmax(q.driftCm, cm);
                q.driftDeg = std::fmax(q.driftDeg, deg);
                q.driftWeight = std::fmax(q.driftWeight, item.wt[h]);
                q.driftHand = h;
            }
    }
    if (item.wt[h] >= 0.999f && clean[h])
        NoteHandCarry(fp::Len(fp::Sub(at, Vec3{clean[h][12], clean[h][13], clean[h][14]})), 1.f);
    g_looseAnswers.fetch_add(1, std::memory_order_relaxed);
    alignas(16) static thread_local float out[16];
    std::memcpy(out, answer.m, sizeof(out));
    return out;
}

// One line per object the movie route holds, every 10 s (Gate 3 row 8, playbook 19.8 census): the largest distance of
// its drawn place from its place in the hand.
void LogDrift() {
    std::lock_guard lock(g_looseMutex);
    for (auto& q : g_loose) {
        if (!q.node || q.driftWeight <= 0.f) continue;
        Log("FPITEM drift 10 s: node %p \"%s\" in the %s hand (weight up to %.2f): at most %.2f cm / %.1f deg from its "
            "place in the animated hand (0 at full weight; a pick-up, release or pass shows here)",
            q.node, q.entity, q.driftHand ? "right" : "left", q.driftWeight, q.driftCm, q.driftDeg);
        q.driftCm = q.driftDeg = q.driftWeight = 0.f;
    }
}

// 0.36.6 (owner 2026-11-20: "i played trough as shelby two times and i never had finger detection"; LIVE 0.36.5 log:
// right after the character appeared both hands "held" a node on the hand bone 6.0 / 7.1 cm out, and the 5 s re-log
// never came again - the nodes stayed for the whole session, so both hands played the animation all the time). A node
// that is already on the hand when the character appears (within 3 s of its skeleton being first seen) is part of the
// hand - an accessory or a helper - unless a playing movie holds it with the player (movielink, the engine's own link
// of every held pair, Gate 1 / 3). A node attached later (a pick-up) is an item as before.
struct HandNode {
    void* node = nullptr;
    void* skeleton = nullptr;
    uint64_t ms = 0;
    bool accessory = false;
};
HandNode g_handNodes[16];
std::mutex g_handNodeMutex;
void* g_handRigSkeleton = nullptr;
uint64_t g_handRigSince = 0;
std::atomic<uint64_t> g_handAccessories{0};
bool HandNodeIsItem(void* node, void* skeleton, int side, int bone, float out) {
    const uint64_t now = GetTickCount64();
    bool accessory = false, fresh = false;
    {
        std::lock_guard lock(g_handNodeMutex);
        if (skeleton != g_handRigSkeleton) {
            g_handRigSkeleton = skeleton;
            g_handRigSince = now;
            for (auto& h : g_handNodes) h = {};
        }
        HandNode* slot = nullptr;
        for (auto& h : g_handNodes)
            if (h.node == node && h.skeleton == skeleton) slot = &h;
        if (!slot) {
            for (auto& h : g_handNodes)
                if (!slot || h.ms < slot->ms) slot = &h;  // a free one (ms 0) or the oldest
            *slot = {node, skeleton, now, now - g_handRigSince < 3000};
            fresh = true;
        }
        slot->ms = now;
        accessory = slot->accessory;
    }
    if (!accessory) return true;
    movielink::Link link;
    const bool held = movielink::Find(node, nullptr, g_node.load(std::memory_order_relaxed), &link) &&
                      (link.cls == itemclass::kHeld || link.cls == itemclass::kNotKnown);
    if (fresh) {
        g_handAccessories.fetch_add(1, std::memory_order_relaxed);
        Log("FPFINGER the %s hand has node %p on it since the character appeared (bone %d, %.1f cm out): %s", side ? "right" : "left",
            node, bone, 100.f * out,
            held ? "a playing movie holds it with the player - an item, the fingers play the animation"
                 : "no movie holds it - part of the hand, the fingers stay on the controllers");
    }
    return held;
}

// What the draw gets for this node (0.17.0 .. 0.31.0: what every caller got). r = the game's matrix.
const float* ItemAnswer(void* node, const float* r) {
    void* skeleton = g_skeleton.load(std::memory_order_relaxed);
    if (!skeleton) return r;
    const float* base = static_cast<const float*>(obj::Ptr(skeleton, kSkelWorlds));
    uint32_t count = 0;
    if (!base || !obj::Get(skeleton, 0, count) || count == 0 || count > uint32_t(kMaxBones)) return r;
    int bone = -1;
    if (r >= base && r < base + size_t(count) * 16 && (r - base) % 16 == 0) {
        bone = int((r - base) / 16);  // one of the player's bones
    } else {
        // the player's body node: the parent's world matrix, NODE+0x90, of the node that owns this skeleton
        const uint8_t* parent = reinterpret_cast<const uint8_t*>(r) - kNodeWorld;
        void* owner = g_node.load(std::memory_order_relaxed);
        if (!owner || parent != owner || obj::Ptr(owner, kNodeSkeleton) != skeleton)
            return LooseAnswer(node, r, skeleton, base, count);  // 0.31.0: not on the player - carried by a clip?
    }
    RememberProp(node, skeleton);
    PlayerPose* p = PoseOfPlayer(skeleton);
    if (!p || p->count != count) return r;
    const RigView& view = p->view;
    Mat4 c, d;
    if (bone >= 0) {
        c = At(p->clean, bone);
        d = At(p->drawn, bone);
        // A bone the head hide collapsed is not a place for an object: it keeps the animated one.
        for (int k = 0; k < 3; ++k) {
            const float lc = fp::Len({c.m[k * 4], c.m[k * 4 + 1], c.m[k * 4 + 2]});
            const float ld = fp::Len({d.m[k * 4], d.m[k * 4 + 1], d.m[k * 4 + 2]});
            if (!(ld > 0.5f * lc)) return r;
        }
    } else {
        if (!obj::Bytes(r, 0, c.m, sizeof(c.m)) || !Finite(c.m)) return r;
        d = p->bodyMoved ? pose::Mul(c, p->body) : c;
    }
    Mat4 answer = d;
    int byHand = -1;
    float byDist = 0.f;
    bool inArm = false;
    if (bone >= 0 && view.haveParents)
        for (int s = 0; s < 2; ++s)
            inArm = inArm || (view.arm[s] >= 0 && (bone == view.arm[s] || Below(view, bone, view.arm[s])));
    Mat4 local;
    // 0.22.0: an item held in a hand turns that hand's finger poses off (19.8 point 2). Held = on the hand bone or a
    // finger at least 5 cm from it (DATA: hand-attached modifiers hold items 5 .. 29 cm out); rings, watches and the
    // like sit closer and leave the fingers on.
    if (bone >= 0 && view.haveParents && obj::Bytes(node, kNodeLocal, local.m, sizeof(local.m)) && Finite(local.m))
        for (int s = 0; s < 2; ++s)
            if (view.hand[s] >= 0 && (bone == view.hand[s] || Below(view, bone, view.hand[s])) &&
                fp::Len(Pos(local)) >= 0.05f && HandNodeIsItem(node, skeleton, s, bone, fp::Len(Pos(local)))) {
                const uint64_t t = GetTickCount64();
                if (t - g_handItemMs[s].exchange(t) > 5000)
                    Log("FPFINGER the %s hand holds node %p (bone %d, %.1f cm out): its fingers play the animation",
                        s ? "right" : "left", node, bone, 100.f * fp::Len(Pos(local)));
            }
    // 0.30.0: the hand rule only for the Hips / body node, and only for an object that moves with a wrist (rigmap.h).
    bool carriedByMotion = false;
    if (!inArm && rigmap::HandRuleBone(bone) && obj::Bytes(node, kNodeLocal, local.m, sizeof(local.m)) && Finite(local.m)) {
        const Mat4 world = pose::Mul(local, c);
        Vec3 rel[2]{};
        bool relValid[2] = {false, false};
        for (int s = 0; s < 2; ++s) {
            Mat4 inv;
            if (view.hand[s] >= 0 && AffineInverse(At(p->clean, view.hand[s]), &inv)) {
                rel[s] = Pos(pose::Mul(world, inv));
                relValid[s] = true;
            }
        }
        carriedByMotion = HeldByMotion(node, bone, Pos(local), rel, relValid);
    }
    if (carriedByMotion && obj::Bytes(node, kNodeLocal, local.m, sizeof(local.m)) && Finite(local.m)) {
        const Vec3 at = Pos(pose::Mul(local, c));  // where the animation has the object
        float best = 0.f;
        for (int s = 0; s < 2; ++s) {
            if (view.hand[s] < 0 || SameMatrix(At(p->base, view.hand[s]), At(p->drawn, view.hand[s]))) continue;
            const float dist = fp::Len(fp::Sub(at, Pos(At(p->clean, view.hand[s]))));
            const float w = rigmap::HandCarryWeight(dist);
            if (w > best) {
                best = w;
                byHand = s;
                byDist = dist;
            }
        }
        Mat4 inv;
        if (byHand >= 0 && AffineInverse(At(p->clean, view.hand[byHand]), &inv)) {
            // world' = (local x clean parent) x (clean hand^-1 x drawn hand): the hand's whole edit, body turn included
            const Mat4 carried = pose::Mul(c, pose::Mul(inv, At(p->drawn, view.hand[byHand])));
            answer = best >= 0.999f ? carried : BlendAffine(d, carried, best);
        } else {
            byHand = -1;
        }
    }
    if (!Finite(answer.m) || SameMatrix(answer, c)) return r;
    g_itemAnswers.fetch_add(1, std::memory_order_relaxed);
    if (byHand >= 0) {
        g_itemByHand.fetch_add(1, std::memory_order_relaxed);
        NoteHandCarry(byDist, rigmap::HandCarryWeight(byDist));
    }
    if (bone < 0) g_itemBody.fetch_add(1, std::memory_order_relaxed);
    else if (byHand < 0) {  // 19.8 census: on a hand bone (or below one) versus any other body bone
        int side = -1;
        for (int s = 0; s < 2 && view.haveParents && side < 0; ++s)
            if (view.hand[s] >= 0 && (bone == view.hand[s] || Below(view, bone, view.hand[s]))) side = s;
        (side >= 0 ? g_itemHandBone : g_itemOtherBone).fetch_add(1, std::memory_order_relaxed);
        if (side >= 0) NoteInHand(node, side);  // 0.31.0: a put-back clip after this starts in the drawn hand
    }
    LogItem(node, bone, byHand, byDist);
    alignas(16) static thread_local float out[16];
    std::memcpy(out, answer.m, sizeof(out));
    return out;
}

// 0.32.0 (playbook 19.8 point 11 Gate 3 rows 2 / 5; item_seam.h, TARGET_ADAPTER_MAP "Item seam"): the drawn answer only
// reaches the draw. NODE::UpdateWorld stores what this hook returns in the node and 290 functions read it - scripts, AI,
// physics, zone tests, the next clip's alignment - so outside DISPLAY_WINDOW::Display a node gets the game's matrix and
// is dirtied when the draw begins; NODE::SetWorld (local from world) always gets the game's matrix. fp_items_seam=off
// goes back to answering every caller (0.17.0 .. 0.31.0).
itemseam::Seam g_seam;
std::atomic<bool> g_seamWanted{true};
std::atomic<uint64_t> g_seamOffThread{0};
std::atomic<uint32_t> g_drawThread{0};
std::atomic<uint64_t> g_planMs{0};
uintptr_t g_updateWorldAt = 0, g_setWorldAt = 0;
constexpr uintptr_t kUpdateWorldSize = 0xC1, kSetWorldSize = 0x150;  // STATIC (functions.json): 193 / 336 bytes
using InvalidateFn = void (*)(void* node, uint32_t clear, uint32_t set);
InvalidateFn g_invalidateSubtree = nullptr;
void (*g_updateWorld)(void* node) = nullptr;

// 0.33.0 (Gate 3 row 5 residual): the edit of an object's own skeleton root, applied to its palette only.
// world'[i] = world[i] x E with E = r^-1 x answer (row vectors: the root world local x r becomes local x answer and every
// bone below it moves with it).
struct RootEdit {
    void* skeleton = nullptr;
    uint64_t ms = 0;
    Mat4 edit;
};
std::mutex g_rootMutex;
RootEdit g_rootEdits[32];
std::atomic<int> g_rootLive{0};
std::atomic<uint64_t> g_rootPalettes{0};

bool RememberRootEdit(void* node, const float* r, const float* answer) {
    void* skeleton = obj::Ptr(node, kNodeSkeleton);
    Mat4 par, ans, inv;
    if (!skeleton || !obj::Bytes(r, 0, par.m, sizeof(par.m)) || !Finite(par.m) || !AffineInverse(par, &inv)) return false;
    std::memcpy(ans.m, answer, sizeof(ans.m));
    const Mat4 edit = pose::Mul(inv, ans);
    if (!Finite(edit.m)) return false;
    const uint64_t now = GetTickCount64();
    std::lock_guard lock(g_rootMutex);
    RootEdit* slot = nullptr;
    for (auto& e : g_rootEdits)
        if (e.skeleton == skeleton) slot = &e;
    if (!slot)
        for (auto& e : g_rootEdits)
            if (!slot || e.ms < slot->ms) slot = &e;  // the oldest (or a free one)
    slot->skeleton = skeleton;
    slot->ms = now;
    slot->edit = edit;
    return true;
}

// The palette hook, for a record that is not the player's: the edit remembered for its skeleton this frame, or false.
bool RootEditOf(const float* worlds, Mat4* edit) {
    const uint64_t now = GetTickCount64();
    std::lock_guard lock(g_rootMutex);
    for (auto& e : g_rootEdits) {
        if (!e.skeleton || now - e.ms > 100) continue;  // not re-answered in the last frames: drawn as the game has it
        if (worlds == obj::Ptr(e.skeleton, kSkelWorlds) || worlds == obj::Ptr(e.skeleton, kSkelWorldsA)) {
            *edit = e.edit;
            return true;
        }
    }
    return false;
}

itemseam::Caller CallerOf(const void* ret) {
    const uintptr_t a = reinterpret_cast<uintptr_t>(ret);
    if (g_updateWorldAt && a - g_updateWorldAt < kUpdateWorldSize) return itemseam::Caller::Update;
    if (g_setWorldAt && a - g_setWorldAt < kSetWorldSize) return itemseam::Caller::SetWorld;
    return itemseam::Caller::Other;  // 0x1409fd4b0: the root of an attached object's own skeleton
}
void SeamInvalidate(void* node) {
    __try {
        g_invalidateSubtree(node, 2, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}
void SeamRebuild(void* node) {
    __try {
        g_updateWorld(node);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}
uint64_t SeamVtable(void* node) {
    uint64_t v = 0;
    return obj::Get(node, 0, v) ? v : 0;
}

// No state, cheap: could ItemAnswer move this node? (ItemAnswer's and LooseAnswer's own first tests.)
bool ItemCandidate(void* node, const float* r) {
    void* skeleton = g_skeleton.load(std::memory_order_relaxed);
    if (!skeleton) return false;
    const float* base = static_cast<const float*>(obj::Ptr(skeleton, kSkelWorlds));
    uint32_t count = 0;
    if (!base || !obj::Get(skeleton, 0, count) || count == 0 || count > uint32_t(kMaxBones)) return false;
    if (r >= base && r < base + size_t(count) * 16 && (r - base) % 16 == 0) return true;  // one of the player's bones
    const uint8_t* parent = reinterpret_cast<const uint8_t*>(r) - kNodeWorld;
    void* owner = g_node.load(std::memory_order_relaxed);
    if (owner && parent == owner && obj::Ptr(owner, kNodeSkeleton) == skeleton) return true;  // the player's body node
    if (!g_looseItems.load(std::memory_order_relaxed) || !g_looseOn.load(std::memory_order_relaxed) || node == owner)
        return false;
    if (LooseTracked(node)) return true;
    Mat4 local, par;
    if (!obj::Bytes(node, kNodeLocal, local.m, sizeof(local.m)) || !Finite(local.m) ||
        !obj::Bytes(r, 0, par.m, sizeof(par.m)) || !Finite(par.m))
        return false;
    const Vec3 at = Pos(pose::Mul(local, par));
    for (int s = 0; s < 2; ++s) {
        const int h = g_handIdx[s].load(std::memory_order_relaxed);
        float w[3];
        if (h >= 0 && uint32_t(h) < count && obj::Bytes(base + size_t(h) * 16 + 12, 0, w, sizeof(w)) &&
            fp::Len(fp::Sub(at, Vec3{w[0], w[1], w[2]})) < 0.6f)
            return true;
    }
    return false;
}

const float* HookParentMatrix(void* node) {
    const float* r = g_origParent(node);
    if (!r || !g_items.load(std::memory_order_relaxed) || !g_active.load(std::memory_order_relaxed)) return r;
    const itemseam::Caller who = CallerOf(_ReturnAddress());
    switch (g_seam.RouteOf(who)) {
    case itemseam::Route::Game:
        if (who == itemseam::Caller::SetWorld && ItemCandidate(node, r))
            g_seam.counters.setWorld.fetch_add(1, std::memory_order_relaxed);
        return r;
    case itemseam::Route::Remember:
        if (ItemCandidate(node, r)) {
            g_seam.Remember(node);
            g_seam.counters.kept.fetch_add(1, std::memory_order_relaxed);
        }
        return r;
    case itemseam::Route::Draw:
        break;
    }
    if (who == itemseam::Caller::Update && g_seam.Drawing()) g_seam.Asked(node);
    const float* a = ItemAnswer(node, r);
    if (a != r && g_seam.On()) {
        if (who == itemseam::Caller::Update) {
            g_seam.Answered(node);
            g_seam.counters.drawn.fetch_add(1, std::memory_order_relaxed);
            if (GetCurrentThreadId() != g_drawThread.load(std::memory_order_relaxed))
                g_seamOffThread.fetch_add(1, std::memory_order_relaxed);
        } else if (RememberRootEdit(node, r, a)) {
            // 0.33.0: 0x1409fd4b0 stores the answer as an object's own skeleton root (instance +0x70) during the
            // animation update - every skinned prop the movie route carries. It keeps the game's matrix; the edit goes
            // to that skeleton's palette, which is render-only (HookPalette, DrawRootEdited).
            g_seam.counters.other.fetch_add(1, std::memory_order_relaxed);
            return r;
        }
    }
    return a;
}

// 0.20.0: the second character of a pair walk (STATIC: SCRIPT_ENTITY_CONTROLLER_MOKIT_SYNC plays her motion kit in
// step with the player's; DATA: Meet Madison at the motel, Fugitive Lizard, Fugitive Subway). Her animation is
// authored against the player's animated body, so she is drawn moved as the drawn Hips are moved (the body turn, the
// idle hold) while she is within 1.5 m of the player. Render-only; her own pose, logic and collisions are untouched.
std::atomic<uint64_t> g_partnerPalettes{0};
bool DrawPartner(void* record, const float* worlds, void* skeleton, uint32_t count) {
    PlayerPose* p = PoseOfPlayer(skeleton);
    if (!p || !p->bodyMoved) return false;
    void* partner = nullptr;
    for (void* q : p->plan.partner)
        if (q && q != skeleton && (worlds == obj::Ptr(q, kSkelWorlds) || worlds == obj::Ptr(q, kSkelWorldsA))) partner = q;
    if (!partner) return false;
    alignas(16) static thread_local float moved[kMaxBones * 16];
    if (!obj::Bytes(worlds, 0, moved, size_t(count) * 64) || !Finite(moved)) return false;
    if (fp::Len(fp::Sub(Pos(At(moved, 0)), Pos(At(p->clean, 0)))) > 1.5f) return false;  // walked apart
    for (uint32_t i = 0; i < count; ++i) Put(moved, int(i), pose::Mul(At(moved, int(i)), p->body));
    static std::atomic<void*> logged{nullptr};
    if (logged.exchange(partner) != partner)
        Log("FPITEM pair walk: the character with skeleton %p is drawn beside the drawn body", partner);
    g_partnerPalettes.fetch_add(1, std::memory_order_relaxed);
    return CallWithWorlds(record, moved);
}

// The palette of an object whose skeleton copies the player's (0.18.0 attached, 0.19.0 also unattached).
// true = built from edited worlds.
bool DrawProp(void* record, const float* worlds) {
    if (!g_items.load(std::memory_order_relaxed)) return false;
    void* skeleton = g_skeleton.load(std::memory_order_relaxed);
    uint32_t count = 0;
    if (!skeleton || !obj::Get(record, kRecCount, count) || count < 3 || count > uint32_t(kMaxBones)) return false;
    const uint64_t now = GetTickCount64();
    if (DrawPartner(record, worlds, skeleton, count)) return true;
    void* prop = nullptr;
    bool attached = false, logged = true, report = false, known = false;
    {
        std::lock_guard lock(g_propMutex);
        for (auto& a : g_props)
            if (a.skeleton && (worlds == a.worlds[0] || worlds == a.worlds[1])) {
                known = true;
                if (a.never) {
                    a.ms = now;
                    return false;
                }
                if (a.attached && now - a.ms > 500) a.attached = false;  // no longer asked for by the hook: let go
                prop = a.skeleton;
                attached = a.attached;
            }
    }
    if (!known) {
        prop = InstanceOfRecord(record, worlds);  // 0.19.0: an object nobody attached
        if (!prop || prop == skeleton) return false;
        std::lock_guard lock(g_propMutex);
        CopyProp* slot = PropSlot(prop, now, true);
        if (!slot) return false;  // the table is full: left alone rather than looked at every frame
        slot->ms = now;
    }
    PlayerPose* p = PoseOfPlayer(skeleton);
    if (!p) return false;
    alignas(16) static thread_local float in[kMaxBones * 16];
    alignas(16) static thread_local float out[kMaxBones * 16];
    static thread_local int16_t parent[kMaxBones], hers[kMaxBones];
    static thread_local uint8_t state[kMaxBones];
    static thread_local uint32_t hash[kMaxBones];
    // the object's hierarchy (skeleton +0x20: `count` = top) and the player's bone of the same name (+0x210)
    const void* parents = obj::Ptr(prop, 0x20);
    const void* names = obj::Ptr(prop, kSkelHashes);
    const void* hersNames = obj::Ptr(skeleton, kSkelHashes);
    uint32_t own = 0;
    if (!parents || !names || !hersNames || !obj::Get(prop, 0, own) || own != count) return false;
    static thread_local uint32_t hersRec[kMaxBones * 2], ownRec[kMaxBones * 2], ownPar[kMaxBones];
    if (!obj::Bytes(hersNames, 0, hersRec, size_t(p->count) * 8) || !obj::Bytes(names, 0, ownRec, size_t(count) * 8) ||
        !obj::Bytes(parents, 0, ownPar, size_t(count) * 4))
        return false;
    for (uint32_t i = 0; i < p->count; ++i) hash[i] = hersRec[size_t(i) * 2];
    static const uint32_t kEye = bones::Crc32("LeftEye");
    int named = 0;
    bool character = false;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t par = ownPar[i], h = ownRec[size_t(i) * 2];
        parent[i] = par < i ? int16_t(par) : int16_t(-1);
        hers[i] = -1;
        character = character || h == kEye;
        for (uint32_t k = 0; k < p->count && hers[i] < 0; ++k)
            if (hash[k] == h) hers[i] = int16_t(k);
        named += hers[i] >= 0;
    }
    {
        std::lock_guard lock(g_propMutex);
        CopyProp* slot = PropSlot(prop, now, true);
        if (!slot) return false;
        if (!known) slot->ms = now;
        if (!slot->attached) slot->ms = now;
        // a character (it has eyes) is never a prop; an object with fewer than three shared names can never be a copy
        if ((character && !slot->attached) || named < rigmap::kMinOnHers) {
            slot->never = true;
            return false;
        }
        logged = slot->logged;
        slot->logged = true;
        report = now - slot->lastLog >= 10000;
        if (report) slot->lastLog = now;
    }
    if (!obj::Bytes(worlds, 0, in, size_t(count) * 64) || !Finite(in)) return false;
    // an attached object was animated against her pose moved as its root was moved; an unattached one against her clean pose
    const float* against = attached ? p->base : p->clean;
    const int on = rigmap::CarryCopy(in, count, parent, hers, against, p->drawn, out, state);
    if (!logged || (report && on >= rigmap::kMinOnHers)) {
        int carried = 0, stays = 0;
        float worstOn = 0.f, nearestMiss = 1e9f;
        for (uint32_t i = 0; i < count; ++i) {
            carried += state[i] == rigmap::kCopyCarried;
            stays += state[i] == rigmap::kCopyStays;
            if (hers[i] < 0) continue;
            const float* a = in + size_t(i) * 16 + 12;
            const float* b = against + size_t(hers[i]) * 16 + 12;
            const float dd = std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
            if (on >= rigmap::kMinOnHers && state[i] == rigmap::kCopyOnHers) worstOn = std::fmax(worstOn, dd);
            else nearestMiss = std::fmin(nearestMiss, dd);
        }
        Log("FPITEM %s object skeleton %p (%u bones, %d named like the player's): %d lie on the player's bones%s (furthest "
            "%.1f cm; nearest named bone that does not: %.1f cm), %d hang below them, %d stay as animated",
            attached ? "attached" : "unattached", prop, count, named, on,
            on >= rigmap::kMinOnHers ? " and are drawn from the drawn pose" : ": not on the player now", worstOn * 100.f,
            nearestMiss > 1e8f ? -1.f : nearestMiss * 100.f, carried, stays);
    }
    if (on < rigmap::kMinOnHers || !Finite(out)) return false;
    g_propPalettes.fetch_add(1, std::memory_order_relaxed);
    return CallWithWorlds(record, out);
}

// ---- simulated cloth (0.21.0, playbook 14.11; docs/TARGET_ADAPTER_MAP.md "Simulated cloth") ----
// Owner (2026-11-17): "the first person clothing ... doesn't follow body in VR". DATA: Shelby's (HR_SSH_EXT / INT) and
// Jayden's (HR_NJA_EXT / INT) outfits carry a CLOTH_DATA object; their character models have no coat or sleeves
// (docs/rigs pictures): the coat is Havok Cloth, drawn from the simulation, never through the skin palette the mod edits.
// STATIC 2026-11-17: the cloth instance set-up (0x140b8ed50) makes one HAVOK_TRANSFORM_SET (ctor 0x140b8f400) from the
// skeleton's TRANSFORM_SETS (SKELETON entity +0x2A8, built by 0x14096dd20 as {count, instance +0x10, instance +0x18}):
// two hkArrays of 0x40-byte matrices at set +0x10 / +0x20 (size +0x18 / +0x28, capacity flag 0x80000000 = not owned)
// that POINT AT the instance's world matrices and their transpose(inverse) (both rewritten by the instance's world
// update 0x1409fd270). Havok reads them in place. So the cloth follows the animation's body, not the drawn one.
// What the mod does (Beyond's "one cloth pose", 14.11 point 1): while first person draws the player, the player's sets
// point at the mod's own copy of the drawn pose - turned, arms solved, the head NOT collapsed - written right after every
// world update of the player's skeleton; the engine's own arrays are never written (everything else keeps the clean
// pose). On the switch in or out the engine's own reset (vtable slot 9, state "toSim1": the cloth starts again from its
// skinned pose) is asked for (14.11 point 2). fp_cloth=off returns the engine's pointers.
using UpdateWorldsFn = void (*)(void*, void*, void*, void*);
using SetCreateFn = void* (*)(void*, void*, void*, void*);
using DestroyFn = void* (*)(void*, uint64_t);
using ClothCreateFn = void (*)(void*, void*, void*, void*, uint64_t);
using ToSimFn = void (*)(void*);
UpdateWorldsFn g_origUpdateWorlds = nullptr;
SetCreateFn g_origSetCreate = nullptr;
DestroyFn g_origSetDestroy = nullptr, g_origClothDestroy = nullptr;
ClothCreateFn g_origClothCreate = nullptr;
ToSimFn g_clothToSim = nullptr;
std::atomic<bool> g_cloth{true}, g_clothTeleport{true}, g_clothHair{true};
constexpr size_t kSetWorlds = 0x10, kSetWorldsSize = 0x18, kSetInvT = 0x20, kSetInvTSize = 0x28;

struct ClothSet {
    void* set = nullptr;
    void* inst = nullptr;     // the HAVOK_CLOTH_INSTANCE that made it (null when made elsewhere or destroyed)
    float* worlds = nullptr;  // the engine's arrays the set was made with
    float* invT = nullptr;
    uint32_t count = 0;
    bool redirected = false;
    uint64_t updates = 0, drawn = 0;  // world updates of its skeleton seen / with the drawn pose (30 s log)
};
ClothSet g_clothSets[64];
// 14.11 point 2: the drawn Hips between two updates of the player's skeleton; a jump of more than 10 deg or 8 cm (a snap
// turn, a recentre) restarts the cloth from its skinned pose. Not the clean -> drawn difference: Heavy Rain turns the
// root round at once to walk backwards while the drawn body stays continuous, and the cloth only sees the drawn body.
// 0.36.0 (owner: "the sleeves get bad when I stop moving sometimes"; LIVE 0.35.0: 8,345 player world updates in 30 s =
// about 3 a frame, and 2,486 .. 6,832 jump resets - half of all updates): the test compared one world update with the
// next, i.e. the game's intermediate poses of the same frame, and restarted the cloth (Shelby's sleeves are cloth only)
// from its skinned pose many times a frame. Now: the drawn Hips of the last update of the previous first-person frame
// (Plan::seq) against the first of this frame; at most one restart per 0.5 s unless the jump is hard (> 45 deg or
// > 0.5 m: a first-person switch, a recentre). How far the updates of ONE frame are apart is counted for the log.
Mat4 g_clothLastBody = pose::Identity();
Mat4 g_clothFrameFirst = pose::Identity();
bool g_clothHaveBody = false;
uint64_t g_clothSeq = 0, g_clothLastReset = 0;
uint64_t g_clothLastLog = 0;
std::atomic<uint64_t> g_clothJumps{0}, g_clothSuppressed{0}, g_clothWithinFrame{0};
// angle (rad) and distance between two drawn Hips
inline void BodyDelta(const Mat4& a, const Mat4& b, float* angle, float* move) {
    Mat4 inv;
    *angle = 0.f;
    *move = 0.f;
    if (!AffineInverse(a, &inv)) return;
    const Mat4 d = pose::Mul(inv, b);
    const float tr = d.m[0] / fp::Len({d.m[0], d.m[1], d.m[2]}) + d.m[5] / fp::Len({d.m[4], d.m[5], d.m[6]}) +
                     d.m[10] / fp::Len({d.m[8], d.m[9], d.m[10]});
    *angle = std::acos(std::fmax(-1.f, std::fmin(1.f, 0.5f * (tr - 1.f))));
    *move = fp::Len({d.m[12], d.m[13], d.m[14]});
}
std::mutex g_clothMutex;
std::atomic<int> g_clothKnown{0}, g_clothRedirected{0};
std::atomic<uint64_t> g_clothPoses{0}, g_clothResets{0}, g_clothFailed{0};
thread_local void* t_clothCreating = nullptr;
// Three buffers in turn: Havok may still read the previous pose on its own threads while the next one is written.
struct ClothBuffers {
    alignas(16) float worlds[3][kMaxBones * 16];
    alignas(16) float invT[3][kMaxBones * 16];
    int next = 0;
};
ClothBuffers* g_clothBuf = nullptr;  // allocated once, never freed (Havok may hold a pointer into it)

void* HookSetCreate(void* set, void* sets, void* a3, void* a4) {
    void* r = g_origSetCreate(set, sets, a3, a4);
    float* worlds = static_cast<float*>(obj::Ptr(set, kSetWorlds));
    float* invT = static_cast<float*>(obj::Ptr(set, kSetInvT));
    uint32_t n = 0, nInv = 0;
    if (!worlds || !obj::Get(set, kSetWorldsSize, n) || !obj::Get(set, kSetInvTSize, nInv) || n == 0 || n != nInv ||
        n > uint32_t(kMaxBones))
        return r;
    void* inst = t_clothCreating;
    {
        std::lock_guard lock(g_clothMutex);
        for (auto& c : g_clothSets)
            if (!c.set) {
                c = ClothSet{set, inst, worlds, invT, n, false};
                g_clothKnown.fetch_add(1);
                break;
            }
    }
    Log("FPCLOTH cloth transform set %p made (%u bones, worlds %p) for cloth instance %p", set, n, worlds, inst);
    return r;
}

void* HookSetDestroy(void* set, uint64_t flags) {
    {
        std::lock_guard lock(g_clothMutex);
        for (auto& c : g_clothSets)
            if (c.set == set) {
                if (c.redirected) {  // not owned by the set (flag 0x80000000), but put the engine's arrays back anyway
                    *reinterpret_cast<float**>(static_cast<uint8_t*>(set) + kSetWorlds) = c.worlds;
                    *reinterpret_cast<float**>(static_cast<uint8_t*>(set) + kSetInvT) = c.invT;
                    g_clothRedirected.fetch_sub(1);
                }
                c = ClothSet{};
                g_clothKnown.fetch_sub(1);
            }
    }
    return g_origSetDestroy(set, flags);
}

void HookClothCreate(void* inst, void* cloth, void* buffers, void* sets, uint64_t a5) {
    void* outer = t_clothCreating;
    t_clothCreating = inst;
    g_origClothCreate(inst, cloth, buffers, sets, a5);
    t_clothCreating = outer;
}

void* HookClothDestroy(void* inst, uint64_t flags) {
    {
        std::lock_guard lock(g_clothMutex);
        for (auto& c : g_clothSets)
            if (c.inst == inst) c.inst = nullptr;
    }
    return g_origClothDestroy(inst, flags);
}

// The cloth's pose of this moment: the drawn body (idle hold, turn, arms) with the head kept, and its transpose(inverse)
// as the engine's world update writes it (1409fde30). false = no first-person plan for this skeleton.
bool ClothPose(void* skeleton, uint32_t count, float* w, float* invT, Mat4* body, uint64_t* seq) {
    Plan plan;
    {
        std::lock_guard lock(g_planMutex);
        plan = g_plan;
    }
    if (!plan.active || plan.skeleton != skeleton || GetTickCount64() - plan.ms > 250) return false;
    *seq = plan.seq;
    const float* base = static_cast<const float*>(obj::Ptr(skeleton, kSkelWorlds));
    RigView* view = new (std::nothrow) RigView;  // large: not on a job thread's stack
    if (!view) return false;
    bool ok = base && obj::Bytes(base, 0, w, size_t(count) * 64) && Finite(w) && View(skeleton, view) &&
              view->count == count;
    if (ok) {
        ApplyIdleHold(w, count, plan, view->head, /*advance=*/false);  // intermediate world updates only apply it
        const bool headHidden = plan.hideHead;
        plan.hideHead = false;  // 14.11: the cloth never gets the collapsed head
        Edit(w, plan, *view);
        // 14.11 point 3: with the head collapsed (fp_hidehead=collapse) Madison's simulated hair would hang in front of the
        // eyes. The head-set bones go 30 m straight down at full size (collapsing them to a point makes cloth explode),
        // so cloth hanging from them is simulated and drawn out of sight; cloth on the neck and body is unaffected.
        if (headHidden && g_clothHair.load(std::memory_order_relaxed) && view->head >= 0)
            for (uint32_t i = 0; i < count; ++i)
                if (int(i) == view->head || view->group[i] == kHead || (view->haveParents && Below(*view, int(i), view->head)))
                    w[size_t(i) * 16 + 13] -= 30.f;
        *body = At(w, 0);  // the drawn Hips (bone 0 on every rig): what the cloth sees jump
        for (uint32_t i = 0; ok && i < count; ++i) {
            Mat4 inv;
            ok = AffineInverse(At(w, int(i)), &inv);
            Mat4 t;
            for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 4; ++c) t.m[r * 4 + c] = inv.m[c * 4 + r];
            Put(invT, int(i), t);
        }
        ok = ok && Finite(w) && Finite(invT);
    }
    delete view;
    return ok;
}

void ResetCloth(void* inst) {
    if (!inst || !g_clothToSim || !g_clothTeleport.load(std::memory_order_relaxed)) return;
    // only through the verified function, and only when the instance's own slot 9 is it
    void* vtbl = obj::Ptr(inst, 0);
    void* slot9 = vtbl ? obj::Ptr(vtbl, 9 * 8) : nullptr;
    if (slot9 != reinterpret_cast<void*>(g_clothToSim)) return;
    g_clothToSim(inst);
    g_clothResets.fetch_add(1, std::memory_order_relaxed);
}

void HookUpdateWorlds(void* instance, void* a2, void* a3, void* a4) {
    g_origUpdateWorlds(instance, a2, a3, a4);
    if (g_clothKnown.load(std::memory_order_relaxed) == 0) return;
    void* player = g_skeleton.load(std::memory_order_relaxed);
    if (instance != player && g_clothRedirected.load(std::memory_order_relaxed) == 0) return;
    float* worlds = static_cast<float*>(obj::Ptr(instance, kSkelWorlds));
    uint32_t count = 0;
    if (!worlds || !obj::Get(instance, 0, count) || count == 0 || count > uint32_t(kMaxBones)) return;
    std::lock_guard lock(g_clothMutex);
    int mine = 0;
    for (auto& c : g_clothSets) mine += c.set && c.worlds == worlds && c.count == count;
    if (!mine) return;
    const bool want = instance == player && g_cloth.load(std::memory_order_relaxed) &&
                      g_active.load(std::memory_order_relaxed) && g_enabled.load(std::memory_order_relaxed);
    float* w = nullptr;
    float* invT = nullptr;
    bool jumped = false;
    if (want && g_clothBuf) {
        const int k = g_clothBuf->next;
        Mat4 body;
        uint64_t seq = 0;
        if (ClothPose(instance, count, g_clothBuf->worlds[k], g_clothBuf->invT[k], &body, &seq)) {
            w = g_clothBuf->worlds[k];
            invT = g_clothBuf->invT[k];
            g_clothBuf->next = (k + 1) % 3;
            g_clothPoses.fetch_add(1, std::memory_order_relaxed);
            float angle = 0.f, move = 0.f;
            if (seq != g_clothSeq) {  // the first update of a new first-person frame
                if (g_clothHaveBody) {
                    BodyDelta(g_clothLastBody, body, &angle, &move);
                    jumped = angle > 0.1745329f || move > 0.08f;
                    const bool hard = angle > 0.7853982f || move > 0.5f;
                    const uint64_t ms = GetTickCount64();
                    if (jumped && !hard && ms - g_clothLastReset < 500) {
                        jumped = false;
                        g_clothSuppressed.fetch_add(1, std::memory_order_relaxed);
                    }
                    if (jumped) g_clothLastReset = ms;
                }
                g_clothSeq = seq;
                g_clothFrameFirst = body;
            } else {
                BodyDelta(g_clothFrameFirst, body, &angle, &move);
                if (angle > 0.1745329f || move > 0.08f) g_clothWithinFrame.fetch_add(1, std::memory_order_relaxed);
            }
            g_clothLastBody = body;
            g_clothHaveBody = true;
        } else {
            g_clothFailed.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (!w) g_clothHaveBody = false;
    if (jumped) g_clothJumps.fetch_add(1, std::memory_order_relaxed);
    const uint64_t now = GetTickCount64();
    const bool report = now - g_clothLastLog >= 30000;
    if (report) g_clothLastLog = now;
    for (auto& c : g_clothSets) {
        if (!c.set || c.worlds != worlds || c.count != count) continue;
        ++c.updates;
        c.drawn += w != nullptr;
        if (report) {
            Log("FPCLOTH set %p (instance %p): %llu of %llu world updates of the player's skeleton in the last 30 s ran "
                "with the drawn pose; %llu jump resets so far (frame to frame, 0.36.0), %llu more held back (0.5 s), "
                "%llu updates of one frame more than 10 deg / 8 cm from its first", c.set, c.inst,
                static_cast<unsigned long long>(c.drawn), static_cast<unsigned long long>(c.updates),
                static_cast<unsigned long long>(g_clothJumps.load()), static_cast<unsigned long long>(g_clothSuppressed.load()),
                static_cast<unsigned long long>(g_clothWithinFrame.load()));
            c.updates = c.drawn = 0;
        }
        if (w && c.redirected && jumped) ResetCloth(c.inst);
        auto** pw = reinterpret_cast<float**>(static_cast<uint8_t*>(c.set) + kSetWorlds);
        auto** pi = reinterpret_cast<float**>(static_cast<uint8_t*>(c.set) + kSetInvT);
        if (w) {
            *pi = invT;
            *pw = w;
            if (!c.redirected) {
                c.redirected = true;
                g_clothRedirected.fetch_add(1);
                ResetCloth(c.inst);
                Log("FPCLOTH the player's cloth (set %p, instance %p, %u bones) follows the drawn body", c.set, c.inst,
                    count);
            }
        } else if (c.redirected) {
            *pw = c.worlds;
            *pi = c.invT;
            c.redirected = false;
            g_clothRedirected.fetch_sub(1);
            ResetCloth(c.inst);
            Log("FPCLOTH cloth set %p back on the animated body", c.set);
        }
    }
}

bool InstallCloth() {
    void* upd = game::Addr(game::Anchor::SkeletonUpdateWorlds);
    void* sc = game::Addr(game::Anchor::TransformSetCreate);
    void* sd = game::Addr(game::Anchor::TransformSetDestroy);
    void* cc = game::Addr(game::Anchor::ClothInstanceCreate);
    void* cd = game::Addr(game::Anchor::ClothInstanceDestroy);
    g_clothToSim = reinterpret_cast<ToSimFn>(game::Addr(game::Anchor::ClothInstanceToSim));
    if (!upd || !sc || !sd || !cc || !cd) {
        Log("FPCLOTH cloth path not found in this build: simulated coats follow the animated body");
        return false;
    }
    g_clothBuf = new (std::nothrow) ClothBuffers;
    if (!g_clothBuf) return false;
    struct H {
        void* at;
        void* fn;
        void** orig;
        const char* name;
    } hooks[] = {
        {sd, reinterpret_cast<void*>(&HookSetDestroy), reinterpret_cast<void**>(&g_origSetDestroy), "HAVOK_TRANSFORM_SET::Destroy"},
        {cd, reinterpret_cast<void*>(&HookClothDestroy), reinterpret_cast<void**>(&g_origClothDestroy), "HAVOK_CLOTH_INSTANCE::Destroy"},
        {sc, reinterpret_cast<void*>(&HookSetCreate), reinterpret_cast<void**>(&g_origSetCreate), "HAVOK_TRANSFORM_SET::Create"},
        {cc, reinterpret_cast<void*>(&HookClothCreate), reinterpret_cast<void**>(&g_origClothCreate), "HAVOK_CLOTH_INSTANCE::Create"},
        {upd, reinterpret_cast<void*>(&HookUpdateWorlds), reinterpret_cast<void**>(&g_origUpdateWorlds), "SKELETON_INSTANCE::UpdateWorlds"},
    };
    bool all = true;
    for (const H& h : hooks) {
        const MH_STATUS s = MH_CreateHook(h.at, h.fn, h.orig);
        const MH_STATUS e = s == MH_OK ? MH_EnableHook(h.at) : s;
        Log("FPCLOTH hook %s %p %s", h.name, h.at, MH_StatusToString(e));
        all = all && e == MH_OK;
    }
    Log("FPCLOTH simulated cloth follows the drawn body in first person: fp_cloth=%s, reset on switch %s (ToSim %p)",
        g_cloth.load() ? "on" : "off", g_clothTeleport.load() ? "on" : "off", reinterpret_cast<void*>(g_clothToSim));
    return all;
}

}  // namespace

bool Install() {
    void* p = game::Addr(game::Anchor::SkinPaletteBuild);
    if (!p) {
        Log("FPBODY palette builder not found in this build: the body is drawn as the game animates it");
        return false;
    }
    const MH_STATUS s = MH_CreateHook(p, reinterpret_cast<void*>(&HookPalette), reinterpret_cast<void**>(&g_orig));
    const MH_STATUS e = s == MH_OK ? MH_EnableHook(p) : s;
    Log("FPBODY hook SKIN::PaletteBuild %p %s", p, MH_StatusToString(e));
    g_installed = e == MH_OK;
    // 0.13.1: the 0.12.0 CAMERA_TEXTURE hook is reverted (owner); with fp_hidehead=clip the head is never removed from
    // the pose, so mirrors show it without any palette swap.
    if (void* r = game::Addr(game::Anchor::DisplayReflections)) {
        const MH_STATUS rs = MH_CreateHook(r, reinterpret_cast<void*>(&HookReflections), reinterpret_cast<void**>(&g_origReflections));
        const MH_STATUS re = rs == MH_OK ? MH_EnableHook(r) : rs;
        Log("FPBODY hook VIEWPORT::DisplayReflections %p %s (the head in mirrors)", r, MH_StatusToString(re));
    } else {
        Log("FPBODY reflection pass not found in this build: mirrors show the first-person body without its head");
    }
    // 0.17.0: objects attached to the player's bones are drawn from the drawn pose (fp_items).
    if (void* pm = g_installed ? game::Addr(game::Anchor::NodeParentMatrix) : nullptr) {
        const MH_STATUS ps = MH_CreateHook(pm, reinterpret_cast<void*>(&HookParentMatrix), reinterpret_cast<void**>(&g_origParent));
        const MH_STATUS pe = ps == MH_OK ? MH_EnableHook(pm) : ps;
        Log("FPBODY hook NODE::ParentMatrix %p %s (objects in the player's hands; fp_items=%s)", pm, MH_StatusToString(pe),
            g_items.load() ? "on" : "off");
        // 0.32.0: the item seam (item_seam.h) - the drawn answer only inside DISPLAY_WINDOW::Display.
        void* upd = game::Addr(game::Anchor::NodeUpdateWorld);
        void* set = game::Addr(game::Anchor::NodeSetWorld);
        void* inv = game::Addr(game::Anchor::NodeInvalidateSubtree);
        if (pe == MH_OK && upd && set && inv) {
            g_updateWorldAt = reinterpret_cast<uintptr_t>(upd);
            g_setWorldAt = reinterpret_cast<uintptr_t>(set);
            g_updateWorld = reinterpret_cast<void (*)(void*)>(upd);
            g_invalidateSubtree = reinterpret_cast<InvalidateFn>(inv);
            itemseam::Engine engine;
            engine.invalidate = &SeamInvalidate;
            engine.rebuild = &SeamRebuild;
            engine.vtable = &SeamVtable;
            g_seam.SetEngine(engine);
        }
        g_seam.SetOn(g_seamWanted.load());
        Log("FPBODY item seam %s (UpdateWorld %p, SetWorld %p, invalidate %p; fp_items_seam=%s): a held object's drawn "
            "place is answered only while the frame is drawn; scripts, physics and the next clip read the game's",
            g_seam.On() ? "on" : "OFF", upd, set, inv, g_seamWanted.load() ? "on" : "off");
    } else {
        Log("FPBODY NODE::ParentMatrix not found in this build: held objects stay at the animated hand");
    }
    // 0.21.0: simulated cloth (Shelby's and Jayden's coats) gets the drawn pose in first person (fp_cloth).
    if (g_installed) InstallCloth();
    // 0.33.0: the movie link for objects a scene clip hands the player (Gate 3 row 9; fp_items_loose).
    if (g_installed && !movielink::Install())
        Log("FPITEM without the movie link no object a scene clip carries goes to the controller hand (they stay animated)");
    return g_installed;
}

void Publish(const Plan& plan) {
    {
        static uint64_t seq = 0;
        std::lock_guard lock(g_planMutex);
        g_plan = plan;
        g_plan.seq = ++seq;
    }
    g_skeleton = plan.active ? plan.skeleton : nullptr;
    g_node = plan.active ? plan.node : nullptr;
    g_looseOn = plan.active && plan.arms;  // 0.31.0: loose items only while the controllers drive the arms
    g_active = plan.active;
    g_planMs = plan.ms;
    // Playbook 19.8 (revision 16): one census line per item route every 10 s while first person draws, with how many
    // answers / palettes each route gave. A route at 0 while an item is visibly left behind is the route it really uses.
    static uint64_t last = 0, prev[8] = {};
    const uint64_t now = GetTickCount64();
    if (!plan.active || now - last < 10000) return;
    last = now;
    const uint64_t cur[8] = {g_itemHandBone.load(), g_itemOtherBone.load(), g_itemBody.load(), g_itemByHand.load(),
                             g_propPalettes.load(), g_partnerPalettes.load(), g_fingerHeld.load(), g_looseAnswers.load()};
    uint64_t d[8], any = 0;
    for (int i = 0; i < 8; ++i) {
        d[i] = cur[i] - prev[i];
        any += d[i];
        prev[i] = cur[i];
    }
    if (any)
        Log("FPITEM census 10 s: hand bones %llu, other body bones %llu, body node %llu, carried by the hand rule %llu, "
            "skeleton-copy palettes %llu, pair-walk partner palettes %llu, hands holding (fingers off) %llu, "
            "held by the movie route (0.33.0) %llu",
            (unsigned long long)d[0], (unsigned long long)d[1], (unsigned long long)d[2], (unsigned long long)d[3],
            (unsigned long long)d[4], (unsigned long long)d[5], (unsigned long long)d[6], (unsigned long long)d[7]);
    // 0.32.0 (Gate 3 row 5, LIVE check of item_seam.h): where the drawn answers went. "off the draw thread" > 0 means
    // a worker read an item inside the draw; "other callers" are attached skeletons' roots (kept as 0.17.0).
    static uint64_t sPrev[9] = {};
    const itemseam::Counters& c = g_seam.counters;
    const uint64_t sCur[9] = {c.drawn.load(),   g_seamOffThread.load(), c.kept.load(),    c.setWorld.load(),
                              c.refreshed.load(), c.gone.load(),         c.rebuilt.load(), c.other.load(), c.unasked.load()};
    uint64_t sd[9], sAny = 0;
    for (int i = 0; i < 9; ++i) {
        sd[i] = sCur[i] - sPrev[i];
        sAny += sd[i];
        sPrev[i] = sCur[i];
    }
    if (sAny)
        Log("FPITEM seam 10 s (%s): drawn answers %llu (%llu off the draw thread), the game's own outside the draw %llu, "
            "SetWorld kept the game's %llu, dirtied at draw start %llu (%llu gone, %llu never asked in the draw), rebuilt "
            "at draw end %llu, skeleton roots kept the game's %llu",
            g_seam.On() ? "on" : "off", (unsigned long long)sd[0], (unsigned long long)sd[1], (unsigned long long)sd[2],
            (unsigned long long)sd[3], (unsigned long long)sd[4], (unsigned long long)sd[5], (unsigned long long)sd[8],
            (unsigned long long)sd[6], (unsigned long long)sd[7]);
    // 0.33.0 (Gate 3 rows 3 / 8 / 9): the movie route - how many answers, objects kept off the hand by their class, the
    // hands told they hold one, and one drift line per held object.
    static uint64_t mPrev[5] = {};
    const uint64_t mCur[5] = {g_looseAnswers.load(), g_looseOffClass.load(), g_looseHandHolds.load(), g_looseSeeded.load(),
                              g_rootPalettes.load()};
    uint64_t md[5], mAny = 0;
    for (int i = 0; i < 5; ++i) {
        md[i] = mCur[i] - mPrev[i];
        mAny += md[i];
        mPrev[i] = mCur[i];
    }
    if (mAny)
        Log("FPITEM movie route 10 s: drawn in a controller hand %llu, kept where the game puts them by their class %llu, "
            "hand-holding signals %llu, started in the hand from the socket %llu, skinned objects drawn from an edited "
            "root %llu (%d objects tracked)",
            (unsigned long long)md[0], (unsigned long long)md[1], (unsigned long long)md[2], (unsigned long long)md[3],
            (unsigned long long)md[4], g_looseLive.load());
    LogDrift();
}

void DrawBegin() {
    // Only for a fresh first-person plan (fpbody.h: older than 250 ms is ignored): the loading screen's own draw loop
    // also runs DISPLAY_WINDOW::Display.
    if (!g_active.load(std::memory_order_relaxed) || GetTickCount64() - g_planMs.load(std::memory_order_relaxed) > 250) {
        g_seam.Forget();
        return;
    }
    g_drawThread.store(GetCurrentThreadId(), std::memory_order_relaxed);
    g_seam.Begin();
}

void DrawEnd() { g_seam.End(); }

void SetItemSeam(bool on) {
    g_seamWanted = on;
    g_seam.SetOn(on);
}

void Suspend() {
    g_active = false;
    g_skeleton = nullptr;
    g_node = nullptr;
    std::lock_guard lock(g_planMutex);
    g_plan.active = false;
}

void SetEnabled(bool on) {
    g_enabled = on;
    Log("FPBODY body edits %s", on ? "on" : "off");
}
bool Enabled() { return g_enabled; }

void SetForearmTwistHelpers(bool on) { g_twistHelpers.store(on); }
void SetItems(bool on) { g_items.store(on); }
void SetLooseItems(bool on) { g_looseItems.store(on); }
void SetCloth(bool on, bool resetOnSwitch, bool hairAway) {
    g_cloth.store(on);
    g_clothTeleport.store(resetOnSwitch);
    g_clothHair.store(hairAway);
}
void SetMirrorHead(bool on) {
    g_mirrorHead = on;
    Log("FPBODY head in mirrors %s", on ? "on" : "off");
}

bool RestElevation(void* skeleton, int from, int to, float* elevation) {
    std::lock_guard lock(g_rigMutex);
    if (!elevation || !g_rig || g_rig->skeleton != skeleton || !g_rig->haveRest || from < 0 || to < 0 ||
        uint32_t(from) >= g_rig->count || uint32_t(to) >= g_rig->count || g_rig->rest.size() < size_t(g_rig->count) * 16)
        return false;
    const float* a = &g_rig->rest[size_t(from) * 16 + 12];
    const float* b = &g_rig->rest[size_t(to) * 16 + 12];
    const float x = b[0] - a[0], y = b[1] - a[1], z = b[2] - a[2];
    const float flat = std::sqrt(x * x + z * z), len = std::sqrt(x * x + y * y + z * z);
    if (!std::isfinite(len) || len < 0.02f || len > 0.5f || flat < 0.2f * len) return false;  // an unusable rest bone
    *elevation = std::atan2(y, flat);
    return true;
}

void RequestDump() {
    g_dumpRequest = true;
    Log("FPBODY skeleton dump requested (written at the next first-person frame)");
}

void LogStatus() {
    movielink::LogStatus();
    std::lock_guard lock(g_rigMutex);
    Log("FPITEM fp_items=%d: %llu attached-object matrices answered from the drawn pose (%llu carried by a hand, %llu on "
        "the body node), %llu palettes of skeleton-copy objects; hand rule: carried at %d .. %d mm from the wrist, %llu "
        "answers in the 300 .. 450 mm fade",
        int(g_items.load()), static_cast<unsigned long long>(g_itemAnswers.load()),
        static_cast<unsigned long long>(g_itemByHand.load()), static_cast<unsigned long long>(g_itemBody.load()),
        static_cast<unsigned long long>(g_propPalettes.load()), g_handMaxMm.load() ? g_handMinMm.load() : 0,
        g_handMaxMm.load(), static_cast<unsigned long long>(g_handFading.load()));
    Log("FPITEM hand rule votes (0.30.0, Hips / body node only): %llu decided carried, %llu decided worn",
        static_cast<unsigned long long>(g_heldVotes.load()), static_cast<unsigned long long>(g_wornVotes.load()));
    Log("FPITEM movie route (0.33.0, fp_items_loose=%d): %llu answers in a controller hand, %llu decided carried, %llu let "
        "go, %llu started from the hand socket, %llu children inherited; %d object(s) near the hands now",
        int(g_looseItems.load()), static_cast<unsigned long long>(g_looseAnswers.load()),
        static_cast<unsigned long long>(g_looseCarried.load()), static_cast<unsigned long long>(g_looseLetGo.load()),
        static_cast<unsigned long long>(g_looseSeeded.load()), static_cast<unsigned long long>(g_looseInherit.load()),
        g_looseLive.load());
    {
        const uint64_t calls = g_propCalls.load(), ticks = g_propTicks.load();
        LARGE_INTEGER freq;
        QueryPerformanceFrequency(&freq);
        Log("FPITEM object test: %llu skinned records looked at, %.2f microseconds each on average; pair-walk palettes %llu",
            static_cast<unsigned long long>(calls),
            calls ? double(ticks) * 1e6 / double(freq.QuadPart) / double(calls) : 0.0,
            static_cast<unsigned long long>(g_partnerPalettes.load()));
    }
    Log("FPCLOTH fp_cloth=%d reset=%d hair=%d: %d cloth transform set(s) known, %d on the drawn body; %llu cloth poses written, "
        "%llu without a first-person plan, %llu resets to the skinned pose (%llu after a jump of the drawn body)",
        int(g_cloth.load()), int(g_clothTeleport.load()), int(g_clothHair.load()), g_clothKnown.load(), g_clothRedirected.load(),
        static_cast<unsigned long long>(g_clothPoses.load()), static_cast<unsigned long long>(g_clothFailed.load()),
        static_cast<unsigned long long>(g_clothResets.load()), static_cast<unsigned long long>(g_clothJumps.load()));
    Log("FPFINGER %llu hand poses (%llu skipped: an item in the hand); bones from the rest shape %llu, from the animation "
        "%llu; thumb about its own Y %llu, about the palm axis %llu; index .. pinky about their own Z %llu, about the palm axis "
        "%llu (0.35.0); rest shape %s (%d usable)",
        static_cast<unsigned long long>(g_fingerHands.load()), static_cast<unsigned long long>(g_fingerHeld.load()),
        static_cast<unsigned long long>(g_fingerRestPlaced.load()), static_cast<unsigned long long>(g_fingerAnimKept.load()),
        static_cast<unsigned long long>(g_fingerThumbOwn.load()), static_cast<unsigned long long>(g_fingerThumbPalm.load()),
        static_cast<unsigned long long>(g_fingerLongOwn.load()), static_cast<unsigned long long>(g_fingerLongPalm.load()),
        g_rig && g_rig->haveRest ? "read" : "not read", g_rig ? g_rig->restUsable : 0);
    Log("FPBODY installed=%d enabled=%d active=%d palette calls=%llu edited=%llu wrist limited=%llu idle holds=%llu "
        "sleeve nodes on roll helpers=%llu rig=%p "
        "samples=%d head set=%d hands=%d/%d hand frames=%d/%d",
        int(g_installed.load()), int(g_enabled.load()), int(g_active.load()),
        static_cast<unsigned long long>(g_calls.load()), static_cast<unsigned long long>(g_edits.load()),
        static_cast<unsigned long long>(g_wristLimited.load()), static_cast<unsigned long long>(g_idleHolds.load()),
        static_cast<unsigned long long>(g_sleeveOnRoll.load()), g_rig ? g_rig->skeleton : nullptr, g_rig ? g_rig->samples : 0, g_rig ? g_rig->headCount : 0,
        g_rig ? g_rig->handCount[0] : 0, g_rig ? g_rig->handCount[1] : 0, g_rig ? int(g_rig->anatValid[0]) : 0,
        g_rig ? int(g_rig->anatValid[1]) : 0);
}

}  // namespace hrvr::fpbody
