#include "prompts.h"

#include <windows.h>

#include <MinHook.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>

#include "engineobj.h"
#include "fpmode.h"
#include "game.h"
#include "log.h"

namespace hrvr::prompts {
namespace {

constexpr float kThoughtsDistance = 0.9f;  // m in front of the eye (the ring of words has a radius)
constexpr float kThoughtsDrop = 0.08f;     // m below the gaze
constexpr size_t kGuiCountA = 0x20, kGuiCountB = 0x38;

std::atomic<bool> g_enabled{true};
std::mutex g_viewMutex;
struct View {
    bool firstPerson = false;
    pose::Vec3 eye{}, gaze{0, 0, -1}, head{};
    uint64_t ms = 0;
} g_view;
std::atomic<void*> g_mparGui{nullptr};
std::atomic<uint64_t> g_lastShown{0};
std::atomic<uint64_t> g_lastElement{0};  // the last MPAR::UpdateElement call of any prompt record

// ---- 7B ----
using SetAnchorFn = void (*)(void* animator, const float* matrix, float dt);
SetAnchorFn g_origSetAnchor = nullptr;
struct Slot {
    void* animator = nullptr;
    fp::WorldLock lock;
};
Slot g_slots[8];
std::mutex g_slotMutex;

Slot* SlotFor(void* animator, uint64_t now) {
    Slot* free = nullptr;
    for (Slot& s : g_slots) {
        if (s.animator == animator) return &s;
        if (!free && (!s.animator || now - s.lock.lastMs > 5000)) free = &s;
    }
    if (free) *free = Slot{animator, {}};
    return free;
}

void HookSetAnchor(void* animator, const float* matrix, float dt) {
    View v;
    {
        std::lock_guard lock(g_viewMutex);
        v = g_view;
    }
    const uint64_t now = GetTickCount64();
    pose::Vec3 anchor{};
    float m[16];
    if (!g_enabled || !v.firstPerson || !matrix || now - v.ms > 250 || !obj::Bytes(matrix, 0, m, sizeof(m))) {
        g_origSetAnchor(animator, matrix, dt);
        return;
    }
    anchor = {m[12], m[13], m[14]};
    if (!fp::InHeadColumn(v.head, anchor)) {  // another character's thoughts stay where the game puts them
        g_origSetAnchor(animator, matrix, dt);
        return;
    }
    alignas(16) float moved[16];  // the game reads it with MOVAPS (0F 28 02): 16-byte aligned
    std::memcpy(moved, m, sizeof(moved));
    bool replaced = false;
    pose::Vec3 p{};
    {
        std::lock_guard lock(g_slotMutex);
        Slot* s = SlotFor(animator, now);
        if (!s) {
            g_origSetAnchor(animator, matrix, dt);
            return;
        }
        p = s->lock.Update(v.eye, v.gaze, kThoughtsDistance, kThoughtsDrop, now, &replaced);
    }
    moved[12] = p.x;
    moved[13] = p.y;
    moved[14] = p.z;
    if (replaced) {
        static uint64_t lastLog = 0;
        if (now - lastLog >= 2000) {
            lastLog = now;
            Log("PROMPTS thoughts at the player's head placed %.2f m in front of the view (anchor was %.2f m from the eye)",
                kThoughtsDistance,
                std::sqrt((anchor.x - v.eye.x) * (anchor.x - v.eye.x) + (anchor.y - v.eye.y) * (anchor.y - v.eye.y) +
                          (anchor.z - v.eye.z) * (anchor.z - v.eye.z)));
        }
    }
    g_origSetAnchor(animator, moved, dt);
}

// ---- 7B for action prompts (0.7.0) ----
// STATIC 2026-10-28: every action prompt, tutorial and help text (prologue "STutoSketch_TutoPar", "SHelp_Help3.MPARC") is
// an MPAR element. Its per-frame update (kernelinput_14023fdb0, record of the global list 0x1415ba070, stride 0x28)
// builds a world matrix from the attached entity's node or bone plus offsets (record+0x08 -> +0x120..+0x128), turns
// it to the camera (record+0x20 mode 1..3) and stores it in the display object *(record+0x10) at +0x30 (position
// +0x60); the 3D sprite is drawn from it with "i3DSize" and 2D size limits, in the eye passes. A prompt attached to the
// player character (swipe / gyro gestures, "listen to your thoughts", help texts) therefore sat inside the first-person
// head (owner 2026-10-28: "not all prompts and text are shown in front of you"). After the game's update, a prompt
// within the player's body column is moved with the others to a point world-locked in front of the view, keeping
// their arrangement around the head, and turned to face the eye. Other characters' and objects' prompts stay.
using MparUpdateFn = void (*)(void* owner, void* record);
MparUpdateFn g_origMparUpdate = nullptr;
constexpr float kPromptDistance = 0.75f, kPromptDrop = 0.12f;  // m: a little nearer and lower than the thoughts
std::mutex g_mparMutex;
uint64_t g_mparLockMs = 0;
pose::Vec3 g_mparPoint{};
std::atomic<uint64_t> g_mparMoved{0}, g_mparKept{0};
std::atomic<bool> g_allFront{true};  // fp_prompts_front=all (default) | player

bool InBodyColumn(pose::Vec3 head, pose::Vec3 p) {
    const float dx = p.x - head.x, dz = p.z - head.z, dy = p.y - head.y;
    return dx * dx + dz * dz <= 0.45f * 0.45f && dy >= -1.9f && dy <= 0.6f;
}

// 0.36.12: the 2D prompts of the last frame, gathered at the screen centre in first person
std::mutex g_2dMutex;
uint64_t g_2dFrameMs = 0;
float g_2dSum[2] = {0.f, 0.f}, g_2dMean[2] = {0.f, 0.f};
int g_2dCount = 0;
std::atomic<uint64_t> g_2dMoved{0};
bool WriteFloats(void* display, size_t off, const float* v, int n) {
    __try {
        std::memcpy(static_cast<uint8_t*>(display) + off, v, size_t(n) * sizeof(float));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool WriteMatrix(void* display, const float* m) {
    __try {
        std::memcpy(static_cast<uint8_t*>(display) + 0x30, m, 16 * sizeof(float));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The front point the player's prompts gather round: in the gaze's heading, following it lazily like the UI screen
// (0.8.0, owner: some prompts "move with my head" - 0.7.0's world lock jumped after the gaze): it turns towards the
// gaze once that is 30 deg away and settles within 3 deg (tau 0.25 s); height from the eye.
struct FrontFollow {
    float yaw = 0.f;
    bool have = false, turning = false;
    uint64_t ms = 0;
    pose::Vec3 Point(pose::Vec3 eye, pose::Vec3 gaze, uint64_t now) {
        const float g = std::atan2(-gaze.x, -gaze.z);  // 0 = -Z
        const float dt = ms && now > ms ? std::min(float(now - ms) / 1000.f, 0.1f) : 0.f;
        ms = now;
        if (!have) {
            yaw = g;
            have = true;
        }
        const float diff = std::remainder(g - yaw, 6.28318531f);
        if (std::fabs(diff) > 0.52f) turning = true;
        if (turning) {
            yaw = std::remainder(yaw + diff * (1.f - std::exp(-dt / 0.25f)), 6.28318531f);
            if (std::fabs(diff) < 0.05f) turning = false;
        }
        return {eye.x - std::sin(yaw) * kPromptDistance, eye.y - kPromptDrop, eye.z - std::cos(yaw) * kPromptDistance};
    }
} g_front;

// Rows of `m` turned to face `eye` from `at` (row 2 towards the viewer, row 1 up: the game's billboard mode 1),
// each row keeping its length; false when degenerate.
bool FaceEye(const float* m, pose::Vec3 at, pose::Vec3 eye, float* out) {
    float z[3] = {eye.x - at.x, eye.y - at.y, eye.z - at.z};
    const float zl = std::sqrt(z[0] * z[0] + z[1] * z[1] + z[2] * z[2]);
    if (!(zl > 1e-3f)) return false;
    for (float& c : z) c /= zl;
    float y[3] = {-z[1] * z[0], 1.f - z[1] * z[1], -z[1] * z[2]};  // world up made square to z
    const float yl = std::sqrt(y[0] * y[0] + y[1] * y[1] + y[2] * y[2]);
    if (!(yl > 1e-3f)) return false;
    for (float& c : y) c /= yl;
    const float x[3] = {y[1] * z[2] - y[2] * z[1], y[2] * z[0] - y[0] * z[2], y[0] * z[1] - y[1] * z[0]};
    const float* rows[3] = {x, y, z};
    std::memcpy(out, m, 16 * sizeof(float));
    for (int r = 0; r < 3; ++r) {
        const float len = std::sqrt(m[r * 4] * m[r * 4] + m[r * 4 + 1] * m[r * 4 + 1] + m[r * 4 + 2] * m[r * 4 + 2]);
        for (int c = 0; c < 3; ++c) out[r * 4 + c] = rows[r][c] * (len > 1e-4f ? len : 1.f);
    }
    return true;
}

void HookMparUpdate(void* owner, void* record) {
    g_origMparUpdate(owner, record);
    // STATIC 2026-10-29: every prompt record the game draws passes here - the list 0x1415ba070 holds one per active
    // USER_ACTION (added by 0x14023d180 -> 0x14023dc20 on activation, removed by USER_ACTION vfunc 0x140239c90 ->
    // 0x14023cec0), and movie QTEs (MOVIE_USER_ACTION_CONDITION 0x1402c7b70 -> 0x14022cfa0 -> 0x14023d180) are
    // USER_ACTIONs too: hold, mash, loop. They need not be in the MPAR_GUI lists (0x1404df570 adds a UA there only when
    // 0x14022df20 allows it), so the lists read 0 + 0 through a mash (owner's log 135-142 s) - this time is the signal.
    if (record) g_lastElement = GetTickCount64();
    View v;
    {
        std::lock_guard lock(g_viewMutex);
        v = g_view;
    }
    const uint64_t now = GetTickCount64();
    if (!g_enabled || !v.firstPerson || now - v.ms > 250 || !record) return;
    void* display = obj::Ptr(record, 0x10);
    float m[16];
    if (!display || !obj::Bytes(display, 0x30, m, sizeof(m))) return;
    const pose::Vec3 at{m[12], m[13], m[14]};
    if (!std::isfinite(at.x) || !std::isfinite(at.y) || !std::isfinite(at.z)) return;
    uint8_t mode3d = 0;
    int billboard = 0;
    obj::Get(display, 7, mode3d);
    obj::Get(record, 0x20, billboard);
    const bool onPlayer = InBodyColumn(v.head, at);
    if (!mode3d) {
        // 0.36.12 (owner: "make prompts where you need to hold a button or spam a button appear infront of you"; "the
        // text ("To interact, ...") and the png icon ... appear infront of you"). STATIC 2026-11-20 (0x14023fdb0 and the
        // element's property table 0x140375540): a 2D prompt - the button prompts (input types STANDARD / HOLD / LOOP /
        // MASH) and the help / tutorial texts, drawn by their MenusMaster menu (element +0x160 "MenuMasterData": InGame,
        // HeadHelp - whose pictures are the MC_TUTO_INFO1..21 controller / keyboard / mouse icons) - is placed on the
        // 2D layer by display +0x0C / +0x10 = its centre's offset from the screen centre (fractions of the screen; the
        // game sets them from ScreenX / ScreenY, element +0xAC / +0xB0, or from the attached bone projected through
        // the GAME camera, so in first person they sat wherever that bone was on the flat screen) and +0x14 / +0x18 =
        // its size. In first person the 2D layer follows the gaze (xr SetUiFollow), so the prompts of a frame are
        // gathered at the screen centre - straight ahead - keeping their arrangement (a button combination, the hold
        // meter beside its button) at 60 % of its spread. fp_prompts_front=player keeps the game's places.
        float f[4] = {};
        if (g_allFront.load(std::memory_order_relaxed) && obj::Bytes(display, 0x0C, f, sizeof(f)) && std::isfinite(f[0]) &&
            std::isfinite(f[1]) && std::fabs(f[0]) < 4.f && std::fabs(f[1]) < 4.f) {
            float mean[2];
            {
                std::lock_guard lock(g_2dMutex);
                if (now - g_2dFrameMs >= 8) {  // a new frame: last frame's centre of all 2D prompts
                    if (g_2dCount) {
                        g_2dMean[0] = g_2dSum[0] / float(g_2dCount);
                        g_2dMean[1] = g_2dSum[1] / float(g_2dCount);
                    } else {
                        g_2dMean[0] = f[0];
                        g_2dMean[1] = f[1];
                    }
                    g_2dSum[0] = g_2dSum[1] = 0.f;
                    g_2dCount = 0;
                    g_2dFrameMs = now;
                }
                g_2dSum[0] += f[0];
                g_2dSum[1] += f[1];
                ++g_2dCount;
                mean[0] = g_2dMean[0];
                mean[1] = g_2dMean[1];
            }
            const float to[2] = {std::clamp((f[0] - mean[0]) * 0.6f, -0.2f, 0.2f),
                                 std::clamp((f[1] - mean[1]) * 0.6f, -0.2f, 0.2f)};
            WriteFloats(display, 0x0C, to, 2);
            g_2dMoved.fetch_add(1, std::memory_order_relaxed);
        }
        static uint64_t lastLog = 0;
        if (now - lastLog >= 5000) {
            lastLog = now;
            char menu[64] = "?";
            if (void* element = obj::Ptr(record, 0x08))
                if (const auto* h = static_cast<const uint8_t*>(obj::Ptr(element, 0x160))) obj::String(h + 0x18, menu, sizeof(menu));
            Log("PROMPTS 2D prompt (menu \"%s\") %s: screen offset %.3f %.3f size %.3f %.3f -> gathered at the screen centre "
                "in first person (%llu moves so far)",
                menu, onPlayer ? "on the player" : "elsewhere", f[0], f[1], f[2], f[3],
                static_cast<unsigned long long>(g_2dMoved.load()));
        }
        return;
    }
    float out[16];
    pose::Vec3 to = at;
    const bool bring = onPlayer || g_allFront.load(std::memory_order_relaxed);
    if (bring) {
        pose::Vec3 point;
        float frontYaw;
        {
            std::lock_guard lock(g_mparMutex);
            if (now - g_mparLockMs >= 8) {  // once per frame for every prompt of the cluster
                g_mparPoint = g_front.Point(v.eye, v.gaze, now);
                g_mparLockMs = now;
            }
            point = g_mparPoint;
            frontYaw = g_front.yaw;
        }
        if (onPlayer) {
            // Keep the prompts' arrangement round the head, gathered at the front point - compressed into the view
            // (0.14.0, owner: "the pressing and holding prompts or smash prompt repeatedly not in front of me"; log:
            // hold / mash prompts sit up to 1.25 m from the head, round the body down to the hands, and 0.13 carried
            // that whole offset to the front point - half a metre low, or off to the side). Only the sideways part
            // (across the view) and the height are kept, scaled down and limited, so every one stays in front.
            const float lx = -std::cos(frontYaw), lz = std::sin(frontYaw);  // the front direction's left
            const float dx = at.x - v.head.x, dz = at.z - v.head.z;
            const float side = std::clamp((dx * lx + dz * lz) * 0.6f, -0.22f, 0.22f);
            const float up = std::clamp((at.y - v.head.y) * 0.25f, -0.14f, 0.1f);
            to = {point.x + lx * side, point.y + up, point.z + lz * side};
        } else {
            // 0.12.0 (owner: "not all button prompts, like swiping the joystick down or up, show in front of me"): the
            // prompts on objects come to the front point too, keeping their direction as a small offset - an object
            // to the left shows its prompt left of the centre (up to 25 cm at 40 deg and more), one above or below a
            // little higher or lower.
            const float dyaw = std::clamp(std::remainder(std::atan2(-(at.x - v.eye.x), -(at.z - v.eye.z)) - frontYaw,
                                                         6.28318531f), -0.7f, 0.7f);
            const float side = std::sin(dyaw) * 0.38f;           // + = left of the front direction
            const float lx = -std::cos(frontYaw), lz = std::sin(frontYaw);  // the front direction's left
            const float up = std::clamp(at.y - v.eye.y, -0.6f, 0.6f) * 0.25f;
            to = {point.x + lx * side, point.y + up + kPromptDrop * 0.5f, point.z + lz * side};
        }
    }
    // Every 3D prompt faces the first-person eye: the game turns them to its own camera, which in first person is
    // somewhere else, so from the eye they could be seen edge-on.
    if (!FaceEye(m, to, v.eye, out)) std::memcpy(out, m, sizeof(out));
    out[12] = to.x;
    out[13] = to.y;
    out[14] = to.z;
    if (!WriteMatrix(display, out)) return;
    if (!onPlayer) {
        g_mparKept.fetch_add(1, std::memory_order_relaxed);
        static uint64_t lastObj = 0;
        if (bring && now - lastObj >= 5000) {
            lastObj = now;
            Log("PROMPTS action prompt on an object (%.2f m from the eye) shown in front of the view",
                std::sqrt((at.x - v.eye.x) * (at.x - v.eye.x) + (at.y - v.eye.y) * (at.y - v.eye.y) + (at.z - v.eye.z) * (at.z - v.eye.z)));
        }
        return;
    }
    const uint64_t moved = g_mparMoved.fetch_add(1, std::memory_order_relaxed);
    static uint64_t lastLog = 0;
    if (!moved || now - lastLog >= 5000) {
        lastLog = now;
        Log("PROMPTS action prompt / help text on the player (%.2f m from the head, billboard %d) shown %.2f m in front "
            "of the view (%llu moved, %llu turned to the eye elsewhere so far)",
            std::sqrt((at.x - v.head.x) * (at.x - v.head.x) + (at.y - v.head.y) * (at.y - v.head.y) +
                      (at.z - v.head.z) * (at.z - v.head.z)),
            billboard, kPromptDistance, static_cast<unsigned long long>(moved + 1),
            static_cast<unsigned long long>(g_mparKept.load()));
    }
}

}  // namespace

bool Install() {
    if (void* m = game::Addr(game::Anchor::MparUpdateElement)) {
        const MH_STATUS ms = MH_CreateHook(m, reinterpret_cast<void*>(&HookMparUpdate), reinterpret_cast<void**>(&g_origMparUpdate));
        const MH_STATUS me = ms == MH_OK ? MH_EnableHook(m) : ms;
        Log("PROMPTS hook MPAR::UpdateElement %p %s", m, MH_StatusToString(me));
    } else {
        Log("PROMPTS MPAR element update not found in this build: action prompts stay where the game puts them");
    }
    void* p = game::Addr(game::Anchor::ThoughtsSetAnchor);
    if (!p) {
        Log("PROMPTS thoughts anchor not found in this build: thoughts stay at the character's head");
        return false;
    }
    const MH_STATUS s = MH_CreateHook(p, reinterpret_cast<void*>(&HookSetAnchor), reinterpret_cast<void**>(&g_origSetAnchor));
    const MH_STATUS e = s == MH_OK ? MH_EnableHook(p) : s;
    Log("PROMPTS hook THOUGHTS_ANIMATOR::SetAnchor %p %s", p, MH_StatusToString(e));
    return e == MH_OK;
}

void SetAllFront(bool on) { g_allFront = on; }

void SetEnabled(bool on) {
    g_enabled = on;
    Log("PROMPTS first-person placement %s", on ? "on" : "off");
}
bool Enabled() { return g_enabled; }

void PublishView(bool firstPerson, const pose::Vec3& eye, const pose::Vec3& gaze, const pose::Vec3& head) {
    std::lock_guard lock(g_viewMutex);
    g_view = {firstPerson, eye, gaze, head, GetTickCount64()};
}

void SetMparGui(void* gui) {
    if (gui != g_mparGui.exchange(gui)) Log("PROMPTS MPAR_GUI %p", gui);
}

bool Showing() {
    void* gui = g_mparGui.load();
    uint32_t a = 0, b = 0;
    const bool showing = gui && obj::HasVtable(gui, game::vt::MparGui) && obj::Get(gui, kGuiCountA, a) &&
                         obj::Get(gui, kGuiCountB, b) && (a | b) != 0 && a < 64 && b < 64;
    if (showing) g_lastShown = GetTickCount64();
    static bool logged = false;
    if (showing != logged) {
        logged = showing;
        Log("PROMPTS action prompts %s (MPAR_GUI lists %u + %u)", showing ? "on offer" : "gone", a, b);
    }
    return showing;
}

uint64_t LastShownMs() { return g_lastShown; }
uint64_t LastElementMs() { return g_lastElement; }

}  // namespace hrvr::prompts
