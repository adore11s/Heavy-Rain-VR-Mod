// ARI glasses by hand (ari.h). STATIC / DATA evidence: docs/ARI_GESTURE.md.
#include "ari.h"

#include <windows.h>
#include <MinHook.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>

#include "ari_copy_draw.h"
#include "ari_draw.h"
#include "ari_gesture.h"
#include "engineobj.h"
#include "fpcam.h"
#include "game.h"
#include "log.h"
#include "story.h"
#include "vrinput.h"

namespace hrvr::ari {
namespace {

// MPAR_ELEMENT_AXIS_ROTATION fields (STATIC, docs/ARI_GESTURE.md)
constexpr size_t kDir = 0x224, kStart = 0x228, kSpan = 0x22C, kTol = 0x230;
constexpr size_t kStick = 0x234;   // negX, posX, posY, negY (4 floats)
constexpr size_t kOffset = 0x268;  // the setter's offsets for those four (4 floats)
constexpr size_t kProgress = 0x25C, kEngaged = 0x60;
// USER_ACTION: the movie its condition starts (0xfbf, id) and that movie's name (engine string)
constexpr size_t kUaMovieId = 0x3C4, kUaMovieName = 0x3C8;
// prompt record (list 0x1415ba070, stride 0x28): +0x08 element, +0x10 display
constexpr size_t kRecElement = 0x08, kRecDisplay = 0x10;

using ActivateFn = void (*)(void* ua, void* entry);
using CheckFn = uint64_t (*)(void* element, float dt);  // dt in xmm1 (passed on to the base timer 0x140374120)
ActivateFn g_origActivate = nullptr;
CheckFn g_origCheck = nullptr;

bool g_enabled = true;          // hrvr.ini ari_gesture
// 1.7.1 (owner: "make the hints appear in every level where jayden has the glasses motion but ... only once per level"):
// the hints are kept per level - hrvr.ini [hrvr_ari_hints] <level>=bits (1 put on done by hand, 2 take off), level =
// ari_gesture.h LevelKey (the chapter's number). 1.6 .. 1.7 kept one pair of flags for the whole game (ari_hint_seen /
// ari_hint_off_seen, no longer read). Caller holds g_mu.
std::wstring IniPath();
const char* KindName(Kind k);
char g_levelKey[24] = "";
uint8_t g_levelSeen = 0;
void LevelKeyNow(const char* movie, char* key, size_t cap) {
    char chapter[96] = {};
    unsigned long long since = 0;
    story::Chapter(chapter, sizeof chapter, &since);
    LevelKey(chapter, movie, key, cap);
}
uint8_t LevelHintsSeen(const char* movie) {
    char key[24];
    LevelKeyNow(movie, key, sizeof key);
    if (std::strcmp(key, g_levelKey) != 0) {
        strncpy_s(g_levelKey, key, _TRUNCATE);
        wchar_t wkey[24] = {};
        MultiByteToWideChar(CP_UTF8, 0, key, -1, wkey, 24);
        g_levelSeen = uint8_t(GetPrivateProfileIntW(L"hrvr_ari_hints", wkey, 0, IniPath().c_str()) & 3);
        Log("ARI hints for level %s: put on %s, take off %s (hrvr.ini [hrvr_ari_hints])", key[0] ? key : "(unknown)",
            g_levelSeen & 1 ? "done here" : "will show", g_levelSeen & 2 ? "done here" : "will show");
    }
    return g_levelSeen;
}
void MarkLevelHintDone(Kind kind, const char* movie) {
    const uint8_t bit = kind == Kind::On ? 1 : 2;
    const uint8_t before = LevelHintsSeen(movie);
    if (before & bit) return;
    g_levelSeen = uint8_t(before | bit);
    wchar_t wkey[24] = {}, val[8] = {};
    MultiByteToWideChar(CP_UTF8, 0, g_levelKey, -1, wkey, 24);
    swprintf_s(val, L"%u", unsigned(g_levelSeen));
    if (wkey[0]) WritePrivateProfileStringW(L"hrvr_ari_hints", wkey, val, IniPath().c_str());
    Log("ARI %s hint done in level %s (hrvr.ini [hrvr_ari_hints] %s=%u): not shown again in this level",
        KindName(kind), g_levelKey[0] ? g_levelKey : "(unknown)", g_levelKey, unsigned(g_levelSeen));
}
// 1.6.3: a grip the gesture uses is not the game's L1 / R1 (R1 = ARI's own action; it cancelled the prompt in 1.6.1) -
// pressed in the start zone with a glasses prompt on offer, or holding; kept until let go. Bit 0 left, bit 1 right.
std::atomic<uint8_t> g_takeGrip{0};
std::atomic<int> g_glassesHand{-1}, g_glassesKind{0};  // 1.6.4: GlassesHand
// the feed as the game's check saw it (1.6.3 diagnostics)
float g_feedMaxProgress = 0.f;
int g_feedChecks = 0;

struct Tag {
    void* element = nullptr;
    Kind kind = Kind::None;
    int dir = 0;
    float start = 0.f, span = 0.f, tol = 0.f;
    char movie[96] = {};
    uint64_t lastInputMs = 0;  // the game read this element's input (it is on offer)
    bool tookOver = false, hidden = false;
};
std::mutex g_mu;
Tag g_tags[8];
int g_next = 0;
ArcFeed g_feed;
void* g_feedElement = nullptr;
bool g_feedLogged = true;
Gesture g_gesture;
Hint g_hint = Hint::None;
int g_hintHand = -1;
pose::Vec3 g_hintHead{}, g_hintFwd{}, g_hintHandPos{};

double Now() {
    static LARGE_INTEGER f{};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return double(c.QuadPart) / double(f.QuadPart);
}

std::wstring IniPath() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring s(exe);
    return s.substr(0, s.find_last_of(L"\\/")) + L"\\hrvr.ini";
}

// F1 (gameplay first person drawn by the mod) with the controllers driving the hands.
bool Gated() { return g_enabled && vrinput::MotionControls() && fpcam::F1ViewNow(); }

Tag* Find(void* element) {
    for (Tag& t : g_tags)
        if (t.element && t.element == element) return &t;
    return nullptr;
}

const char* KindName(Kind k) { return k == Kind::On ? "put on" : k == Kind::Off ? "take off" : "none"; }

bool WriteInt(void* at, int v) {
    __try {
        *static_cast<int*>(at) = v;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool WriteChar(char* at, char v) {
    __try {
        *at = v;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool WriteStick(void* element, float x, float y) {
    float off[4] = {};
    if (!obj::Bytes(element, kOffset, off, sizeof(off))) return false;
    // as the setter 0x14045dea0 stores them: value + offset, x split into -/+ parts, y into +/- parts
    const float v[4] = {(x < 0.f ? -x : 0.f) + off[0], (x > 0.f ? x : 0.f) + off[1], (y > 0.f ? y : 0.f) + off[2],
                        (y < 0.f ? -y : 0.f) + off[3]};
    __try {
        std::memcpy(static_cast<uint8_t*>(element) + kStick, v, sizeof(v));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// 1.6.5: ARI's wave prompt R1 <-> R2 in place (ari_gesture.h kWaveMovies). STATIC: the accepted input ids are read
// every frame from the element's input list (+0x88 handle -> +0x48 count, +0x50 entries of 12 bytes, id at +0;
// filter 0x140374b40); the picture is the element's menu parameter "sInitTexture" (list +0x48: next +0x10, name
// +0x20, value engine string at +0x38 - the game rewrites it in place too, 0x140380d80), read when the prompt's
// display is made at activation. "Button_R1" / "Button_R2" have the same length: one character changes.
int SwapWave(void* element, bool toR2) {
    int changed = 0;
    const int from = toR2 ? kInputR1 : kInputR2, to = toR2 ? kInputR2 : kInputR1;
    if (void* list = obj::Cached(obj::Ptr(element, 0x88))) {
        uint32_t n = 0;
        uint8_t* ids = static_cast<uint8_t*>(obj::Ptr(list, 0x50));
        if (ids && obj::Get(list, 0x48, n) && n <= 16)
            for (uint32_t i = 0; i < n; ++i) {
                int id = 0;
                if (obj::Get(ids, size_t(i) * 12, id) && id == from && WriteInt(ids + size_t(i) * 12, to)) ++changed;
            }
    }
    for (void* node = obj::Ptr(element, 0x48); node; node = obj::Ptr(node, 0x10)) {
        char name[32] = {};
        if (!obj::String(static_cast<uint8_t*>(node) + 0x20, name, sizeof(name)) || std::strcmp(name, "sInitTexture") != 0)
            continue;
        void* value = obj::Ptr(node, 0x38);
        char tex[32] = {};
        char* chars = value ? static_cast<char*>(obj::Ptr(value, 0)) : nullptr;
        if (chars && obj::String(value, tex, sizeof(tex)) && std::strcmp(tex, toR2 ? "Button_R1" : "Button_R2") == 0 &&
            WriteChar(chars + 8, toR2 ? '2' : '1'))
            ++changed;
        break;
    }
    return changed;
}

void HookActivate(void* ua, void* entry) {
    char movie[96] = {};
    obj::String(static_cast<uint8_t*>(ua) + kUaMovieName, movie, sizeof(movie));
    const bool wave = IsWaveMovie(movie);
    if (wave)  // before the display is made from the element's parameters (the element is cached after the first time)
        if (void* el = obj::Cached(obj::Ptr(entry, 0x18))) SwapWave(el, Gated());
    g_origActivate(ua, entry);  // resolves and caches the element, adds its prompt record
    void* element = obj::Cached(obj::Ptr(entry, 0x18));
    if (wave && element) {
        const bool r2 = Gated();
        const int n = SwapWave(element, r2);
        static bool logged[2] = {false, false};
        if (!logged[r2]) {
            logged[r2] = true;
            Log("ARI wave prompt (movie \"%s\"): %s (%d change(s); input + picture)", movie,
                r2 ? "R1 -> R2 in first person with motion controls (the grip is free for the glasses)" : "R1 (the game's)", n);
        }
        return;
    }
    if (!element || !obj::HasVtable(element, game::vt::MparAxisRotation)) return;
    int movieId = 0;
    obj::Get(ua, kUaMovieId, movieId);
    const Kind kind = KindOfMovie(movie);
    std::lock_guard lock(g_mu);
    Tag* t = Find(element);
    if (kind == Kind::None) {  // a reused element pointer is no longer a glasses prompt
        if (t) *t = Tag{};
        return;
    }
    if (!t) {
        t = &g_tags[g_next];
        g_next = (g_next + 1) % 8;
    }
    // 1.6.2 (LIVE: the game re-activates the same prompt every ~3 s): keep "on offer" across a re-activation
    const uint64_t keepMs = t->element == element ? t->lastInputMs : 0;
    *t = Tag{};
    t->element = element;
    t->kind = kind;
    t->lastInputMs = keepMs;
    obj::Get(element, kDir, t->dir);
    obj::Get(element, kStart, t->start);
    obj::Get(element, kSpan, t->span);
    obj::Get(element, kTol, t->tol);
    strncpy_s(t->movie, movie, _TRUNCATE);
    Log("ARI prompt %s offered: movie \"%s\" (0xfbf %d), semicircle start %.3f span %.3f direction %d tolerance %.2f "
        "(%s)",
        KindName(kind), movie, movieId, t->start, t->span, t->dir, t->tol,
        Gated() ? "first person + motion controls: answered by the hand gesture" : "the game's own prompt");
}

void LogFeedEnd(void* element, Tag* t, double now);

// 1.6.1 (LIVE 2026-11-24, owner's 0801 run: the stick still did the prompt, no gesture): the input vfunc 0x14045ea10 never
// runs for these prompts, so the stick fields are now written right BEFORE the game's per-frame check reads them.
uint64_t HookCheck(void* element, float dt) {
    Kind answered = Kind::None;
    {
        std::lock_guard lock(g_mu);
        Tag* t = Find(element);
        if (t) {
            t->lastInputMs = GetTickCount64();
            const double now = Now();
            // 1.6.6: a feed the hand started in F1 goes on while the game shows its own camera (the movie it engaged)
            const bool feeding = g_feedElement == element && g_feed.Active(now);
            if (Gated() || feeding) {
                float x = 0.f, y = 0.f;
                if (feeding) g_feed.Step(now, &x, &y);
                if (WriteStick(element, x, y)) {
                    if (!t->tookOver) {
                        t->tookOver = true;
                        Log("ARI prompt %s: the thumbstick is ignored for it, the hand gesture answers it", KindName(t->kind));
                    }
                    answered = t->kind;
                }
                LogFeedEnd(element, t, now);
            }
        }
    }
    const uint64_t r = g_origCheck(element, dt);
    if (answered != Kind::None) {
        std::lock_guard lock(g_mu);
        if (g_feedElement == element && g_feed.t0 >= 0.0) {
            float progress = 0.f;
            obj::Get(element, kProgress, progress);
            ++g_feedChecks;
            g_feedMaxProgress = std::max(g_feedMaxProgress, progress);
            if ((r & 0xFF) != 0) {  // answered (2) or failed (1): the feed ends here
                Log("ARI the game %s the %s: semicircle check returned %d after %d checks (%d restart(s)), highest "
                    "progress %.2f, the view %s",
                    (r & 0xFF) == 2 ? "took" : "REJECTED", KindName(answered), int(r & 0xFF), g_feedChecks,
                    g_feed.restarts, g_feedMaxProgress, Gated() ? "first person" : "the game's camera");
                g_feed.t0 = -1.0;
                g_feedLogged = true;
            } else {
                uint8_t engaged = 1;
                obj::Get(element, kEngaged, engaged);
                if (g_feed.AfterCheck(engaged != 0))
                    Log("ARI feed (%s): the game reset the prompt during the hold (not engaged, progress %.2f) - fed again "
                        "from the rest (restart %d)",
                        KindName(answered), progress, g_feed.restarts);
            }
        }
    }
    return r;
}

void LogFeedEnd(void* element, Tag* t, double now) {
    if (g_feedElement == element && !g_feed.Active(now) && !g_feedLogged) {
        g_feedLogged = true;
        float progress = -1.f;
        uint8_t engaged = 0;
        obj::Get(element, kProgress, progress);
        obj::Get(element, kEngaged, engaged);
        Log("ARI arc fed (%s) and held without an answer: progress now %.2f (highest %.2f over %d checks), engaged %d "
            "(success at 0.85)",
            KindName(t->kind), progress, g_feedMaxProgress, g_feedChecks, int(engaged));
    }
}

// A prompt's display object after the game's update: a 3D prompt's world position (+0x30 matrix, row 3) far below the
// scene, a 2D prompt's screen offset (+0x0C / +0x10, fractions of the screen) far outside it. Render-only.
void MoveOutOfSight(void* display) {
    uint8_t mode3d = 0;
    obj::Get(display, 7, mode3d);
    __try {
        float* f = reinterpret_cast<float*>(static_cast<uint8_t*>(display) + (mode3d ? 0x30 : 0x0C));
        if (mode3d) {
            f[13] -= 1000.f;
        } else {
            f[0] = 10.f;
            f[1] = 10.f;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

}  // namespace

int GlassesHand(int* kind) {
    if (kind) *kind = g_glassesKind.load(std::memory_order_relaxed);
    return g_glassesHand.load(std::memory_order_relaxed);
}

bool TakesGrip(int hand) { return (g_takeGrip.load(std::memory_order_relaxed) >> (hand & 1) & 1) != 0; }

bool Install() {
    wchar_t v[16];
    GetPrivateProfileStringW(L"hrvr", L"ari_gesture", L"on", v, 16, IniPath().c_str());
    g_enabled = _wcsicmp(v, L"off") != 0 && _wcsicmp(v, L"0") != 0;
    void* a = game::Addr(game::Anchor::UserActionActivate);
    void* i = game::Addr(game::Anchor::AxisRotationCheck);
    if (!a || !i) {
        Log("ARI gesture: user action / semicircle input not found in this build - the game's own glasses prompts");
        return false;
    }
    MH_STATUS s = MH_CreateHook(a, reinterpret_cast<void*>(&HookActivate), reinterpret_cast<void**>(&g_origActivate));
    const MH_STATUS ea = s == MH_OK ? MH_EnableHook(a) : s;
    s = MH_CreateHook(i, reinterpret_cast<void*>(&HookCheck), reinterpret_cast<void**>(&g_origCheck));
    const MH_STATUS ei = s == MH_OK ? MH_EnableHook(i) : s;
    Log("ARI gesture %s: hook USER_ACTION::Activate %p %s, semicircle check %p %s; hints once per level (hrvr.ini "
        "[hrvr_ari_hints]); the grip does it (kept from the game's R1 while used)",
        g_enabled ? "on (first person + motion controls)" : "off (ari_gesture=off)", a, MH_StatusToString(ea), i,
        MH_StatusToString(ei));
    if (g_enabled) aricopy::StartLoad();  // v1.3: the glasses copy's mesh from the player's own archives (worker thread)
    return ea == MH_OK && ei == MH_OK;
}

void Frame(const xr::FrameInput& in) {
    const pose::Vec3 head{in.headRel.m[12], in.headRel.m[13], in.headRel.m[14]};
    float fx = -in.headRel.m[8], fz = -in.headRel.m[10];
    const float fl = std::sqrt(fx * fx + fz * fz);
    if (fl < 1e-4f) return;
    fx /= fl;
    fz /= fl;
    HandIn hands[2];
    for (int h = 0; h < 2; ++h) {
        const xr::HandInput& hs = in.hand[h];
        hands[h].active = hs.active && hs.gripValid;
        hands[h].pos = {hs.gripRel.m[12], hs.gripRel.m[13], hs.gripRel.m[14]};
        hands[h].press = hs.squeeze;  // 1.6.3 (owner): the grip again; kept from the game (R1 / L1) while used
    }
    const uint64_t nowMs = GetTickCount64();
    const double now = Now();
    std::lock_guard lock(g_mu);
    Tag* offered = nullptr;
    for (Tag& t : g_tags)
        if (t.element && nowMs - t.lastInputMs < 300 && (!offered || t.lastInputMs > offered->lastInputMs)) offered = &t;
    const bool gated = Gated();
    const Kind kind = offered && gated && !(g_feed.Active(now)) ? offered->kind : Kind::None;
    const Gesture::Event ev = g_gesture.Feed(kind, now, {head.x, head.y, head.z}, {fx, 0.f, fz}, hands);
    if (ev == Gesture::Event::Grabbed)
        Log("ARI gesture: %s hand grabbed at the %s", g_gesture.HoldingHand() == 0 ? "left" : "right",
            kind == Kind::On ? "chest" : "face");
    if (ev == Gesture::Event::Released) Log("ARI gesture: let go / too slow - start again");
    if (ev == Gesture::Event::Done && offered) {
        g_feed = ArcFeed{};
        g_feed.start = offered->start;
        g_feed.span = offered->span;
        g_feed.dir = offered->dir;
        g_feed.Begin(now);
        g_feedElement = offered->element;
        g_feedLogged = false;
        g_feedMaxProgress = 0.f;
        g_feedChecks = 0;
        Log("ARI gesture done (%s): feeding the semicircle (start %.3f span %.3f direction %d) to the game",
            KindName(offered->kind), offered->start, offered->span, offered->dir);
        MarkLevelHintDone(offered->kind, offered->movie);
    }
    // The grips the gesture owns (start zone with a prompt on offer, or holding), kept until let go.
    {
        uint8_t take = g_takeGrip.load(), next = 0;
        for (int h = 0; h < 2; ++h) {
            const bool own = kind != Kind::None && hands[h].active &&
                             (g_gesture.HoldingHand() == h || g_gesture.InStartZone(kind, {head.x, head.y, head.z}, {fx, 0.f, fz}, hands[h].pos));
            const bool stillPulled = (take >> h & 1) && hands[h].active && hands[h].press >= 0.2f;
            if (own || stillPulled) next |= uint8_t(1u << h);
        }
        g_takeGrip = next;
    }
    {
        const uint8_t seen = offered ? LevelHintsSeen(offered->movie) : 0;
        g_hint = HintStage((seen & 1) != 0, (seen & 2) != 0,
                           gated && offered && !g_feed.Active(now) ? offered->kind : Kind::None, g_gesture.Holding());
    }
    g_hintHand = g_gesture.HoldingHand();
    g_glassesHand = gated && g_gesture.Holding() ? g_gesture.HoldingHand() : -1;
    g_glassesKind = int(kind);
    g_hintHead = head;
    g_hintFwd = {fx, 0.f, fz};
    if (g_hintHand >= 0) g_hintHandPos = {hands[g_hintHand].pos.x, hands[g_hintHand].pos.y, hands[g_hintHand].pos.z};
}

bool HintLayer(HintQuad* out) {
    std::lock_guard lock(g_mu);
    if (g_hint == Hint::None) return false;
    const int pic = g_hint == Hint::GrabOn ? 0 : g_hint == Hint::PutOn ? 1 : g_hint == Hint::GrabOff ? 2 : 3;
    const uint32_t* px = HintPixels(pic);
    if (!px) return false;
    static Hint logged = Hint::None;
    if (logged != g_hint) {
        logged = g_hint;
        static const char* const kText[4] = {"Grab AR Glasses (chest)", "Put on AR Glasses", "Grab AR Glasses (face)",
                                             "Take off AR Glasses"};
        Log("ARI hint: \"%s\"", kText[pic]);
    }
    const pose::Vec3 f = g_hintFwd;
    out->picture = pic;
    out->pixels = px;
    out->widthM = kHintWm;
    out->heightM = kHintHm;
    out->yaw = std::atan2(-f.x, -f.z);  // the quad's +Z toward the head
    if (pic == 0) {
        // the arrow tip (bottom edge) just in front of the chest, the label leaning away toward the eyes' view
        const float tilt = 0.61f;  // 35 deg
        const Params p;
        const pose::Vec3 tip{g_hintHead.x + f.x * (p.chestFwd + 0.10f), g_hintHead.y - p.chestDown,
                             g_hintHead.z + f.z * (p.chestFwd + 0.10f)};
        const float h = 0.5f * kHintHm;
        out->center = {tip.x + f.x * h * std::sin(tilt), tip.y + h * std::cos(tilt), tip.z + f.z * h * std::sin(tilt)};
        out->pitch = -tilt;
    } else if (pic == 1) {
        // the arrow starting just above the holding hand, pointing up toward the eyes; the label under it
        const pose::Vec3 base{g_hintHandPos.x + f.x * 0.05f, g_hintHandPos.y - 0.10f, g_hintHandPos.z + f.z * 0.05f};
        out->center = {base.x, base.y + 0.5f * kHintHm, base.z};
        out->pitch = 0.f;
    } else if (pic == 2) {
        // take off, before the pull: the arrow points up to the face (its tip 6 cm below the eyes, 28 cm ahead), the
        // label under it
        const pose::Vec3 tip{g_hintHead.x + f.x * 0.28f, g_hintHead.y - 0.06f, g_hintHead.z + f.z * 0.28f};
        out->center = {tip.x, tip.y - 0.5f * kHintHm, tip.z};
        out->pitch = 0.f;
    } else {
        // take off, holding: the label at the hand, the arrow pointing down toward the chest
        out->center = {g_hintHandPos.x + f.x * 0.05f, g_hintHandPos.y + 0.06f - 0.5f * kHintHm, g_hintHandPos.z + f.z * 0.05f};
        out->pitch = 0.f;
    }
    return true;
}

bool HidePrompt(void* record) {
    void* element = obj::Ptr(record, kRecElement);
    if (!element) return false;
    {
        std::lock_guard lock(g_mu);
        Tag* t = Find(element);
        if (!t || !Gated()) return false;
        if (!t->hidden) {
            t->hidden = true;
            Log("ARI prompt %s: the game's semicircle is not drawn in first person with motion controls", KindName(t->kind));
        }
    }
    if (void* display = obj::Ptr(record, kRecDisplay)) MoveOutOfSight(display);
    return true;
}

}  // namespace hrvr::ari
