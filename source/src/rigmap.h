#pragma once
// Bone rules that the rig pictures proved on every character rig of the game files (0.16.0; playbook 12.10,
// docs/RIG_PICTURES.md, tools/rig_viz.py holds the same rules in Python and tests/rigmap_fixture.inc ties the two
// together on a real rig).
//
// STATIC (NODEAR reader 0x1409fa4f0, all 75 character rigs) + LIVE (hrvr_logs/skeleton_226 / 250 / 252, 32 pose
// samples each, distance 0.0000 m in every sample): Heavy Rain's skin helpers that are not below the bone they
// skin hang from one group under the Hips (class word 4 in the name table). Each helper group node is held on a
// body joint by a bone constraint - the wrist (Hand), the elbow (ForeArm), the shoulder (Arm), the Head (Madison's
// fringe "Meche" helpers), the knees - and its children are the skin bones 10 cm out (Left / RightUpWristSkin,
// DownWristSkin, the elbow pair). A group node of one joint can be the CHILD of another joint's group node (the
// wrist group hangs from the elbow group), so the joint a bone rests on decides before its parent does.
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace hrvr::rigmap {

constexpr float kJointRadius = 0.015f;  // m: "rests on the joint" (the files and the live samples give 0.000)

// kOnRoll: rests on a forearm roll helper and is placed from it; kViaRoll: hangs below such a bone (follows its parent).
enum Seg : uint8_t {
    kNoSeg = 0, kHandSeg = 1, kForeSeg = 2, kUpperSeg = 3, kSegMask = 3, kViaRoll = 0x20, kOnRoll = 0x40, kOnJoint = 0x80
};

inline bool Below(const int16_t* parent, int i, int ancestor) {
    for (int p = parent[i], guard = 0; p >= 0 && guard < 64; p = parent[p], ++guard)
        if (p == ancestor) return true;
    return false;
}

inline float Dist(const float* worlds, int a, int b) {
    const float* p = worlds + size_t(a) * 16 + 12;
    const float* q = worlds + size_t(b) * 16 + 12;
    const float x = p[0] - q[0], y = p[1] - q[1], z = p[2] - q[2];
    return std::sqrt(x * x + y * y + z * z);
}

// Which arm segment carries each bone that is outside the arm's own subtree (those follow their parents anyway):
// seg[i] = the segment | kOnJoint when the bone rests on that segment's joint and must be placed explicitly, the
// parent's segment when its parent is carried, 0 otherwise. Nothing at or below `faceRoot` (the Neck) is carried.
// `worlds` is the clean pose (row-major 4x4, translation at +12), parents precede children.
//
// 0.35.0 (owner 2026-11-20: "weird clothing around wrist enlargement" on some characters). LIVE (the game's own poses,
// hrvr_logs/skeleton_244 Shelby x 64 samples, skeleton_252 Jayden x 32) + DATA (all 75 rigs): Shelby, Jayden and
// Lauren hang a second forearm skin group from the elbow's group node: a node 10 cm up the forearm that rests EXACTLY
// on ForeArmRoll2 and turns with it (0.0 deg in every sample), and below it the sleeve-end bones that rest on the
// wrist joint and turn with that node, NOT with the hand (0.0 deg against their parent; 12 .. 15 deg median, 33 .. 39
// deg at the 95th percentile against the hand). The joint rule sent those to the hand, so the sleeve end took the
// hand's whole bend and roll. Now, with the roll helpers given (`roll`, `rolls`): a bone that rests on no arm joint
// but on a roll helper is placed from that roll helper (kOnRoll, `rollOf` = its index), and a bone below such a bone
// follows its parent even when it rests on the wrist joint (kViaRoll). Ethan's and Madison's rigs have no such bone
// (every reference unchanged on skeleton_226 / 250). The wrist's own group node (parent on the elbow joint) keeps the
// joint rule: 0.0 deg against the hand on every rig.
inline void ArmCarry(const float* worlds, uint32_t count, const int16_t* parent, int arm, int fore, int hand,
                     int faceRoot, uint8_t* seg, const int* roll = nullptr, int rolls = 0, int16_t* rollOf = nullptr) {
    for (uint32_t i = 0; i < count; ++i) {
        seg[i] = kNoSeg;
        if (rollOf) rollOf[i] = -1;
    }
    if (arm < 0 || fore < 0 || hand < 0) return;
    const int joint[3] = {hand, fore, arm};
    for (uint32_t n = 0; n < count; ++n) {
        const int i = int(n);
        if (i == arm || Below(parent, i, arm)) continue;
        if (faceRoot >= 0 && (i == faceRoot || Below(parent, i, faceRoot))) continue;
        const int p = parent[i];
        const bool parentViaRoll = p >= 0 && (seg[p] & (kOnRoll | kViaRoll));
        for (int k = 0; k < 3 && !seg[i]; ++k)
            if (Dist(worlds, i, joint[k]) < kJointRadius) seg[i] = uint8_t((k + 1) | kOnJoint);
        if (parentViaRoll && (seg[i] & kOnJoint) && (seg[i] & kSegMask) == kHandSeg)
            seg[i] = uint8_t((seg[p] & kSegMask) | kViaRoll);  // the sleeve end on the wrist: with its parent
        for (int k = 0; k < rolls && !seg[i]; ++k)
            if (roll[k] >= 0 && uint32_t(roll[k]) < count && Dist(worlds, i, roll[k]) < kJointRadius) {
                seg[i] = uint8_t(kForeSeg | kOnRoll);
                if (rollOf) rollOf[i] = int16_t(roll[k]);
            }
        if (!seg[i] && p >= 0 && seg[p]) seg[i] = uint8_t((seg[p] & kSegMask) | (parentViaRoll ? kViaRoll : 0));
    }
}

// Bones collapsed with the head although they are not below it: a bone resting on the Head joint (a hair helper
// group) and everything below such a bone. out[i] = 1 for those; the Head's own subtree is not marked.
inline void HeadFollowers(const float* worlds, uint32_t count, const int16_t* parent, int head, int neck,
                          uint8_t* out) {
    for (uint32_t i = 0; i < count; ++i) out[i] = 0;
    if (head < 0) return;
    for (uint32_t n = 0; n < count; ++n) {
        const int i = int(n);
        if (i == head || i == neck || Below(parent, i, head)) continue;
        if (Dist(worlds, i, head) < kJointRadius || (parent[i] >= 0 && out[parent[i]])) out[i] = 1;
    }
}

// Share of the wrist's twist for a forearm roll helper at `place` (0 elbow .. 1 wrist) along the forearm.
// STATIC: the helpers rest at 0.33 / 0.66 of the forearm on the playable rigs (0.28 .. 0.81 on the others, one at
// 0.39 .. 0.56 on background bodies). LIVE (the game's own poses, hand twists of 10 .. 64 deg on Ethan, Madison
// and Jayden): the game turns them by 0.32 .. 0.41 and 0.79 .. 1.01 of the hand's twist. 1.25 x place gives
// 0.41 / 0.83; INFERENCE between those points.
inline float RollShare(float place) {
    const float s = 1.25f * place;
    return s < 0.f ? 0.f : s > 1.f ? 1.f : s;
}

// Place of point p along a -> b (0 at a, 1 at b), from world translations.
inline float PlaceAlong(const float* worlds, int a, int b, int p) {
    const float* A = worlds + size_t(a) * 16 + 12;
    const float* B = worlds + size_t(b) * 16 + 12;
    const float* P = worlds + size_t(p) * 16 + 12;
    const float dx = B[0] - A[0], dy = B[1] - A[1], dz = B[2] - A[2];
    const float d2 = dx * dx + dy * dy + dz * dz;
    return d2 > 1e-9f ? ((P[0] - A[0]) * dx + (P[1] - A[1]) * dy + (P[2] - A[2]) * dz) / d2 : 0.f;
}

// 0.17.0 (playbook 13.8): how much of a tracked hand's edit an object takes that hangs from a bone outside the arms
// (Heavy Rain attaches most props to the Hips and lets the prop's own animation bring it to the hand): all of it
// within 30 cm of the wrist in the clean pose, none beyond 45 cm, linear between. INFERENCE from DATA: the fixed
// offsets of the game's 45 hand-attached animations are 0.05 .. 0.29 m from the hand bone (median 0.09 m).
inline float HandCarryWeight(float distanceToWrist) {
    const float w = (0.45f - distanceToWrist) / 0.15f;
    return w < 0.f ? 0.f : w > 1.f ? 1.f : w;
}

// 0.30.0 (LIVE 0.20.0 log 2026-11-15: at the first F1 frame eight objects on Hips, RightUpLeg, LeftUpLeg, Spine and
// Spine1 were "carried with that hand" because the idle animation hangs the hands beside the hips - worn things would
// follow a raised controller). Two gates before HandCarryWeight, playbook 19.8 point 7 (the engine's link first):
// 1. Only an object on the Hips (bone 0) or on the body node (-1) can be a carried prop. DATA: of all 29,437 animation
//    modifiers the ones that do not name a hand bone name the Hips (498); none names a leg or spine bone.
inline bool HandRuleBone(int bone) { return bone <= 0; }
// 2. On the Hips it must MOVE like a carried thing: a worn object keeps its place against the Hips, a prop that the
//    animation brings to the hand keeps its place against the wrist. Each sample feeds how far the object moved
//    against its parent (hipsStep) and against the nearer clean wrist (wristStep), metres. Undecided = worn: nothing
//    follows a hand until the motion says so. INFERENCE (logged per object; one run settles it).
struct CarryVote {
    static constexpr float kAlpha = 0.2f;      // smoothing of the per-sample steps
    static constexpr float kMoving = 0.0015f;  // a smoothed step that counts as moving (metres per sample)
    static constexpr float kRatio = 3.f;       // one frame must be this much steadier than the other
    float hipsEma = 0.f, wristEma = 0.f;
    bool held = false;
    // Returns true when the decision changed.
    bool Feed(float hipsStep, float wristStep) {
        if (!(hipsStep >= 0.f) || !(wristStep >= 0.f) || hipsStep > 1.f || wristStep > 1.f) return false;  // a jump
        hipsEma += (hipsStep - hipsEma) * kAlpha;
        wristEma += (wristStep - wristEma) * kAlpha;
        const bool was = held;
        if (hipsEma > kMoving && hipsEma > kRatio * wristEma) held = true;         // moves with the wrist
        else if (wristEma > kMoving && wristEma > kRatio * hipsEma) held = false;  // stays with the hips
        return held != was;
    }
};

// ---- 0.18.0: objects whose skeleton copies the character's (coats, pullovers, a cape, jeans; playbook 13.8) ----
// DATA (tools/rig_items.py, docs/items): 13 clothing props carry 7 .. 16 bones named like the character's, resting on
// those bones, plus cloth bones below them. The engine attaches such a prop to a bone (the Hips) and plays the prop's
// own animation, authored with the character's: its sleeve bones lie on her arm bones in every frame.
// A prop bone "lies on hers" when it has the name of one of her bones and is within kOnHers of that bone in the pose
// the prop was animated against (her clean pose, moved as the prop's root was moved). Those bones take her drawn bone
// (keeping their own small offset from it); every other bone below one of them hangs off its parent with this frame's
// local; a bone below none of them is left where the animation has it.
constexpr float kOnHers = 0.05f;
constexpr int kMinOnHers = 3;  // fewer: not a copy (every prop's root is called "Hips")
enum CopyState : uint8_t { kCopyStays = 0, kCopyOnHers = 1, kCopyCarried = 2 };

inline void MulAffine(const float* a, const float* b, float* out) {  // row vectors: out = a x b
    float r[16];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            r[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j] + a[i * 4 + 3] * b[12 + j];
    for (int k = 0; k < 16; ++k) out[k] = r[k];
}

inline bool InvAffine(const float* m, float* out) {
    const float a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9], i = m[10];
    const float det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (!(std::fabs(det) > 1e-12f)) return false;
    const float s = 1.f / det;
    float r[16] = {(e * i - f * h) * s, (c * h - b * i) * s, (b * f - c * e) * s, 0.f,
                   (f * g - d * i) * s, (a * i - c * g) * s, (c * d - a * f) * s, 0.f,
                   (d * h - e * g) * s, (b * g - a * h) * s, (a * e - b * d) * s, 0.f,
                   0.f,                 0.f,                 0.f,                 1.f};
    r[12] = -(m[12] * r[0] + m[13] * r[4] + m[14] * r[8]);
    r[13] = -(m[12] * r[1] + m[13] * r[5] + m[14] * r[9]);
    r[14] = -(m[12] * r[2] + m[13] * r[6] + m[14] * r[10]);
    for (int k = 0; k < 16; ++k) out[k] = r[k];
    return true;
}

// ---- 0.31.0: objects the animation carries WITHOUT any parent (docs/ITEMS.md 4g, docs/items/CARRY.md) ----
// DATA (tools/rig_carry.py on the decoded clips): Heavy Rain moves a handled object with its own clip, paired with the
// character's (`<scene>_CT_<character>_<verb>` / `_CT_<object>_<verb>`) in one scene space; the object is not under the
// character (0403 pizza box TAKE: 22.8 cm from the left wrist for 65 frames). STATIC + DATA 2026-11-20: such an object
// is a rigid mesh on its own NODE chain under the room node (HR_ETHANHOUSE_INT: LANDING_LIVING_KITCHEN -> PIZZABOX_L
// -> PIZZABOX; the clip's track 0 animates PIZZABOX_L), so its world is local x ParentMatrix(room) - fpbody answers
// that ParentMatrix call (no skin palette: the box mesh has no skin weights).
// The same vote as CarryVote with the WORLD as the object's parent (step in the world against step in the wrist's
// frame; measured against the Hips a still fridge door read as carried while the character moved), then the 30 / 45 cm
// weight against the nearer wrist. Fed at the clips' 30 Hz (fpbody samples every 33 ms) with the object's root and
// the player's CLEAN wrist matrices. The weight never changes by more than 1 / kRamp per sample (0.2 s from none to
// all): a decision moves the object from the animated hand to the drawn one, and the ramp keeps that from being a
// jump (playbook HAND-012, "ramp the grip").
struct LooseCarry {
    static constexpr int kRamp = 6;
    CarryVote vote;
    float ramp = 0.f;
    int lastHand = -1;
    float last[3] = {0.f, 0.f, 0.f};
    float rel[2][3] = {};
    bool have = false;
    // An object that was just in that hand's socket (the game attached it, fpbody answered it with the drawn hand)
    // starts carried at full weight: the PUT clip that follows must not first jump to the animated hand.
    void Seed(int hand) {
        vote.held = true;
        ramp = 1.f;
        lastHand = hand;
    }
    // o: the object's root (world); w[s]: the clean wrist worlds (16 floats, row vectors), nullptr = no such wrist.
    // Returns the weight of the hand's edit (0 = none) and that hand in *hand (-1 = none).
    float Feed(const float* o, const float* const w[2], int* hand) {
        *hand = -1;
        float r[2][3] = {};
        bool ok[2] = {false, false};
        for (int s = 0; s < 2; ++s) {
            float inv[16];
            if (!w[s] || !InvAffine(w[s], inv)) continue;
            for (int k = 0; k < 3; ++k) r[s][k] = o[0] * inv[k] + o[1] * inv[4 + k] + o[2] * inv[8 + k] + inv[12 + k];
            ok[s] = true;
        }
        if (!ok[0] && !ok[1]) {
            have = false;
            ramp = 0.f;
            lastHand = -1;
            return 0.f;
        }
        if (have) {
            const float dx = o[0] - last[0], dy = o[1] - last[1], dz = o[2] - last[2];
            const float worldStep = std::sqrt(dx * dx + dy * dy + dz * dz);
            float wristStep = 1e9f;
            for (int s = 0; s < 2; ++s) {
                if (!ok[s]) continue;
                const float ax = r[s][0] - rel[s][0], ay = r[s][1] - rel[s][1], az = r[s][2] - rel[s][2];
                wristStep = std::fmin(wristStep, std::sqrt(ax * ax + ay * ay + az * az));
            }
            vote.Feed(worldStep, wristStep);
        }
        for (int k = 0; k < 3; ++k) last[k] = o[k];
        for (int s = 0; s < 2; ++s)
            for (int k = 0; k < 3; ++k) rel[s][k] = r[s][k];
        have = true;
        float best = 0.f;
        int bestHand = -1;
        for (int s = 0; s < 2; ++s) {
            if (!ok[s]) continue;
            const float* p = w[s] + 12;
            const float dx = o[0] - p[0], dy = o[1] - p[1], dz = o[2] - p[2];
            const float wt = HandCarryWeight(std::sqrt(dx * dx + dy * dy + dz * dz));
            if (wt > best) {
                best = wt;
                bestHand = s;
            }
        }
        // The weight goes toward its target (the 30 / 45 cm weight while carried, else 0) by at most 1 / kRamp per
        // sample, also when the distance fades it (a hand pulling away at 5 cm per sample would drop it by a third). A
        // change of hand fades the old hand out first: the object never jumps from one drawn hand to the other.
        const bool on = vote.held && bestHand >= 0;
        float target = on ? best : 0.f;
        if (on && lastHand >= 0 && bestHand != lastHand && ramp > 0.f) target = 0.f;
        else if (on) lastHand = bestHand;
        const float step = 1.f / float(kRamp);
        ramp += std::fmin(step, std::fmax(-step, target - ramp));
        if (ramp < 1e-4f) ramp = 0.f;
        if (ramp <= 0.f || lastHand < 0 || !ok[lastHand]) return 0.f;
        *hand = lastHand;
        return ramp;
    }
};

// ---- Playbook 19.8 point 11 Gate 2 CANDIDATE (2026-11-20, desk only: NOT called by fpbody until Gates 2 and 3 pass) ----
// Objects a playing movie hands a playable character (docs/items/HOLDING.md route 1 / 4). The engine's link is the
// running MOVIE's entity list (+0x150): it says the object and the player play TOGETHER, not in which frames the hand
// holds it (the pizza box sits in the fridge for 38 of its 101 TAKE frames while in the same movie). So, per sample:
//   link   - the object is in a playing movie with the player (fpbody reads it from the movie; offline: the pair);
//   hand   - the nearer CLEAN wrist within kNear (30 cm: Heavy Rain's sockets reach 28.7 cm; playbook 19.8 point 8
//            says 25), only to pick WHICH hand;
//   held   - the object keeps its place against a wrist while it moves in the world (in one sample: < kSteady
//            against it while >= kMoving in the world; or accumulated: within kSlip while kPick): the clip pair
//            carries it (one sample also needs it to move kRatio times more in the world than against the hand).
//            Its place in the hand at that sample is the ANCHOR; it stays held while it is within kKeep (30 cm) and
//            within kSlip of the anchor (which follows slow re-grips, kAnchor per sample; an item that slides but
//            still moves is re-anchored), and is let go after kLoose samples where it rests in the world away from
//            its place in the hand (a put-down) or is beyond kKeep (35 cm).
//            DATA (Gate 2, 2401 matchbox SURPRISE): a trembling hand bone moves 1 .. 1.5 cm per frame around a box
//            it holds, within +-1 cm of one place - a per-sample test let go of it. Without all this (linkOnly, the
//            playbook's literal "link + 25 cm") anything within 25 cm of a hand counts - Gate 2 compares the two on the clips.
// Each hand has its own weight that moves toward its target by at most 1 / kRamp per sample, so a pick-up, a release
// and a pass to the other hand are cross-fades (the two weights sum to <= 1), never a jump. Seed(): the object was
// just in that hand's socket (the previous clip's attach), so a put-back clip starts at full weight.
struct MovieCarry {
    // DATA (SOCKETS.md): Heavy Rain's hand sockets put an item's origin up to 28.7 cm from the wrist (Beyond's 25 cm,
    // playbook 19.8 point 8, is too short: the 0201 bag hangs 25 .. 28 cm from the hand).
    // (35 / 40 cm was tried for long items held at one end, e.g. the 0201 ruler at 29 .. 30 cm: 32 open Gate 2 pairs
    // instead of 26 and more drops - kept at 30 / 35.)
    static constexpr float kNear = 0.30f;
    static constexpr float kKeep = 0.35f;  // once held: the edge of kNear must not flicker
    static constexpr float kSlip = 0.03f;
    static constexpr float kSteady = 0.015f;
    static constexpr float kMoving = 0.002f;
    static constexpr float kAnchor = 0.1f;
    static constexpr float kRatio = 2.f;
    static constexpr float kPick = 0.02f;
    static constexpr float kWithHand = 0.02f;  // a sample's step against the hand that still counts as "with it"
    // m per sample: below this the item does not really move (1 mm: the 5001 hangman sheet pinned to the table by the
    // left hand jitters 0.1 .. 0.7 mm per frame and was passed to that hand; the sub-mm handcuffs are worn, not held)
    static constexpr float kStill = 0.001f;
    static constexpr float kRest = 0.001f;   // smoothed world speed below this (m per sample): at rest
    static constexpr float kRestAlpha = 0.5f;
    static constexpr float kCut = 0.5f;  // m in one sample: the clip jumps (a scene cut), never carried across it
    // On the HEAD, not in the hand (owner 2026-11-20 "distinguish when he has it in hands and when on his body"; Jayden's
    // ARI glasses): within kHead of the head bone and kHeadRatio times steadier against the head than against the
    // hand (smoothed per-sample steps, the hand's at least kMoving). DATA (Gate 2, 0020 ARIGLASSES_PUTON): on the face
    // the glasses move 0.01 cm per frame against the head, 13 cm from it, while the hand leaves at 5 .. 9 cm per frame.
    // A phone held to the ear is steady against both, so it stays in the hand.
    static constexpr float kHead = 0.20f;
    static constexpr float kHeadRatio = 3.f;
    // On the FOREARM, not in the hand (owner 2026-11-20 "Stay with the arm": a bag hanging on the forearm while that hand
    // does something else - 3003 shop bag while Madison phones): a model bone lies along that forearm (elbow -> wrist,
    // kArmT0 .. kArmT1 of the way, within kArmR of the line). DATA (Gate 2, 3003 BAG01): on the arm the nearest bone is
    // at 0.69 .. 0.91, 1 .. 11 cm from the line; held in the hand the nearest bone is beyond the wrist (1.0).
    static constexpr float kArmR = 0.12f;
    static constexpr float kArmT0 = 0.1f;
    static constexpr float kArmT1 = 0.95f;
    // It goes on the forearm only while it also slides against that hand (smoothed step >= kMoving: 3003 on the arm 2 ..
    // 15 mm per frame; 4101 bag held in both hands, along the right forearm, ~0), and stays there until it leaves the
    // arm (no model bone within kKeep of elbow -> wrist) - it swings past the wrist and back (3003 frames 600 .. 1800;
    // a release when steadier against the hand let it into the hand for 3 s). Only for an object of the ARM class
    // (item_class.h kArm, decided per pair from its whole clip by rig_gate2.py: >= 10 s on the arm) - fpbody passes
    // `fore` only for those; tested on every skinned prop the per-frame test opened 8 Gate 2 pairs.
    bool armLatch[2] = {false, false};
    static constexpr int kLoose = 2;
    static constexpr int kRamp = 6;
    bool linkOnly = false;
    bool held = false;
    int hand = -1;
    int loose = 0;
    float weight[2] = {0.f, 0.f};
    float anchor[3] = {0.f, 0.f, 0.f};
    bool anchored = false;
    // Per hand: the item's place in that hand and in the world when it came within kNear (or last slipped).
    float last[3] = {0.f, 0.f, 0.f};
    float rel[2][3] = {};
    float worldEma = 0.f;  // smoothed world speed: a slowly swinging bag (1.6 .. 7 mm per frame) is not at rest
    float relHead[3] = {0.f, 0.f, 0.f};
    float emaHead = 0.f, emaHand[2] = {0.f, 0.f};
    bool haveHead = false;
    bool cand[2] = {false, false};
    float candRel[2][3] = {}, candTravel[2] = {0.f, 0.f};
    bool have = false;
    // Playbook r37 19.8 point 11 L2: distances to the object's NEAREST MODEL BONE, never only its root (a skinned prop's
    // root can be far from the part in the fingers - DATA docs/items/L2.md: 45 held pairs, the 3003 shop bag's strap
    // 40 cm from its root). Per hand: the bone nearest that wrist, kept while that hand holds it or is building a pick-up
    // (its place in the hand must stay one point); point 0 = the root. With one point this is exactly the 0.33.0 rule.
    int bone[2] = {0, 0};
    bool boneChosen[2] = {false, false};
    float lastPt[2][3] = {};
    static constexpr int kWin = 10;  // samples: rig_gate2's truth keeps a run while it moved <= kSlip in 10 frames
    float hist[2][kWin][3] = {};
    int histN[2] = {0, 0};
    void Seed(int h) {
        held = h >= 0;
        hand = h;
        loose = 0;
        anchored = false;  // taken from the first sample
        boneChosen[0] = boneChosen[1] = false;  // the seeded hand's bone too: the nearest in the first sample
        histN[0] = histN[1] = 0;
        weight[0] = weight[1] = 0.f;
        if (h >= 0) weight[h] = 1.f;
    }
    // o: the object's root (world, animated); w[s]: the clean wrist worlds (16 floats, row vectors) or nullptr.
    // Writes the two hands' weights to out[2]. Returns the hand with the larger weight (-1 = none).
    // head: the clean Head bone world (16 floats) or nullptr (then nothing is taken for worn on the head).
    int Feed(bool link, const float* o, const float* const w[2], float* out, const float* head = nullptr) {
        return FeedPoints(link, o, 1, w, out, head);
    }
    // pts: n model-bone positions (world, 3 floats each), pts[0..2] = the root.
    // fore[s]: the clean ForeArm bone world (the elbow; nullptr = no forearm test for that hand).
    int FeedPoints(bool link, const float* pts, int n, const float* const w[2], float* out, const float* head = nullptr,
                   const float* const* fore = nullptr) {
        const float* o = pts;  // the root: world speed, cuts, the head test
        if (n < 1) n = 1;
        float r[2][3] = {}, d[2] = {1e9f, 1e9f};
        bool ok[2] = {false, false}, fresh[2] = {false, false};
        const float* po[2] = {pts, pts};
        int nearestBone[2] = {0, 0};
        float nearestD[2] = {1e9f, 1e9f};
        for (int s = 0; s < 2; ++s) {
            if (w[s]) {
                const bool locked = boneChosen[s] && bone[s] < n && ((held && hand == s) || cand[s]);
                float best = 1e30f;
                for (int i = 0; i < n; ++i) {
                    const float* q = pts + 3 * i;
                    const float dd = (q[0] - w[s][12]) * (q[0] - w[s][12]) + (q[1] - w[s][13]) * (q[1] - w[s][13]) +
                                     (q[2] - w[s][14]) * (q[2] - w[s][14]);
                    if (dd < best) best = dd, nearestBone[s] = i;
                }
                nearestD[s] = std::sqrt(best);
                const int b = locked ? bone[s] : nearestBone[s];
                fresh[s] = b != bone[s];  // another point than lastPt[s] / rel[s] describe: no history for this hand
                bone[s] = b;
                boneChosen[s] = true;
            }
            po[s] = pts + 3 * (bone[s] < n ? bone[s] : 0);
            const float* q = po[s];
            float inv[16];
            if (!w[s] || !InvAffine(w[s], inv)) continue;
            for (int k = 0; k < 3; ++k) r[s][k] = q[0] * inv[k] + q[1] * inv[4 + k] + q[2] * inv[8 + k] + inv[12 + k];
            const float* p = w[s] + 12;
            d[s] = std::sqrt((q[0] - p[0]) * (q[0] - p[0]) + (q[1] - p[1]) * (q[1] - p[1]) + (q[2] - p[2]) * (q[2] - p[2]));
            ok[s] = true;
        }
        // Per hand: the world step of that hand's point (the root's step with one point).
        bool hv[2] = {have && !fresh[0], have && !fresh[1]};
        float ws[2] = {0.f, 0.f};
        for (int s = 0; s < 2; ++s)
            if (hv[s]) {
                const float* q = po[s];
                ws[s] = std::sqrt((q[0] - lastPt[s][0]) * (q[0] - lastPt[s][0]) + (q[1] - lastPt[s][1]) * (q[1] - lastPt[s][1]) +
                                  (q[2] - lastPt[s][2]) * (q[2] - lastPt[s][2]));
            }
        for (int s = 0; s < 2; ++s)
            if (fresh[s]) cand[s] = false;
        // 1.0.5 hand-over history: the point's place in each hand over the last kWin samples (one point: a bone switch
        // or a cut starts it again). drift = how far it moved against the hand since the oldest of them.
        float drift[2] = {0.f, 0.f};
        for (int s = 0; s < 2; ++s) {
            const bool cutNow = have && (o[0] - last[0]) * (o[0] - last[0]) + (o[1] - last[1]) * (o[1] - last[1]) +
                                                (o[2] - last[2]) * (o[2] - last[2]) > kCut * kCut;
            if (!ok[s] || fresh[s] || cutNow) histN[s] = 0;
            if (!ok[s]) continue;
            if (histN[s] == kWin) {
                for (int i = 1; i < kWin; ++i)
                    for (int k = 0; k < 3; ++k) hist[s][i - 1][k] = hist[s][i][k];
                --histN[s];
            }
            for (int k = 0; k < 3; ++k) hist[s][histN[s]][k] = r[s][k];
            ++histN[s];
            const float* a = hist[s][0];
            drift[s] = std::sqrt((r[s][0] - a[0]) * (r[s][0] - a[0]) + (r[s][1] - a[1]) * (r[s][1] - a[1]) +
                                 (r[s][2] - a[2]) * (r[s][2] - a[2]));
        }
        int nearest = -1;
        for (int s = 0; s < 2; ++s)
            if (ok[s] && d[s] <= kNear && (nearest < 0 || d[s] < d[nearest])) nearest = s;
        // Moves with hand s: it kept its place against s (within kSlip) while it moved kPick in the world since it
        // came within reach - accumulated, so a handcuff that moves 0.4 mm per frame with a resting hand counts
        // (Gate 2, 3202 HANDCUFF_LIFT) and a box at rest beside a resting hand never does.
        // Or, in one sample, it kept its place against s (< kSteady) while it moved kMoving (a quick pick-up: the
        // 0403 pizza box leaves the fridge at 2 .. 3 mm per frame while the hand still settles on it).
        float worldStep = 0.f;
        if (have)
            worldStep = std::sqrt((o[0] - last[0]) * (o[0] - last[0]) + (o[1] - last[1]) * (o[1] - last[1]) +
                                  (o[2] - last[2]) * (o[2] - last[2]));
        if (have && worldStep <= kCut) worldEma += kRestAlpha * (worldStep - worldEma);
        bool movesWith[2] = {false, false}, nowWith[2] = {false, false};
        float relStep[2] = {1e9f, 1e9f};
        for (int s = 0; s < 2; ++s) {
            if (hv[s] && ok[s]) {
                const float ax = r[s][0] - rel[s][0], ay = r[s][1] - rel[s][1], az = r[s][2] - rel[s][2];
                relStep[s] = std::sqrt(ax * ax + ay * ay + az * az);
            }
            if (hv[s] && ok[s] && d[s] <= kKeep && ws[s] >= kMoving) {
                const float ax = r[s][0] - rel[s][0], ay = r[s][1] - rel[s][1], az = r[s][2] - rel[s][2];
                // and it moved much less against the hand than in the world: a still hand beside an item another hand
                // nudges sees it move as much as the world does (Gate 2, 5001 origami nudged 0.6 cm next to the
                // resting left hand was picked and kept for 250 frames).
                const float rs = std::sqrt(ax * ax + ay * ay + az * az);
                nowWith[s] = rs <= kSteady && rs * kRatio <= ws[s];
                movesWith[s] = nowWith[s] && d[s] <= kNear;
            }
            if (!ok[s] || d[s] > (cand[s] ? kKeep : kNear)) {  // starts within kNear, may go on to kKeep
                cand[s] = false;
                continue;
            }
            const float sx = r[s][0] - candRel[s][0], sy = r[s][1] - candRel[s][1], sz = r[s][2] - candRel[s][2];
            if (!cand[s] || sx * sx + sy * sy + sz * sz > kSlip * kSlip) {
                cand[s] = true;
                candTravel[s] = 0.f;
                for (int k = 0; k < 3; ++k) candRel[s][k] = r[s][k];
                continue;
            }
            for (int k = 0; k < 3; ++k) candRel[s][k] += kAnchor * (r[s][k] - candRel[s][k]);  // slow re-grips
            // path length WITH the hand: only samples where it kept its place against it (a resting item's clip does
            // not move at all; a notepad flung 11 cm in one frame while 5 cm against the hand does not count - Gate 2,
            // 1101 NOTEPAD_THROW was picked up again in flight)
            if (relStep[s] <= kWithHand) candTravel[s] += ws[s];
            // (A ratio on the whole stretch, with 1 cm, was tried: the hand settling on the item counts as drift and
            // the pizza box came 7 frames later; 164 open pairs instead of 82. Gate 2, 2026-11-20.)
            // ... and only in a sample where it moves at all (> kStill): a memory card at rest beside a still hand that
            // had carried it earlier was passed to it (Gate 2, 2901 PHONEMEMCARD_TAKE)
            movesWith[s] = movesWith[s] || (candTravel[s] >= kPick && ws[s] > kStill);
        }
        // Hanging on a forearm? (any model bone along elbow -> wrist; latched, see armLatch)
        bool alongArm[2] = {false, false}, nearArm[2] = {false, false};
        // Only a prop with a skeleton of its own (a strap / cloth that can hang) - a rigid sword or bat lies along the
        // forearm in the hand (Gate 2: 0201 sword pairs opened when every object was tested).
        for (int s = 0; s < 2 && fore && n > 1; ++s) {
            if (!fore[s] || !w[s]) continue;
            const float* a = fore[s] + 12;
            const float* b = w[s] + 12;
            const float ab[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
            const float l2 = ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2];
            if (l2 < 1e-6f) continue;
            for (int i = 0; i < n; ++i) {
                const float* q = pts + 3 * i;
                const float t = ((q[0] - a[0]) * ab[0] + (q[1] - a[1]) * ab[1] + (q[2] - a[2]) * ab[2]) / l2;
                const float tc = t < 0.f ? 0.f : t > 1.f ? 1.f : t;
                const float x = q[0] - a[0] - tc * ab[0], y = q[1] - a[1] - tc * ab[1], z = q[2] - a[2] - tc * ab[2];
                const float dd = x * x + y * y + z * z;
                if (dd <= kKeep * kKeep) nearArm[s] = true;
                if (t >= kArmT0 && t <= kArmT1 && dd <= kArmR * kArmR) alongArm[s] = true;
            }
        }
        // Worn on the head?
        bool onHead[2] = {false, false};
        {
            float hinv[16], rh[3] = {0.f, 0.f, 0.f}, dh = 1e9f;
            const bool hok = head && InvAffine(head, hinv);
            if (hok) {
                for (int k = 0; k < 3; ++k) rh[k] = o[0] * hinv[k] + o[1] * hinv[4 + k] + o[2] * hinv[8 + k] + hinv[12 + k];
                dh = std::sqrt((o[0] - head[12]) * (o[0] - head[12]) + (o[1] - head[13]) * (o[1] - head[13]) +
                               (o[2] - head[14]) * (o[2] - head[14]));
                if (haveHead && worldStep <= kCut) {
                    const float hx = rh[0] - relHead[0], hy = rh[1] - relHead[1], hz = rh[2] - relHead[2];
                    emaHead += kRestAlpha * (std::sqrt(hx * hx + hy * hy + hz * hz) - emaHead);
                }
                for (int k = 0; k < 3; ++k) relHead[k] = rh[k];
            }
            haveHead = hok;
            for (int s = 0; s < 2; ++s) {
                if (have && ok[s] && relStep[s] < 1e8f && worldStep <= kCut) emaHand[s] += kRestAlpha * (relStep[s] - emaHand[s]);
                onHead[s] = hok && dh <= kHead && emaHand[s] >= kMoving && emaHead * kHeadRatio <= emaHand[s];
                // Near the head it is picked up only when it clearly moves with the hand rather than the head (glasses
                // taken off leave the face); glasses on the face of a lying, almost still Jayden (4801) beside his
                // resting hand were taken by the 2 cm accumulated test.
                if (hok && dh <= kHead && !(emaHand[s] * kHeadRatio <= emaHead)) movesWith[s] = false;
            }
        }
        // The forearm latch (after the head test: emaHand is this sample's).
        bool onArm[2] = {false, false};
        for (int s = 0; s < 2; ++s) {
            const bool fok = fore && fore[s] && w[s];
            if (!fok || !nearArm[s]) armLatch[s] = false;
            else if (alongArm[s] && emaHand[s] >= kMoving) armLatch[s] = true;  // along it AND sliding against the hand
                                                                                 // (4101: a bag held in both hands lies
                                                                                 // along the right forearm, still in hand)
        }
        // On either arm: no hand takes it (3003: the right hand holding the phone beside the bag on the left forearm
        // took it for 3 s).
        for (int s = 0; s < 2; ++s) {
            onArm[s] = armLatch[0] || armLatch[1];
            if (onArm[s]) movesWith[s] = false;
        }
        for (int k = 0; k < 3; ++k) last[k] = o[k];
        for (int s = 0; s < 2; ++s)
            for (int k = 0; k < 3; ++k) rel[s][k] = r[s][k], lastPt[s][k] = po[s][k];
        // A cut: the hold ends in this sample and both weights go to 0 at once (Gate 2: 1903 memory card, 10.9 m in
        // one frame while held - the ramp drew it metres off the hand for 8 frames). The engine cuts the view too.
        const bool cut = worldStep > kCut;
        if (cut) {
            weight[0] = weight[1] = 0.f;
            for (int s = 0; s < 2; ++s) cand[s] = false;
        }
        if (!link || cut || (linkOnly && nearest < 0)) {
            held = false;
            hand = -1;
            loose = 0;
            anchored = false;
        } else if (linkOnly) {
            held = true;
            hand = nearest;
        } else if (!held) {
            const int pick = movesWith[0] && movesWith[1] ? (d[0] <= d[1] ? 0 : 1) : movesWith[0] ? 0 : movesWith[1] ? 1 : -1;
            if (pick >= 0) {
                held = true;
                hand = pick;
                loose = 0;
                for (int k = 0; k < 3; ++k) anchor[k] = r[pick][k];
                anchored = true;
            }
        } else {
            if (!anchored && ok[hand]) {
                for (int k = 0; k < 3; ++k) anchor[k] = r[hand][k];
                anchored = true;
            }
            // The other hand takes it when it moves with that one and no longer with this one (a pass).
            const int other = 1 - hand;
            float slip = 1e9f;
            if (ok[hand]) {
                const float ax = r[hand][0] - anchor[0], ay = r[hand][1] - anchor[1], az = r[hand][2] - anchor[2];
                slip = std::sqrt(ax * ax + ay * ay + az * az);
            }
            // Let go only when it comes to REST in the world away from its place in the hand (a put-down) or leaves
            // kKeep. While it still moves it slides in the hand (a re-grip, or the item's keys lag a fast swing by
            // 3 .. 4 cm per frame - Gate 2, 3601 gun) and is re-anchored, never let go and picked again (that made
            // the weight flicker between 5/6 and 1).
            // "At rest" is the smoothed world speed (2801 bag swings 1.6 .. 7 mm per frame; 1903 key in the lock: 0).
            const bool moving = worldEma >= kRest;
            // (Tried and dropped, for the thrown 1101 notepad: ending a slide faster than 5 cm per frame against the
            // hand - also only while it gets farther, also only above 8 cm per frame in the world; the fights' swung
            // weapons then dropped out: 18 .. 38 open pairs instead of 16.)
            const bool slides = slip > kSlip && (nowWith[hand] || moving);
            // Put down: it rests in the world this sample while the hand moves against it (> kSteady). Two such
            // samples (kLoose) let go even before it is kSlip from its place (Gate 2: the 0201 plate, 1101 notepad,
            // 2901 memory card were put down with the hand resting beside them and held for up to 50 more frames).
            const bool putDown = hv[hand] && ws[hand] < kMoving && relStep[hand] > kSteady && relStep[hand] < 1e8f;
            const bool keeps = ok[hand] && d[hand] <= kKeep && (slip <= kSlip || slides) && !onHead[hand] && !onArm[hand] &&
                               !putDown;
            // A held item that keeps its place against the OTHER hand while it still moves is passed, never let go
            // and picked again (2801 golf club in both hands shifts to the right hand at 1.6 mm per frame).
            const bool passes = !keeps && !onHead[other] && !onArm[other] && (movesWith[other] ||
                                           (moving && ws[other] >= kRest && ok[other] && d[other] <= kNear &&
                                            relStep[other] <= kSteady));  // not an item at rest this sample (2901
                                                                          // memory card put down beside the still
                                                                          // left hand was passed to it)
            // 1.0.5 (L5 per-hand check): it slides against this hand while the OTHER hand holds it steady - a hand-over,
            // or the far hand of two took it first. Kept by "slides" it stayed in the wrong controller (Gate 2: 23 pairs,
            // 954 frames - 1601 gun, 1301 knife, 5203 TV, 3202 cape ...).
            // The test is Gate 2's own (rig_gate2 truth: a run in a hand lasts while its point stays within kSlip of
            // where it was kWin samples ago, and at a break the NEAREST hand takes it): it left this hand by that test
            // and the other hand, nearer than this one and within kNear, moves. Tried and dropped (Gate 2, 2026-11-21, 24 open before): one sample -
            // nowWith[other] - flickered two-handed holds between the hands, 17 open but 5 new and +31 drops; smoothed 3x
            // steadier - emaHand - 23 open (both differ from the truth's test); the truth's test within kKeep without
            // "nearer" passed thrown items between the hands in flight (wrong-follow 275 -> 784); the same held for 3
            // samples in a row - 23 open; also requiring the other hand steady (drift <= kSlip) - 20 open, 543 frames
            // (the far hand kept 10 items: TV, knife, ticket); restarting the new hand's window at the take - no better.
            // the taking hand also moving with it (<= kWithHand that sample) - 4 open, drops 310.
            // This one: wrong-hand frames 954 -> 30 (0 pairs), drops 282 -> 270, wrong-follow 275 -> 303; 2 open (1903
            // seatbelt pick-up 31 frames, 2901 scissors re-picked) - reviewed verdicts in gate2_reviewed.json.
            const bool handOver = keeps && histN[hand] >= 2 && drift[hand] > kSlip &&
                                  !onHead[other] && !onArm[other] && ok[other] &&
                                  d[other] <= kNear && d[other] < d[hand] && ws[other] >= kMoving;
            if (handOver) {
                hand = other, loose = 0;
                for (int k = 0; k < 3; ++k) anchor[k] = r[other][k];
            } else if (keeps) {
                loose = 0;
                for (int k = 0; k < 3; ++k) anchor[k] = slides ? r[hand][k] : anchor[k] + kAnchor * (r[hand][k] - anchor[k]);
            } else if (passes) {
                hand = other, loose = 0;
                for (int k = 0; k < 3; ++k) anchor[k] = r[other][k];
            } else if (n > 1 && ok[hand] && moving && !onHead[hand] && !onArm[hand] && !putDown && nearestBone[hand] != bone[hand] &&
                       nearestD[hand] <= kNear) {
                // L2 re-grip: the hand's bone left it but another bone of the same object is in it while the object
                // still moves (cloth slides through the fingers: Gate 2, 4301 / 2801 coats dropped 11 .. 17 frames).
                // The hand keeps the object on that bone; its place in the hand starts again from this sample.
                const int b = nearestBone[hand];
                const float* q = pts + 3 * b;
                float inv[16];
                if (InvAffine(w[hand], inv)) {
                    for (int k = 0; k < 3; ++k)
                        r[hand][k] = q[0] * inv[k] + q[1] * inv[4 + k] + q[2] * inv[8 + k] + inv[12 + k];
                    bone[hand] = b;
                    // rel / lastPt were stored above for the old bone: the next sample compares against this one
                    for (int k = 0; k < 3; ++k) anchor[k] = r[hand][k], rel[hand][k] = r[hand][k], lastPt[hand][k] = q[k];
                    loose = 0;
                    cand[hand] = false;
                }
            } else if (++loose >= kLoose) {
                held = false, hand = -1, loose = 0, anchored = false;
            }
        }
        have = true;
        const float step = 1.f / float(kRamp);
        for (int s = 0; s < 2; ++s) {
            const float target = (held && hand == s && ok[s]) ? 1.f : 0.f;
            weight[s] += std::fmin(step, std::fmax(-step, target - weight[s]));
            if (weight[s] < 1e-4f) weight[s] = 0.f;
        }
        out[0] = weight[0];
        out[1] = weight[1];
        return weight[0] == 0.f && weight[1] == 0.f ? -1 : weight[1] > weight[0] ? 1 : 0;
    }
};

// The drawn matrix of an object that the hands carry with weights wt[2]: full weight = anim x clean^-1 x drawn of
// that hand (it keeps its place in the drawn hand exactly as in the animated one); partial weights blend the origin
// linearly and the rotation by a normalised quaternion sum (row vectors, rows 0..2 = axes, row 3 = origin).
inline void QuatOf(const float* m, float* q) {  // x, y, z, w of the rotation rows (orthonormal assumed)
    const float t = m[0] + m[5] + m[10];
    if (t > 0.f) {
        const float s = std::sqrt(t + 1.f) * 2.f;
        q[3] = 0.25f * s, q[0] = (m[6] - m[9]) / s, q[1] = (m[8] - m[2]) / s, q[2] = (m[1] - m[4]) / s;
    } else if (m[0] > m[5] && m[0] > m[10]) {
        const float s = std::sqrt(1.f + m[0] - m[5] - m[10]) * 2.f;
        q[3] = (m[6] - m[9]) / s, q[0] = 0.25f * s, q[1] = (m[1] + m[4]) / s, q[2] = (m[8] + m[2]) / s;
    } else if (m[5] > m[10]) {
        const float s = std::sqrt(1.f + m[5] - m[0] - m[10]) * 2.f;
        q[3] = (m[8] - m[2]) / s, q[0] = (m[1] + m[4]) / s, q[1] = 0.25f * s, q[2] = (m[6] + m[9]) / s;
    } else {
        const float s = std::sqrt(1.f + m[10] - m[0] - m[5]) * 2.f;
        q[3] = (m[1] - m[4]) / s, q[0] = (m[8] + m[2]) / s, q[1] = (m[6] + m[9]) / s, q[2] = 0.25f * s;
    }
}

inline void RowsOf(const float* q, float* m) {  // the inverse of QuatOf (same convention as rig_anim.quat_rows)
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    m[0] = 1 - 2 * (y * y + z * z), m[1] = 2 * (x * y + w * z), m[2] = 2 * (x * z - w * y);
    m[4] = 2 * (x * y - w * z), m[5] = 1 - 2 * (x * x + z * z), m[6] = 2 * (y * z + w * x);
    m[8] = 2 * (x * z + w * y), m[9] = 2 * (y * z - w * x), m[10] = 1 - 2 * (x * x + y * y);
    m[3] = m[7] = m[11] = 0.f;
}

// anim: the object's animated world; clean[s] / drawn[s]: that hand's animated and drawn wrist (nullptr = none).
inline void BlendCarry(const float* anim, const float* const clean[2], const float* const drawn[2], const float* wt,
                       float* out) {
    float qa[4], acc[4], pos[3] = {anim[12], anim[13], anim[14]};
    QuatOf(anim, qa);
    float rest = 1.f;
    for (int k = 0; k < 4; ++k) acc[k] = 0.f;
    for (int s = 0; s < 2; ++s) {
        float inv[16], c[16], full[16], q[4];
        if (!(wt[s] > 0.f) || !clean[s] || !drawn[s] || !InvAffine(clean[s], inv)) continue;
        MulAffine(inv, drawn[s], c);
        MulAffine(anim, c, full);
        for (int k = 0; k < 3; ++k) pos[k] += wt[s] * (full[12 + k] - anim[12 + k]);
        QuatOf(full, q);
        const float sign = q[0] * qa[0] + q[1] * qa[1] + q[2] * qa[2] + q[3] * qa[3] < 0.f ? -1.f : 1.f;
        for (int k = 0; k < 4; ++k) acc[k] += wt[s] * sign * q[k];
        rest -= wt[s];
    }
    if (rest < 0.f) rest = 0.f;
    for (int k = 0; k < 4; ++k) acc[k] += rest * qa[k];
    const float n = std::sqrt(acc[0] * acc[0] + acc[1] * acc[1] + acc[2] * acc[2] + acc[3] * acc[3]);
    if (n > 1e-8f)
        for (int k = 0; k < 4; ++k) acc[k] /= n;
    else
        for (int k = 0; k < 4; ++k) acc[k] = qa[k];
    RowsOf(acc, out);
    out[12] = pos[0], out[13] = pos[1], out[14] = pos[2], out[15] = 1.f;
}

// prop: the object's bone worlds (count matrices), parent: its hierarchy (-1 = top; a parent precedes its children),
// hers[i]: the index of the character's bone with the same name as prop bone i, or -1; hersBase / hersDrawn: her bone
// worlds in the pose the prop was animated against and in the drawn pose. Writes out (count matrices) and
// state (CopyState per bone). Returns how many bones lie on hers; below kMinOnHers out is a plain copy.
inline int CarryCopy(const float* prop, uint32_t count, const int16_t* parent, const int16_t* hers,
                     const float* hersBase, const float* hersDrawn, float* out, uint8_t* state) {
    int on = 0;
    for (uint32_t i = 0; i < count; ++i) {
        state[i] = kCopyStays;
        for (int k = 0; k < 16; ++k) out[size_t(i) * 16 + k] = prop[size_t(i) * 16 + k];
        if (hers[i] < 0) continue;
        const float* p = prop + size_t(i) * 16 + 12;
        const float* q = hersBase + size_t(hers[i]) * 16 + 12;
        const float x = p[0] - q[0], y = p[1] - q[1], z = p[2] - q[2];
        if (x * x + y * y + z * z < kOnHers * kOnHers) {
            state[i] = kCopyOnHers;
            ++on;
        }
    }
    if (on < kMinOnHers) {
        for (uint32_t i = 0; i < count; ++i) state[i] = kCopyStays;
        return on;
    }
    float inv[16], rel[16];
    for (uint32_t i = 0; i < count; ++i) {
        float* o = out + size_t(i) * 16;
        if (state[i] == kCopyOnHers) {
            if (!InvAffine(hersBase + size_t(hers[i]) * 16, inv)) continue;
            MulAffine(prop + size_t(i) * 16, inv, rel);  // its place in her bone's frame
            MulAffine(rel, hersDrawn + size_t(hers[i]) * 16, o);
        } else if (parent[i] >= 0 && uint32_t(parent[i]) < i && state[parent[i]] != kCopyStays) {
            if (!InvAffine(prop + size_t(parent[i]) * 16, inv)) continue;
            MulAffine(prop + size_t(i) * 16, inv, rel);  // this frame's local
            MulAffine(rel, out + size_t(parent[i]) * 16, o);
            state[i] = kCopyCarried;
        }
    }
    return on;
}

}  // namespace hrvr::rigmap
