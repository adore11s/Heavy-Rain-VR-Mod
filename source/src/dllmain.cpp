// dinput8.dll proxy: forwards DirectInput to the system DLL and starts HRVR (Heavy Rain VR).
// Derived from the Beyond: Two Souls VR mod (B2SVR), MIT.
#include <windows.h>
#include <bcrypt.h>
#include <unknwn.h>

#include <atomic>
#include <cstring>
#include <string>

#include "summary.h"
#include "welcome.h"
#include "crashreport.h"
#include "game.h"
#include "log.h"
#include "stereo.h"
#include "vrinput.h"

#include <MinHook.h>

#define HRVR_VERSION "1.1"

namespace {

HMODULE g_self = nullptr;
HMODULE g_realDinput = nullptr;

HMODULE RealDinput() {
    if (!g_realDinput) {
        wchar_t path[MAX_PATH];
        GetSystemDirectoryW(path, MAX_PATH);
        wcscat_s(path, L"\\dinput8.dll");
        g_realDinput = LoadLibraryW(path);
    }
    return g_realDinput;
}

template <typename Fn>
Fn RealProc(const char* name) {
    HMODULE m = RealDinput();
    return m ? reinterpret_cast<Fn>(GetProcAddress(m, name)) : nullptr;
}

std::wstring DirOf(const std::wstring& path) { return path.substr(0, path.find_last_of(L"\\/")); }

std::string Narrow(const std::wstring& w) {
    std::string s(WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), int(s.size()), nullptr, nullptr);
    s.pop_back();
    return s;
}

// Hashes the mapped .text section. File reads of the EXE are not trustworthy: Steam
// emulators redirect them, and SteamStub builds decrypt .text only at startup.
std::string Sha256Text() {
    auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (std::memcmp(sec->Name, ".text", 6) != 0) continue;
        BCRYPT_ALG_HANDLE alg = nullptr;
        BCRYPT_HASH_HANDLE hash = nullptr;
        BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
        BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0);
        BCryptHashData(hash, base + sec->VirtualAddress, sec->Misc.VirtualSize, 0);
        UCHAR digest[32];
        BCryptFinishHash(hash, digest, sizeof(digest), 0);
        BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(alg, 0);
        std::string hex;
        char tmp[3];
        for (UCHAR b : digest) {
            sprintf_s(tmp, "%02x", b);
            hex += tmp;
        }
        return hex;
    }
    return "no-.text";
}

// SHA-256 of a whole file (the exe as stored, the settings file), "unreadable" when it cannot be read.
std::string Sha256File(const std::wstring& path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return "unreadable";
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0);
    static UCHAR buf[1 << 16];
    DWORD got = 0;
    while (ReadFile(f, buf, sizeof(buf), &got, nullptr) && got) BCryptHashData(hash, buf, got, 0);
    CloseHandle(f);
    UCHAR digest[32];
    BCryptFinishHash(hash, digest, sizeof(digest), 0);
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    std::string hex;
    char tmp[3];
    for (UCHAR b : digest) {
        sprintf_s(tmp, "%02x", b);
        hex += tmp;
    }
    return hex;
}

// ---- 0.36.15: the game starts in a window (owner: "the installer doesnt make the game window windowed, so fix that") ----
// The VR mod needs the game in a window (the headset picture size and the small desktop window resize it). The installer
// sets user_setting.ini [GRAPHIC_SECTION] ScreenMode=2, but only when that file already exists - a fresh copy has none
// until the game's first run, and then STATIC 0x140a71630 makes the mode 1 (the borderless screen-sized window).
// Two parts, both off with hrvr.ini force_windowed=off:
//  1. DllMain (before any game code runs): an existing user_setting.ini with another ScreenMode gets ScreenMode=2.
//  2. RENDER::LoadDisplaySettings 0x140a70cc0 (renderer start-up; it reads that file, or the defaults without one) is
//     hooked; after it g_ConfigScreenMode 0x141a2e108 = 2, which its caller 0x140a716e0 copies into the current mode
//     before the game window is created. The player can still pick another mode in the game's Options afterwards.
int g_screenModeBefore = -2;  // -2 not looked at, -1 no user_setting.ini, else the file's value before part 1
bool ForceWindowedWanted(const std::wstring& gameDir) {
    wchar_t v[16]{};
    GetPrivateProfileStringW(L"hrvr", L"force_windowed", L"on", v, 16, (gameDir + L"\\hrvr.ini").c_str());
    return _wcsicmp(v, L"off") != 0 && _wcsicmp(v, L"0") != 0 && _wcsicmp(v, L"false") != 0;
}
void ForceWindowedFile() {
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    const std::wstring dir = DirOf(exePath);
    if (!ForceWindowedWanted(dir)) return;
    const std::wstring us = dir + L"\\user_setting.ini";
    if (GetFileAttributesW(us.c_str()) == INVALID_FILE_ATTRIBUTES) {
        g_screenModeBefore = -1;
        return;
    }
    g_screenModeBefore = int(GetPrivateProfileIntW(L"GRAPHIC_SECTION", L"ScreenMode", -1, us.c_str()));
    if (g_screenModeBefore != 2) WritePrivateProfileStringW(L"GRAPHIC_SECTION", L"ScreenMode", L"2", us.c_str());
}
using LoadDisplayFn = uint64_t (*)();
LoadDisplayFn g_origLoadDisplay = nullptr;
std::atomic<bool> g_forceWindowed{true};
uint64_t HookLoadDisplay() {
    const uint64_t r = g_origLoadDisplay();
    if (int* mode = hrvr::game::Global<int>(hrvr::game::g::ConfigScreenMode); mode && g_forceWindowed.load()) {
        const int was = *mode;
        *mode = 2;
        hrvr::Log("WINDOW display mode at start: %d -> 2 (a window; force_windowed)", was);
    }
    return r;
}

DWORD WINAPI InitThread(LPVOID) {
    wchar_t exePath[MAX_PATH], dllPath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    GetModuleFileNameW(g_self, dllPath, MAX_PATH);
    const std::wstring gameDir = DirOf(exePath);
    hrvr::LogOpen(gameDir.c_str());

    // RE-03: the first line alone must prove which bytes ran in which process with which settings.
    const std::wstring ini = gameDir + L"\\hrvr.ini";
    // 28.3: no settings file yet (a fresh install, or the player removed it): the built-in default (hrvr_default.ini).
    bool wroteDefault = false;
    if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES) {
        static const char kDefaultIni[] =
#include "default_ini.inc"
            ;
        const HANDLE f = CreateFileW(ini.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            DWORD n = 0;
            wroteDefault = WriteFile(f, kDefaultIni, DWORD(sizeof(kDefaultIni) - 1), &n, nullptr) && n == sizeof(kDefaultIni) - 1;
            CloseHandle(f);
        }
    }
    wchar_t stage[32]{};
    GetPrivateProfileStringW(L"hrvr", L"stage", L"vr", stage, 32, ini.c_str());
    // 28.1: "<mod> <version> linked <link time> | dll=<path> | exe=<path> | pid=<n> | arch=x64" (the installer knows the
    // mod's DLL by "hrvr " + " linked "), then the stage and the hashes.
    hrvr::Log("hrvr " HRVR_VERSION " linked " __DATE__ " " __TIME__ " | dll=%s | exe=%s | pid=%lu | arch=x64 | stage=%s | "
              "dll.sha256=%s | exe.sha256=%s | config=%s config.sha256=%s",
              Narrow(dllPath).c_str(), Narrow(exePath).c_str(), GetCurrentProcessId(), Narrow(stage).c_str(),
              Sha256File(dllPath).c_str(), Sha256File(exePath).c_str(), Narrow(ini).c_str(), Sha256File(ini).c_str());
    hrvr::Log("exe base=%p", GetModuleHandleW(nullptr));
    if (wroteDefault) hrvr::Log("hrvr.ini was missing: the built-in default settings were written");
    g_forceWindowed = ForceWindowedWanted(gameDir);
    if (g_screenModeBefore >= 0 && g_screenModeBefore != 2)
        hrvr::Log("WINDOW user_setting.ini ScreenMode %d -> 2 before the game read it (force_windowed)", g_screenModeBefore);
    else if (g_screenModeBefore == -1)
        hrvr::Log("WINDOW no user_setting.ini yet: the window mode is set when the game loads its defaults");
    hrvr::crash::Install(HRVR_VERSION);
    hrvr::summary::SetVersion(HRVR_VERSION);
    // SteamStub builds decrypt .text after our DLL loads: retry until the core addresses resolve
    // (the analysed build by its hash, any other build by signature), at most 60 s.
    std::string textHash;
    bool core = false;
    const ULONGLONG start = GetTickCount64();
    int attempts = 0;
    while (!core && GetTickCount64() - start < 60000) {
        textHash = Sha256Text();
        core = hrvr::game::ResolveAll(textHash.c_str(), false);
        ++attempts;
        if (!core) Sleep(250);
    }
    // 0.34.0: a SteamStub copy (the stock Steam exe and the Steam demo carry a .bind section) decrypts .text while this
    // loop runs; the core addresses can appear before the whole section is done. Before the final search, wait until
    // .text hashes the same twice 250 ms apart (at most 10 s more), so no other address is searched in half-decrypted code.
    if (core && attempts > 1) {
        int settle = 0;
        for (std::string again; settle < 40; ++settle) {
            Sleep(250);
            again = Sha256Text();
            if (again == textHash) break;
            textHash = again;
        }
        hrvr::Log("game code settled after %d more check(s) (encrypted at start: SteamStub)", settle + 1);
    }
    hrvr::Log("game addresses: %d attempt(s), %.1f s", attempts, (GetTickCount64() - start) / 1000.0);
    hrvr::Log(".text sha256=%s", textHash.c_str());
    core = hrvr::game::ResolveAll(textHash.c_str(), true);
    constexpr int kAll = int(hrvr::game::Anchor::Count);
    const int resolved = hrvr::game::ResolvedAnchors();
    hrvr::Log("anchors resolved %d/%d", resolved, kAll);
    hrvr::crash::InstallLate(textHash.c_str(), core);
    if (!core) {
        hrvr::Log("FAIL-CLOSED: core game addresses not found in this build; mod disabled, game runs vanilla");
        return 0;
    }
    if (MH_Initialize() != MH_OK) {
        hrvr::Log("FAIL-CLOSED: MinHook could not start; game runs vanilla");
        return 0;
    }
    hrvr::crash::InstallAbortHook();
    if (g_forceWindowed) {  // 0.36.15: before the renderer starts, every stage
        void* p = hrvr::game::Addr(hrvr::game::Anchor::LoadDisplaySettings);
        const MH_STATUS s = p ? MH_CreateHook(p, reinterpret_cast<void*>(&HookLoadDisplay),
                                              reinterpret_cast<void**>(&g_origLoadDisplay))
                              : MH_ERROR_NOT_EXECUTABLE;
        const MH_STATUS e = s == MH_OK ? MH_EnableHook(p) : s;
        hrvr::Log("WINDOW hook RENDER::LoadDisplaySettings %p %s", p, p ? MH_StatusToString(e) : "anchor missing");
    }
    if (!hrvr::stereo::Install()) {
        MH_DisableHook(MH_ALL_HOOKS);
        hrvr::Log("FAIL-CLOSED: render hooks incomplete; every hook removed, game runs vanilla");
        return 0;
    }
    hrvr::vrinput::Install();  // optional: without it VR still renders, the controllers do nothing
    hrvr::welcome::Install();  // first-start welcome windows: Enter held back from the game while one takes it
    hrvr::Log("HRVR ready: F9 VR on/off, F2 recenter, F10 Xbox prompts, F11 PlayStation prompts");
    return 0;
}

// The game denies PROCESS_TERMINATE and can ignore WM_CLOSE while unfocused, so dev
// tooling (tools/close_game.ps1) signals this event and the process ends itself.
DWORD WINAPI QuitWatchThread(LPVOID) {
    // Per-process, auto-reset: a stale signalled event from a closed instance (a script still
    // holding its handle) must never terminate the next launch.
    wchar_t name[64];
    swprintf_s(name, L"Local\\HRVR_Quit_%lu", GetCurrentProcessId());
    HANDLE quit = CreateEventW(nullptr, FALSE, FALSE, name);
    if (!quit) return 0;
    WaitForSingleObject(quit, INFINITE);
    hrvr::Log("quit event received; terminating");
    hrvr::LogFlush();
    TerminateProcess(GetCurrentProcess(), 0);
    return 0;
}

}  // namespace

extern "C" {

HRESULT WINAPI Proxy_DirectInput8Create(HINSTANCE inst, DWORD ver, REFIID riid, LPVOID* out, IUnknown* outer) {
    using Fn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, IUnknown*);
    auto fn = RealProc<Fn>("DirectInput8Create");
    return fn ? fn(inst, ver, riid, out, outer) : E_FAIL;
}
HRESULT WINAPI Proxy_DllCanUnloadNow() {
    auto fn = RealProc<HRESULT(WINAPI*)()>("DllCanUnloadNow");
    return fn ? fn() : S_FALSE;
}
HRESULT WINAPI Proxy_DllGetClassObject(REFCLSID clsid, REFIID riid, LPVOID* out) {
    auto fn = RealProc<HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID*)>("DllGetClassObject");
    return fn ? fn(clsid, riid, out) : E_FAIL;
}
HRESULT WINAPI Proxy_DllRegisterServer() {
    auto fn = RealProc<HRESULT(WINAPI*)()>("DllRegisterServer");
    return fn ? fn() : E_FAIL;
}
HRESULT WINAPI Proxy_DllUnregisterServer() {
    auto fn = RealProc<HRESULT(WINAPI*)()>("DllUnregisterServer");
    return fn ? fn() : E_FAIL;
}

}  // extern "C"

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = inst;
        DisableThreadLibraryCalls(inst);
        ForceWindowedFile();  // 0.36.15: a small profile-file edit, before any game code reads the file
        // Keep work off the loader lock.
        if (HANDLE t = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr)) CloseHandle(t);
        if (HANDLE t = CreateThread(nullptr, 0, QuitWatchThread, nullptr, 0, nullptr)) CloseHandle(t);
    }
    return TRUE;
}
