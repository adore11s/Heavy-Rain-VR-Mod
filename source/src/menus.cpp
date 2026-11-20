// The game's own menus (menus.h; playbook 10.6).
#include "menus.h"

#include <windows.h>

#include <MinHook.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "game.h"
#include "log.h"

namespace hrvr::menus {
namespace {

constexpr uintptr_t kMenuPage = 0x290, kMenuPageAlt = 0x298, kMenuState = 0x378;  // MmDk::CMenu
constexpr int kMaxMenus = 12;

using HandleFn = void (*)(void* menu, int time);
HandleFn g_orig = nullptr;

struct Seen {
    void* menu = nullptr;
    char page[64] = "";
    uint64_t ms = 0;
};
Seen g_menus[kMaxMenus];
std::mutex g_mutex;  // never held while logging
std::vector<std::string> g_pages;  // set once at Install
std::atomic<uint64_t> g_handles{0}, g_readFaults{0};

// The menu's current page name, as the game's GetCurPage reads it; false on a fault or no page.
bool ReadPage(void* menu, char* out, size_t cap) {
    out[0] = 0;
    __try {
        auto* m = static_cast<const uint8_t*>(menu);
        const int state = *reinterpret_cast<const int*>(m + kMenuState);
        const auto* page = *reinterpret_cast<const uint8_t* const*>(m + (state == 1 || state == 4 ? kMenuPageAlt : kMenuPage));
        if (!page) return true;  // no page: an empty name
        const char* name = *reinterpret_cast<const char* const*>(page);
        if (!name) return true;
        size_t i = 0;
        for (; i + 1 < cap && name[i]; ++i) {
            const char c = name[i];
            if (c < 0x20 || c > 0x7e) return false;  // not a name: read nothing
            out[i] = c;
        }
        out[i] = 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out[0] = 0;
        return false;
    }
}

void HookHandle(void* menu, int time) {
    g_orig(menu, time);
    g_handles.fetch_add(1, std::memory_order_relaxed);
    char page[64];
    if (!ReadPage(menu, page, sizeof(page))) {
        if (g_readFaults.fetch_add(1, std::memory_order_relaxed) < 3) Log("MENU page of menu %p not readable", menu);
        return;
    }
    char was[64] = "";
    bool changed = false;
    {
        std::lock_guard lock(g_mutex);
        const uint64_t now = GetTickCount64();
        Seen* slot = nullptr;
        for (auto& s : g_menus)
            if (s.menu == menu) slot = &s;
        if (!slot)  // a new menu: a free slot, else the one not handled for longest
            for (auto& s : g_menus)
                if (!slot || s.ms < slot->ms) slot = &s;
        if (slot->menu != menu || std::strcmp(slot->page, page) != 0) {
            changed = true;
            if (slot->menu == menu) strcpy_s(was, slot->page);
            slot->menu = menu;
            strcpy_s(slot->page, page);
        }
        slot->ms = now;
    }
    if (changed) Log("MENU menu %p page \"%s\" (was \"%s\")", menu, page, was);
}


std::wstring IniPath() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring s(exe);
    return s.substr(0, s.find_last_of(L"\\/")) + L"\\hrvr.ini";
}

}  // namespace

bool Install() {
    wchar_t v[512];
    GetPrivateProfileStringW(L"hrvr", L"panel_pages", L"", v, 512, IniPath().c_str());
    std::string list;
    for (const wchar_t* p = v; *p; ++p) list += char(*p < 128 ? *p : '?');
    if (list.empty()) list = kDefaultPages;
    g_pages = ParseList(list);
    void* p = game::Addr(game::Anchor::MenuHandle);
    if (!p) {
        Log("MENU CMenu::Handle not found: the VR panel shows only while the game is paused");
        return false;
    }
    const MH_STATUS s = MH_CreateHook(p, reinterpret_cast<void*>(&HookHandle), reinterpret_cast<void**>(&g_orig));
    const MH_STATUS e = s == MH_OK ? MH_EnableHook(p) : s;
    Log("MENU hook CMenu::Handle %p %s; the VR panel also shows on the pages: %s", p, MH_StatusToString(e), list.c_str());
    return e == MH_OK;
}

bool OptionsOpen() {
    const uint64_t now = GetTickCount64();
    std::lock_guard lock(g_mutex);
    for (const auto& s : g_menus)
        if (s.menu && now - s.ms < 300 && PageMatches(s.page, g_pages)) return true;
    return false;
}

bool Command(const char* verb, const char*) {
    if (std::strcmp(verb, "menus") != 0) return false;
    Seen copy[kMaxMenus];
    {
        std::lock_guard lock(g_mutex);
        std::memcpy(copy, g_menus, sizeof(copy));
    }
    const uint64_t now = GetTickCount64();
    Log("MENU status: %llu handles, %llu read faults, Options open %d", static_cast<unsigned long long>(g_handles.load()),
        static_cast<unsigned long long>(g_readFaults.load()), int(OptionsOpen()));
    for (const auto& s : copy)
        if (s.menu)
            Log("MENU   menu %p page \"%s\" handled %llu ms ago", s.menu, s.page, static_cast<unsigned long long>(now - s.ms));
    return true;
}

}  // namespace hrvr::menus
