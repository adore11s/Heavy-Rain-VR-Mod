#include "movielink.h"

#include <windows.h>

#include <MinHook.h>

#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include "bones.h"
#include "engineobj.h"
#include "game.h"
#include "item_class.h"
#include "log.h"

namespace hrvr::movielink {
namespace {

constexpr size_t kCtrlMovie = 0x110;   // MOVIE_CONTROLLER_PLAY: the movie's COM instance
constexpr size_t kMovieFlags = 0x2b0;  // bit 0: playing
constexpr size_t kMovieList = 0x150, kMovieCount = 0x158;
constexpr size_t kInstName = 0x18;     // COM instance: engine string (char*, +8 length)
constexpr int kMovies = 8, kPerMovie = 48;
constexpr uint64_t kFreshMs = 250;

struct Object {
    void* node = nullptr;
    int cls = itemclass::kNotKnown;
    char name[64] = {};
};
struct Movie {
    void* movie = nullptr;
    uint64_t ms = 0;
    char name[64] = {};
    int count = 0;
    Object obj[kPerMovie];
    bool logged = false;  // listed once while it plays with the player
};

using UpdateFn = void (*)(void*, void*, void*, void*);
UpdateFn g_orig = nullptr;
std::mutex g_mutex;
Movie g_movies[kMovies];
bool g_installed = false;

void Read(void* ctrl) {
    void* inst = obj::Ptr(ctrl, kCtrlMovie);
    void* movie = obj::Cached(inst);
    uint32_t flags = 0;
    if (!movie || !obj::Get(movie, kMovieFlags, flags) || !(flags & 1)) return;
    Movie m;
    m.movie = movie;
    m.ms = GetTickCount64();
    obj::String(static_cast<uint8_t*>(inst) + kInstName, m.name, sizeof(m.name));
    uint32_t n = 0;
    const uint8_t* list = static_cast<const uint8_t*>(obj::Ptr(movie, kMovieList));
    if (!list || !obj::Get(movie, kMovieCount, n)) return;
    for (uint32_t i = 0; i < n && i < 256 && m.count < kPerMovie; ++i) {
        void* rec = obj::Ptr(list, size_t(i) * 8);
        void* ent = rec ? obj::Ptr(rec, 0) : nullptr;  // the entity's COM instance
        void* script = obj::Cached(ent);
        void* node = script ? bones::CharacterNode(script) : nullptr;  // script entity +0x160 -> entity +0x68 -> node
        if (!node) continue;
        Object& o = m.obj[m.count++];
        o.node = node;
        obj::String(static_cast<uint8_t*>(ent) + kInstName, o.name, sizeof(o.name));
        o.cls = itemclass::Of(m.name, o.name);
    }
    std::lock_guard lock(g_mutex);
    Movie* slot = nullptr;
    for (Movie& q : g_movies)
        if (q.movie == movie) slot = &q;
    if (!slot)
        for (Movie& q : g_movies)
            if (!slot || q.ms < slot->ms) slot = &q;  // the oldest
    const bool keepLogged = slot->movie == movie && std::strcmp(slot->name, m.name) == 0 && slot->logged;
    *slot = m;
    slot->logged = keepLogged;
}

void Hook(void* ctrl, void* a2, void* a3, void* a4) {
    g_orig(ctrl, a2, a3, a4);
    Read(ctrl);
}

}  // namespace

bool Install() {
    void* p = game::Addr(game::Anchor::MovieControllerUpdate);
    if (!p) {
        Log("FPITEM movie controller not found in this build: no object goes to the controller hand by the movie link");
        return false;
    }
    const MH_STATUS s = MH_CreateHook(p, reinterpret_cast<void*>(&Hook), reinterpret_cast<void**>(&g_orig));
    const MH_STATUS e = s == MH_OK ? MH_EnableHook(p) : s;
    Log("FPITEM hook MOVIE_CONTROLLER_PLAY::Update %p %s (the movie link, Gate 3 row 9; %zu names in the class table)", p,
        MH_StatusToString(e), sizeof(itemclass::kTable) / sizeof(itemclass::kTable[0]));
    g_installed = e == MH_OK;
    return g_installed;
}

bool Available() { return g_installed; }

bool Find(void* node, const void* parent, void* player, Link* out) {
    if (!g_installed || !node || !player) return false;
    const uint64_t now = GetTickCount64();
    std::lock_guard lock(g_mutex);
    for (Movie& m : g_movies) {
        if (!m.movie || now - m.ms > kFreshMs) continue;
        bool withPlayer = false;
        const Object* hit = nullptr;
        for (int i = 0; i < m.count; ++i) {
            withPlayer = withPlayer || m.obj[i].node == player;
            if (!hit && (m.obj[i].node == node || (parent && m.obj[i].node == parent))) hit = &m.obj[i];
        }
        if (!withPlayer || !hit) continue;
        if (!m.logged) {
            m.logged = true;
            int per[5] = {};
            std::string list;
            for (int i = 0; i < m.count; ++i) {
                if (m.obj[i].node == player) continue;
                ++per[m.obj[i].cls + 1];
                if (list.size() < 900) list += std::string(list.empty() ? "" : ", ") + m.obj[i].name + " (" + itemclass::Name(m.obj[i].cls) + ")";
            }
            Log("FPITEM movie \"%s\" plays with the player: %d objects - held %d, worn %d, stays %d, person %d, not in the "
                "table %d: %s", m.name, m.count - 1, per[1], per[2], per[3], per[4], per[0], list.c_str());
        }
        out->cls = hit->cls;
        std::snprintf(out->entity, sizeof(out->entity), "%s", hit->name);
        std::snprintf(out->movie, sizeof(out->movie), "%s", m.name);
        return true;
    }
    return false;
}

void LogStatus() {
    const uint64_t now = GetTickCount64();
    std::lock_guard lock(g_mutex);
    int live = 0;
    for (const Movie& m : g_movies)
        if (m.movie && now - m.ms < 1000) {
            ++live;
            Log("FPITEM movie \"%s\" playing: %d entities", m.name, m.count);
        }
    if (!live) Log("FPITEM no movie playing (movie link %s)", g_installed ? "on" : "not installed");
}

}  // namespace hrvr::movielink
