#pragma once

// Frame pacing ownership. Heavy Rain's EndFrame presents with the sync interval when g_bVSync is
// set and then spins until 1000 / g_TargetFps ms have passed (also only when g_bVSync is set).
// While OpenXR runs, xrWaitFrame paces the game: VSync and the spin limiter are switched off, and
// the game's own value is restored when VR stops. Both globals are only read by the renderer; the
// options keep their own copy (what the game saves), so the player's setting is never changed.
namespace hrvr::pacing {

void Install();

// Render thread, once per Present: returns the sync interval the game's Present should use.
unsigned OnPresent(unsigned gameSync, bool xrRunning);

}  // namespace hrvr::pacing
