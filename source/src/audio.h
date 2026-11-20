#pragma once
// Head-tracked 3D sound (playbook chapter 17 / REQ-16), STATIC 2026-10-27.
//
// Heavy Rain mixes its 3D sounds itself on XAudio2 (KSOUND, no X3DAudio): every positional sound buffer
// picks the nearest of up to ten listeners (table 0x141a1f550, kernelsound_140bac9c0) and is spatialised
// against it (140bbaab0 with listener+0x80). A viewport's per-frame task (viewport_1404ddb10) feeds its
// camera to its listener: SetPosition(listener, camera node +0xC0) then SetOrientation(listener, front =
// world(0,0,-1), up = world(0,1,0)); the game eases both itself (9 % per call, slerp).
//
// With the headset on, both calls get the head instead of the game camera: in third person the head's offset
// from the game camera (what the eyes show), in first person the head where fpcam places it. Only the main
// camera's listener (a single full-screen view) and only while the eye passes publish a fresh head; split
// screens, flat screens and VR off keep the game's own listener.
#include "audio_math.h"

namespace hrvr::audio {

bool Install();
// Render thread, once per stereo pair (stereo.cpp): the head of this pair, or valid=false.
void Publish(const pose::Mat4& head, const pose::Mat4& authored, bool absolute, bool valid);
bool Enabled();
void SetEnabled(bool on);  // saved to hrvr.ini (audio_head)
// Command file verb: audiohead on|off|status.
bool Command(const char* verb, const char* arg);

}  // namespace hrvr::audio
