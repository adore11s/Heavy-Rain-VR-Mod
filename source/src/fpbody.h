#pragma once
// Render-only body edits for first person (B2S guide section 6, Heavy Rain adaptation).
//
// STATIC 2026-10-27: a skinned mesh is a render record of type 3 (entity+0x48). Each frame the SKELETON
// entity publishes its skeleton's arrays into it (QDT::M3D::SKELETON slot 105, 0x14096da50: record+0x58 =
// skeleton+0x10 bone worlds, +0x60 bone count, +0x68 the palette buffer), and SKIN::PaletteBuild
// (0x140a61920) then writes palette[i] = invBind[i] (+0x48) x world[i] (+0x58) x inverse(node world, +0x18).
// The edits hook that builder: for the player's meshes only, record+0x58 points at an edited COPY of the bone
// worlds for the duration of the call and is restored after it. Game logic, IK, look-at, bounds and the
// camera anchors keep the clean pose (B2S rule b); every mesh of the character (body, head, hair) reads the
// same skeleton, so one plan covers them all.
//
// Edits, in order: the idle hold (F1 standing), body yaw about the eye (F1), two-bone arm IK to the controllers with
// a limited wrist and the fingers riding on the hand as animated (F1), head + face + hair collapsed into the neck
// (F1 and F3). Which bones belong to the head, the hands, the forearms and the upper arms is learned from the clean
// pose (rigid offsets over time; HumanIK names for the core chain), since this build stores bone names only as
// CRC32 hashes.
#include <cstdint>

#include "posemath.h"

namespace hrvr::fpbody {

struct Plan {
    bool active = false;          // a first-person frame
    void* skeleton = nullptr;     // the player's skeleton instance (scene node +0x160)
    void* node = nullptr;         // that scene node (0.18.0: objects attached to the body node)
    void* partner[2] = {nullptr, nullptr};  // 0.20.0: skeletons of characters walking as a pair with the player
    bool hideHead = true;
    bool turnBody = false;        // F1: the drawn body follows the view's heading
    float bodyYaw = 0.f;          // wanted heading of the drawn body (radians, 0 = -Z)
    float facingYaw = 0.f;        // the character's own heading this frame
    pose::Vec3 pivot{};           // the eye base: the body turns about it, so the head stays under the view
    // 0.36.12 (B2S EXP-036 / 0.16.12): F1, the drawn body moved so its head is under the headset's eye (headEye, world):
    // across fully, up / down only beyond 20 cm (the feet stay on the floor while the player's head bobs or leans)
    bool followHead = false;
    pose::Vec3 headEye{};
    float eyeForward = 0.17f, eyeUp = 0.07f;  // the eye's offset from the head bone (fp_eye_fwd / fp_eye_up)
    bool arms = false;            // F1: the controllers drive the arms
    bool handValid[2] = {false, false};
    pose::Mat4 grip[2] = {pose::Identity(), pose::Identity()};  // controller grip poses in the world
    float wristPitch = 0.f;       // radians: fingers further down while holding the controller
    float wristBack = 0.07f;      // m from the grip point back to the wrist joint
    // Idle hold (F1, standing still): the drawn body keeps a snapshot relative to the character node instead of the
    // idle animation (fpbody_math.h IdleHoldState).
    bool idleHold = true;
    bool haveRoot = false;
    pose::Mat4 root = pose::Identity();  // the character node's world this frame
    bool moveStick = false;              // the movement stick is pushed (walking)
    // 0.22.0 (playbook 14.12): finger poses on the tracked hands. curl[hand][finger] 0 open .. 1 closed, finger order
    // thumb, index, middle, ring, little (fingers_math.h FromInput, rate-limited in fpcam); fingerRest = start from the
    // rig's rest shape (fp_fingers_rest_shape).
    bool fingers = false;
    bool fingerRest = true;
    float curl[2][5] = {};
    uint64_t ms = 0;
    uint64_t seq = 0;  // 0.36.0: set by Publish, one per first-person frame (the cloth's frame-to-frame jump test)
};

bool Install();
// fpcam (render thread), every first-person frame; a plan older than 250 ms is ignored.
void Publish(const Plan& plan);
void Suspend();
void SetEnabled(bool on);
bool Enabled();
// Mirrors show the player's head while first person hides it (fp_mirror_head, default on).
void SetMirrorHead(bool on);
// fp_forearm_twist: true (default, 0.16.0) = the forearm roll helpers share the wrist twist by their place along
// the forearm; false = the forearm bone rolls half the way (0.6.0 .. 0.15.1).
void SetForearmTwistHelpers(bool on);
// fp_items (default on, 0.17.0): objects attached to the player's bones are drawn from the drawn pose, and an
// object the animation brings to a tracked hand goes with that hand.
void SetItems(bool on);
// fp_items_loose (default on, 0.31.0; 0.33.0 the movie route): with the arms on the controllers, an object a scene
// clip carries with no parent link to the player (the pizza box out of the fridge, a juice bottle) is drawn in the
// controller hand while a movie plays it with the player, its class is "held" (item_class.h) and it moves with the
// animated wrist (rigmap::MovieCarry, movie_item.h).
void SetLooseItems(bool on);
// fp_items_seam (default on, 0.32.0; item_seam.h): a held object's drawn place is answered only while the frame is
// drawn (DISPLAY_WINDOW::Display); scripts, physics and the next clip read the game's own place. off = every caller
// gets the drawn place (0.17.0 .. 0.31.0).
void SetItemSeam(bool on);
// stereo.cpp HookWindow, around the game's DISPLAY_WINDOW::Display (the thread that updates and draws).
void DrawBegin();
void DrawEnd();
// fp_cloth (default on, 0.21.0): the player's simulated cloth (Havok Cloth: Shelby's and Jayden's coats) reads the
// drawn pose instead of the animated one while first person draws him; fp_cloth_reset (default on): the cloth starts
// again from its skinned pose when that begins or ends or the drawn body jumps (the engine's own "toSim1" reset);
// fp_cloth_hair (default on): with fp_hidehead=collapse, cloth on the head (Madison's hair) is simulated 30 m below.
void SetCloth(bool on, bool resetOnSwitch, bool hairAway);
// Writes the learned rig and pose samples of the player's skeleton to hrvr_logs\skeleton_*.csv
// (tools/plot_skeleton.py draws the body and the fingers from it).
void RequestDump();
// 0.36.6: the elevation (radians above the horizontal) of bone `from` -> `to` in the rig's bind pose (the rest shape read
// from the skin data, object space, Y up). False until the rest shape of that skeleton has been read.
bool RestElevation(void* skeleton, int from, int to, float* elevation);
// One status line (fpstatus).
void LogStatus();

}  // namespace hrvr::fpbody
