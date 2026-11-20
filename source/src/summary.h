#pragma once
// The log's summary block (playbook 28.1, GoW xr.cpp LogSummary): lines starting "summary:" once VR runs - the mod and
// game build, the PC, the OpenXR runtime and headset, eye sizes and field of view, eye distance and cant, controller
// profiles and aim-versus-grip angles, the game's frame / window / monitor, its own graphics settings, the flat screen,
// the chapter, the compatibility switches and every mod setting. Logged again when the swap chain size, the 3D / flat
// state or the controller profiles change, at most 6 blocks a session.
namespace hrvr::summary {

// The mod version for the block (dllmain, once).
void SetVersion(const char* version);
// Game thread at Present.
void Tick(bool vrRunning, bool stereo3D);

}  // namespace hrvr::summary
