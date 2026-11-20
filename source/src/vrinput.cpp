// VR controllers as a gamepad (derived from the B2S VR mod, MIT).
//
// Heavy Rain (Steam, 2020) reads gamepads only through ISteamController v007 (PC_CONTROLLER_STEAM;
// no XInput import). One controller object polls every Steam pad: GetConnectedControllers fills
// its handle array (+0xF0), the first non-empty enumeration loads the "GamepadSetting" action set
// (+0x38), 16 digital (+0x40) and 2 analog (+0xC0) action handles, and the pad with input becomes
// the active one (+0xE8). Prompts follow GetInputTypeForHandle (5 = PlayStation glyphs, otherwise
// Xbox) and, for types 1 and 5, GetMotionData's posAccel feeds SensorX/Y/Z.
//
// The VR controllers are one more Steam pad: a per-object shadow vtable on the game's
// ISteamController answers for a synthetic handle; real pads pass through untouched.
#include "vrinput.h"

#include "panel.h"

#include <windows.h>
#include <Xinput.h>
#include <MinHook.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>

#include "game.h"
#include "haptics_math.h"
#include "log.h"
#include "steam_shadow.h"
#include "welcome.h"
#include "xinput_pad.h"

namespace hrvr::vrinput {
namespace {
constexpr uint64_t kController = 0x4852000000000001ull;
constexpr uint64_t kActionSet = 0x4852300000000001ull;
constexpr uint64_t kDigitalBase = 0x4852100000000000ull;
constexpr uint64_t kAnalogBase = 0x4852200000000000ull;
constexpr int kMaxControllers = 16;
constexpr int kDigitalCount = 16;

// Heavy Rain's digital action names, in the order of its handle table (exe 0x1413c9380):
// x_button, y_button, a_button, b_button, back_button, start_button, dpad_up, dpad_down,
// dpad_left, dpad_right, left_bumper, left_trigger, left_click, right_bumper, right_trigger,
// right_click. Mapped to the B2S button cases (Button() below).
constexpr const char* kDigitalNames[kDigitalCount] = {
    "x_button", "y_button", "a_button", "b_button", "back_button", "start_button", "dpad_up", "dpad_down",
    "dpad_left", "dpad_right", "left_bumper", "left_trigger", "left_click", "right_bumper", "right_trigger",
    "right_click"};
constexpr int kToButton[kDigitalCount] = {8, 9, 6, 7, 1, 0, 2, 3, 4, 5, 10, 11, 12, 13, 14, 15};
constexpr uintptr_t kCtlActionSet = 0x38, kCtlDigital = 0x40, kCtlAnalog = 0xC0;

struct DigitalData { uint8_t state, active; };
struct AnalogData { int mode; float x, y; uint8_t active; uint8_t padding[3]; };
// Steam InputMotionData_t: rotQuat x y z w, posAccel x y z, rotVel x y z.
struct MotionData { float rotQuat[4], posAccel[3], rotVel[3]; };
static_assert(sizeof(DigitalData) == 2 && sizeof(AnalogData) == 16 && sizeof(MotionData) == 40);

using EnumerateFn = uint64_t (*)(void* controller);
using Connected = int (*)(void*, uint64_t*);
using HandleByName = uint64_t (*)(void*, const char*);
using Activate = void (*)(void*, uint64_t, uint64_t);
using DeactivateLayers = void (*)(void*, uint64_t);
using Digital = DigitalData* (*)(void*, DigitalData*, uint64_t, uint64_t);
using Analog = AnalogData* (*)(void*, AnalogData*, uint64_t, uint64_t);
using Motion = MotionData* (*)(void*, MotionData*, uint64_t);
using Vibration = void (*)(void*, uint64_t, uint16_t, uint16_t);
using InputType = int (*)(void*, uint64_t);
EnumerateFn g_enumerate = nullptr;
Connected g_connected = nullptr;
HandleByName g_setHandle = nullptr, g_digitalHandle = nullptr, g_analogHandle = nullptr;
Activate g_activate = nullptr;
DeactivateLayers g_deactivateLayers = nullptr;
Digital g_digital = nullptr;
Analog g_analog = nullptr;
Motion g_motion = nullptr;
Vibration g_vibration = nullptr;
InputType g_inputType = nullptr;
void* g_context = nullptr;
std::atomic<void*> g_gameController{nullptr};  // PC_CONTROLLER_STEAM
detail::ShadowTable g_shadow;
std::atomic<bool> g_contextReady{false};
std::atomic<uint64_t> g_digitalQueries{0}, g_digitalActive{0};
std::atomic<uint64_t> g_analogQueries{0}, g_analogNonzero{0}, g_synthHandles{0};
std::mutex g_installMutex, g_sampleMutex;
Sample g_sample{};
std::atomic<int> g_padType{2};
std::atomic<bool> g_motionControls{true};  // motion_controls: the right controller is the PlayStation motion sensor
std::atomic<float> g_countsPerG{16384.f};
uint64_t g_sampleTick = 0;
thread_local bool g_enumerating = false;

bool ExecutableSteamFunction(void* function) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!function || !VirtualQuery(function, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT ||
        !(mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) ||
        (mbi.Protect & PAGE_GUARD))
        return false;
    wchar_t path[MAX_PATH]{};
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(function), &module) ||
        !GetModuleFileNameW(module, path, MAX_PATH))
        return false;
    const wchar_t* name = wcsrchr(path, L'\\');
    name = name ? name + 1 : path;
    return _wcsicmp(name, L"steam_api64.dll") == 0 || _wcsicmp(name, L"steamclient64.dll") == 0;
}

// STATIC: `lea rcx,[ctx]; call [SteamInternal_ContextInit]` (kernelinput_1401e0010). The returned
// wrapper's first qword is the game's ISteamController*.
void* ReadSteamContext() {
    __try {
        auto* code = static_cast<uint8_t*>(game::Addr(game::Anchor::SteamContextAccess));
        if (!code) return nullptr;
        void* data = code + 7 + *reinterpret_cast<int32_t*>(code + 3);
        auto* import = code + 13 + *reinterpret_cast<int32_t*>(code + 9);
        using Init = void** (*)(void*);
        auto init = *reinterpret_cast<Init*>(import);
        if (!ExecutableSteamFunction(reinterpret_cast<void*>(init))) return nullptr;
        void** wrapper = init(data);
        return wrapper ? *wrapper : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

bool ReadMethods(void* context, void** methods, void*** originalTable) {
    __try {
        void** vt = *reinterpret_cast<void***>(context);
        *originalTable = vt;
        for (size_t i = 0; i < detail::ShadowTable::kOverrides; ++i) {
            methods[i] = vt[detail::kOverrideSlots[i]];
            if (!ExecutableSteamFunction(methods[i])) return false;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool BuildAndPublishTable(void* context, void** expected, void* const* detours) {
    __try {
        g_shadow.Build(expected, detours);
        DWORD oldProtect = 0;
        if (!VirtualProtect(context, sizeof(void*), PAGE_READWRITE, &oldProtect)) return false;
        auto* slot = reinterpret_cast<void* volatile*>(context);
        void* old = InterlockedCompareExchangePointer(slot, g_shadow.Table(), expected);
        DWORD ignored = 0;
        VirtualProtect(context, sizeof(void*), oldProtect, &ignored);
        return old == expected;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool Synthetic(void* context, uint64_t handle) {
    return g_contextReady.load(std::memory_order_acquire) && handle == kController && context == g_context;
}

// The game's own action handle table (filled by its handle loader) maps a queried action to its
// index; synthetic handles (returned when Steam has no binding for an action) map directly.
int DigitalIndex(uint64_t action) {
    if (action > kDigitalBase && action <= kDigitalBase + kDigitalCount) return int(action - kDigitalBase - 1);
    __try {
        if (auto* ctl = static_cast<const uint8_t*>(g_gameController.load()))
            for (int i = 0; i < kDigitalCount; ++i)
                if (action && game::Read<uint64_t>(ctl, kCtlDigital + 8 * i) == action) return i;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return -1;
}
int AnalogIndex(uint64_t action) {
    if (action == kAnalogBase + 1) return 0;
    if (action == kAnalogBase + 2) return 1;
    __try {
        if (auto* ctl = static_cast<const uint8_t*>(g_gameController.load()))
            for (int i = 0; i < 2; ++i)
                if (action && game::Read<uint64_t>(ctl, kCtlAnalog + 8 * i) == action) return i;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return -1;
}

int HookConnected(void* context, uint64_t* handles) {
    const int count = g_connected(context, handles);
    if (!g_enumerating || !g_contextReady.load(std::memory_order_acquire) || context != g_context ||
        !xr::Running() || !handles || count < 0 || count >= kMaxControllers)
        return count;
    for (int i = 0; i < count; ++i)
        if (handles[i] == kController) return count;
    handles[count] = kController;
    return count + 1;
}
// Steam without a binding for an action returns 0, and the game then shows a modal "Load Steam
// Controller Configuration File Failed" box. While the VR pad is plugged in, a missing handle is
// answered with a synthetic one that only the VR pad uses.
uint64_t Synthesize(uint64_t real, uint64_t synthetic) {
    if (real || !g_enumerating || !xr::Running()) return real;
    g_synthHandles.fetch_add(1, std::memory_order_relaxed);
    return synthetic;
}
uint64_t HookSetHandle(void* context, const char* name) {
    const uint64_t r = g_setHandle(context, name);
    return context == g_context ? Synthesize(r, kActionSet) : r;
}
uint64_t HookDigitalHandle(void* context, const char* name) {
    const uint64_t r = g_digitalHandle(context, name);
    if (context != g_context || r || !name) return r;
    for (int i = 0; i < kDigitalCount; ++i)
        if (_stricmp(name, kDigitalNames[i]) == 0) return Synthesize(r, kDigitalBase + i + 1);
    return r;
}
uint64_t HookAnalogHandle(void* context, const char* name) {
    const uint64_t r = g_analogHandle(context, name);
    if (context != g_context || r || !name) return r;
    if (_stricmp(name, "left_axis_abs") == 0) return Synthesize(r, kAnalogBase + 1);
    if (_stricmp(name, "right_axis_abs") == 0) return Synthesize(r, kAnalogBase + 2);
    return r;
}
void HookActivate(void* context, uint64_t handle, uint64_t set) {
    if (!Synthetic(context, handle) && set != kActionSet) g_activate(context, handle, set);
}
void HookDeactivateLayers(void* context, uint64_t handle) {
    if (!Synthetic(context, handle)) g_deactivateLayers(context, handle);
}

// B2S button cases: 0 start, 1 back, 2-5 dpad up/down/left/right, 6 A, 7 B, 8 X, 9 Y,
// 10 left bumper, 11 left trigger, 12 left stick click, 13 right bumper, 14 right trigger,
// 15 right stick click.
bool Button(const LegacyState& state, int which) {
    switch (which) {
        // Touch-style controllers (Quest, PSVR2 through SteamVR) have one menu button, on the
        // left hand: it is Start (pause). Held left grip + menu is Back.
        case 0: return (state.buttons & kRightMenu) != 0 ||
                       ((state.buttons & kLeftMenu) != 0 && (state.buttons & kLeftSqueeze) == 0);
        case 1: return (state.buttons & kLeftMenu) != 0 && (state.buttons & kLeftSqueeze) != 0;
        case 2: return (state.buttons & kDpadUp) != 0;
        case 3: return (state.buttons & kDpadDown) != 0;
        case 4: return (state.buttons & kDpadLeft) != 0;
        case 5: return (state.buttons & kDpadRight) != 0;
        case 6: return (state.buttons & kRightA) != 0;
        case 7: return (state.buttons & kRightB) != 0;
        case 8: return (state.buttons & kLeftA) != 0;
        case 9: return (state.buttons & kLeftB) != 0;
        case 10: return (state.buttons & (kLeftGrip | kLeftSqueeze)) != 0;
        case 11: return (state.buttons & kLeftTriggerButton) != 0;
        case 12: return (state.buttons & kLeftStickClick) != 0;
        case 13: return (state.buttons & (kRightGrip | kRightSqueeze)) != 0;
        case 14: return (state.buttons & kRightTriggerButton) != 0;
        case 15: return (state.buttons & kRightStickClick) != 0;
        default: return false;
    }
}
// Index of "a_button" in kDigitalNames: a pad's confirm button (A / Cross), the welcome windows' CONTINUE (welcome.h).
constexpr int kConfirmIndex = 2;
DigitalData* HookDigital(void* context, DigitalData* output, uint64_t handle, uint64_t action) {
    if (!Synthetic(context, handle)) {
        DigitalData* r = g_digital(context, output, handle, action);
        // A real pad (Xbox, or any pad through Steam Input): its confirm button closes a welcome window and is kept
        // from the game while a window is up and until released after the last one.
        if (r && context == g_context && DigitalIndex(action) == kConfirmIndex) {
            welcome::NotePadConfirm(r->state != 0);
            if (r->state && welcome::TakesPadConfirm()) r->state = 0;
        }
        return r;
    }
    g_digitalQueries.fetch_add(1, std::memory_order_relaxed);
    const int index = DigitalIndex(action);
    // While a welcome window is up the VR controllers give the game nothing (playbook 11.4: mandatory).
    const bool pressed = index >= 0 && !welcome::Active() && Button(Snapshot(), kToButton[index]);
    if (pressed) g_digitalActive.fetch_add(1, std::memory_order_relaxed);
    *output = {uint8_t(pressed), uint8_t(index >= 0)};
    return output;
}
AnalogData* HookAnalog(void* context, AnalogData* output, uint64_t handle, uint64_t action) {
    if (!Synthetic(context, handle)) return g_analog(context, output, handle, action);
    g_analogQueries.fetch_add(1, std::memory_order_relaxed);
    const LegacyState state = Snapshot();
    const int index = AnalogIndex(action);
    *output = {6, 0.f, 0.f, uint8_t(index >= 0), {0, 0, 0}};  // joystick-move mode
    if (welcome::Active()) return output;  // a welcome window is up: the VR controllers give the game nothing
    if (index == 0) { output->x = state.moveX; output->y = state.moveY; }
    else if (index == 1) { output->x = state.viewX; output->y = state.viewY; }
    if (output->x != 0.f || output->y != 0.f) g_analogNonzero.fetch_add(1, std::memory_order_relaxed);
    return output;
}

// PlayStation motion sensor. STATIC (kernelinput_140b4d220): for a pad of type 1 or 5 the game
// reads GetMotionData's posAccel, divides each axis by 16383 and clamps to +-2 (Steam's +-2 g
// range, 16384 counts per g), then SensorX = -ax/3, SensorY = -ay/3, SensorZ = az/3. The right
// VR controller is that pad: its aim frame (X right, Y up, Z toward the player, as a DualShock
// held in front of you) reads the specific force, so tilting it or moving it up / down / left /
// right reads as on a real pad. Only while the pad is PlayStation (F11); Xbox keeps a still pad.
struct MotionState {
    bool valid = false;
    uint64_t tick = 0;
    float pos[3]{}, vel[3]{}, acc[3]{};  // LOCAL space, metres / s / s^2 (smoothed)
    float force[3]{};                    // specific force in the controller frame, g
    float quat[4]{0.f, 0.f, 0.f, 1.f};
} g_motionState;
std::mutex g_motionMutex;
std::atomic<uint64_t> g_motionReads{0};

void UpdateMotion(const xr::FrameInput& frame) {
    const xr::HandInput& h = frame.hand[1].aimValid ? frame.hand[1] : frame.hand[0];
    std::lock_guard lock(g_motionMutex);
    MotionState& m = g_motionState;
    if (!h.aimValid || !frame.tickMs) {
        m.valid = false;
        return;
    }
    const float* a = h.aimRel.m;
    const float p[3] = {a[12], a[13], a[14]};
    const float dt = m.valid && frame.tickMs > m.tick ? std::min(float(frame.tickMs - m.tick) / 1000.f, 0.1f) : 0.f;
    if (dt > 0.f) {
        const float k = 1.f - std::exp(-dt / 0.03f);  // ~30 ms smoothing
        for (int i = 0; i < 3; ++i) {
            const float v = (p[i] - m.pos[i]) / dt;
            const float acc = (v - m.vel[i]) / dt;
            m.vel[i] += (v - m.vel[i]) * k;
            m.acc[i] += (std::clamp(acc, -30.f, 30.f) - m.acc[i]) * k;
        }
    } else {
        for (int i = 0; i < 3; ++i) m.vel[i] = m.acc[i] = 0.f;
    }
    for (int i = 0; i < 3; ++i) m.pos[i] = p[i];
    m.tick = frame.tickMs;
    m.valid = true;
    const float f[3] = {m.acc[0] / 9.81f, 1.f + m.acc[1] / 9.81f, m.acc[2] / 9.81f};
    for (int r = 0; r < 3; ++r) {
        const float ax = a[r * 4], ay = a[r * 4 + 1], az = a[r * 4 + 2];
        const float len = std::sqrt(ax * ax + ay * ay + az * az);
        m.force[r] = len > 1e-4f ? (f[0] * ax + f[1] * ay + f[2] * az) / len : 0.f;
    }
    const float r00 = a[0], r01 = a[1], r02 = a[2], r10 = a[4], r11 = a[5], r12 = a[6], r20 = a[8], r21 = a[9], r22 = a[10];
    const float tr = r00 + r11 + r22;
    float q[4];
    if (tr > 0.f) {
        const float sq = std::sqrt(tr + 1.f) * 2.f;
        q[3] = 0.25f * sq; q[0] = (r12 - r21) / sq; q[1] = (r20 - r02) / sq; q[2] = (r01 - r10) / sq;
    } else if (r00 > r11 && r00 > r22) {
        const float sq = std::sqrt(1.f + r00 - r11 - r22) * 2.f;
        q[3] = (r12 - r21) / sq; q[0] = 0.25f * sq; q[1] = (r01 + r10) / sq; q[2] = (r20 + r02) / sq;
    } else if (r11 > r22) {
        const float sq = std::sqrt(1.f + r11 - r00 - r22) * 2.f;
        q[3] = (r20 - r02) / sq; q[0] = (r01 + r10) / sq; q[1] = 0.25f * sq; q[2] = (r12 + r21) / sq;
    } else {
        const float sq = std::sqrt(1.f + r22 - r00 - r11) * 2.f;
        q[3] = (r01 - r10) / sq; q[0] = (r20 + r02) / sq; q[1] = (r12 + r21) / sq; q[2] = 0.25f * sq;
    }
    const float ql = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (ql > 1e-4f && std::isfinite(ql))
        for (int i = 0; i < 4; ++i) m.quat[i] = q[i] / ql;
}

MotionData* HookMotion(void* context, MotionData* output, uint64_t handle) {
    if (!Synthetic(context, handle)) return g_motion(context, output, handle);
    *output = {};
    output->rotQuat[3] = 1.f;
    const float perG = g_countsPerG.load();
    if (g_padType.load(std::memory_order_relaxed) != 5) return output;
    if (!g_motionControls.load(std::memory_order_relaxed)) {
        output->posAccel[1] = perG;  // motion controls off: a pad lying still and flat
        return output;
    }
    std::lock_guard lock(g_motionMutex);
    const MotionState& m = g_motionState;
    const uint64_t now = GetTickCount64();
    if (!m.valid || now < m.tick || now - m.tick > 250) {
        output->posAccel[1] = perG;  // a pad lying still and flat
        return output;
    }
    for (int i = 0; i < 4; ++i) output->rotQuat[i] = m.quat[i];
    for (int i = 0; i < 3; ++i) output->posAccel[i] = std::clamp(m.force[i] * perG, -32767.f, 32767.f);
    if (g_motionReads.fetch_add(1, std::memory_order_relaxed) == 0)
        Log("VRINPUT PlayStation motion: the right controller is the pad's motion sensor (accel %.0f %.0f %.0f)",
            output->posAccel[0], output->posAccel[1], output->posAccel[2]);
    return output;
}
// The game's rumble for the VR pad (playbook 02.7 / REQ-05). STATIC: PC_CONTROLLER_STEAM 0x140b4de70 sends the active
// pad (+0xE8) one level for both motors, set every frame by the running PAD_VIBRATION_FX and 0 when it ends (haptics_math.h).
// A real pad that is the active one keeps its own rumble; the VR controllers get only what is sent to the VR pad.
std::atomic<uint64_t> g_rumbleCalls{0}, g_rumbleOn{0};
void HookVibration(void* context, uint64_t handle, uint16_t right, uint16_t left) {
    if (!Synthetic(context, handle)) {
        g_vibration(context, handle, right, left);
        return;
    }
    const float level = haptics::FromSteamSpeed(left, right);
    xr::SetRumble(level);
    g_rumbleCalls.fetch_add(1, std::memory_order_relaxed);
    static std::atomic<bool> wasOn{false};
    const bool on = level > 0.f;
    if (on != wasOn.exchange(on) && g_rumbleOn.fetch_add(1, std::memory_order_relaxed) < 40)
        Log("VRINPUT rumble from the game %s (level %.2f)", on ? "on" : "off", level);
}
int HookInputType(void* context, uint64_t handle) {
    // 5 -> PlayStation glyphs and motion; 2 (Xbox) -> the ordinary gamepad path.
    return Synthetic(context, handle) ? g_padType.load() : g_inputType(context, handle);
}

bool InstallContextHooks() {
    if (g_contextReady.load(std::memory_order_acquire)) return true;
    std::lock_guard lock(g_installMutex);
    if (g_contextReady.load(std::memory_order_relaxed)) return true;
    void* context = ReadSteamContext();
    void* targets[detail::ShadowTable::kOverrides]{};
    void** originalTable = nullptr;
    if (!context || !ReadMethods(context, targets, &originalTable)) return false;
    // Order = detail::kOverrideSlots: 3, 4, 5, 9, 11, 12, 14, 15, 20, 23, 26.
    void* detours[]{reinterpret_cast<void*>(&HookConnected),     reinterpret_cast<void*>(&HookSetHandle),
                    reinterpret_cast<void*>(&HookActivate),      reinterpret_cast<void*>(&HookDeactivateLayers),
                    reinterpret_cast<void*>(&HookDigitalHandle), reinterpret_cast<void*>(&HookDigital),
                    reinterpret_cast<void*>(&HookAnalogHandle),  reinterpret_cast<void*>(&HookAnalog),
                    reinterpret_cast<void*>(&HookMotion),        reinterpret_cast<void*>(&HookVibration),
                    reinterpret_cast<void*>(&HookInputType)};
    void** originals[]{reinterpret_cast<void**>(&g_connected),     reinterpret_cast<void**>(&g_setHandle),
                       reinterpret_cast<void**>(&g_activate),      reinterpret_cast<void**>(&g_deactivateLayers),
                       reinterpret_cast<void**>(&g_digitalHandle), reinterpret_cast<void**>(&g_digital),
                       reinterpret_cast<void**>(&g_analogHandle),  reinterpret_cast<void**>(&g_analog),
                       reinterpret_cast<void**>(&g_motion),        reinterpret_cast<void**>(&g_vibration),
                       reinterpret_cast<void**>(&g_inputType)};
    static_assert(std::size(detours) == detail::ShadowTable::kOverrides);
    for (size_t i = 0; i < std::size(targets); ++i) *originals[i] = targets[i];
    if (!BuildAndPublishTable(context, originalTable, detours)) {
        static bool reported = false;
        if (!reported) {
            Log("VRINPUT Steam controller interface table unavailable; native input retained");
            reported = true;
        }
        return false;
    }
    g_context = context;
    g_contextReady.store(true, std::memory_order_release);
    Log("VRINPUT Steam controller interface virtualized with an instance table (VR controllers = one more pad)");
    return true;
}

uint64_t HookEnumerate(void* controller) {
    g_gameController = controller;
    const bool previous = g_enumerating;
    g_enumerating = InstallContextHooks();
    const uint64_t r = g_enumerate(controller);
    g_enumerating = previous;
    static bool logged = false;
    if (!logged && xr::Running() && g_contextReady) {
        logged = true;
        Log("VRINPUT first enumeration with VR running (synthetic action handles so far: %llu)",
            static_cast<unsigned long long>(g_synthHandles.load()));
    }
    return r;
}

std::wstring IniPath() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring s = path;
    return s.substr(0, s.find_last_of(L"\\/") + 1) + L"hrvr.ini";
}

// ---- 0.34.0: the XInput route for store builds without Steam (playbook 10.1; Beyond 0.16.7) ----
// The 2019 Epic exe imports no steam_api64.dll and reads pads through XINPUT1_3 by ordinal (2 GetState, 3 SetState;
// reverse_engineering/legacy_epic_2019/imports_exports.json), so the Steam route above can never reach it. Here the
// XInput functions are hooked INSIDE the XInput DLLs (MinHook), which sees every caller however the game imported them.
// Pad 1 (user 0) only, only while VR runs: the VR controllers are a connected pad of their own (a missing real pad
// reads as a zeroed, connected one); with a real pad the two are merged (xinput_pad.h); the mapping is the Steam
// route's (Button()), so both stores play the same. The game's XInputSetState rumble goes to the VR controllers.
// Limits: Xbox prompts only (no PlayStation glyphs or motion sensor without Steam's input type), as in Beyond.
using XGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
using XSetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_VIBRATION*);
using XGetCapsFn = DWORD(WINAPI*)(DWORD, DWORD, XINPUT_CAPABILITIES*);
struct XDll {
    const wchar_t* name = nullptr;
    XGetStateFn getState = nullptr, getStateEx = nullptr;
    XSetStateFn setState = nullptr;
    XGetCapsFn getCaps = nullptr;
};
XDll g_xdll[3];
std::atomic<bool> g_xRoute{false};
std::atomic<uint64_t> g_xPolls{0}, g_xActive{0}, g_xRumble{0}, g_xNoRealPad{0};
std::atomic<DWORD> g_xPacket{0};
std::mutex g_xMutex;
xpad::Pad g_xLastVr;

xpad::Pad VrPad() {
    if (welcome::Active()) return {};  // a welcome window is up: the VR controllers give the game nothing (11.4)
    const LegacyState s = Snapshot();  // empty while VR is off or the sample is older than 250 ms
    bool pressed[16];
    for (int i = 0; i < 16; ++i) pressed[i] = Button(s, i);
    return xpad::FromVr(pressed, s.moveX, s.moveY, s.viewX, s.viewY, s.leftTrigger, s.rightTrigger);
}

DWORD MergeState(int dll, bool ex, DWORD user, XINPUT_STATE* state) {
    const XGetStateFn orig = ex ? g_xdll[dll].getStateEx : g_xdll[dll].getState;
    const DWORD r = orig(user, state);
    if (user != 0 || !state) return r;
    const bool realOn = r == ERROR_SUCCESS;
    // counted and logged with VR off too: the proof that this build reads its pads here (playbook 10.1 step 3)
    if (g_xPolls.fetch_add(1, std::memory_order_relaxed) == 0)
        Log("VRINPUT the game polls pad 1 through %ls%s (real pad there: %s; VR %s)", g_xdll[dll].name,
            ex ? " (GetStateEx)" : "", realOn ? "yes" : "no", xr::Running() ? "running" : "off");
    if (!xr::Running()) return r;
    xpad::Pad real;
    DWORD packet = 0;
    if (realOn) {
        std::memcpy(&real, &state->Gamepad, sizeof(real));
        packet = state->dwPacketNumber;
        // A real pad's A closes a welcome window and is kept from the game meanwhile (as on the Steam route).
        const bool a = (real.buttons & xpad::kA) != 0;
        welcome::NotePadConfirm(a);
        if (a && welcome::TakesPadConfirm()) real.buttons = uint16_t(real.buttons & ~xpad::kA);
    } else {
        g_xNoRealPad.fetch_add(1, std::memory_order_relaxed);
    }
    const xpad::Pad vr = VrPad();
    {
        std::lock_guard lock(g_xMutex);
        if (std::memcmp(&vr, &g_xLastVr, sizeof(vr)) != 0) {  // the packet number advances while the VR pad changes
            g_xLastVr = vr;
            g_xPacket.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (!xpad::Empty(vr)) g_xActive.fetch_add(1, std::memory_order_relaxed);
    const xpad::Pad merged = xpad::Merge(real, vr);
    state->dwPacketNumber = packet + g_xPacket.load(std::memory_order_relaxed);
    std::memcpy(&state->Gamepad, &merged, sizeof(merged));
    return ERROR_SUCCESS;  // the VR controllers are a connected pad
}
template <int D> DWORD WINAPI HookXGetState(DWORD user, XINPUT_STATE* s) { return MergeState(D, false, user, s); }
template <int D> DWORD WINAPI HookXGetStateEx(DWORD user, XINPUT_STATE* s) { return MergeState(D, true, user, s); }
template <int D> DWORD WINAPI HookXGetCaps(DWORD user, DWORD flags, XINPUT_CAPABILITIES* caps) {
    const DWORD r = g_xdll[D].getCaps(user, flags, caps);
    if (user != 0 || !caps || r == ERROR_SUCCESS || !xr::Running()) return r;
    *caps = {};  // no real pad: describe the VR controllers as a full gamepad with rumble
    caps->Type = XINPUT_DEVTYPE_GAMEPAD;
    caps->SubType = XINPUT_DEVSUBTYPE_GAMEPAD;
    caps->Gamepad.wButtons = 0xF3FF;
    caps->Gamepad.bLeftTrigger = caps->Gamepad.bRightTrigger = 255;
    caps->Gamepad.sThumbLX = caps->Gamepad.sThumbLY = caps->Gamepad.sThumbRX = caps->Gamepad.sThumbRY = -64;
    caps->Vibration.wLeftMotorSpeed = caps->Vibration.wRightMotorSpeed = 0xFFFF;
    return ERROR_SUCCESS;
}
template <int D> DWORD WINAPI HookXSetState(DWORD user, XINPUT_VIBRATION* v) {
    const DWORD r = g_xdll[D].setState(user, v);
    if (user != 0 || !v || !xr::Running()) return r;
    const float level = xpad::RumbleLevel(v->wLeftMotorSpeed, v->wRightMotorSpeed);
    xr::SetRumble(level);
    g_xRumble.fetch_add(1, std::memory_order_relaxed);
    static std::atomic<bool> wasOn{false};
    const bool on = level > 0.f;
    if (on != wasOn.exchange(on) && g_rumbleOn.fetch_add(1, std::memory_order_relaxed) < 40)
        Log("VRINPUT rumble from the game %s (XInput, level %.2f)", on ? "on" : "off", level);
    return ERROR_SUCCESS;
}

template <int D> int HookOneDll(HMODULE mod, const wchar_t* name) {
    XDll& x = g_xdll[D];
    x.name = name;
    struct H {
        int ordinal;
        void* detour;
        void** orig;
    } hooks[] = {{2, reinterpret_cast<void*>(&HookXGetState<D>), reinterpret_cast<void**>(&x.getState)},
                 {3, reinterpret_cast<void*>(&HookXSetState<D>), reinterpret_cast<void**>(&x.setState)},
                 {4, reinterpret_cast<void*>(&HookXGetCaps<D>), reinterpret_cast<void**>(&x.getCaps)},
                 {100, reinterpret_cast<void*>(&HookXGetStateEx<D>), reinterpret_cast<void**>(&x.getStateEx)}};
    int ok = 0;
    char line[160] = {};
    for (const H& h : hooks) {
        void* target = reinterpret_cast<void*>(GetProcAddress(mod, MAKEINTRESOURCEA(h.ordinal)));
        bool hooked = false;
        if (target && MH_CreateHook(target, h.detour, h.orig) == MH_OK) {
            hooked = MH_EnableHook(target) == MH_OK;
            if (!hooked) MH_RemoveHook(target);
        }
        if (!hooked) *h.orig = nullptr;
        ok += hooked;
        const size_t n = std::strlen(line);
        std::snprintf(line + n, sizeof(line) - n, "%s%s=%d", n ? " " : "",
                      h.ordinal == 2 ? "GetState" : h.ordinal == 3 ? "SetState" : h.ordinal == 4 ? "GetCapabilities" : "GetStateEx",
                      int(hooked));
    }
    Log("VRINPUT XInput path %ls: %s", name, line);
    return ok;
}

bool InstallXInput() {
    // Every XInput DLL the game has loaded; none loaded yet: load 1_3 (the Epic exe's import) and 1_4, and hook both,
    // so whichever the game reads later is covered.
    const wchar_t* names[3] = {L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"};
    HMODULE mods[3] = {};
    int loaded = 0;
    for (int i = 0; i < 3; ++i) loaded += (mods[i] = GetModuleHandleW(names[i])) != nullptr;
    if (!loaded) {
        mods[0] = LoadLibraryW(names[0]);
        mods[1] = LoadLibraryW(names[1]);
        Log("VRINPUT no XInput DLL loaded by the game yet: the mod loaded %s%s", mods[0] ? "xinput1_4 " : "",
            mods[1] ? "xinput1_3" : "");
    }
    int hooked = 0;
    if (mods[0]) hooked += HookOneDll<0>(mods[0], names[0]);
    if (mods[1]) hooked += HookOneDll<1>(mods[1], names[1]);
    if (mods[2]) hooked += HookOneDll<2>(mods[2], names[2]);
    g_xRoute = hooked > 0;
    if (!hooked)
        Log("VRINPUT XInput route unavailable: VR controllers cannot reach the game (send hrvr.log: the build's pad route "
            "is unknown)");
    else
        Log("VRINPUT XInput route on: the VR controllers are pad 1 (Xbox prompts; a real pad is merged). No \"the game "
            "polls pad 1\" line after the main menu = this build does not read pads through XInput");
    return g_xRoute;
}
}  // namespace

bool Install() {
    std::lock_guard lock(g_installMutex);
    if (g_enumerate || g_xRoute) return true;
    {
        wchar_t value[32]{};
        GetPrivateProfileStringW(L"hrvr", L"controller", L"xbox", value, 32, IniPath().c_str());
        g_padType = _wcsicmp(value, L"playstation") == 0 ? 5 : 2;
        GetPrivateProfileStringW(L"hrvr", L"motion_controls", L"on", value, 32, IniPath().c_str());
        g_motionControls = _wcsicmp(value, L"off") != 0;
        GetPrivateProfileStringW(L"hrvr", L"motion_counts_per_g", L"16384", value, 32, IniPath().c_str());
        const float perG = float(_wtof(value));
        g_countsPerG = perG > 100.f && perG < 100000.f ? perG : 16384.f;
        GetPrivateProfileStringW(L"hrvr", L"rumble_strength", L"1.0", value, 32, IniPath().c_str());
        const float strength = std::clamp(float(_wtof(value)), 0.f, 2.f);
        xr::SetRumbleStrength(strength);
        Log("VRINPUT controller type from hrvr.ini: %s; motion %.0f counts per g; rumble_strength %.2f",
            g_padType.load() == 5 ? "PlayStation" : "Xbox", g_countsPerG.load(), strength);
    }
    // 0.34.0 (playbook 10.1): one input route per build. Steam copies read pads through Steam Input (the route below);
    // a copy without steam_api64.dll (Epic, GOG) or without the Steam controller code gets the XInput route.
    void* target = game::Addr(game::Anchor::SteamEnumerate);
    const bool steamApi = GetModuleHandleW(L"steam_api64.dll") != nullptr;
    if (!steamApi || !target || !game::Addr(game::Anchor::SteamContextAccess)) {
        Log("VRINPUT %s: the XInput route (playbook 10.1)",
            !steamApi ? "this copy of the game does not use Steam (no steam_api64.dll: Epic / GOG)"
                      : "the Steam controller code was not found in this build");
        return InstallXInput();
    }
    if (MH_CreateHook(target, reinterpret_cast<void*>(&HookEnumerate), reinterpret_cast<void**>(&g_enumerate)) != MH_OK ||
        MH_EnableHook(target) != MH_OK) {
        MH_RemoveHook(target);
        g_enumerate = nullptr;
        Log("VRINPUT Steam controller enumeration hook failed");
        return false;
    }
    Log("VRINPUT Steam controller enumeration hook installed");
    return true;
}
void Suspend() {
    std::lock_guard lock(g_sampleMutex);
    g_sample = {};
    g_sampleTick = 0;
}
FaceChord g_chord;
bool g_startPulse = false;
HeadSwitch g_headSwitch;
uint64_t g_headSwitchAt = 0;  // a switch waiting for fpcam (expires after 1 s)
void Publish(const xr::FrameInput& frame) {
    {
        std::lock_guard lock(g_sampleMutex);
        const uint64_t now = GetTickCount64();
        g_sample = SampleFromFrame(frame, now);
        g_sampleTick = frame.tickMs;
        if (g_sample.fresh && !welcome::Active()) {  // no gestures behind a welcome window
            const bool tracked = frame.hand[1].active && frame.hand[1].gripValid;
            const HeadZone zone = tracked ? HeadZoneOf(frame.headRel, frame.hand[1].gripRel) : HeadZone{};
            const bool atHead = InHeadZone(zone);
            const bool newPress = g_sample.rightB && !g_headSwitch.down;
            if (g_headSwitch.Feed(g_sample, atHead)) {
                g_headSwitchAt = now;
                xr::HapticPulse(1, 0.5f, 40);  // playbook 10.3: a short pulse on the right controller confirms the switch
            }
            if (newPress) {
                // 26.13: every press logged with the zone's numbers, so a press that did not switch says why (at most
                // 4 lines a second: a mashed prompt)
                static uint64_t windowStart = 0;
                static int inWindow = 0, skipped = 0;
                if (now - windowStart >= 1000) {
                    if (skipped) Log("VRINPUT head-button: %d more presses in the last second (not logged)", skipped);
                    windowStart = now;
                    inWindow = skipped = 0;
                }
                if (++inWindow <= 4) {
                    if (!tracked) Log("VRINPUT head-button press (B / O): right hand not tracked -> the game's");
                    else
                        Log("VRINPUT head-button press (B / O): right hand %.2f m from the eyes, %.2f m ahead of the face "
                            "plane, %.2f m up -> %s", zone.dist, zone.ahead, zone.up,
                            atHead ? "first-person switch" : "the game's (zone: < 0.25 m, > -0.10 m ahead, -0.25..0.10 m up)");
                } else {
                    ++skipped;
                }
            }
            const bool wasPulse = g_startPulse;
            g_startPulse = g_chord.Feed(g_sample, now);
            if (g_startPulse && !wasPulse) Log("VRINPUT all four face buttons: Start (pause screen, VR settings)");
        }
    }
    UpdateMotion(frame);
}
bool ConsumeHeadSwitch() {
    std::lock_guard lock(g_sampleMutex);
    const bool pending = g_headSwitchAt && GetTickCount64() - g_headSwitchAt <= 1000;
    g_headSwitchAt = 0;
    return pending;
}
FirstPersonMove g_fpMove;
uint64_t g_fpMoveTick = 0;
void SetFirstPersonMove(const FirstPersonMove& move) {
    std::lock_guard lock(g_sampleMutex);
    g_fpMove = move;
    g_fpMoveTick = GetTickCount64();
}
LegacyState Snapshot() {
    std::lock_guard lock(g_sampleMutex);
    const uint64_t now = GetTickCount64();
    if (!xr::Running() || !g_sampleTick || now < g_sampleTick || now - g_sampleTick > 250) return {};
    // A first-person move older than 250 ms is stale (first person ended without saying so).
    const bool fresh = g_fpMoveTick && now >= g_fpMoveTick && now - g_fpMoveTick <= 250;
    LegacyState out = MapFirstPerson(g_sample, fresh ? g_fpMove : FirstPersonMove{});
    if (g_startPulse && now < g_chord.pulseUntil) out.buttons |= kRightMenu;  // Start (Button case 0)
    // A controller pointing at the VR settings panel: its trigger clicks the panel, never the game.
    if (panel::CapturesTrigger(0)) {
        out.leftTrigger = 0.f;
        out.buttons &= ~kLeftTriggerButton;
    }
    if (panel::CapturesTrigger(1)) {
        out.rightTrigger = 0.f;
        out.buttons &= ~kRightTriggerButton;
    }
    return out;
}
void SetPadType(int type) {
    if (type != 2 && type != 5) return;
    g_padType = type;
    WritePrivateProfileStringW(L"hrvr", L"controller", type == 5 ? L"playstation" : L"xbox", IniPath().c_str());
    Log("VRINPUT controller type -> %s (saved to hrvr.ini)", type == 5 ? "PlayStation" : "Xbox");
}
int PadType() { return g_padType.load(); }
bool MotionControls() { return g_motionControls.load(); }
void SetMotionControls(bool on) {
    g_motionControls = on;
    WritePrivateProfileStringW(L"hrvr", L"motion_controls", on ? L"on" : L"off", IniPath().c_str());
    Log("VRINPUT motion controls %s (saved to hrvr.ini)", on ? "on" : "off");
}
void LogStatus() {
    static std::atomic<uint64_t> last{0};
    const uint64_t now = GetTickCount64();
    uint64_t expected = last.load(std::memory_order_relaxed);
    if (expected && now - expected < 10000) return;
    if (!last.compare_exchange_strong(expected, now, std::memory_order_relaxed)) return;
    Sample sample{};
    uint64_t tick = 0;
    {
        std::lock_guard lock(g_sampleMutex);
        sample = g_sample;
        tick = g_sampleTick;
    }
    char left[128] = "<none>", right[128] = "<none>";
    xr::GetInteractionProfile(0, left, sizeof(left));
    xr::GetInteractionProfile(1, right, sizeof(right));
    const uint64_t age = tick && now >= tick ? now - tick : UINT64_MAX;
    Log("VRINPUT status focus=%d fresh=%d age=%llu L(stick=%.2f,%.2f trig=%.2f) R(stick=%.2f,%.2f trig=%.2f) "
        "profileL=%s profileR=%s type=%d steam(ctx=%d dq=%llu da=%llu aq=%llu an=%llu synth=%llu rumble=%llu)",
        sample.focused ? 1 : 0, sample.fresh ? 1 : 0, static_cast<unsigned long long>(age), sample.leftX, sample.leftY,
        sample.leftTrigger, sample.rightX, sample.rightY, sample.rightTrigger, left, right, g_padType.load(),
        g_contextReady.load() ? 1 : 0, static_cast<unsigned long long>(g_digitalQueries.load()),
        static_cast<unsigned long long>(g_digitalActive.load()), static_cast<unsigned long long>(g_analogQueries.load()),
        static_cast<unsigned long long>(g_analogNonzero.load()), static_cast<unsigned long long>(g_synthHandles.load()),
        static_cast<unsigned long long>(g_rumbleCalls.load()));
    if (g_xRoute.load())
        Log("VRINPUT XInput route: polls of pad 1 %llu (no real pad %llu), with VR input %llu, rumble calls %llu",
            static_cast<unsigned long long>(g_xPolls.load()), static_cast<unsigned long long>(g_xNoRealPad.load()),
            static_cast<unsigned long long>(g_xActive.load()), static_cast<unsigned long long>(g_xRumble.load()));
}

}  // namespace hrvr::vrinput
