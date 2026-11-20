#pragma once
#include "xr.h"
#include "vrinput_math.h"

namespace hrvr::vrinput {
// Guarded native Steam controller seams add the VR controllers as one more pad; real pads pass through.
bool Install();
void Suspend();
void Publish(const xr::FrameInput& frame);
// fpcam, every first-person frame: how the left stick reaches the game (expires after 250 ms).
void SetFirstPersonMove(const FirstPersonMove& move);
LegacyState Snapshot();
// The right hand was at the head when its upper face button (B / O) was pressed (vrinput_math.h HeadSwitch, one tap
// at any time - playbook 10.3): true once per press, within 1 s. That press never reaches the game.
bool ConsumeHeadSwitch();
void LogStatus();
// Controller type the virtual pad reports: 2 = Xbox-style gamepad (default, F10),
// 5 = PlayStation (F11: PlayStation button prompts and the right controller as motion sensor).
void SetPadType(int type);
int PadType();
// Motion controls (VR settings panel): with PlayStation buttons the right controller is the pad's motion
// sensor; off = a still pad. Saved in hrvr.ini (motion_controls).
bool MotionControls();
void SetMotionControls(bool on);
}  // namespace hrvr::vrinput
