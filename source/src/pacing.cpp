#include "pacing.h"

#include <cstdint>

#include "game.h"
#include "log.h"

namespace hrvr::pacing {
namespace {
bool* g_vsync = nullptr;
bool g_saved = false, g_savedValue = false;
}  // namespace

void Install() {
    g_vsync = game::Global<bool>(game::g::VSync);
    Log("PACING vsync global %s; under VR the headset paces the game", g_vsync ? "found" : "NOT found (game pacing kept)");
}

unsigned OnPresent(unsigned gameSync, bool xrRunning) {
    if (!g_vsync) return xrRunning ? 0u : gameSync;
    if (xrRunning) {
        if (!g_saved) {
            g_saved = true;
            g_savedValue = *g_vsync;
            Log("PACING VR: game VSync/limiter off (game value %d kept for later)", int(g_savedValue));
        } else if (*g_vsync) {
            g_savedValue = true;  // the options re-applied VSync while VR ran
        }
        *g_vsync = false;
        return 0u;
    }
    if (g_saved) {
        *g_vsync = g_savedValue;
        g_saved = false;
        Log("PACING VR stopped: game VSync restored (%d)", int(g_savedValue));
    }
    return gameSync;
}

}  // namespace hrvr::pacing
