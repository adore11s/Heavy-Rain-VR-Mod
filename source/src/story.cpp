// Where the player is in the story (story.h).
#include "story.h"

#include <windows.h>

#include <MinHook.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

#include "game.h"
#include "log.h"

namespace hrvr::story {
namespace {

// STORYBOARD (vtable 0x140c90d40): elements +0x88 (handles, count +0x90), NEXT +0xB8, CURRENT +0xE0. A handle is a
// COM_INSTANCE: object +0x50. STORYBOARD_ELEMENT: candidate scenes +0x48 (list object: head node +0x50, node +0x10
// next, +0x18 scene handle), own name +0x68 (game string, length +0x70). STORYBOARD_SCENE: name +0x88 (length +0x90).
constexpr uintptr_t kStoryboardVt = 0x140c90d40, kUpdateVa = 0x140250df0;
constexpr uintptr_t kSbElements = 0x88, kSbCount = 0x90, kSbNext = 0xB8, kSbCurrent = 0xE0, kInstObject = 0x50,
                    kElScenes = 0x48, kElName = 0x68, kElNameLength = 0x70, kListHead = 0x50, kNodeNext = 0x10,
                    kNodeHandle = 0x18, kScName = 0x88, kScNameLength = 0x90;
// Script manager (g_ScriptManager): +0x18 number of states, +0x20 lua_State*, +0x198 the variables table's name.
constexpr uintptr_t kSmCount = 0x18, kSmState = 0x20, kSmVarName = 0x198;
// lua_State: +0x10 top, +0x18 base (TValue = 16 bytes: value, then the type at +8).
constexpr uintptr_t kLTop = 0x10, kLBase = 0x18;
constexpr int kGlobalsIndex = -10002;  // LUA_GLOBALSINDEX (Lua 5.1)
constexpr int kTypeBool = 1, kTypeNumber = 3, kTypeString = 4, kTypeTable = 5;
constexpr int kNameMax = 96;

using UpdateFn = void (*)(void* sb);
UpdateFn g_orig = nullptr;
std::atomic<TaskCallback> g_callback{nullptr};
bool g_hooked = false;

uint8_t* g_story = nullptr;  // (storyboard task) the storyboard with the most elements
ULONGLONG g_storyMs = 0;
uint8_t* g_curElement = nullptr;
char g_chapter[2][kNameMax] = {"", ""};  // published by index (crash reports read without locks)
std::atomic<int> g_chapterIdx{0};
std::atomic<ULONGLONG> g_chapterSince{0};

// Story variables: two buffers, the finished one published by index.
constexpr size_t kVarsMax = 16384;
char g_vars[2][kVarsMax];
std::atomic<int> g_varsIdx{-1};
std::atomic<ULONGLONG> g_varsMs{0};
std::atomic<int> g_varsCount{0};
bool g_varsOn = true, g_varsBroken = false;
ULONGLONG g_nextVars = 0;
char g_varTable[kNameMax] = "";

struct Lua {
    void* (*index2adr)(void*, int);
    int (*next)(void*, int);
    int (*type)(void*, int);
    void (*gettable)(void*, int);
    const char* (*tolstring)(void*, int, size_t*);
    void (*pushstring)(void*, const char*);
    void (*settop)(void*, int);
    void (*pushnil)(void*);
    int (*checkstack)(void*, int);
} g_lua{};

bool Plausible(const void* p) { return reinterpret_cast<uintptr_t>(p) > 0x10000; }

// ---- storyboard readers (inside __try only) ----
uint8_t* ObjectOf(uint8_t* handle) {
    if (!Plausible(handle)) return nullptr;
    uint8_t* obj = *reinterpret_cast<uint8_t**>(handle + kInstObject);
    return Plausible(obj) ? obj : nullptr;
}
bool CopyName(uint8_t* at, uintptr_t textOff, uintptr_t lengthOff, char* out) {
    const char* text = *reinterpret_cast<char**>(at + textOff);
    const int len = *reinterpret_cast<int*>(at + lengthOff);
    if (!Plausible(text) || len <= 0 || len >= kNameMax) return false;
    std::memcpy(out, text, size_t(len));
    out[len] = 0;
    return true;
}
bool ChapterLike(const char* s) {
    if (s[0] != '_') return false;
    for (int i = 1; i <= 4; ++i)
        if (s[i] < '0' || s[i] > '9') return false;
    return s[5] == '_';
}
// The element's name: its own, else the first candidate scene named like a chapter, else its first.
bool ElementName(uint8_t* element, char* out) {
    if (CopyName(element, kElName, kElNameLength, out)) return true;
    uint8_t* list = ObjectOf(*reinterpret_cast<uint8_t**>(element + kElScenes));
    uint8_t* node = list ? *reinterpret_cast<uint8_t**>(list + kListHead) : nullptr;
    bool have = false, chapter = false;
    for (int guard = 0; Plausible(node) && guard < 64; ++guard) {
        char name[kNameMax];
        uint8_t* scene = ObjectOf(*reinterpret_cast<uint8_t**>(node + kNodeHandle));
        if (scene && CopyName(scene, kScName, kScNameLength, name) && (!have || (!chapter && ChapterLike(name)))) {
            strcpy_s(out, kNameMax, name);
            chapter = ChapterLike(name);
            have = true;
        }
        node = *reinterpret_cast<uint8_t**>(node + kNodeNext);
    }
    return have;
}
uint32_t CountOf(uint8_t* sb) {
    const uintptr_t vt = game::Va(kStoryboardVt);
    if (!Plausible(sb) || !vt) return 0;
    __try {
        if (*reinterpret_cast<uintptr_t*>(sb) != vt) return 0;
        const uint32_t n = *reinterpret_cast<uint32_t*>(sb + kSbCount);
        return n <= 4096 ? n : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}
// The current element and its name; false when not readable.
bool ReadCurrent(uint8_t* sb, uint8_t** element, int* index, char* name) {
    __try {
        uint8_t* cur = *reinterpret_cast<uint8_t**>(sb + kSbCurrent);
        *element = cur;
        *index = -1;
        name[0] = 0;
        if (!cur) return true;
        const uint32_t n = *reinterpret_cast<uint32_t*>(sb + kSbCount);
        auto** slots = *reinterpret_cast<uint8_t***>(sb + kSbElements);
        for (uint32_t i = 0; i < n && i < 4096; ++i)
            if (slots[i] == cur) {
                *index = int(i);
                break;
            }
        uint8_t* obj = ObjectOf(cur);
        if (obj) ElementName(obj, name);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ---- story variables: the Lua walk (game thread, inside the storyboard task) ----
struct Walk {
    char* out;
    size_t used;
    int count;
    int visited;
    bool full;
};
void Append(Walk& w, const char* prefix, const char* key, const char* value) {
    if (w.full) return;
    char item[200];
    const int n = snprintf(item, sizeof(item), "%s.%s=%s; ", prefix, key, value);
    if (n <= 0) return;
    const size_t len = size_t(n) < sizeof(item) ? size_t(n) : sizeof(item) - 1;
    if (w.used + len + 1 >= kVarsMax) {
        w.full = true;
        return;
    }
    std::memcpy(w.out + w.used, item, len);
    w.used += len;
    w.out[w.used] = 0;
    ++w.count;
}
// The table at the stack top: every string key's boolean / number / string value, sub-tables to depth 3.
void WalkTable(void* L, const char* prefix, int depth, Walk& w) {
    if (!g_lua.checkstack(L, 3)) return;
    g_lua.pushnil(L);
    while (g_lua.next(L, -2)) {
        if (++w.visited > 4000 || w.full) {
            g_lua.settop(L, -3);  // value and key: the iteration ends here
            return;
        }
        if (g_lua.type(L, -2) == kTypeString) {
            const char* key = g_lua.tolstring(L, -2, nullptr);  // a string key: no conversion
            const int vt = g_lua.type(L, -1);
            if (key && vt == kTypeBool) {
                const auto* tv = static_cast<const int*>(g_lua.index2adr(L, -1));
                Append(w, prefix, key, tv && *tv ? "true" : "false");
            } else if (key && (vt == kTypeNumber || vt == kTypeString)) {
                const char* v = g_lua.tolstring(L, -1, nullptr);  // (a number becomes a string in this stack slot only)
                char shortV[64];
                strncpy_s(shortV, v ? v : "?", _TRUNCATE);
                Append(w, prefix, key, shortV);
            } else if (key && vt == kTypeTable && depth < 3) {
                char sub[160];
                snprintf(sub, sizeof(sub), "%s.%s", prefix, key);
                WalkTable(L, sub, depth + 1, w);
            }
        }
        g_lua.settop(L, -2);  // the value; the key stays for lua_next
    }
}
// false: a fault (the snapshots stop for this session).
bool SnapshotUnsafe(Walk& w, char* tableName, double* ms) {
    __try {
        const auto* pm = game::Global<uint8_t*>(game::g::ScriptManager);
        uint8_t* mgr = pm ? *pm : nullptr;
        if (!Plausible(mgr) || *reinterpret_cast<int*>(mgr + kSmCount) < 1) return true;  // no script state yet
        void* L = *reinterpret_cast<void**>(mgr + kSmState);
        if (!Plausible(L) || !CopyName(mgr, kSmVarName, kSmVarName + 8, tableName)) return true;
        LARGE_INTEGER f, t0, t1;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&t0);
        auto* L8 = static_cast<uint8_t*>(L);
        const int top = int((*reinterpret_cast<uint8_t**>(L8 + kLTop) - *reinterpret_cast<uint8_t**>(L8 + kLBase)) / 16);
        if (top < 0 || top > 8000 || !g_lua.checkstack(L, 4)) return true;
        g_lua.pushstring(L, tableName);
        g_lua.gettable(L, kGlobalsIndex);
        if (g_lua.type(L, -1) == kTypeTable) WalkTable(L, tableName, 0, w);
        g_lua.settop(L, top);  // the stack exactly as it was
        QueryPerformanceCounter(&t1);
        *ms = double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(f.QuadPart);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
void Snapshot(ULONGLONG now) {
    const int next = g_varsIdx.load() == 0 ? 1 : 0;
    Walk w{g_vars[next], 0, 0, 0, false};
    g_vars[next][0] = 0;
    char table[kNameMax] = "";
    double ms = 0.0;
    if (!SnapshotUnsafe(w, table, &ms)) {
        g_varsBroken = true;
        Log("STORY variables could not be read (fault) - no more snapshots this session");
        return;
    }
    if (!table[0]) return;  // no script state yet
    strcpy_s(g_varTable, table);
    g_varsCount = w.count;
    g_varsMs = now;
    g_varsIdx = next;
    static int logs = 0;
    if (logs++ < 3)
        Log("STORY variables: %d values from the Lua table \"%s\" (%zu bytes%s, %.2f ms) - kept for crash reports", w.count,
            table, w.used, w.full ? ", cut at 16 KB" : "", ms);
}

void HookUpdate(void* self) {
    auto* sb = static_cast<uint8_t*>(self);
    const ULONGLONG now = GetTickCount64();
    // More than one storyboard can run (a menu storyboard next to the production one): the one with the most
    // elements is the story.
    if (sb == g_story || !g_story || now - g_storyMs > 1000 || CountOf(sb) > CountOf(g_story)) {
        g_story = sb;
        g_storyMs = now;
        uint8_t* element = nullptr;
        int index = -1;
        char name[kNameMax];
        if (ReadCurrent(sb, &element, &index, name) && element != g_curElement) {
            g_curElement = element;
            const int slot = g_chapterIdx.load() == 0 ? 1 : 0;
            strcpy_s(g_chapter[slot], name);
            g_chapterIdx = slot;
            g_chapterSince = now;
            Log("STORY chapter \"%s\" (element %d of %u)", name[0] ? name : "(none)", index, CountOf(sb));
            g_nextVars = now + 2000;  // the new chapter's variables, once it has started
        }
        if (g_varsOn && !g_varsBroken && g_lua.next && now >= g_nextVars) {
            g_nextVars = now + 30000;
            Snapshot(now);
        }
    }
    if (TaskCallback cb = g_callback.load()) cb(self);
    g_orig(self);
}

std::wstring IniPath() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring s(exe);
    return s.substr(0, s.find_last_of(L"\\/")) + L"\\hrvr.ini";
}

}  // namespace

bool Install() {
    wchar_t v[16];
    GetPrivateProfileStringW(L"hrvr", L"crash_story_vars", L"on", v, 16, IniPath().c_str());
    g_varsOn = _wcsicmp(v, L"off") != 0;
    auto fn = [](uint64_t va) { return reinterpret_cast<void*>(game::Va(va)); };
    g_lua.index2adr = reinterpret_cast<void* (*)(void*, int)>(fn(0x140bbd180));
    g_lua.next = reinterpret_cast<int (*)(void*, int)>(fn(0x140bbd240));
    g_lua.type = reinterpret_cast<int (*)(void*, int)>(fn(0x140bbd8f0));
    g_lua.gettable = reinterpret_cast<void (*)(void*, int)>(fn(0x140bbd960));
    g_lua.tolstring = reinterpret_cast<const char* (*)(void*, int, size_t*)>(fn(0x140bbdc10));
    g_lua.pushstring = reinterpret_cast<void (*)(void*, const char*)>(fn(0x140bbde80));
    g_lua.settop = reinterpret_cast<void (*)(void*, int)>(fn(0x140bbe2b0));
    g_lua.pushnil = reinterpret_cast<void (*)(void*)>(fn(0x140bbe390));
    g_lua.checkstack = reinterpret_cast<int (*)(void*, int)>(fn(0x140bbe3d0));
    const bool lua = g_lua.index2adr && g_lua.next && g_lua.type && g_lua.gettable && g_lua.tolstring && g_lua.pushstring &&
                     g_lua.settop && g_lua.pushnil && g_lua.checkstack && game::Va(game::g::ScriptManager);
    if (!lua) g_lua.next = nullptr;  // (the walk checks this one)
    void* update = fn(kUpdateVa);
    if (!update || !game::Va(kStoryboardVt)) {
        Log("STORY storyboard task not found in this build: no chapter in crash reports");
        return false;
    }
    const MH_STATUS a = MH_CreateHook(update, reinterpret_cast<void*>(&HookUpdate), reinterpret_cast<void**>(&g_orig));
    const MH_STATUS b = a == MH_OK ? MH_EnableHook(update) : a;
    g_hooked = b == MH_OK;
    Log("STORY hook STORYBOARD::Update %p %s; story variables %s", update, MH_StatusToString(b),
        !g_varsOn ? "off (crash_story_vars=off)" : lua ? "every 30 s and at each chapter (crash reports)" : "NOT available");
    return g_hooked;
}

void SetTaskCallback(TaskCallback cb) { g_callback = cb; }
bool TaskHooked() { return g_hooked; }

void Chapter(char* out, size_t cap, unsigned long long* sinceMs) {
    strncpy_s(out, cap, g_chapter[g_chapterIdx.load()], _TRUNCATE);
    const ULONGLONG s = g_chapterSince.load();
    *sinceMs = s ? GetTickCount64() - s : 0;
}

bool InStoryChapter() {
    char c[96];
    unsigned long long since = 0;
    Chapter(c, sizeof(c), &since);
    return c[0] == '_' && c[1] >= '0' && c[1] <= '9';
}

void DescribeForCrash(void (*line)(const char* text, void* ctx), void* ctx) {
    char buf[256];
    const ULONGLONG now = GetTickCount64(), since = g_chapterSince.load();
    const char* ch = g_chapter[g_chapterIdx.load()];
    if (since)
        snprintf(buf, sizeof(buf), "Chapter:     %s (for %llu min %llu s)", ch[0] ? ch : "(none: a menu)",
                 (now - since) / 60000, (now - since) / 1000 % 60);
    else
        snprintf(buf, sizeof(buf), "Chapter:     (not known yet: no storyboard running)");
    line(buf, ctx);
    const int idx = g_varsIdx.load();
    if (idx < 0) {
        line(g_varsOn ? "Story variables: (no snapshot yet)" : "Story variables: (off, crash_story_vars=off)", ctx);
        return;
    }
    snprintf(buf, sizeof(buf), "Story variables (%d from the Lua table \"%s\", taken %llu s before the crash):",
             g_varsCount.load(), g_varTable, (now - g_varsMs.load()) / 1000);
    line(buf, ctx);
    // wrapped at about 110 characters, between entries
    const char* p = g_vars[idx];
    while (*p) {
        size_t n = 0, cut = 0;
        while (p[n] && n < 110) {
            if (p[n] == ';') cut = n + 1;
            ++n;
        }
        if (!p[n]) cut = n;
        if (!cut) cut = n;
        char part[160] = "  ";
        std::memcpy(part + 2, p, cut);
        part[2 + cut] = 0;
        line(part, ctx);
        p += cut;
        while (*p == ' ') ++p;
    }
}

}  // namespace hrvr::story
