// First-start welcome windows (welcome.h; playbook 11.4, GoW vr/panel.cpp WelcomeFrame D-318 / D-331, MIT).
//
// STATIC (0.23.0, HeavyRain.exe 2020-05-12) - how the game reads the keys and buttons the windows take:
//   keyboard  USER32 GetKeyState only, through the 147-entry virtual-key table 0x1413c9060: kernelinput_140b4a8d0
//             (one key: GetKeyState(table[i]) & 0x80, while the focus byte 0x1418d96f7 is set), kernelinput_140b4a770
//             (any key), PC_CONTROLLER_KEYBORAD_MOUSE vfunc 0x140b4bab0 (the table's states into 0x141a33b90),
//             kernelinput_140b43e00 (a second table 0x141196528); input_14038e830 calls GetKeyboardState (text entry).
//             Its DirectInput8 device (+0x40 of the keyboard / mouse controller, vfunc 0x140b4c9a0) is the MOUSE
//             (GetDeviceState 0x10 bytes = DIMOUSESTATE). The window procedure 0x1407f0b70 acts only on WM_SYSKEYDOWN
//             Alt+Enter (no WM_KEYDOWN / WM_CHAR game input). So Enter is held back at the game's two imports.
//             (The game's own "Enter still held" latch 0x1418d96ff, set after its exit box, blocks every key until
//             Enter is up; not used here - it would also block keys pressed with Enter.)
//   pads      ISteamController v007 only (vrinput.cpp: no XInput import); every pad - Xbox, Steam Input's PlayStation /
//             Switch pads - is a Steam pad and its confirm button is the digital action "a_button" (table index 2).
//             vrinput's GetDigitalActionData hook reports it (NotePadConfirm) and clears it while TakesPadConfirm().
#include "welcome.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <string>

#include "log.h"
#include "panel.h"
#include "panel_math.h"
#include "player.h"
#include "welcome_draw.h"

namespace hrvr::welcome {
namespace {

constexpr int kWelcomeVersion = 1;  // hrvr.ini welcome_seen below this = shown; raise for a new set of windows
constexpr float kWelW = 1.3f, kWelH = kWelW * float(kWH) / float(kWW);

int g_page = 0, g_drawnPage = -1;
bool g_drawnHover = false, g_hover = false;
std::atomic<bool> g_active{false};
bool g_done = false, g_checked = false;
panel::WelcomeFollow g_follow;
double g_stereoSince = 0.0, g_lastT = 0.0;
bool g_altArmed = false;  // a key / pad press counts only once released since the window showed
panel::TriggerClick g_click[2];
std::atomic<bool> g_padHeld{false};
std::atomic<bool> g_enterHeldAfter{false}, g_padHeldAfter{false};

std::wstring IniPath() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring s(exe);
    return s.substr(0, s.find_last_of(L"\\/")) + L"\\hrvr.ini";
}
float IniFloat(const wchar_t* key, float def) {
    wchar_t v[32];
    GetPrivateProfileStringW(L"hrvr", key, L"", v, 32, IniPath().c_str());
    return v[0] ? float(_wtof(v)) : def;
}

double NowS() {
    static LARGE_INTEGER f{};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return double(t.QuadPart) / double(f.QuadPart);
}

// The game's window (or another window of this process) is in front: keys typed elsewhere never count.
bool GameInFront() {
    DWORD pid = 0;
    const HWND f = GetForegroundWindow();
    return f && GetWindowThreadProcessId(f, &pid) && pid == GetCurrentProcessId();
}

// The next window (CONTINUE) or the end (START). False = all windows seen.
bool Next(const char* how) {
    if (++g_page >= kWelPages) {
        wchar_t v[16];
        swprintf_s(v, L"%d", kWelcomeVersion);
        WritePrivateProfileStringW(L"hrvr", L"welcome_seen", v, IniPath().c_str());
        g_done = true;
        g_active = false;
        Log("WELCOME START (%s) - all windows seen, hrvr.ini welcome_seen %d", how, kWelcomeVersion);
        return false;
    }
    Log("WELCOME CONTINUE (%s) - window %d of %d", how, g_page + 1, kWelPages);
    return true;
}

// ---- Enter held back from the game (its GetKeyState / GetKeyboardState imports) ----
using GetKeyStateFn = SHORT(WINAPI*)(int);
using GetKeyboardStateFn = BOOL(WINAPI*)(PBYTE);
GetKeyStateFn g_realGetKeyState = nullptr;
GetKeyboardStateFn g_realGetKeyboardState = nullptr;

SHORT WINAPI HookGetKeyState(int vk) {
    const SHORT s = g_realGetKeyState(vk);
    return vk == VK_RETURN && TakesEnter() ? SHORT(s & 1) : s;  // "up" (the toggle bit kept)
}
BOOL WINAPI HookGetKeyboardState(PBYTE keys) {
    const BOOL ok = g_realGetKeyboardState(keys);
    if (ok && keys && TakesEnter()) keys[VK_RETURN] &= 1;
    return ok;
}

// Points the game module's import `name` from USER32 at `hook`; returns the real function (nullptr: not imported).
void* PatchUser32Import(const char* name, void* hook) {
    auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    void* real = nullptr;
    __try {
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
        const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (!dir.VirtualAddress) return nullptr;
        for (auto* d = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); d->Name; ++d) {
            if (_stricmp(reinterpret_cast<const char*>(base + d->Name), "USER32.dll") != 0 || !d->OriginalFirstThunk) continue;
            auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + d->OriginalFirstThunk);
            auto* iat = reinterpret_cast<IMAGE_THUNK_DATA*>(base + d->FirstThunk);
            for (; names->u1.AddressOfData; ++names, ++iat) {
                if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
                const auto* ibn = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
                if (std::strcmp(reinterpret_cast<const char*>(ibn->Name), name) != 0) continue;
                DWORD old = 0;
                if (VirtualProtect(&iat->u1.Function, sizeof(void*), PAGE_READWRITE, &old)) {
                    real = reinterpret_cast<void*>(iat->u1.Function);
                    iat->u1.Function = reinterpret_cast<ULONGLONG>(hook);
                    VirtualProtect(&iat->u1.Function, sizeof(void*), old, &old);
                }
                return real;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return real;
}

}  // namespace

void Install() {
    if (IniFloat(L"welcome_seen", 0.f) >= float(kWelcomeVersion)) {
        g_done = g_checked = true;
        Log("WELCOME already seen (hrvr.ini welcome_seen %.0f) - not shown", IniFloat(L"welcome_seen", 0.f));
        return;  // nothing to take from the game, ever
    }
    g_realGetKeyState = reinterpret_cast<GetKeyStateFn>(PatchUser32Import("GetKeyState", reinterpret_cast<void*>(&HookGetKeyState)));
    g_realGetKeyboardState =
        reinterpret_cast<GetKeyboardStateFn>(PatchUser32Import("GetKeyboardState", reinterpret_cast<void*>(&HookGetKeyboardState)));
    // An import not found keeps the real function (the hooks are only reached through a patched import).
    Log("WELCOME Enter held back from the game while a window takes it: GetKeyState %s, GetKeyboardState %s",
        g_realGetKeyState ? "hooked" : "NOT FOUND", g_realGetKeyboardState ? "hooked" : "NOT FOUND");
}

bool Active() { return g_active.load(std::memory_order_relaxed); }

bool TakesEnter() {
    if (g_active.load(std::memory_order_relaxed)) {
        g_enterHeldAfter = true;
        return true;
    }
    if (g_enterHeldAfter.load(std::memory_order_relaxed)) {
        if (GetAsyncKeyState(VK_RETURN) & 0x8000) return true;
        g_enterHeldAfter = false;
    }
    return false;
}

bool TakesPadConfirm() {
    if (g_active.load(std::memory_order_relaxed)) {
        g_padHeldAfter = true;
        return true;
    }
    if (g_padHeldAfter.load(std::memory_order_relaxed)) {
        if (g_padHeld.load(std::memory_order_relaxed)) return true;
        g_padHeldAfter = false;
    }
    return false;
}

void NotePadConfirm(bool held) { g_padHeld.store(held, std::memory_order_relaxed); }

void FlatFrame() {
    g_stereoSince = 0.0;  // the 3 s count starts again at the next 3D frames
    if (g_active.exchange(false)) Log("WELCOME a 2D screen - window %d hidden until the 3D view is back", g_page + 1);
    g_follow.placed = false;
}

bool Frame(const xr::FrameInput& in, bool paused, Layer* out) {
    const double now = NowS();
    const float dt = g_lastT > 0.0 ? float(std::fmin(now - g_lastT, 0.1)) : 0.f;
    g_lastT = now;
    if (g_done) return false;
    if (!g_checked) {  // (Install read it; a later hrvr.ini edit is read once more here)
        g_checked = true;
        if (IniFloat(L"welcome_seen", 0.f) >= float(kWelcomeVersion)) {
            g_done = true;
            return false;
        }
    }
    if (paused) {
        FlatFrame();
        return false;
    }
    if (!g_active.load()) {
        // ~3 s into the 3D view, not in a cutscene (no active walking gameplay and the "player" alias set: QTE scenes
        // and cinematics); the main menu and walking count
        if (player::Poll().control == player::Control::Cutscene || !in.focused) {
            g_stereoSince = 0.0;
            return false;
        }
        if (g_stereoSince == 0.0) g_stereoSince = now;
        static const double delay = std::fmax(IniFloat(L"welcome_delay_s", 3.f), 0.f);
        if (now - g_stereoSince < delay) return false;
        if (!MakeD2D()) {
            g_done = true;
            return false;
        }
        g_active = true;
        g_drawnPage = -1;
        g_altArmed = false;  // a key / pad button already held (in play) does not count
        g_click[0] = g_click[1] = panel::TriggerClick{};
        g_click[0].armed = g_click[1].armed = false;  // a trigger held in play does not click
        g_follow.placed = false;
        Log("WELCOME window %d of %d shown", g_page + 1, kWelPages);
    }
    // follows the view slowly (panel_math.h WelcomeFollow)
    const pose::Vec3 head{in.headRel.m[12], in.headRel.m[13], in.headRel.m[14]};
    const float fx = -in.headRel.m[8], fz = -in.headRel.m[10];
    const float headYaw = std::hypot(fx, fz) > 0.2f ? std::atan2(-fx, -fz) : g_follow.yaw;
    const panel::Placement pl = g_follow.Step(head, headYaw, dt);
    bool hover = false, cursor = false;
    int pointingHand = 1;
    pose::Vec3 hitP{};
    for (int hand = 1; hand >= 0; --hand) {  // the right hand first
        const xr::HandInput& hs = in.hand[hand];
        bool hit = false;
        if (hs.active && hs.aimValid) {
            pose::Vec3 o{hs.aimRel.m[12], hs.aimRel.m[13], hs.aimRel.m[14]};
            pose::Vec3 d{-hs.aimRel.m[8], -hs.aimRel.m[9], -hs.aimRel.m[10]};  // aim -Z
            panel::SmoothAim(hand, o, d);
            float px = 0.f, py = 0.f;
            pose::Vec3 q{};
            if (panel::HitQuad(pl, kWelW, kWelH, kWW, kWH, o, d, &px, &py, &q)) {
                hit = true;
                if (!cursor) {
                    cursor = true;
                    pointingHand = hand;
                    hitP = q;
                    hover = px >= float(g_wBtn.left) && px < float(g_wBtn.right) && py >= float(g_wBtn.top) &&
                            py < float(g_wBtn.bottom);
                    if (g_click[hand].Feed(hs.trigger) && hover) {
                        xr::HapticPulse(hand, 0.5f, 50);  // 11.4: a stronger tick on the click (GoW 0.5, 50 ms)
                        if (!Next("trigger")) return false;
                    }
                }
            }
        }
        if (!hit) g_click[hand].Feed(hs.active ? hs.trigger : 0.f);  // re-arm only; a pull begun elsewhere never clicks
    }
    // Without VR controllers (playbook 11.4, GoW D-331): Enter with the game in front, or a pad's A / Cross; a press
    // counts once released since the window showed.
    {
        const bool key = (GetAsyncKeyState(VK_RETURN) & 0x8000) != 0 && GameInFront();
        const bool pad = g_padHeld.load(std::memory_order_relaxed);
        if (!key && !pad) {
            g_altArmed = true;
        } else if (g_altArmed) {
            g_altArmed = false;
            if (!Next(key ? "keyboard" : "gamepad")) return false;
        }
    }
    if (hover && !g_hover) xr::HapticPulse(pointingHand, 0.15f, 20);  // 11.4: a light tick entering the button (GoW)
    g_hover = hover;
    out->dirty = false;
    if (g_page != g_drawnPage || hover != g_drawnHover) {
        WelcomeDraw(g_page, hover);
        g_drawnPage = g_page;
        g_drawnHover = hover;
        out->dirty = true;
    }
    out->center = pl.center;
    out->yaw = pl.yaw;
    out->widthM = kWelW;
    out->heightM = kWelH;
    out->pixels = g_wOut;
    out->cursor = cursor;
    if (cursor) out->cursorAt = {hitP.x + std::sin(pl.yaw) * 0.01f, hitP.y, hitP.z + std::cos(pl.yaw) * 0.01f};
    return true;
}

}  // namespace hrvr::welcome
