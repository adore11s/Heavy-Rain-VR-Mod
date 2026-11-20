#pragma once
// Active camera behaviour of the game's CAMERA_DIRECTOR (B2S guide 2.1). STATIC 2026-10-27: the director's
// per-frame task "CAMERA_DIRECTOR::UpdateChangeCamera" (0x1403d32b0, this = director) compares the
// behaviour handle at +0x1F0 with the previous one at +0xC0 and announces "GAMEENGINE.NewCameraBehavior".
// Each behaviour's class is identified by its RTTI vtable.
#include "fpmode.h"

namespace hrvr::camdirector {

// Hooks the director task to learn the director instance; false when the anchor is missing.
bool Install();
// Active behaviour object (cached handle), or nullptr when unknown.
void* Active();
// Behaviour -> class group for fp::Classify: only the game's own first person matters for overriding.
fp::BehaviourClass Classify(void* behaviour);
// Short class name for logs ("CHASE", "SHOT", ...), "?" when unknown.
const char* Name(void* behaviour);

}  // namespace hrvr::camdirector
