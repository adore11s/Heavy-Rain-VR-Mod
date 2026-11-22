#pragma once
// Prompts in first person (B2S guide section 7, Heavy Rain adaptation).
//
// 7B thoughts and dialogue choices (STATIC 2026-10-27): THOUGHTS_GUI's per-frame task (1404bcea0) builds an
// anchor = the character's bone (GuiBoneName, default "Neck") + local/global offsets and hands it to its
// animator: THOUGHTS_ANIMATOR::SetAnchor(gui+0x180, matrix, dt) stores it at animator+0x180; every word is
// then placed on a horizontal ring around that anchor (1404ff340 / 140501250). Dialogue choices use the
// same GUI (MOVIE_DIALOG_GROUP_CONDITION "ThoughtsDisplayData"). In first person an anchor at the player's
// head gets a moved COPY: world-locked in front of the view, the game's rotation kept (B2S 7B). The GUI's
// own fields are never written.
// 7E the right stick: Heavy Rain's action prompts (MPAR user actions) are right-stick gestures. MPAR_GUI
// lists the user actions on offer (+0x18/+0x20 and +0x30/+0x38, mpargui_1404df570 adds, 1404e0270 clears);
// while either list is non-empty the right stick belongs to the game (no view turning in F1).
#include <cstdint>

#include "posemath.h"

namespace hrvr::prompts {

bool Install();
void SetEnabled(bool on);
// First person: every action prompt in front of the view (true, fp_prompts_front=all) or only the player's own.
void SetAllFront(bool on);
bool Enabled();
// fpcam, every first-person frame (render thread): the view, the player's head bone and the game's own camera place
// (nullptr when unknown; 1.2: the tutorial / help texts keep the size they had on the game's screen).
void PublishView(bool firstPerson, const pose::Vec3& eye, const pose::Vec3& gaze, const pose::Vec3& head,
                 const pose::Vec3* gameCamera = nullptr);
// The MPAR_GUI singleton found by the player scan (nullptr when unknown).
void SetMparGui(void* gui);
// Action prompts are on offer now (right stick = the game's).
bool Showing();
// GetTickCount64 of the last frame with prompts on offer (0: never).
uint64_t LastShownMs();
// GetTickCount64 of the last frame the game drew any prompt record (movie QTEs too - hold / mash / loop - which the
// MPAR_GUI lists may not count); 0: never.
uint64_t LastElementMs();

}  // namespace hrvr::prompts
