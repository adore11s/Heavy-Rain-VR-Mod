#include "camdirector.h"

#include <windows.h>

#include <MinHook.h>

#include <atomic>

#include "engineobj.h"
#include "game.h"
#include "log.h"

namespace hrvr::camdirector {
namespace {

constexpr size_t kActiveBehaviour = 0x1F0;  // director: KCOM handle of the active behaviour

struct ClassDef {
    uint64_t vtable;
    const char* name;
    fp::BehaviourClass group;
};
using G = fp::BehaviourClass;
// Recorded in sigdb (Vtable entries). Heavy Rain decides walking vs cutscene by who controls the
// character (player.h), not by the camera: authored FIXED / SELECTOR cameras serve both.
constexpr ClassDef kClasses[] = {
    {0x140cf2558, "CHASE", G::Unknown},           {0x140cf8f48, "CIRCLE", G::Unknown},
    {0x140cf8410, "CONTAINER", G::Unknown},       {0x140d46d40, "CROWD_CHASE", G::Unknown},
    {0x140cf8990, "FIRST_PERSON", G::NativeFirstPerson},
    {0x140cf0dd0, "FIXED", G::Unknown},           {0x140cf1a90, "FREE", G::Unknown},
    {0x140cf7e70, "FREE_GAMEPLAY", G::Unknown},   {0x140cf9878, "FREE_ROAMING", G::Unknown},
    {0x140cfba08, "INSERTCAM_SELECTOR", G::Unknown}, {0x140cf75c0, "RAIL", G::Unknown},
    {0x140cfafe0, "SELECTOR", G::Unknown},        {0x140cf6c98, "SHOT", G::Unknown},
    {0x140cf3358, "STANDALONE_CHASE", G::Unknown}, {0x140cfcb78, "SWIM", G::Unknown},
    {0x140cfd558, "INTERACTIVE_CAMERA", G::Unknown},
};

using UpdateFn = void (*)(void* director);
UpdateFn g_origUpdate = nullptr;
std::atomic<void*> g_director{nullptr};

void HookUpdate(void* director) {
    g_director = director;
    g_origUpdate(director);
}

const ClassDef* Find(void* behaviour) {
    if (!behaviour) return nullptr;
    for (const ClassDef& c : kClasses)
        if (obj::HasVtable(behaviour, c.vtable)) return &c;
    return nullptr;
}

}  // namespace

bool Install() {
    void* p = game::Addr(game::Anchor::DirectorUpdate);
    if (!p) {
        Log("CAMDIR director update not found in this build: the game's own first person is not detected");
        return false;
    }
    const MH_STATUS s = MH_CreateHook(p, reinterpret_cast<void*>(&HookUpdate), reinterpret_cast<void**>(&g_origUpdate));
    const MH_STATUS e = s == MH_OK ? MH_EnableHook(p) : s;
    Log("CAMDIR hook CAMERA_DIRECTOR::UpdateChangeCamera %p %s", p, MH_StatusToString(e));
    return e == MH_OK;
}

void* Active() {
    void* dir = g_director.load();
    return dir ? obj::Cached(obj::Ptr(dir, kActiveBehaviour)) : nullptr;
}

fp::BehaviourClass Classify(void* behaviour) {
    const ClassDef* c = Find(behaviour);
    return c ? c->group : fp::BehaviourClass::Unknown;
}

const char* Name(void* behaviour) {
    const ClassDef* c = Find(behaviour);
    return c ? c->name : "?";
}

}  // namespace hrvr::camdirector
