#pragma once
// First person for Heavy Rain (ported from the B2S VR mod's fpcam, MIT; HEAVY_RAIN_FIRST_PERSON_GUIDE.txt).
//
//   F1  first person while the player walks the character (THIRD_PERSON_GAMEPLAY active): the eyes at the
//       character's eyes, the left stick walks where you look, the right stick turns (smooth or snap).
//   F3  first person in cutscenes: the eyes ride on the character's head as the game animates it; the
//       view follows where the face points, the headset adds your own head movement on top.
//   Controllers (GoW): the right hand at your head + its upper face button (B / O) = F1 while walking, F3 in a
//   cutscene. All four face buttons = Start (the pause screen with the VR settings).
//
// Only the drawn eye passes move (stereo.cpp: node world = eyeRel * Base for the duration of an eye
// pass); game logic, prompts, AI and sound keep the game camera.
#include <cstddef>

#include "posemath.h"
#include "xr.h"

namespace hrvr::fpcam {

void Install();
// Game thread at Present: F1 / F3 keys.
void Frame();
// Render thread, once per stereo pair before the eye passes. `eligible`: exactly one full-screen view
// (split screens and picture-in-picture keep the game's cameras).
void Prepare(void* cameraNode, const pose::Mat4& authoredCamera, const xr::FrameInput& input, bool eligible);
// The first-person base for this camera node (node world = eyeRel * base); false = the game camera.
bool Base(void* cameraNode, pose::Mat4* out);
// Near clip distance for the first-person camera in metres; 0 = keep the game's.
float NearPlane();
// Command file verbs (fp, fpcutscenes, fpstatus, fpeye, fpeyeup, turn, fpprompts, fpplayer, fpautowalk, fpnear).
bool Command(const char* verb, const char* arg);

// Settings for the VR settings panel; every setter saves to hrvr.ini at once.
bool GameplayFP();
// 1.6: the gameplay first-person view (F1) was drawn within the last 250 ms (ari.cpp: the glasses gesture only there).
bool F1ViewNow();
// 1.7: the drawn hand's controller offsets (fp_wrist_pitch radians, fp_wrist_back metres) - the ARI glasses copy uses the
// same ones so it sits where the drawn hand holds it.
void WristOffsets(float* pitchRad, float* backM);
void SetGameplayFP(bool on);
bool CutsceneFP();
void SetCutsceneFP(bool on);
// 0.36.12: panel row "Hide head (first person)" - On = fp_hidehead's mode (clip by default), Off = head drawn; live.
bool HideHead();
void SetHideHead(bool on);
bool SnapTurn();
void SetSnapTurn(bool on);
float SnapDegrees();
void SetSnapDegrees(float degrees);
float TurnSpeed();  // smooth turning, fraction of 120 deg/s (0.2 .. 1)
void SetTurnSpeed(float fraction);
void DescribeForCrash(char* out, size_t cap);

}  // namespace hrvr::fpcam
