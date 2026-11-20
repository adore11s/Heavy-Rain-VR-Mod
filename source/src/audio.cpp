#include "audio.h"

#include <windows.h>

#include <MinHook.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>

#include "game.h"
#include "log.h"

namespace hrvr::audio {
namespace {

// The listener follows a camera that moved at most this far since the pair was drawn (engine units; a cut or
// another viewport's camera is further).
constexpr float kSameCamera = 1.0f;

using SetPosFn = void (*)(void* listener, const float* pos);
using SetOriFn = void (*)(void* listener, const float* front, const float* up);
SetPosFn g_origPos = nullptr;
SetOriFn g_origOri = nullptr;

std::atomic<bool> g_enabled{true};
std::mutex g_pubMutex;
HeadPublish g_pub;

// Per listener: the game's last inputs (the position call comes first, with last frame's orientation).
struct Listener {
    void* self = nullptr;
    float pos[3] = {}, front[3] = {0, 0, -1}, up[3] = {0, 1, 0};
    bool havePos = false, haveOri = false;
};
Listener g_listeners[10];  // the engine's table has ten slots
std::mutex g_listenerMutex;
std::atomic<uint64_t> g_corrected{0}, g_kept{0};

std::wstring IniPath() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring s(exe);
    return s.substr(0, s.find_last_of(L"\\/")) + L"\\hrvr.ini";
}

Listener& Of(void* self) {
    for (Listener& l : g_listeners)
        if (l.self == self) return l;
    for (Listener& l : g_listeners)
        if (!l.self) {
            l.self = self;
            return l;
        }
    static Listener spare;
    spare = Listener{self};
    return spare;
}

bool Snapshot(HeadPublish* out) {
    if (!g_enabled.load(std::memory_order_relaxed)) return false;
    std::lock_guard lock(g_pubMutex);
    *out = g_pub;
    return out->valid;
}

void Report(bool applied) {
    (applied ? g_corrected : g_kept).fetch_add(1, std::memory_order_relaxed);
    static uint64_t next = 0, firstShown = 0;
    const uint64_t now = GetTickCount64();
    if (applied && !firstShown++) Log("AUDIO the sound listener follows the headset from now on");
    if (now >= next) {
        const uint64_t head = g_corrected.exchange(0), own = g_kept.exchange(0);
        if (next && head)  // silent while VR is off (only the game's own listener)
            Log("AUDIO listener updates: %llu from the head, %llu the game's own (last 30 s)",
                static_cast<unsigned long long>(head), static_cast<unsigned long long>(own));
        next = now + 30000;
    }
}

void HookPos(void* self, const float* pos) {
    Listener l;
    {
        std::lock_guard lock(g_listenerMutex);
        Listener& s = Of(self);
        std::memcpy(s.pos, pos, sizeof(s.pos));
        s.havePos = true;
        l = s;
    }
    HeadPublish p;
    Mat4 cam;
    if (!Snapshot(&p) || !l.haveOri || !Applies(p, pos, GetTickCount64(), kSameCamera) ||
        !FrameFromVectors(l.front, l.up, pos, &cam)) {
        Report(false);
        g_origPos(self, pos);
        return;
    }
    const Mat4 w = ListenerWorld(p, cam);
    alignas(16) float out[4] = {w.m[12], w.m[13], w.m[14], pos[3]};  // the engine reads it with movaps
    Report(true);
    g_origPos(self, out);
}

void HookOri(void* self, const float* front, const float* up) {
    Listener l;
    {
        std::lock_guard lock(g_listenerMutex);
        Listener& s = Of(self);
        std::memcpy(s.front, front, sizeof(s.front));
        std::memcpy(s.up, up, sizeof(s.up));
        s.haveOri = true;
        l = s;
    }
    HeadPublish p;
    Mat4 cam;
    if (!Snapshot(&p) || !l.havePos || !Applies(p, l.pos, GetTickCount64(), kSameCamera) ||
        !FrameFromVectors(front, up, l.pos, &cam)) {
        g_origOri(self, front, up);
        return;
    }
    const Mat4 w = ListenerWorld(p, cam);
    alignas(16) float f[4] = {0, 0, 0, front[3]}, u[4] = {0, 0, 0, up[3]};
    FrontUpOf(w, f, u);
    g_origOri(self, f, u);
}

bool HookOne(game::Anchor a, void* detour, void** orig) {
    void* p = game::Addr(a);
    if (!p) {
        Log("AUDIO %s not found: the game's own listener stays", game::AnchorName(a));
        return false;
    }
    const MH_STATUS s = MH_CreateHook(p, detour, orig);
    const MH_STATUS e = s == MH_OK ? MH_EnableHook(p) : s;
    Log("AUDIO hook %s %p %s", game::AnchorName(a), p, MH_StatusToString(e));
    return e == MH_OK;
}

}  // namespace

bool Install() {
    wchar_t v[16];
    GetPrivateProfileStringW(L"hrvr", L"audio_head", L"on", v, 16, IniPath().c_str());
    g_enabled = _wcsicmp(v, L"off") != 0;
    // Both or neither: a listener at the head facing the camera's way would be worse than either.
    if (!game::Addr(game::Anchor::ListenerSetPosition) || !game::Addr(game::Anchor::ListenerSetOrientation)) {
        Log("AUDIO listener anchors missing: the game's own listener stays");
        return false;
    }
    const bool ok = HookOne(game::Anchor::ListenerSetPosition, reinterpret_cast<void*>(&HookPos),
                            reinterpret_cast<void**>(&g_origPos)) &&
                    HookOne(game::Anchor::ListenerSetOrientation, reinterpret_cast<void*>(&HookOri),
                            reinterpret_cast<void**>(&g_origOri));
    Log("AUDIO head-tracked listener %s (audio_head=%s)", ok ? "installed" : "NOT installed", g_enabled ? "on" : "off");
    return ok;
}

void Publish(const pose::Mat4& head, const pose::Mat4& authored, bool absolute, bool valid) {
    std::lock_guard lock(g_pubMutex);
    g_pub.head = head;
    g_pub.authored = authored;
    g_pub.absolute = absolute;
    g_pub.valid = valid;
    g_pub.ms = GetTickCount64();
}

bool Enabled() { return g_enabled; }

void SetEnabled(bool on) {
    g_enabled = on;
    WritePrivateProfileStringW(L"hrvr", L"audio_head", on ? L"on" : L"off", IniPath().c_str());
    Log("AUDIO head-tracked listener %s", on ? "on" : "off");
}

bool Command(const char* verb, const char* arg) {
    if (std::strcmp(verb, "audiohead") != 0) return false;
    if (arg && std::strcmp(arg, "on") == 0) SetEnabled(true);
    else if (arg && std::strcmp(arg, "off") == 0) SetEnabled(false);
    HeadPublish p;
    {
        std::lock_guard lock(g_pubMutex);
        p = g_pub;
    }
    Log("AUDIO status: %s, head %s (%s, %llu ms old) at (%.2f %.2f %.2f), game camera (%.2f %.2f %.2f)",
        g_enabled ? "on" : "off", p.valid ? "published" : "none", p.absolute ? "first person" : "on the game camera",
        static_cast<unsigned long long>(p.ms ? GetTickCount64() - p.ms : 0), p.head.m[12], p.head.m[13], p.head.m[14],
        p.authored.m[12], p.authored.m[13], p.authored.m[14]);
    return true;
}

}  // namespace hrvr::audio
