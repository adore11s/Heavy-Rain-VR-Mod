#include "player.h"

#include <windows.h>

#include <atomic>
#include <cstring>
#include <mutex>

#include <cstdio>
#include <string>

#include "bones.h"
#include "engineobj.h"
#include "fpmode.h"
#include "posemath.h"
#include "game.h"
#include "heapscan.h"
#include "log.h"
#include "prompts.h"
#include "story.h"

namespace hrvr::player {
namespace {

// ---- 0.36.8: which main character is "the player" when the game does not say (owner 2026-11-20) ----
// LIVE (0.36.7 log, _5803_EPL_SHAUN_DEAD_ETHAN_IN_JAIL): no active gameplay, no "player" alias - the mod kept the last
// walked character (Shelby, from an earlier chapter: the CHARACTER object outlives its chapters) and F3 found every shot
// > 20 m from him, so "the first person switch didnt do anything". The owner: "make it so that it reads the last level
// i played with the character and based off that it chooses and if there is no other level i played then do random but
// make the choice permanent at least for the level".
// The four main characters are found by their engine instance names (the CHARACTER object's own handle +0x20 -> name
// +0x18), the names the scene scripts and Gate 1 use (DATA: tools/rig_holding.py PLAYABLE_NAME, class 0xfa6 ids
// 3 / 4 / 6 / 7). INFERENCE until a log: the run-time names equal the data's (`PLAYER main characters:` lists them).
// Order: a walked character (the game's) > the "player" alias (the game's) > the character walked in THIS chapter >
// the chapter's saved choice (hrvr.ini [hrvr_characters] <chapter>=<name>) > the last character the player walked in
// any chapter (hrvr.ini last_played_character) > random - then saved for the chapter. A new choice prefers the main
// characters within 20 m of the game camera (fpcam's F3 range), else any.
constexpr int kMaxMains = 8;
const char* const kFamilies[4] = {"Ethan", "Madison", "Jayden", "Shelby"};
struct Main {
    void* character;
    int family;  // index into kFamilies
};
Main g_mains[kMaxMains] = {};
int g_mainCount = 0;
float g_cam[3] = {0.f, 0.f, 0.f};
bool g_camKnown = false;
char g_walkedChapter[96] = "";  // the chapter in which g_lastWalked was last walked
int g_walkedFamily = -1;
char g_choiceChapter[96] = "";  // the chapter the current choice is for
void* g_choice = nullptr;
int g_choiceFamily = -1;

int FamilyOfName(const char* n) {
    if (!n || !*n) return -1;
    if (std::strcmp(n, "Ethan_Mars") == 0) return 0;
    if (std::strncmp(n, "Madison_Paige", 13) == 0 || std::strcmp(n, "CHRONIC01_MOTORIDE_MadisonPaige") == 0) return 1;
    if (std::strcmp(n, "Norman_Jayden") == 0) return 2;
    if (std::strcmp(n, "Scott_Shelby") == 0) return 3;
    return -1;
}
int FamilyOf(void* character) {
    char name[96];
    const uint8_t* own = static_cast<const uint8_t*>(obj::Ptr(character, 0x20));
    if (!own || !obj::String(own + 0x18, name, sizeof(name))) return -1;
    return FamilyOfName(name);
}
std::wstring IniPath() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring s(exe);
    return s.substr(0, s.find_last_of(L"\\/")) + L"\\hrvr.ini";
}
std::wstring Wide(const char* s) {
    std::wstring w;
    for (; s && *s; ++s) w += wchar_t(static_cast<unsigned char>(*s));
    return w;
}
int ReadFamily(const wchar_t* section, const wchar_t* key) {
    wchar_t v[32];
    GetPrivateProfileStringW(section, key, L"", v, 32, IniPath().c_str());
    for (int f = 0; f < 4; ++f)
        if (_wcsicmp(v, Wide(kFamilies[f]).c_str()) == 0) return f;
    return -1;
}
void WriteFamily(const wchar_t* section, const wchar_t* key, int f) {
    if (f >= 0 && f < 4) WritePrivateProfileStringW(section, key, Wide(kFamilies[f]).c_str(), IniPath().c_str());
}

constexpr size_t kTpgActive = 0x54;      // byte: IsActive
constexpr size_t kTpgCharacter = 0x1E8;  // handle: Character
constexpr size_t kTpgCtrlMode = 0x200;   // int: CtrlMode
constexpr size_t kAliasName = 0x18;      // engine string
constexpr size_t kAliasEntity = 0x30;    // handle
constexpr int kMaxTpg = 32;
constexpr int kMaxSync = 16;
constexpr size_t kSyncEntity = 0x110, kSyncWith = 0x128;  // MOKIT_SYNC: the controlled entity, EntitySync (handles)
constexpr int kMaxAlias = 1024;
constexpr DWORD kRescanEmptyMs = 1000;  // nothing known yet
constexpr DWORD kRescanMs = 10000;      // new chapters create new gameplay objects

std::mutex g_mutex;
void* g_tpgs[kMaxTpg] = {};
void* g_syncs[kMaxSync] = {};
int g_syncCount = 0;
int g_tpgCount = 0;
void* g_playerAlias = nullptr;  // CAMERA_TARGET_ALIAS named exactly "player"
std::atomic<bool> g_scanning{false};
DWORD g_lastScan = 0;
void* g_lastWalked = nullptr;
State g_logged{};

bool IsCharacter(void* c) { return c && obj::IsLive(c, game::vt::Character); }

DWORD WINAPI ScanThread(LPVOID) {
    static void* hits[5 * kMaxAlias];
    const uintptr_t vts[5] = {game::vt::ThirdPersonGameplay, game::vt::CameraTargetAlias, game::vt::MparGui,
                              game::vt::MokitSync, game::vt::Character};
    int counts[5] = {};
    size_t mb = 0;
    heapscan::FindByVtables(vts, 5, hits, kMaxAlias, counts, &mb);
    // 0.36.8: the live main characters (Ethan, Madison, Jayden, Shelby) by their instance names
    Main mains[kMaxMains] = {};
    int nMains = 0;
    for (int i = 0; i < counts[4] && nMains < kMaxMains; ++i) {
        void* c = hits[4 * kMaxAlias + i];
        if (!IsCharacter(c)) continue;
        const int f = FamilyOf(c);
        if (f >= 0) mains[nMains++] = {c, f};
    }
    {
        std::lock_guard lock(g_mutex);
        std::memcpy(g_mains, mains, sizeof(g_mains));
        g_mainCount = nMains;
    }
    {
        static std::string lastList = "?";
        std::string list;
        for (int i = 0; i < nMains; ++i) {
            char one[64];
            std::snprintf(one, sizeof(one), "%s%s %p", i ? ", " : "", kFamilies[mains[i].family], mains[i].character);
            list += one;
        }
        if (list != lastList) Log("PLAYER main characters: %s (of %d CHARACTER objects)", list.empty() ? "none" : list.c_str(), counts[4]);
        lastList = list;
    }
    void* syncs[kMaxSync] = {};
    int nSync = 0;
    for (int i = 0; i < counts[3] && nSync < kMaxSync; ++i)
        if (obj::IsLive(hits[3 * kMaxAlias + i], game::vt::MokitSync)) syncs[nSync++] = hits[3 * kMaxAlias + i];
    // MPAR_GUI is a plain singleton (GAME_RUNTIME_INFO+0x128), not a KCOM object: no back-pointer to check.
    // A freed copy may keep the vtable; take one whose two lists look sane (counts < 64, arrays readable).
    void* gui = nullptr;
    int sane = 0;
    for (int i = 0; i < counts[2]; ++i) {
        void* g = hits[2 * kMaxAlias + i];
        uint32_t a = 0, b = 0;
        if (!obj::Get(g, 0x20, a) || !obj::Get(g, 0x38, b) || a >= 64 || b >= 64) continue;
        if ((a && !obj::Ptr(g, 0x18)) || (b && !obj::Ptr(g, 0x30))) continue;
        gui = g;
        ++sane;
    }
    if (sane > 1) Log("PLAYER %d MPAR_GUI candidates, using %p", sane, gui);
    prompts::SetMparGui(gui);
    void* tpgs[kMaxTpg] = {};
    int n = 0;
    for (int i = 0; i < counts[0] && n < kMaxTpg; ++i)
        if (obj::IsLive(hits[i], game::vt::ThirdPersonGameplay)) tpgs[n++] = hits[i];
    // Aliases are plain list elements (no KCOM handle of their own): vtable plus a readable name.
    void* alias = nullptr;
    int aliases = 0;
    for (int i = 0; i < counts[1]; ++i) {
        void* a = hits[kMaxAlias + i];
        char name[64];
        if (!obj::HasVtable(a, game::vt::CameraTargetAlias) || !obj::String(static_cast<uint8_t*>(a) + kAliasName, name, sizeof(name)))
            continue;
        ++aliases;
        if (_stricmp(name, "player") == 0 && IsCharacter(obj::Cached(obj::Ptr(a, kAliasEntity)))) alias = a;
    }
    {
        std::lock_guard lock(g_mutex);
        std::memcpy(g_tpgs, tpgs, sizeof(g_tpgs));
        g_tpgCount = n;
        std::memcpy(g_syncs, syncs, sizeof(g_syncs));
        g_syncCount = nSync;
        g_playerAlias = alias;
    }
    static int lastSync = -1;
    if (nSync != lastSync) Log("PLAYER scan: %d pair-walk controller(s) (MOKIT_SYNC) live", nSync);
    lastSync = nSync;
    static int lastN = -1;
    static void* lastAlias = nullptr;
    if (n != lastN || alias != lastAlias)
        Log("PLAYER scan: %d THIRD_PERSON_GAMEPLAY live in %zu MB, %d camera target aliases, \"player\" alias %s", n, mb,
            aliases, alias ? "found" : "missing");
    lastN = n;
    lastAlias = alias;
    g_scanning = false;
    return 0;
}

void StartScan(DWORD minGapMs) {
    const DWORD now = GetTickCount();
    if (g_scanning || (g_lastScan && now - g_lastScan < minGapMs)) return;
    g_lastScan = now;
    g_scanning = true;
    if (HANDLE t = CreateThread(nullptr, 0, ScanThread, nullptr, 0, nullptr)) CloseHandle(t);
    else g_scanning = false;
}

// Not standing in its bind pose (fpmode.h LooksLikeTPose: both hands level with the shoulders, far out).
bool Animated(void* character) {
    pose::Mat4 la{}, ra{}, lh{}, rh{};
    if (!bones::CharacterBone(character, bones::Crc32("LeftArm"), &la) || !bones::CharacterBone(character, bones::Crc32("RightArm"), &ra) ||
        !bones::CharacterBone(character, bones::Crc32("LeftHand"), &lh) || !bones::CharacterBone(character, bones::Crc32("RightHand"), &rh))
        return false;
    auto pos = [](const pose::Mat4& m) { return pose::Vec3{m.m[12], m.m[13], m.m[14]}; };
    return !fp::LooksLikeTPose(pos(la), pos(lh), pos(ra), pos(rh));
}

// The chapter's main character when the game names none (see the top). nullptr: no main character known.
void* Choose(const char* chapter) {
    Main mains[kMaxMains];
    int n = 0;
    float cam[3];
    bool camKnown = false;
    {
        std::lock_guard lock(g_mutex);
        n = g_mainCount;
        std::memcpy(mains, g_mains, sizeof(mains));
        std::memcpy(cam, g_cam, sizeof(cam));
        camKnown = g_camKnown;
    }
    // 0.36.10 (LIVE 0.36.9: all four main characters exist at the main menu - "Jayden chosen as the player in
    // LOADING_SCENE_0201_SPLASHS"): only in story chapters, and only characters the game animates (a character it does
    // not animate stands in its bind pose, the T-pose fpcam already refuses).
    if (!story::InStoryChapter()) return nullptr;
    int live = 0;
    for (int i = 0; i < n; ++i)
        if (IsCharacter(mains[i].character) && Animated(mains[i].character)) mains[live++] = mains[i];
    n = live;
    if (n == 0) return nullptr;
    // the same chapter's choice stays (permanent for the level) while that character exists
    if (*chapter && std::strcmp(chapter, g_choiceChapter) == 0 && g_choice)
        for (int i = 0; i < n; ++i)
            if (mains[i].character == g_choice) return g_choice;
    auto pick = [&](int family) -> void* {
        for (int i = 0; i < n; ++i)
            if (mains[i].family == family) return mains[i].character;
        return nullptr;
    };
    const char* why = nullptr;
    void* c = nullptr;
    int family = -1;
    // a choice saved for this chapter (also one the player walked here before)
    if (*chapter) {
        family = ReadFamily(L"hrvr_characters", Wide(chapter).c_str());
        if ((c = pick(family)) != nullptr) why = "this chapter's saved character";
    }
    // the main characters near the game camera (within F3's 20 m), else all of them
    Main nearby[kMaxMains];
    int nNearby = 0;
    for (int i = 0; i < n; ++i) {
        pose::Mat4 w{};
        float d2 = 0.f;
        if (camKnown && bones::NodeWorld(bones::CharacterNode(mains[i].character), &w)) {
            const float dx = w.m[12] - cam[0], dy = w.m[13] - cam[1], dz = w.m[14] - cam[2];
            d2 = dx * dx + dy * dy + dz * dz;
        }
        if (!camKnown || d2 <= 20.f * 20.f) nearby[nNearby++] = mains[i];
    }
    if (nNearby == 0) {
        std::memcpy(nearby, mains, sizeof(Main) * size_t(n));
        nNearby = n;
    }
    if (!c) {
        family = ReadFamily(L"hrvr", L"last_played_character");
        for (int i = 0; i < nNearby && !c; ++i)
            if (nearby[i].family == family) c = nearby[i].character;
        if (c) why = "the character you played last";
    }
    if (!c) {
        LARGE_INTEGER q;
        QueryPerformanceCounter(&q);
        const Main& m = nearby[size_t(q.QuadPart ^ (q.QuadPart >> 17)) % size_t(nNearby)];
        c = m.character;
        family = m.family;
        why = "random (the character you played last is not here)";
    }
    for (int i = 0; i < n; ++i)
        if (mains[i].character == c) family = mains[i].family;
    if (*chapter) WriteFamily(L"hrvr_characters", Wide(chapter).c_str(), family);
    strcpy_s(g_choiceChapter, chapter);
    g_choice = c;
    g_choiceFamily = family;
    Log("PLAYER %s chosen as the player in %s: %s; %d main character(s) here, %d near the camera (saved for this chapter: "
        "hrvr.ini [hrvr_characters])", kFamilies[family], *chapter ? chapter : "(unknown chapter)", why, n, nNearby);
    return c;
}

}  // namespace

void SetCamera(float x, float y, float z) {
    std::lock_guard lock(g_mutex);
    g_cam[0] = x;
    g_cam[1] = y;
    g_cam[2] = z;
    g_camKnown = true;
}

int Partners(void* character, void** out, int max) {
    if (!character || max <= 0) return 0;
    void* syncs[kMaxSync];
    int n = 0, found = 0;
    {
        std::lock_guard lock(g_mutex);
        n = g_syncCount;
        std::memcpy(syncs, g_syncs, sizeof(syncs));
    }
    for (int i = 0; i < n && found < max; ++i) {
        if (!obj::IsLive(syncs[i], game::vt::MokitSync)) continue;
        void* a = obj::Cached(obj::Ptr(syncs[i], kSyncEntity));
        void* b = obj::Cached(obj::Ptr(syncs[i], kSyncWith));
        void* other = a == character ? b : b == character ? a : nullptr;
        if (!other || other == character || !IsCharacter(other)) continue;
        bool dup = false;
        for (int k = 0; k < found; ++k) dup = dup || out[k] == other;
        if (!dup) out[found++] = other;
    }
    return found;
}

State Poll() {
    State s;
    void* tpgs[kMaxTpg];
    int n;
    void* alias;
    {
        std::lock_guard lock(g_mutex);
        std::memcpy(tpgs, g_tpgs, sizeof(tpgs));
        n = g_tpgCount;
        alias = g_playerAlias;
    }
    StartScan(n ? kRescanMs : kRescanEmptyMs);
    // The active gameplay's character; with several active (should not happen), the one walked before.
    for (int i = 0; i < n; ++i) {
        uint8_t active = 0;
        if (!obj::IsLive(tpgs[i], game::vt::ThirdPersonGameplay) || !obj::Get(tpgs[i], kTpgActive, active) || active != 1)
            continue;
        void* c = obj::Cached(obj::Ptr(tpgs[i], kTpgCharacter));
        if (!IsCharacter(c)) continue;
        if (!s.character || c == g_lastWalked) {
            s.character = c;
            s.tpg = tpgs[i];
        }
    }
    char chapter[96] = "";
    unsigned long long since = 0;
    story::Chapter(chapter, sizeof(chapter), &since);
    if (s.character) {
        s.control = Control::Walking;
        obj::Get(s.tpg, kTpgCtrlMode, s.ctrlMode);
        // 0.36.8: remember who was walked in which chapter, and the last character the player walked (saved)
        if (s.character != g_lastWalked || std::strcmp(g_walkedChapter, chapter) != 0) {
            const int f = FamilyOf(s.character);
            if (f >= 0) {
                WriteFamily(L"hrvr", L"last_played_character", f);
                if (*chapter) WriteFamily(L"hrvr_characters", Wide(chapter).c_str(), f);
                Log("PLAYER walking %s in %s (saved: last_played_character and this chapter's character)", kFamilies[f],
                    *chapter ? chapter : "(unknown chapter)");
            }
            g_walkedFamily = f;
            strcpy_s(g_walkedChapter, chapter);
        }
        g_lastWalked = s.character;
    } else {
        void* fromAlias = alias && obj::HasVtable(alias, game::vt::CameraTargetAlias) ? obj::Cached(obj::Ptr(alias, kAliasEntity)) : nullptr;
        if (IsCharacter(fromAlias)) {
            s.character = fromAlias;
        } else if (IsCharacter(g_lastWalked) && *chapter && std::strcmp(g_walkedChapter, chapter) == 0) {
            s.character = g_lastWalked;  // walked in this chapter: still the player's
        } else if (void* chosen = Choose(chapter)) {
            s.character = chosen;
        } else if (IsCharacter(g_lastWalked)) {
            s.character = g_lastWalked;  // no main character found by name: as before 0.36.8
        } else {
            g_lastWalked = nullptr;
        }
        if (s.character) s.control = Control::Cutscene;
    }
    if (s.character != g_logged.character || s.control != g_logged.control || s.tpg != g_logged.tpg) {
        Log("PLAYER character=%p control=%s tpg=%p ctrlMode=%d (%d gameplay objects known, \"player\" alias %s)", s.character,
            Name(s.control), s.tpg, s.ctrlMode, n, alias ? "known" : "unknown");
        g_logged = s;
    }
    return s;
}

const char* Name(Control c) {
    switch (c) {
        case Control::Walking: return "walking";
        case Control::Cutscene: return "cutscene";
        default: return "unknown";
    }
}

void Dump() {
    std::lock_guard lock(g_mutex);
    for (int i = 0; i < g_tpgCount; ++i) {
        uint8_t active = 0;
        int mode = -1;
        obj::Get(g_tpgs[i], kTpgActive, active);
        obj::Get(g_tpgs[i], kTpgCtrlMode, mode);
        void* c = obj::Cached(obj::Ptr(g_tpgs[i], kTpgCharacter));
        Log("PLAYER   tpg %p live=%d active=%d ctrlMode=%d character=%p (%s)", g_tpgs[i],
            int(obj::IsLive(g_tpgs[i], game::vt::ThirdPersonGameplay)), int(active), mode, c,
            IsCharacter(c) ? "CHARACTER" : "not a character");
    }
    Log("PLAYER   \"player\" alias %p -> %p", g_playerAlias,
        g_playerAlias ? obj::Cached(obj::Ptr(g_playerAlias, kAliasEntity)) : nullptr);
}

}  // namespace hrvr::player
