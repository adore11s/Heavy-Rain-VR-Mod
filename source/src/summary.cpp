// The log's summary block (summary.h).
#include "summary.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "crashreport.h"
#include "game.h"
#include "log.h"
#include "story.h"
#include "xr.h"

namespace hrvr::summary {
namespace {

const char* g_version = "?";
int g_blocks = 0;
std::string g_lastKey;
ULONGLONG g_nextProfileCheck = 0;
std::string g_profiles;

std::wstring GameDir() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring s(exe);
    return s.substr(0, s.find_last_of(L"\\/"));
}

// Every key=value of one ini section on lines of at most ~200 characters, after `head`.
void LogSection(const std::wstring& path, const wchar_t* section, const char* head) {
    std::wstring buf(32768, L'\0');
    const DWORD n = GetPrivateProfileSectionW(section, buf.data(), DWORD(buf.size()), path.c_str());
    std::string line = head;
    int items = 0;
    for (const wchar_t* p = buf.c_str(); p < buf.c_str() + n && *p; p += wcslen(p) + 1) {
        char item[300];
        WideCharToMultiByte(CP_UTF8, 0, p, -1, item, sizeof(item), nullptr, nullptr);
        if (line.size() + strlen(item) > 200 && items) {
            Log("summary: %s", line.c_str());
            line = std::string(head) + " (cont.)";
            items = 0;
        }
        line += items ? ", " : " ";
        line += item;
        ++items;
    }
    if (!n) line += " (none)";
    Log("summary: %s", line.c_str());
}

void LogBlock(bool stereo3D) {
    ++g_blocks;
    const std::wstring dir = GameDir();
    Log("summary: ---- block %d (%s) ----", g_blocks, stereo3D ? "3D in the headset" : "a flat screen");
    // the mod and the game build
    {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        auto* base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        unsigned long long size = 0;
        if (GetFileAttributesExW(exe, GetFileExInfoStandard, &fa)) size = (unsigned long long)fa.nFileSizeHigh << 32 | fa.nFileSizeLow;
        char build[160];
        crash::BuildText(build, sizeof(build));
        Log("summary: HRVR %s; HeavyRain.exe time stamp 0x%08lx, %llu bytes, %s", g_version,
            static_cast<unsigned long>(nt->FileHeader.TimeDateStamp), size, build);
    }
    {
        char os[96], cpu[96], ram[32], gpu[192];
        crash::PcText(os, cpu, ram, gpu);
        Log("summary: %s; CPU %s, %s RAM; GPU %s", os, cpu, ram, gpu);
    }
    xr::LogSummary();
    // the game's frame, window and monitor
    {
        auto u32 = [](uint64_t va) {
            const auto* p = game::Global<uint32_t>(va);
            return p ? *p : 0u;
        };
        const auto* hp = game::Global<HWND>(game::g::MainWnd);
        const HWND h = hp ? *hp : nullptr;
        RECT c{};
        if (h) GetClientRect(h, &c);
        MONITORINFOEXW mi{};
        mi.cbSize = sizeof(mi);
        DEVMODEW dm{};
        dm.dmSize = sizeof(dm);
        unsigned mw = 0, mh = 0, hz = 0;
        if (h && GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi)) {
            mw = unsigned(mi.rcMonitor.right - mi.rcMonitor.left);
            mh = unsigned(mi.rcMonitor.bottom - mi.rcMonitor.top);
            if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)) hz = dm.dmDisplayFrequency;
        }
        Log("summary: game render %ux%u, back buffer %ux%u, swap chain %ux%u; desktop window client %ldx%ld; monitor %ux%u "
            "at %u Hz", u32(game::g::RenderWidth), u32(game::g::RenderHeight), u32(game::g::BackbufferWidth),
            u32(game::g::BackbufferHeight), u32(game::g::SwapWidth), u32(game::g::SwapHeight), c.right - c.left,
            c.bottom - c.top, mw, mh, hz);
    }
    LogSection(dir + L"\\user_setting.ini", L"GRAPHIC_SECTION", "game settings (user_setting.ini):");
    {
        char ch[96];
        unsigned long long since = 0;
        story::Chapter(ch, sizeof(ch), &since);
        Log("summary: chapter %s", ch[0] ? ch : "(none yet)");
    }
    // the compatibility switches (their values or defaults) and every mod setting as written in hrvr.ini
    {
        const std::wstring ini = dir + L"\\hrvr.ini";
        std::string line = "compatibility switches:";
        const struct {
            const wchar_t* key;
            const wchar_t* def;
        } keys[] = {{L"symfov", L"on"},   {L"enginefov", L"on"},   {L"prime", L"off"},      {L"small_views", L"fixed"},
                    {L"ui", L"panel"},    {L"world_scale", L"1.0"}, {L"rendering_method", L"afr"}, {L"vr_dof", L"off"},
                    {L"desktop_window", L"720"}, {L"desktop_crop", L"1"}};
        for (const auto& k : keys) {
            wchar_t v[64];
            GetPrivateProfileStringW(L"hrvr", k.key, k.def, v, 64, ini.c_str());
            char item[160];
            snprintf(item, sizeof(item), " %ls=%ls", k.key, v);
            line += item;
        }
        Log("summary: %s", line.c_str());
        LogSection(ini, L"hrvr", "mod settings (hrvr.ini):");
    }
}

}  // namespace

void SetVersion(const char* version) { g_version = version; }

void Tick(bool vrRunning, bool stereo3D) {
    if (!vrRunning || g_blocks >= 6) return;
    const ULONGLONG now = GetTickCount64();
    if (now >= g_nextProfileCheck) {  // the controllers' profiles, every 2 s
        g_nextProfileCheck = now + 2000;
        char l[256] = "", r[256] = "";
        xr::GetInteractionProfile(0, l, sizeof(l));
        xr::GetInteractionProfile(1, r, sizeof(r));
        g_profiles = std::string(l) + "|" + r;
    }
    auto u32 = [](uint64_t va) {
        const auto* p = game::Global<uint32_t>(va);
        return p ? *p : 0u;
    };
    char key[400];
    snprintf(key, sizeof(key), "%ux%u %d %s", u32(game::g::SwapWidth), u32(game::g::SwapHeight), int(stereo3D),
             g_profiles.c_str());
    if (key == g_lastKey) return;
    // a new state: logged once it has held for a moment (a size switch passes through intermediate states)
    static std::string pending;
    static ULONGLONG pendingSince = 0;
    if (pending != key) {
        pending = key;
        pendingSince = now;
        return;
    }
    if (now - pendingSince < 1500) return;
    g_lastKey = key;
    LogBlock(stereo3D);
}

}  // namespace hrvr::summary
