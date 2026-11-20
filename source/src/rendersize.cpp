#include "rendersize.h"

#include <windows.h>
#include <dxgi.h>

#include <MinHook.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "game.h"
#include "log.h"
#include "xr.h"

namespace hrvr::rendersize {
namespace {
using namespace game;

using ComputeBackbufferFn = void (*)(uint64_t windowed);  // the game passes a bool in cl
using ComputeRenderFn = void (*)();
using ResizeFn = void (*)(void* swapChain);
ComputeBackbufferFn g_origBackbuffer = nullptr;
ComputeRenderFn g_origRender = nullptr;
ResizeFn g_origResize = nullptr;
bool g_installed = false;

int g_setting = kAuto;
std::atomic<uint32_t> g_target{0};  // N x N while the square size is in use (the hooks read it)
uint32_t g_applied = 0;
uint32_t g_lastWritten = 0;           // the N last written into g_SwapWidth / g_SwapHeight
uint32_t g_windowW = 0, g_windowH = 0;  // the game's own swap size, given back when the square size ends
bool g_failed = false;
SwitchPolicy g_policy;

// ---- the desktop window while VR runs (playbook 08 / 08.4, GoW D-329; 0.25.0) ----
// STATIC (0.25.0): the game takes its swap size ONLY from WM_SIZE (crash_1407f0b70 case 5 -> 0x140a6eae0: g_SwapWidth /
// Height = the client size, a re-centring SetWindowPos without resize, SWAP_CHAIN::Resize 0x140a70560); its backbuffer
// size is the configured resolution (0x140a6ac30 uses the window only for its frame thickness); WM_DISPLAYCHANGE
// (0x7e -> 0x140a6fbf0 -> 0x140a708a0) calls IDXGISwapChain::ResizeTarget. It handles no WM_SIZING / WM_GETMINMAXINFO;
// WM_EXITSIZEMOVE only re-clips the cursor. So while the mod owns the window its WM_SIZE is answered by DefWindowProc
// and the game never sees the window's size: N x N in 3D, the player's own size on flat screens (HookResize).
// The window is the mod's for the whole VR session (08 revision 10: never resized or moved by the mod on loads, menus,
// videos, pause or an idle headset), HD by default (desktop_window = the client height, 1280 x 720), resizable by the
// player and always 16:9 (08.4); the player's height is kept in hrvr.ini (desktop_window_h / _w) for the next VR start.
// Every window change is made on the window's own thread, in our procedure in front of the game's, on a message the
// render thread posts (never a synchronous SetWindowPos / SetWindowLongPtr(GWL_STYLE) from the render thread).
int g_desktopWindow = 720;
bool g_desktopCrop = true, g_desktopFit = true;
UINT g_msg = 0;
HWND g_hwnd = nullptr;
WNDPROC g_gameWndProc = nullptr;
std::atomic<bool> g_wantOwn{false};  // render thread: the window should be the mod's (VR runs, not exclusive fullscreen)
std::atomic<bool> g_owned{false};    // window thread: it is (WM_SIZE kept from the game meanwhile)
std::atomic<bool> g_posted{false};
std::atomic<bool> g_sizing{false};   // the player drags an edge / the title bar
uint64_t g_lastPost = 0;
WINDOWPLACEMENT g_saved{sizeof(WINDOWPLACEMENT)};
LONG_PTR g_addedStyle = 0;
bool g_placed = false;
int g_userH = -1;  // the player's client height (hrvr.ini desktop_window_h; 0 = none)

std::wstring IniPath();

void FrameSize(HWND w, int* fw, int* fh) {
    RECT f{0, 0, 0, 0};
    AdjustWindowRectEx(&f, DWORD(GetWindowLongPtrW(w, GWL_STYLE)), FALSE, DWORD(GetWindowLongPtrW(w, GWL_EXSTYLE)));
    *fw = f.right - f.left;
    *fh = f.bottom - f.top;
}
// The largest 16:9 client height on the window's monitor (0: none), and the monitor.
int MaxClientH(HWND w, MONITORINFO* out = nullptr) {
    MONITORINFO mi{sizeof(mi)};
    if (!GetMonitorInfoW(MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST), &mi)) return 0;
    int fw = 0, fh = 0;
    FrameSize(w, &fw, &fh);
    const int mw = (mi.rcWork.right - mi.rcWork.left) - fw, mh = (mi.rcWork.bottom - mi.rcWork.top) - fh;
    if (mw < W169(kMinClientPx) || mh < kMinClientPx) return 0;
    if (out) *out = mi;
    return W169(mh) > mw ? H169(mw) : mh;
}

// Window thread: the window becomes (or stays) the mod's HD, resizable 16:9 window.
void OwnNow(HWND w) {
    if (!g_owned.load()) {
        WINDOWPLACEMENT p{sizeof(p)};
        if (!GetWindowPlacement(w, &p)) return;
        g_saved = p;
        g_placed = false;
        g_owned = true;  // before any change: the game must not see the WM_SIZE that follows
        if (IsZoomed(w)) ShowWindow(w, SW_RESTORE);
    }
    const LONG_PTR st = GetWindowLongPtrW(w, GWL_STYLE);
    const LONG_PTR sizing = WS_THICKFRAME | WS_MAXIMIZEBOX;
    if ((st & WS_CAPTION) == WS_CAPTION && (st & sizing) != sizing) {  // (a borderless window stays as it is)
        g_addedStyle |= sizing & ~st;
        SetWindowLongPtrW(w, GWL_STYLE, st | sizing);
        SetWindowPos(w, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
    if (g_sizing.load() || IsZoomed(w) || IsIconic(w)) return;  // the player's own doing: left alone
    if (g_userH < 0) {
        const int h = int(GetPrivateProfileIntW(L"hrvr", L"desktop_window_h", 0, IniPath().c_str()));
        g_userH = h >= kMinClientPx && h <= 16384 ? h : 0;
    }
    int tw = 0, th = 0;
    TargetClient(g_userH, g_desktopWindow, MaxClientH(w), tw, th);
    RECT c{};
    GetClientRect(w, &c);
    const int cw = c.right - c.left, ch = c.bottom - c.top;
    if (g_placed && std::abs(ch - th) <= 1 && std::abs(cw - W169(ch)) <= 1) return;  // (WM_SIZING's rounding)
    if (g_placed) {  // later (a snap, or the game set its window): 16:9 at the height it has now, where it is
        th = ch >= kMinClientPx ? ch : th;
        tw = W169(th);
        if (std::abs(cw - tw) <= 1) return;
    }
    RECT r{0, 0, tw, th};
    AdjustWindowRectEx(&r, DWORD(GetWindowLongPtrW(w, GWL_STYLE)), FALSE, DWORD(GetWindowLongPtrW(w, GWL_EXSTYLE)));
    const int ww = r.right - r.left, wh = r.bottom - r.top;
    if (!g_placed) {  // the first time this session: in the middle of its monitor
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowPos(w, nullptr, mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - ww) / 2,
                     mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - wh) / 2, ww, wh, SWP_NOZORDER | SWP_NOACTIVATE);
        g_placed = true;
    } else {
        SetWindowPos(w, nullptr, 0, 0, ww, wh, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    static int logs = 0;
    if (logs++ < 8)
        Log("RENDERSIZE desktop window %dx%d, 16:9, resizable (was %dx%d; the headset keeps its own picture)", tw, th, cw, ch);
}

// Window thread: the player's window back as before VR (position, size, maximized), the added border taken away.
void ReleaseNow(HWND w) {
    if (!g_owned.load()) return;
    if (g_addedStyle) {
        SetWindowLongPtrW(w, GWL_STYLE, GetWindowLongPtrW(w, GWL_STYLE) & ~g_addedStyle);
        g_addedStyle = 0;
        SetWindowPos(w, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
    WINDOWPLACEMENT p = g_saved;
    p.length = sizeof(p);
    p.flags = 0;
    p.showCmd = p.showCmd == SW_SHOWMAXIMIZED ? SW_SHOWMAXIMIZED : SW_SHOWNOACTIVATE;
    SetWindowPlacement(w, &p);  // (its WM_SIZE still kept from the game: its swap size is already the player's)
    g_sizing = false;
    g_owned = false;
    const RECT& r = p.rcNormalPosition;
    Log("RENDERSIZE desktop window back to %ldx%ld%s", r.right - r.left, r.bottom - r.top,
        p.showCmd == SW_SHOWMAXIMIZED ? " (maximized)" : "");
}

// Window thread: the player let go of the window's edge - that height is theirs (hrvr.ini, next VR start too).
void RememberUserSize(HWND w) {
    if (!g_owned.load() || IsZoomed(w) || IsIconic(w)) return;
    RECT c{};
    GetClientRect(w, &c);
    const int ch = c.bottom - c.top;
    if (ch < kMinClientPx || ch == g_userH) return;
    if (g_userH <= 0 && ch == g_desktopWindow) return;  // only moved
    g_userH = ch;
    const std::wstring ini = IniPath();
    WritePrivateProfileStringW(L"hrvr", L"desktop_window_h", std::to_wstring(ch).c_str(), ini.c_str());
    WritePrivateProfileStringW(L"hrvr", L"desktop_window_w", std::to_wstring(W169(ch)).c_str(), ini.c_str());
    Log("RENDERSIZE the player resized the desktop window to %dx%d (16:9; kept: hrvr.ini desktop_window_h / _w)",
        W169(ch), ch);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_msg && msg == g_msg) {
        g_posted = false;
        if (g_wantOwn.load()) OwnNow(hwnd);
        else ReleaseNow(hwnd);
        return 0;
    }
    switch (msg) {
        case WM_SIZE:
            if (g_owned.load(std::memory_order_relaxed)) return DefWindowProcW(hwnd, msg, wp, lp);
            break;
        case WM_ENTERSIZEMOVE:
            if (g_owned.load()) g_sizing = true;
            break;
        case WM_EXITSIZEMOVE:
            if (g_sizing.exchange(false)) RememberUserSize(hwnd);
            break;  // (the game re-clips its cursor)
        case WM_SIZING: {
            const LRESULT r = CallWindowProcW(g_gameWndProc, hwnd, msg, wp, lp);
            if (!g_owned.load() || !lp) return r;
            auto* rc = reinterpret_cast<RECT*>(lp);
            int fw = 0, fh = 0;
            FrameSize(hwnd, &fw, &fh);
            int l = rc->left, t = rc->top, rr = rc->right, b = rc->bottom;
            Sizing169(int(wp), fw, fh, MaxClientH(hwnd), l, t, rr, b);
            *rc = {l, t, rr, b};
            return TRUE;
        }
        case WM_GETMINMAXINFO: {
            const LRESULT r = CallWindowProcW(g_gameWndProc, hwnd, msg, wp, lp);
            if (g_owned.load() && lp) {
                auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
                int fw = 0, fh = 0;
                FrameSize(hwnd, &fw, &fh);
                mm->ptMinTrackSize.x = W169(kMinClientPx) + fw;
                mm->ptMinTrackSize.y = kMinClientPx + fh;
                // maximized = the largest 16:9 window on its monitor, centred in the work area; ptMaxPosition is
                // relative to the monitor's own corner
                MONITORINFO mi{sizeof(mi)};
                if (const int ch = MaxClientH(hwnd, &mi)) {
                    const int ww = W169(ch) + fw, wh = ch + fh;
                    mm->ptMaxSize.x = ww;
                    mm->ptMaxSize.y = wh;
                    mm->ptMaxPosition.x = mi.rcWork.left - mi.rcMonitor.left + (mi.rcWork.right - mi.rcWork.left - ww) / 2;
                    mm->ptMaxPosition.y = mi.rcWork.top - mi.rcMonitor.top + (mi.rcWork.bottom - mi.rcWork.top - wh) / 2;
                    if (mm->ptMaxTrackSize.x < ww) mm->ptMaxTrackSize.x = ww;
                    if (mm->ptMaxTrackSize.y < wh) mm->ptMaxTrackSize.y = wh;
                }
            }
            return r;
        }
        default:
            break;
    }
    return CallWindowProcW(g_gameWndProc, hwnd, msg, wp, lp);
}

// Render thread, every Present: whether the window should be the mod's; the window thread does the rest. While it is,
// a size check every second (a snapped window gets its 16:9 width; the game's own window changes are undone).
void DesktopTick(bool xrRunning, bool fullscreen) {
    if (g_desktopWindow <= 0) return;
    if (!g_hwnd) {
        auto* h = Global<HWND>(g::MainWnd);
        if (!h || !*h || !IsWindow(*h)) return;
        if (!g_msg) g_msg = RegisterWindowMessageW(L"hrvr_desktop_window");
        if (!g_msg) return;
        g_gameWndProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(*h, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&WndProc)));
        if (!g_gameWndProc) {
            g_desktopWindow = 0;
            Log("RENDERSIZE window procedure not set: the desktop window keeps the player's size");
            return;
        }
        g_hwnd = *h;
        Log("RENDERSIZE window procedure set (the game does not see the desktop window's size while VR runs)");
    }
    const bool want = xrRunning && !fullscreen;
    const uint64_t now = GetTickCount64();
    if (want == g_wantOwn.load() && !(want && now - g_lastPost >= 1000)) return;
    g_wantOwn = want;
    g_lastPost = now;
    if (!g_posted.exchange(true) && !PostMessageW(g_hwnd, g_msg, 0, 0)) g_posted = false;
}

std::wstring IniPath() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring s(exe);
    return s.substr(0, s.find_last_of(L"\\/")) + L"\\hrvr.ini";
}

void SetSquare(uint32_t n) {
    for (uint64_t va : {g::BackbufferWidth, g::BackbufferHeight, g::RenderWidth, g::RenderHeight})
        if (auto* p = Global<uint32_t>(va)) *p = n;
}

void HookBackbuffer(uint64_t windowed) {
    g_origBackbuffer(windowed);
    if (const uint32_t n = g_target.load()) {
        if (auto* w = Global<uint32_t>(g::BackbufferWidth)) *w = n;
        if (auto* h = Global<uint32_t>(g::BackbufferHeight)) *h = n;
    }
}

void HookRender() {
    g_origRender();
    if (const uint32_t n = g_target.load()) {
        if (auto* w = Global<uint32_t>(g::RenderWidth)) *w = n;
        if (auto* h = Global<uint32_t>(g::RenderHeight)) *h = n;
    }
}

// Every resize the game makes (window messages, a lost device at Present) goes through here, so the square
// size survives them and the window's own size is remembered for later.
void HookResize(void* swapChain) {
    auto* sw = Global<uint32_t>(g::SwapWidth);
    auto* sh = Global<uint32_t>(g::SwapHeight);
    const uint32_t n = g_target.load();
    if (sw && sh) {
        if (n) {
            // The game's own value (a window message wrote it), not one of ours: remember it.
            const bool ours = (*sw == g_lastWritten && *sh == g_lastWritten) || (*sw == n && *sh == n);
            if (!ours) {
                g_windowW = *sw;
                g_windowH = *sh;
            }
            *sw = *sh = n;
            g_lastWritten = n;
            SetSquare(n);  // a fullscreen path sets the back buffer from the configured size directly
        } else if (g_lastWritten && *sw == g_lastWritten && *sh == g_lastWritten && g_windowW && g_windowH) {
            *sw = g_windowW;
            *sh = g_windowH;
            g_lastWritten = 0;
        }
    }
    g_origResize(swapChain);
}

bool Fullscreen(void* scObj) {
    auto* sc = scObj ? Read<IDXGISwapChain*>(scObj, off::SwapChainDxgi) : nullptr;
    BOOL fs = FALSE;
    if (sc) sc->GetFullscreenState(&fs, nullptr);
    return fs != FALSE;
}

bool Apply(uint32_t n) {
    void* scObj = *Global<void*>(g::MainSwapChain);
    if (!scObj) return false;
    const bool fs = Fullscreen(scObj);
    g_target = n;
    // The game's own sequence (dx11pc_140a6b3d0 / 140a6af10): sizes, then the swap chain and every target.
    HookBackbuffer(fs ? 0 : 1);
    HookRender();
    HookResize(scObj);
    const uint32_t rw = *Global<uint32_t>(g::RenderWidth), rh = *Global<uint32_t>(g::RenderHeight);
    DXGI_SWAP_CHAIN_DESC d{};
    if (auto* sc = Read<IDXGISwapChain*>(scObj, off::SwapChainDxgi)) sc->GetDesc(&d);
    const bool ok = !n || (rw == n && rh == n && d.BufferDesc.Width == n && d.BufferDesc.Height == n);
    Log("RENDERSIZE %s: render %ux%u, swap chain %ux%u%s", n ? "square VR picture" : "the game's own size", rw, rh,
        d.BufferDesc.Width, d.BufferDesc.Height, ok ? "" : " (NOT the size asked for: VR picture size off)");
    if (!ok) {
        g_failed = true;
        g_target = 0;
        HookBackbuffer(fs ? 0 : 1);
        HookRender();
        HookResize(scObj);
        return false;
    }
    return true;
}

uint32_t Target() {
    if (g_failed) return 0;
    if (g_setting == kAuto) return AutoSize();
    return ValidFixed(g_setting);
}

bool HookOne(Anchor a, void* detour, void** orig) {
    void* p = Addr(a);
    if (!p) return false;
    const MH_STATUS s = MH_CreateHook(p, detour, orig);
    const MH_STATUS e = s == MH_OK ? MH_EnableHook(p) : s;
    Log("RENDERSIZE hook %s %p %s", AnchorName(a), p, MH_StatusToString(e));
    return e == MH_OK;
}

}  // namespace

bool Install() {
    wchar_t v[32];
    GetPrivateProfileStringW(L"hrvr", L"render_size_auto", L"on", v, 32, IniPath().c_str());
    const bool autoSize = _wcsicmp(v, L"off") != 0;
    GetPrivateProfileStringW(L"hrvr", L"render_size", L"2048", v, 32, IniPath().c_str());
    const int fixed = _wcsicmp(v, L"game") == 0 ? kGame : _wtoi(v);
    g_desktopWindow = int(GetPrivateProfileIntW(L"hrvr", L"desktop_window", 720, IniPath().c_str()));
    if (g_desktopWindow != 0) g_desktopWindow = std::clamp(g_desktopWindow, 240, 4096);
    g_desktopCrop = GetPrivateProfileIntW(L"hrvr", L"desktop_crop", 1, IniPath().c_str()) != 0;
    g_desktopFit = GetPrivateProfileIntW(L"hrvr", L"desktop_fit", 1, IniPath().c_str()) != 0;
    g_setting = autoSize ? kAuto : (fixed == kGame ? kGame : (ValidFixed(fixed) ? int(ValidFixed(fixed)) : kGame));
    for (uint64_t va : {g::BackbufferWidth, g::BackbufferHeight, g::SwapWidth, g::SwapHeight, g::RenderWidth,
                        g::RenderHeight, g::MainSwapChain})
        if (!Va(va)) {
            Log("RENDERSIZE global 0x%llx missing: eyes keep the game's resolution", static_cast<unsigned long long>(va));
            return false;
        }
    if (!Addr(Anchor::ComputeBackbufferSize) || !Addr(Anchor::ComputeRenderSize) || !Addr(Anchor::SwapChainResize)) {
        Log("RENDERSIZE anchors missing: eyes keep the game's resolution");
        return false;
    }
    g_installed = HookOne(Anchor::ComputeBackbufferSize, reinterpret_cast<void*>(&HookBackbuffer),
                          reinterpret_cast<void**>(&g_origBackbuffer)) &&
                  HookOne(Anchor::ComputeRenderSize, reinterpret_cast<void*>(&HookRender),
                          reinterpret_cast<void**>(&g_origRender)) &&
                  HookOne(Anchor::SwapChainResize, reinterpret_cast<void*>(&HookResize),
                          reinterpret_cast<void**>(&g_origResize));
    Log("RENDERSIZE VR picture size %s, setting %s", g_installed ? "ready" : "NOT available",
        g_setting == kAuto ? "Auto" : g_setting == kGame ? "game resolution" : std::to_string(g_setting).c_str());
    return g_installed;
}

void Frame(bool xrRunning, bool scene3D, bool video) {
    if (!g_installed) return;
    {
        void* scObj = *Global<void*>(g::MainSwapChain);
        DesktopTick(xrRunning || xr::SessionExists(), scObj && Fullscreen(scObj));
    }
    const uint64_t now = GetTickCount64();
    const uint32_t want = g_policy.Want(xrRunning, scene3D, Target(), g_applied, now, video);
    if (want == g_applied) return;
    g_policy.lastChange = now;
    g_applied = Apply(want) ? want : 0;
}

int Setting() { return g_setting; }

void SetSetting(int v) {
    g_setting = v == kAuto || v == kGame ? v : (ValidFixed(v) ? int(ValidFixed(v)) : kGame);
    g_failed = false;
    g_policy.lastChange = 0;  // a choice in the panel applies at the next frame
    const std::wstring ini = IniPath();
    WritePrivateProfileStringW(L"hrvr", L"render_size_auto", g_setting == kAuto ? L"on" : L"off", ini.c_str());
    if (g_setting != kAuto)
        WritePrivateProfileStringW(L"hrvr", L"render_size",
                                   g_setting == kGame ? L"game" : std::to_wstring(g_setting).c_str(), ini.c_str());
    Log("RENDERSIZE setting -> %s", g_setting == kAuto ? "Auto" : g_setting == kGame ? "game resolution"
                                                                                    : std::to_string(g_setting).c_str());
}

uint32_t AutoSize() {
    uint32_t w = 0, h = 0;
    return xr::RecommendedEyeSize(&w, &h) ? AutoFrom(w, h) : 0;
}

uint32_t Applied() { return g_applied; }

bool DesktopPicture(int* clientW, int* clientH, bool* crop) {
    if (!g_desktopFit || !g_owned.load(std::memory_order_relaxed) || !g_hwnd || g_sizing.load(std::memory_order_relaxed))
        return false;
    RECT c{};
    if (!GetClientRect(g_hwnd, &c) || c.right - c.left < 16 || c.bottom - c.top < 16) return false;
    *clientW = c.right - c.left;
    *clientH = c.bottom - c.top;
    *crop = g_desktopCrop;
    return true;
}

bool Command(const char* verb, const char* arg) {
    if (std::strcmp(verb, "rendersize") != 0) return false;
    if (arg && std::strcmp(arg, "auto") == 0) SetSetting(kAuto);
    else if (arg && std::strcmp(arg, "game") == 0) SetSetting(kGame);
    else if (arg && std::atoi(arg) > 0) SetSetting(std::atoi(arg));
    Log("RENDERSIZE status: setting %d (-1 Auto, 0 game), Auto %u, in use %u, window %ux%u, failed %d", g_setting,
        AutoSize(), g_applied, g_windowW, g_windowH, int(g_failed));
    return true;
}

}  // namespace hrvr::rendersize
