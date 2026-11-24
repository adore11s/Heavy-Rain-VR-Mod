#include "stereo.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <MinHook.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "audio.h"
#include "bones.h"
#include "crashreport.h"
#include "devtools.h"
#include "engineobj.h"
#include "fpbody.h"
#include "fpcam.h"
#include "game.h"
#include "gamma_filter.h"
#include "harness.h"
#include "hotkeys.h"
#include "log.h"
#include "pacing.h"
#include "player.h"
#include "posemath.h"
#include "menus.h"
#include "story.h"
#include "summary.h"
#include "rendersize.h"
#include "stereo_math.h"
#include "videowatch.h"
#include "vrinput.h"
#include "xr.h"

namespace hrvr::stereo {
namespace {
using namespace game;

// ---- engine entry points (x64: integer args rcx rdx r8 r9, float args by position in xmm) ----
using ListDisplayFn = void (*)(void* list, void* a2, void* a3, void* a4);
using DisplayWindowFn = void (*)(void* win, uint64_t flag, void* a3, void* a4);
using CameraBuildFn = void (*)(void* cam, float aspect, float guardX, float guardY);
using Layer2DFn = void (*)(void* layer, void* vp, uint64_t flag, void* a4);
using PresentFn = void (*)(void* a1, uint64_t sync, void* a3, void* a4);
using SetupCameraFn = uint64_t (*)(void* vp, void* params, uint64_t flag);
using SetViewportFn = void (*)(void* ctx, uint32_t x, uint32_t y, uint32_t w, uint32_t h);
using NodeUpdateFn = void (*)(void* node);
using ResizeBuffersFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

ListDisplayFn g_origList = nullptr;
DisplayWindowFn g_origWindow = nullptr;
CameraBuildFn g_origBuild = nullptr;
Layer2DFn g_origLayer = nullptr;
PresentFn g_origPresent = nullptr;
ResizeBuffersFn g_origResize = nullptr;

// ---- settings (hrvr.ini [hrvr]) ----
// The vr-modding-playbook proof ladder (docs/STEREO_ROUTE.md): each stage proves one claim and a
// lower stage's success never implies the next one.
enum class Stage { Observe, Gate, XrColor, XrFlat, Vr };
Stage g_stage = Stage::Observe;
const char* StageName(Stage s) {
    switch (s) {
        case Stage::Observe: return "observe";
        case Stage::Gate: return "gate";
        case Stage::XrColor: return "xrcolor";
        case Stage::XrFlat: return "xrflat";
        default: return "vr";
    }
}
bool XrStage() { return g_stage == Stage::XrColor || g_stage == Stage::XrFlat || g_stage == Stage::Vr; }
std::atomic<bool> g_xrWanted{true};  // vr=on|off (F9 toggles); only in the xr stages
bool g_uiPanel = true;               // ui=panel|eyes
bool g_prime = false;                // prime=on: run each viewport's camera setup before its eye pass
bool g_engineFov = true;             // enginefov=on|off: FOV/aspect scalars describe the headset frustum
float g_worldScale = 1.f;            // world_scale: engine units per metre for the eye offsets
// Rendering method (rendering_method=native|afr|aer|mono; default native, owner 2026-11-20 - was afr 2026-10-28):
//   Native  both eyes every game frame.
//   AFR     (playbook 18) one eye per game frame, alternating; the other eye keeps its last image and is submitted
//           with the pose it was rendered for (xr::OnSingleEye). Roughly halves the render work; moving things
//           differ a little between the eyes.
//   AER     alternate eye rendering with matched pairs: one eye per game frame like AFR, but both eyes of a pair are
//           rendered from the SAME head pose (the second eye reuses the views held when the first was rendered) and
//           submitted together once the pair is complete; until then every XR frame shows the previous pair. The
//           two eyes always agree on where the head was (no left/right pose mismatch when the head moves fast),
//           at one game frame more latency; the runtime reprojects the pair to the current head pose.
//   Mono    (playbook 07.7, 0.23.0) one render per game frame, always eye slot 0, from the centre of the head
//           with a symmetric frustum covering both eyes (xr BeginGameFrame); each eye is cut from that one picture
//           and submitted with the centre pose. Cheapest and most robust; the world has no stereo depth.
std::atomic<int> g_method{kNative};
int g_afrNext = 0;
int g_aerPhase = 0;          // 0: next frame starts a pair (eye 0); 1: next frame finishes it (eye 1)
xr::EyeView g_aerEyes[2];    // the views the current pair is rendered with
uint64_t g_eyeTexGen = 0;    // eye textures re-made (size change): a half-finished AER pair is dropped
// vr_dof=off (default, playbook 18.5): no depth of field in the headset. A blurred background that does not follow
// where the eyes look reads as a focus fault in VR. The camera's own DoF switch (+0x128) is cleared for the eye passes
// only and restored after them, so the game's scripts and the flat screen keep theirs.
bool g_vrDof = false;
// vr_film_grain=off (default since 1.2; owner 2026-11-22 "remove film grain" - REVERSES the 2026-11-20 "DONT TURN OFF
// NOISE"): the camera's noise switch (+0x144, read by the same post-effect fill as DoF) is cleared for the eye passes like DoF;
// since 1.4 also for every other render (HookBuild: the desktop window and flat screens, owner). vr_film_grain=on keeps the game's grain.
bool g_vrGrain = false;
// vr_yellow_filter=on (default, owner 2026-11-24): the game's per-channel colour gamma (the "yellow filter", gamma_filter.h)
// as the game sets it; off = 1 / 1 / 1 as the 3DMigoto "No PP" mod gets it. Live from the VR panel (any thread); applied
// in HookBuild on the game thread.
std::atomic<bool> g_yellowFilter{true};
gamma::GammaFilter g_gammaFilter;

std::wstring IniPath() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring s(exe);
    return s.substr(0, s.find_last_of(L"\\/")) + L"\\hrvr.ini";
}
std::wstring IniString(const wchar_t* key, const wchar_t* def) {
    wchar_t v[64];
    GetPrivateProfileStringW(L"hrvr", key, def, v, 64, IniPath().c_str());
    return v;
}
bool IniOn(const wchar_t* key, bool def) {
    const std::wstring v = IniString(key, def ? L"on" : L"off");
    return _wcsicmp(v.c_str(), L"on") == 0 || _wcsicmp(v.c_str(), L"true") == 0 || v == L"1";
}

// ---- D3D objects (obtained at the first Present) ----
std::recursive_mutex g_lock;  // XR work: display and Present may come from the loading thread too
IDXGISwapChain* g_sc = nullptr;
ID3D11Device* g_dev = nullptr;
ID3D11DeviceContext* g_ctx = nullptr;
ID3D11Texture2D* g_eyeTex[2] = {};
D3D11_TEXTURE2D_DESC g_eyeDesc{};

// ---- per-frame stereo state (render thread) ----
constexpr int kMaxCams = 8;
int g_eye = -1;  // eye being rendered, -1 outside the stereo passes
extern bool g_pass;
bool g_pass = false;   // inside a mod-driven list render: 2D layers are held back and drawn once after
bool g_testProj = false;  // gate D: the projection model written over the engine's own
void* g_cams[kMaxCams] = {};
int g_camCount = 0;
// Cameras of the smaller (not full-screen) viewports. small_views=head (default): they follow the head
// and the eye offset too but keep their own projection and draw their 2D in place; =fixed: the game's
// own camera, identical in both eyes.
void* g_smallCams[kMaxCams] = {};
int g_smallCount = 0;
bool g_smallHead = false;
bool IsSmallCam(const void* cam) {
    for (int i = 0; i < g_smallCount; ++i)
        if (g_smallCams[i] == cam) return true;
    return false;
}
xr::EyeView g_eyes[2];
int g_pendingUiSlot = -1;
bool g_stereoThisFrame = false;
uint64_t g_lastStereoMs = 0;
uint64_t g_pairs = 0;
struct Deferred2D {
    void* layer;
    void* vp;
    uint64_t flag;
    void* a4;
};
std::vector<Deferred2D> g_deferred2D;
void* g_mainVps[kMaxCams] = {};  // the main (full-screen) viewports of the current list
int g_mainVpCount = 0;

bool IsStereoCam(const void* cam) {
    for (int i = 0; i < g_camCount; ++i)
        if (g_cams[i] == cam) return true;
    return false;
}

// ---- eye pose on the camera node (B2S EXP-012 "node pass") ----
// CAMERA::Build inverts the node world (+0x90) into the view, and SetupCamera builds frustums and
// the camera position from it: composing the eye onto the node for the duration of the eye pass
// keeps the view and every node consumer in agreement; restored after each pass so game logic
// never sees the head pose.
struct NodeOverride {
    uint8_t* node = nullptr;
    float saved[16];
    float written[16];
} g_nodeOv[kMaxCams];

void RestoreOne(NodeOverride& o) {
    if (!o.node) return;
    float* w = reinterpret_cast<float*>(o.node + off::NodeWorld);
    if (std::memcmp(w, o.written, sizeof(o.written)) == 0) std::memcpy(w, o.saved, sizeof(o.saved));
    o.node = nullptr;
}
void RestoreNodes() {
    for (auto& o : g_nodeOv) RestoreOne(o);
}

uint8_t* CameraNode(void* cam) {
    auto* node = static_cast<uint8_t*>(ResolveHandle(Read<void*>(cam, off::CamNode)));
    if (!node) return nullptr;
    if (((Read<uint32_t>(node, off::NodeFlags) >> 1) & 1) == 0) {
        auto update = reinterpret_cast<NodeUpdateFn>(Addr(Anchor::NodeUpdateWorld));
        if (!update) return nullptr;
        update(node);
    }
    return node;
}

// node world = rel * base; base = the game's own camera world unless first person supplies one.
void SetNodeRelative(uint8_t* node, const pose::Mat4& rel, const pose::Mat4* base = nullptr, pose::Vec3 shift = {}) {
    float* w = reinterpret_cast<float*>(node + off::NodeWorld);
    NodeOverride* o = nullptr;
    for (auto& e : g_nodeOv)
        if (e.node == node) o = &e;
    if (!o)
        for (auto& e : g_nodeOv)
            if (!e.node && !o) o = &e;
    if (!o) return;
    const bool ours = o->node == node && std::memcmp(w, o->written, sizeof(o->written)) == 0;
    if (!ours) {
        RestoreOne(*o);
        o->node = node;
        std::memcpy(o->saved, w, sizeof(o->saved));
    }
    pose::Mat4 world;
    if (base) world = *base;
    else std::memcpy(world.m, o->saved, sizeof(world.m));
    world.m[12] += shift.x;  // 0.36.6: the third-person camera moved closer (camera_forward)
    world.m[14] += shift.z;
    const pose::Mat4 m = pose::Mul(rel, world);
    std::memcpy(o->written, m.m, sizeof(m.m));
    std::memcpy(w, m.m, sizeof(m.m));
}

pose::Mat4 EyeRel(int eye) {
    pose::Mat4 offset;
    std::memcpy(offset.m, g_eyes[eye].viewOffset, sizeof(offset.m));
    pose::Mat4 rel = pose::RigidInverse(offset);  // eye pose in the camera's space (metres)
    for (int i = 12; i < 15; ++i) rel.m[i] *= g_worldScale;
    return rel;
}

// 0.36.6 (owner 2026-11-20: "from this mod: ...BEYONDDD import the z axis from vr panel"): Beyond 1.0.3's "Camera closer
// (third person)". The game's authored cameras stand where the flat screen's narrow view looks fine, but the headset's
// wide view and the head's own movement show walls, and characters far away look small. While no first-person base
// drives the camera (F1 / F3 off or not active), the main camera is moved `camera_forward` metres along its horizontal
// view direction (its height kept: no dipping into floors on a downward shot), never to less than kPushKeep in front of
// the player's character, not at all when the camera looks almost straight down / up, when the player's character is
// not known (menus, loading) or with more than one full-screen view (split screens). One world offset per frame, so
// both eyes move together. hrvr.ini camera_forward, panel row "Camera closer (third person)". 0.36.8 (owner: "make the z
// axis bit more like 5 meters upto and make 2meters defautl"): 0 .. 5 m, default 2 m (0 = off).
std::atomic<float> g_cameraPush{2.f};
constexpr float kPushMax = 5.f, kPushKeep = 0.75f;
pose::Vec3 g_pushOffset{};  // this frame's world offset (0 = the game's place)

pose::Vec3 ThirdPersonPush(const pose::Mat4& world) {
    const float want = g_cameraPush.load(std::memory_order_relaxed);
    if (!(want > 0.f)) return {};
    // 0.36.10 (owner: "the z axis so far away just at the main menu", "the recenter button worked weird"): only in story
    // chapters - never the main menu's 3D backdrop, the install / splash screens or a jump's "(none)".
    if (!story::InStoryChapter()) return {};
    // row-vector camera world: row 2 = +Z, the camera looks along -Z
    float fx = -world.m[8], fz = -world.m[10];
    const float fn = std::sqrt(fx * fx + fz * fz);
    if (fn < 0.3f) return {};
    fx /= fn;
    fz /= fn;
    // 0.36.9 (owner: "the z axis dont work?" - LIVE 0.36.8: never moved, the epilogue had no known player character):
    // like Beyond, the push also applies without a known character; it only stops kPushKeep in front of one it knows.
    // 0.36.11 (owner: "it says Closer not more far away"; LIVE 0.36.9: without a character to stop at, 5 m took the
    // camera straight through the menu's scene and out past it, so everything looked farther away): the push needs the
    // player's character (0.36.10 finds a chapter's main character when the game names none) and only ever moves the
    // camera TOWARD it - at most to kPushKeep in front of it, never past it, and not at all when the character is
    // behind the camera or beside it (less than 0.5 m of its distance along the view).
    const player::State st = player::Poll();
    pose::Mat4 body{};
    void* node = st.character ? bones::CharacterNode(st.character) : nullptr;
    if (!node || !bones::NodeWorld(node, &body)) return {};
    const float dx = body.m[12] - world.m[12], dz = body.m[14] - world.m[14];
    const float ahead = dx * fx + dz * fz, flat = std::sqrt(dx * dx + dz * dz);
    if (!(ahead > 0.5f * flat) || ahead <= kPushKeep) return {};
    const float push = std::min(want, ahead - kPushKeep);
    return {fx * push, 0.f, fz * push};
}

// A main camera's eye pass: the eye on the first-person base (fpcam) or on the game's own camera (moved closer by
// camera_forward when that is set).
void ApplyEye(uint8_t* node, int eye) {
    pose::Mat4 base;
    if (fpcam::Base(node, &base)) SetNodeRelative(node, EyeRel(eye), &base);
    else SetNodeRelative(node, EyeRel(eye), nullptr, g_pushOffset);
}

// ---- engine FOV / aspect scalars (B2S EXP-014/019) ----
// p0 = cot(fov/2) / (scale * aspectParam) and p5 = p0 * aspect (camerasystem_140b54610): the FOV at
// +0x104 is horizontal. Consumers of the scalars (cones, LOD) should see the headset frustum.
struct FovSave {
    uint8_t* cam = nullptr;
    float fov;
} g_fovSave[kMaxCams];

void HeadsetHalfTangents(float& h, float& v) {
    h = std::max({-g_eyes[0].tanLeft, g_eyes[0].tanRight, -g_eyes[1].tanLeft, g_eyes[1].tanRight});
    v = std::max({g_eyes[0].tanUp, -g_eyes[0].tanDown, g_eyes[1].tanUp, -g_eyes[1].tanDown});
}

void ApplyEngineFov(void* camp) {
    auto* cam = static_cast<uint8_t*>(camp);
    FovSave* slot = nullptr;
    for (auto& f : g_fovSave)
        if (f.cam == cam) return;
    for (auto& f : g_fovSave)
        if (!f.cam && !slot) slot = &f;
    if (!slot) return;
    float h, v;
    HeadsetHalfTangents(h, v);
    float s = Read<float>(cam, 0x10C) * Read<float>(cam, 0x108);
    if (!(s > 0.01f && s < 100.f)) s = 1.f;
    auto* fov = reinterpret_cast<float*>(cam + off::CamFov);
    *slot = {cam, *fov};
    *fov = EngineFov(h, s);
}
void RestoreEngineFov() {
    for (auto& f : g_fovSave) {
        if (!f.cam) continue;
        *reinterpret_cast<float*>(f.cam + off::CamFov) = f.fov;
        f.cam = nullptr;
    }
}

// ---- observation (vr-modding-playbook RE-04/RE-05: observe before redirecting) ----
struct Counters {
    uint64_t present = 0, window = 0, list = 0, build = 0, layer = 0;
};
Counters g_count, g_countAtPresent;
uint64_t g_frame = 0;  // Presents seen

bool Paused() {
    const auto* p = Global<uint8_t>(g::GamePaused);
    return p && *p != 0;
}

// The engine projection model (camerasystem_140b54610 + 1401831a0), checked live against the
// matrix CAMERA::Build actually produced: p0 = cot(fov/2)/s/gx, p5 = cot(fov/2)*aspect/s/gy.
struct ModelCheck {
    float fov, aspect, s, gx, gy, nearZ, farZ;
    float p0, p5, p8, p9, p10, p11, p14;
    float predP0, predP5, residual;  // max relative error of p0/p5
    float viewResidual;             // |view * world - I| (view = inverse(node world))
    float axis[3], ortho;            // node world axis lengths, max |dot| between axes
    float pos[3];
    bool worldValid;
};
bool CheckModel(void* camp, ModelCheck* m) {
    auto* cam = static_cast<uint8_t*>(camp);
    const auto* p = reinterpret_cast<const float*>(cam + off::CamProj);
    m->fov = Read<float>(cam, off::CamFov);
    m->aspect = Read<float>(cam, off::CamAspect);
    m->s = Read<float>(cam, 0x10C) * Read<float>(cam, 0x108);
    m->gx = Read<float>(cam, off::CamGuardX);
    m->gy = Read<float>(cam, off::CamGuardY);
    m->nearZ = Read<float>(cam, 0x114);
    m->farZ = Read<float>(cam, 0x118);
    m->p0 = p[0]; m->p5 = p[5]; m->p8 = p[8]; m->p9 = p[9]; m->p10 = p[10]; m->p11 = p[11]; m->p14 = p[14];
    const float c = 1.f / std::tan(m->fov * 0.5f);
    m->predP0 = c / m->s / m->gx;
    m->predP5 = c * m->aspect / m->s / m->gy;
    m->residual = std::max(std::fabs(m->predP0 - p[0]) / std::max(std::fabs(p[0]), 1e-6f),
                           std::fabs(m->predP5 - p[5]) / std::max(std::fabs(p[5]), 1e-6f));
    // Observation only: a node whose world is not valid is reported, never recomputed here.
    auto* node = static_cast<uint8_t*>(ResolveHandle(Read<void*>(cam, off::CamNode)));
    if (!node) return false;
    m->worldValid = ((Read<uint32_t>(node, off::NodeFlags) >> 1) & 1) != 0;
    pose::Mat4 world, view;
    std::memcpy(world.m, node + off::NodeWorld, 64);
    std::memcpy(view.m, cam + off::CamView, 64);
    const pose::Mat4 id = pose::Mul(view, world);
    m->viewResidual = 0.f;
    for (int i = 0; i < 16; ++i) m->viewResidual = std::max(m->viewResidual, std::fabs(id.m[i] - ((i % 5) == 0 ? 1.f : 0.f)));
    float ax[3][3];
    for (int r = 0; r < 3; ++r) {
        for (int k = 0; k < 3; ++k) ax[r][k] = world.m[r * 4 + k];
        m->axis[r] = std::sqrt(ax[r][0] * ax[r][0] + ax[r][1] * ax[r][1] + ax[r][2] * ax[r][2]);
    }
    auto dot = [&](int a, int b) { return std::fabs(ax[a][0] * ax[b][0] + ax[a][1] * ax[b][1] + ax[a][2] * ax[b][2]); };
    m->ortho = std::max({dot(0, 1), dot(0, 2), dot(1, 2)});
    for (int k = 0; k < 3; ++k) m->pos[k] = world.m[12 + k];
    return true;
}
void LogModel(const char* tag, void* cam) {
    ModelCheck m{};
    if (!CheckModel(cam, &m)) return;
    Log("%s cam=%p fov=%.4f (%.1f deg) aspect=%.4f s=%.4f guard=%.4f,%.4f near=%.4f far=%.1f | p0=%.5f p5=%.5f p8=%.5f "
        "p9=%.5f p10=%.5f p11=%.3f p14=%.5f | model p0=%.5f p5=%.5f residual=%.2e | view*world-I=%.2e axes=%.4f %.4f %.4f "
        "maxdot=%.2e pos=(%.3f %.3f %.3f) worldValid=%d",
        tag, cam, m.fov, m.fov * 57.29578f, m.aspect, m.s, m.gx, m.gy, m.nearZ, m.farZ, m.p0, m.p5, m.p8, m.p9, m.p10,
        m.p11, m.p14, m.predP0, m.predP5, m.residual, m.viewResidual, m.axis[0], m.axis[1], m.axis[2], m.ortho, m.pos[0],
        m.pos[1], m.pos[2], int(m.worldValid));
}

// ---- the per-frame light-record table (LIVE 2026-10-26: the game's abort() at HeavyRain.exe+0x8ed64f) ----
// Every viewport pass copies each visible light into a fixed 180-entry table (owner+0x6F0 records,
// owner+0x6E4 count, reset once per frame). Two renders of a scene with many viewports exceeded it.
// Heavy Rain renders immediately, so the first render's records are consumed before the second starts:
// the second render rewinds each table to where the first began, and both use what the flat game uses.
using LightAllocFn = void* (*)(void* owner, void* light);
LightAllocFn g_origLightAlloc = nullptr;
constexpr uintptr_t kLightCount = 0x6E4;
constexpr int kLightCapacity = 0xB4;
int g_render = -1;          // -1 ordinary; 0 / 1: first / second of the mod's two renders of this frame
void LogShadowProbe();      // read-only shadow probe, defined with its hooks below
uint64_t g_renderPair = 0;  // incremented at every pair of renders

// ---- per-eye draw probe (0.15.1; owner: objects vanish in the right eye only, Native) ----
// STATIC 2026-11-11: Beyond's cause (EXP-053, one per-frame primitive budget shared by both eye passes) does not
// exist here: the viewport pass 0x1408a15e0 rewinds the per-frame pools (scene +0x4A8..+0x4D0) itself, at every
// pass, and a full pool is an abort(), not a skipped object. So the two renders are measured instead: the D3D11 draw
// calls of each render of a pair are counted (the game draws immediately, on this thread), and `eyesame on` gives
// both renders the left eye's pose - fewer right-eye draws WITH the same pose means the second render itself loses
// objects (re-entry state); equal draws with the same pose and fewer with its own pose means culling by pose.
std::atomic<bool> g_eyeSame{false};
uint64_t g_drawNow[2] = {0, 0};  // this pair
struct DrawStats {
    uint64_t pairs = 0, sum[2] = {0, 0}, worst[2] = {0, 0}, low = 0;
    double worstRatio = 2.0;
} g_drawStats;
using DrawIndexedFn = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
using DrawFn = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
using DrawIndexedInstancedFn = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
using DrawInstancedFn = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
DrawIndexedFn g_origDrawIndexed = nullptr;
DrawFn g_origDraw = nullptr;
DrawIndexedInstancedFn g_origDrawIndexedInstanced = nullptr;
DrawInstancedFn g_origDrawInstanced = nullptr;
extern bool g_pass;

// ---- 0.35.0 draw diff (owner 2026-11-20: "during loadings I can only see the character in front of me in my left and
// not in the right eye", Native; LIVE the same day: 600 of 600 loading pairs, left 99 / right 60 draw calls, and the
// shadow probe saw a map rendered every frame in the left render only, with no pool request) ----
// Read-only. The static path did not show which per-frame state the first render uses up, so the next run names it:
// after a pair whose right render drew under 90 % of the left, ONE pair (at most every 5 s, 40 times a session) keys
// every draw call of both renders by the first four return addresses inside HeavyRain.exe, and the call sites the
// left render drew more often are logged as exe+RVA chains (reverse_engineering/tools/whereis.py names them).
struct DrawSite {
    uint64_t key = 0;
    uint32_t rva[4] = {};
    uint32_t count[2] = {};
};
constexpr int kDrawSites = 512;
DrawSite g_drawSites[kDrawSites];
bool g_diffArmed = false, g_diffActive = false;
uintptr_t g_exeLo = 0, g_exeHi = 0;
void RecordDrawSite(int render) {
    if (!g_exeLo) {
        auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        g_exeLo = reinterpret_cast<uintptr_t>(base);
        g_exeHi = g_exeLo + nt->OptionalHeader.SizeOfImage;
    }
    void* frames[32];
    const USHORT n = RtlCaptureStackBackTrace(1, 32, frames, nullptr);
    DrawSite site;
    int k = 0;
    for (USHORT i = 0; i < n && k < 4; ++i) {
        const auto a = reinterpret_cast<uintptr_t>(frames[i]);
        if (a < g_exeLo || a >= g_exeHi) continue;
        site.rva[k++] = uint32_t(a - g_exeLo);
    }
    uint64_t h = 1469598103934665603ull;
    for (uint32_t r : site.rva) h = (h ^ r) * 1099511628211ull;
    if (!h) h = 1;
    for (int probe = 0; probe < kDrawSites; ++probe) {
        DrawSite& s = g_drawSites[(h + uint64_t(probe)) % kDrawSites];
        if (s.key == 0) {
            s = site;
            s.key = h;
        }
        if (s.key == h) {
            ++s.count[render];
            return;
        }
    }
}
void LogDrawDiff(uint64_t pair, uint64_t left, uint64_t right) {
    struct Row {
        int i;
        int d;
    } rows[kDrawSites];
    int m = 0;
    for (int i = 0; i < kDrawSites; ++i)
        if (g_drawSites[i].key && g_drawSites[i].count[0] != g_drawSites[i].count[1])
            rows[m++] = {i, int(g_drawSites[i].count[0]) - int(g_drawSites[i].count[1])};
    std::sort(rows, rows + m, [](const Row& a, const Row& b) { return a.d > b.d; });
    Log("STEREO draw diff (pair %llu: left %llu / right %llu draw calls, %d call sites differ); left minus right by "
        "call site (exe+RVA, innermost first):", static_cast<unsigned long long>(pair),
        static_cast<unsigned long long>(left), static_cast<unsigned long long>(right), m);
    for (int r = 0; r < m && r < 8; ++r) {
        const DrawSite& s = g_drawSites[rows[r].i];
        Log("STEREO draw diff   %+d (left %u, right %u): exe+0x%x < exe+0x%x < exe+0x%x < exe+0x%x", rows[r].d, s.count[0],
            s.count[1], s.rva[0], s.rva[1], s.rva[2], s.rva[3]);
    }
}

inline void CountDraw() {
    if (g_pass && (g_render == 0 || g_render == 1)) {
        ++g_drawNow[g_render];
        if (g_diffActive) RecordDrawSite(g_render);
    }
}
void STDMETHODCALLTYPE HookDrawIndexed(ID3D11DeviceContext* c, UINT n, UINT start, INT base) {
    CountDraw();
    g_origDrawIndexed(c, n, start, base);
}
void STDMETHODCALLTYPE HookDraw(ID3D11DeviceContext* c, UINT n, UINT start) {
    CountDraw();
    g_origDraw(c, n, start);
}
void STDMETHODCALLTYPE HookDrawIndexedInstanced(ID3D11DeviceContext* c, UINT n, UINT inst, UINT start, INT base, UINT startInst) {
    CountDraw();
    g_origDrawIndexedInstanced(c, n, inst, start, base, startInst);
}
void STDMETHODCALLTYPE HookDrawInstanced(ID3D11DeviceContext* c, UINT n, UINT inst, UINT start, UINT startInst) {
    CountDraw();
    g_origDrawInstanced(c, n, inst, start, startInst);
}
// Once, at the first pair of renders: the immediate context's four draw methods (vtable 12, 13, 20, 21).
void InstallDrawProbe(ID3D11DeviceContext* ctx) {
    static bool done = false;
    if (done || !ctx) return;
    done = true;
    void** vt = *reinterpret_cast<void***>(ctx);
    struct {
        int slot;
        void* detour;
        void** orig;
    } hooks[] = {{12, reinterpret_cast<void*>(&HookDrawIndexed), reinterpret_cast<void**>(&g_origDrawIndexed)},
                 {13, reinterpret_cast<void*>(&HookDraw), reinterpret_cast<void**>(&g_origDraw)},
                 {20, reinterpret_cast<void*>(&HookDrawIndexedInstanced), reinterpret_cast<void**>(&g_origDrawIndexedInstanced)},
                 {21, reinterpret_cast<void*>(&HookDrawInstanced), reinterpret_cast<void**>(&g_origDrawInstanced)}};
    int ok = 0;
    for (auto& h : hooks)
        if (MH_CreateHook(vt[h.slot], h.detour, h.orig) == MH_OK && MH_EnableHook(vt[h.slot]) == MH_OK) ++ok;
    Log("STEREO draw probe: %d of 4 D3D11 draw methods counted per eye render", ok);
}
// After a pair of two renders.
void NoteDrawPair() {
    const uint64_t l = g_drawNow[0], r = g_drawNow[1];
    DrawStats& st = g_drawStats;
    ++st.pairs;
    st.sum[0] += l;
    st.sum[1] += r;
    const double ratio = l ? double(r) / double(l) : 1.0;
    if (g_diffActive) {
        g_diffActive = false;
        LogDrawDiff(g_renderPair, l, r);
    }
    {
        static ULONGLONG lastDiff = 0;
        static int diffs = 0;
        const ULONGLONG now = GetTickCount64();
        if (ratio < 0.9 && diffs < 40 && now - lastDiff >= 5000) {
            lastDiff = now;
            ++diffs;
            g_diffArmed = true;  // the next pair is keyed by call site
        }
    }
    if (ratio < st.worstRatio) {
        st.worstRatio = ratio;
        st.worst[0] = l;
        st.worst[1] = r;
    }
    if (ratio < 0.9) {
        ++st.low;
        static ULONGLONG last = 0;
        const ULONGLONG now = GetTickCount64();
        if (now - last >= 1000) {
            last = now;
            Log("STEREO draw probe: this pair the right eye drew %llu calls, the left %llu (eyesame %s)",
                static_cast<unsigned long long>(r), static_cast<unsigned long long>(l), g_eyeSame ? "on" : "off");
        }
    }
    if (st.pairs >= 600) {
        Log("STEREO draw probe (%llu pairs): draw calls per render left %.0f / right %.0f on average; worst pair left "
            "%llu / right %llu; %llu pairs with the right eye under 90 %% of the left; eyesame %s, prime %d",
            static_cast<unsigned long long>(st.pairs), double(st.sum[0]) / double(st.pairs),
            double(st.sum[1]) / double(st.pairs), static_cast<unsigned long long>(st.worst[0]),
            static_cast<unsigned long long>(st.worst[1]), static_cast<unsigned long long>(st.low),
            g_eyeSame ? "on" : "off", int(g_prime));
        st = DrawStats{};
    }
}
struct LightTable {
    void* owner = nullptr;
    uint64_t pair = 0;
    int base = 0;
    bool rewound = false;
} g_lightTables[8];
std::atomic<uint64_t> g_lightRewinds{0}, g_lightSaves{0};

void* HookLightAlloc(void* owner, void* light) {
    auto* count = reinterpret_cast<int*>(static_cast<uint8_t*>(owner) + kLightCount);
    if (g_render >= 0) {
        LightTable* t = nullptr;
        for (auto& e : g_lightTables)
            if (e.owner == owner) t = &e;
        if (!t)
            for (auto& e : g_lightTables)
                if (!t && (!e.owner || e.pair + 2 < g_renderPair)) t = &e;
        if (t) {
            if (t->owner != owner || t->pair != g_renderPair) {
                *t = {owner, g_renderPair, *count, g_render == 1};  // first seen in render 2: nothing to rewind
            } else if (g_render == 1 && !t->rewound) {
                *count = t->base;
                t->rewound = true;
                g_lightRewinds.fetch_add(1, std::memory_order_relaxed);
            }
        }
        if (*count >= kLightCapacity) {
            // Never let a mod-driven render end the game: reuse the last record (one light may be wrong).
            *count = kLightCapacity - 1;
            if (g_lightSaves.fetch_add(1, std::memory_order_relaxed) < 3)
                Log("STEREO light table full during a mod render: last record reused instead of an abort");
        }
    }
    return g_origLightAlloc(owner, light);
}

// ---- the per-frame particle vertex buffers (0.35.0; LIVE 2026-11-20 13:35: the game's abort() at
// HeavyRain.exe+0x8ab550 in the right eye pass, _2501_THE_NURSE, Native) ----
// STATIC: the particle batch draw 0x14087f740 (called per emitter group from PARTICLE_EMITTER display) takes the next
// dynamic vertex buffer with 0x1408ab530: ++scene+0x84 and abort() when it reaches 12, else the buffer
// scene+0x90[(scene+0x80) * 12 + depth] (scene = *0x141a33aa8). The frame function 0x14085ab50 flips +0x80 and sets
// +0x84 = 0 once per frame, so one frame has 11 buffers - the flat game's budget for ONE render. Each buffer is mapped
// WRITE_DISCARD (0x140a72360 -> ID3D11DeviceContext::Map type 4) and drawn at once, so the second render may take the
// same buffers again: it starts where the first began, like the light table above.
using ParticleVbFn = void* (*)(uint8_t* scene);
ParticleVbFn g_origParticleVb = nullptr;
constexpr uintptr_t kParticleDepth = 0x84;
constexpr int kParticleLast = 11;  // the last index the game hands out (12 aborts)
struct ParticleRewind {
    uint8_t* scene = nullptr;
    uint64_t pair = 0;
    int base = 0;
    bool rewound = false;
} g_particle;
std::atomic<uint64_t> g_particleRewinds{0}, g_particleSaves{0};
std::atomic<int> g_particleMost[2] = {0, 0};  // deepest index per render since the last log line

void* HookParticleVb(uint8_t* scene) {
    if (scene && g_render >= 0) {
        auto* depth = reinterpret_cast<int*>(scene + kParticleDepth);
        ParticleRewind& p = g_particle;
        if (p.scene != scene || p.pair != g_renderPair) {
            p = {scene, g_renderPair, *depth, g_render == 1};  // first seen in render 2: nothing to rewind
        } else if (g_render == 1 && !p.rewound) {
            *depth = p.base;
            p.rewound = true;
            g_particleRewinds.fetch_add(1, std::memory_order_relaxed);
        }
        if (*depth >= kParticleLast) {
            // Never let a mod-driven render end the game: hand out the last buffer again (it is mapped with discard).
            *depth = kParticleLast - 1;
            if (g_particleSaves.fetch_add(1, std::memory_order_relaxed) < 3)
                Log("STEREO particle buffers full during a mod render (render %d): the last buffer is reused instead of "
                    "an abort", g_render);
        }
        const int next = *depth + 1;
        std::atomic<int>& seen = g_particleMost[g_render == 1 ? 1 : 0];
        if (next > seen.load(std::memory_order_relaxed)) seen.store(next, std::memory_order_relaxed);
    }
    return g_origParticleVb(scene);
}

// ---- 0.36.1 the right eye's lights: a read-only probe (owner 2026-11-20: loading screens show the character in the left
// eye only, Native) ----
// LIVE (0.35.0 draw diff): the right render drew characters and meshes with fewer shader passes, i.e. fewer lights.
// LIVE (0.36.0 `STEREO lights`): in gameplay the drawn parts had the SAME light totals in both renders; at the 1801 loading
// the right render's list of the face was shorter, and giving it the left render's list (0.36.0 light carry) crashed the
// light pass set-up (null read at exe+0x9edb10): the missing light has no per-pass shadow data in the right render.
// So that light is not gathered for the object in the right render. STATIC (0x140a0d160, the visibility batch job's light
// gather of one culling record): the character rig's lights (key / fill / rim, light-entry indices rig+0x194 / +0x196 /
// +0x198) are only taken when query+0x252 (query = param_1 - 0x200) is set, rig+6 is set, the record's zone (+0x98) is
// in that slot's zone list (rig + 8 + (slot * 0x10 + i) * 8, counts rig+0x188), record+0xb8 bit 1 is set, and the light
// entry's flags (+0x58 in the entries at query+0x210, 0xe0 each) have 0x209. The probe keeps those inputs per record in
// the left render and, when the right render ends with fewer lights for the same record, logs both side by side (20 a
// session). It changes nothing.
using LightGatherFn = void (*)(uint8_t* q200, uint8_t* rec, void* a3, uint8_t* rig);
LightGatherFn g_origLightGather = nullptr;
struct GatherInputs {
    uint8_t count = 0, q252 = 0, rig4 = 0, rig5 = 0, rig6 = 0, rig7 = 0, recFlags = 0, inZone[3] = {0, 0, 0};
    uint32_t zones[3] = {0, 0, 0};
    uint16_t rigLight[3] = {0, 0, 0}, rigFlags[3] = {0, 0, 0};
    uint32_t candidates = 0;
};
struct GatherSnap {
    uint8_t* rec = nullptr;
    uint64_t pair = 0;
    GatherInputs in;
};
constexpr int kGatherSnaps = 1024;
GatherSnap g_gatherSnaps[kGatherSnaps];
std::mutex g_gatherMutex;
std::atomic<uint64_t> g_gatherCalls[2] = {0, 0}, g_gatherFewer{0};
std::atomic<int> g_gatherLogs{0};

GatherInputs ReadGather(uint8_t* q200, uint8_t* rec, uint8_t* rig) {
    GatherInputs g;
    obj::Get(rec, 0x1e8, g.count);
    obj::Get(rec, 0x1ec, g.candidates);
    obj::Get(q200, 0x52, g.q252);
    uint32_t flags = 0;
    if (obj::Get(rec, 0xb8, flags)) g.recFlags = uint8_t(flags & 0xff);
    if (!rig) return g;
    obj::Get(rig, 4, g.rig4);
    obj::Get(rig, 5, g.rig5);
    obj::Get(rig, 6, g.rig6);
    obj::Get(rig, 7, g.rig7);
    uint64_t zone = 0;
    obj::Get(rec, 0x98, zone);
    void* entries = nullptr;
    obj::Get(q200, 0x10, entries);
    for (int k = 0; k < 3; ++k) {
        obj::Get(rig, 0x188 + 4 * k, g.zones[k]);
        for (uint32_t i = 0; i < g.zones[k] && i < 0x10; ++i) {
            uint64_t z = 0;
            if (obj::Get(rig, 8 + (size_t(k) * 0x10 + i) * 8, z) && z == zone) g.inZone[k] = 1;
        }
        obj::Get(rig, 0x194 + 2 * k, g.rigLight[k]);
        if (entries) obj::Get(entries, size_t(g.rigLight[k]) * 0xe0 + 0x58, g.rigFlags[k]);
    }
    return g;
}

// ---- 0.36.5 the close-up light group is kept for the second render, by the game's own switch ----
// LIVE (0.36.1 probe, the 1801 loading face): the rig zone lists were full in the first render and EMPTY in the second.
// The rig is part of the close-up light group S = *(viewport+0x268) object (rig = S+0x48): zone lists S+0x50, counts
// S+0x1d0 (shared), light indices S+0x1dc, three lists of AddRef'd handles S+0x1e8 (16 each), designated light S+0x368.
// STATIC (2026-11-20): the game update fills the lists once per frame (add 0x14097adb0 skips duplicates, max 16;
// callers the camera system 0x140522820 and a script binding 0x14097b570), and the viewport pass 0x1408a15e0 EMPTIES
// all three at its end (ClearList 0x14097ca70: count = 0, every handle released) - unless the byte g_KeepCloseUpLists
// (0x141a1aeef) is set. The game itself sets that byte only around its tiled screenshot's N x N renders of one frame
// (0x1408566b0), for exactly this reason. Native's two renders are the same case: the byte is set for the first render
// only, so the second render sees the lists the first saw and clears them itself at its end - the frame ends exactly as
// in the unmodified game. Nothing is written into the group (0.36.2 .. 0.36.3 wrote it back and crashed: the handles
// had been released). `vr_closeup_keep` (hrvr.ini, default on), `closeupkeep on|off` (hrvr_cmd.txt).
std::atomic<bool> g_closeUpKeep{true};
std::atomic<uint64_t> g_closeUpKept{0}, g_closeUpAlreadySet{0};
uint8_t* CloseUpKeepFlag() {
    static uint8_t* flag = [] {
        auto* p = Global<uint8_t>(g::KeepCloseUpLists);
        Log("STEREO close-up light lists: switch %s", p ? "found (kept for the right render in Native)"
                                                         : "NOT found - the right eye can miss close-up character lights");
        return p;
    }();
    return flag;
}

void HookLightGather(uint8_t* q200, uint8_t* rec, void* a3, uint8_t* rig) {
    g_origLightGather(q200, rec, a3, rig);
    const int r = g_render;
    if (r < 0 || !rec || !q200) return;
    uint32_t flags = 0;
    if (!obj::Get(rec, 0xb8, flags) || !(flags & 2)) return;  // only records lit by the character rig (characters)
    const int e = r == 1 ? 1 : 0;
    g_gatherCalls[e].fetch_add(1, std::memory_order_relaxed);
    const GatherInputs now = ReadGather(q200, rec, rig);
    std::lock_guard lock(g_gatherMutex);
    const uint64_t h = (reinterpret_cast<uintptr_t>(rec) >> 4) * 0x9E3779B97F4A7C15ull;
    GatherSnap* slot = nullptr;
    for (int probe = 0; probe < 8 && !slot; ++probe) {
        GatherSnap& s = g_gatherSnaps[(h + uint64_t(probe)) % kGatherSnaps];
        if (s.rec == rec || (e == 0 && (s.rec == nullptr || s.pair + 2 < g_renderPair))) slot = &s;
    }
    if (!slot) return;
    if (e == 0) {
        slot->rec = rec;
        slot->pair = g_renderPair;
        slot->in = now;
        return;
    }
    if (slot->rec != rec || slot->pair != g_renderPair || now.count >= slot->in.count) return;
    g_gatherFewer.fetch_add(1, std::memory_order_relaxed);
    if (g_gatherLogs.fetch_add(1, std::memory_order_relaxed) >= 20) return;
    const GatherInputs& l = slot->in;
    Log("STEREO light gather: record %p has %u lights in the left render, %u in the right. left / right: query+0x252 "
        "%u/%u, rig %p bytes +4..+7 %u%u%u%u/%u%u%u%u, record+0xb8 0x%02x/0x%02x, record zone in rig slot lists "
        "%u%u%u/%u%u%u (zones %u,%u,%u / %u,%u,%u), rig lights %u,%u,%u flags 0x%03x,0x%03x,0x%03x / 0x%03x,0x%03x,0x%03x "
        "(0x209 needed), candidates %u/%u", rec, l.count, now.count, l.q252, now.q252, rig, l.rig4, l.rig5, l.rig6,
        l.rig7, now.rig4, now.rig5, now.rig6, now.rig7, l.recFlags, now.recFlags, l.inZone[0], l.inZone[1], l.inZone[2],
        now.inZone[0], now.inZone[1], now.inZone[2], l.zones[0], l.zones[1], l.zones[2], now.zones[0], now.zones[1],
        now.zones[2], now.rigLight[0], now.rigLight[1], now.rigLight[2], l.rigFlags[0], l.rigFlags[1], l.rigFlags[2],
        now.rigFlags[0], now.rigFlags[1], now.rigFlags[2], l.candidates, now.candidates);
}
void LogLightGather() {
    static uint64_t last = 0;
    const uint64_t now = GetTickCount64();
    if (now - last < 10000) return;
    last = now;
    const uint64_t l = g_gatherCalls[0].exchange(0), r = g_gatherCalls[1].exchange(0);
    if (!l && !r) return;
    Log("STEREO light gather 10 s (rig-lit records): left %llu / right %llu gathers, right with fewer lights than the "
        "left %llu; close-up lists kept for the right render %llu pairs (vr_closeup_keep %s), switch already set by "
        "the game %llu",
        static_cast<unsigned long long>(l), static_cast<unsigned long long>(r),
        static_cast<unsigned long long>(g_gatherFewer.exchange(0)),
        static_cast<unsigned long long>(g_closeUpKept.exchange(0)), g_closeUpKeep.load() ? "on" : "off",
        static_cast<unsigned long long>(g_closeUpAlreadySet.exchange(0)));
}

// 1.5: the yellow filter (gamma_filter.h) per camera Build - every render of the frame (headset, desktop window, flat
// screens) and the videos / menus that read the same three floats. Build runs on the game thread, as the scripts that
// call SetGammaCorrection and the post pass that reads it.
void ApplyYellowFilter() {
    float* ch[3] = {Global<float>(g::GammaR), Global<float>(g::GammaG), Global<float>(g::GammaB)};
    if (!ch[0] || !ch[1] || !ch[2]) return;
    const bool on = g_yellowFilter.load(std::memory_order_relaxed);
    if (g_gammaFilter.Apply(ch, on))
        Log("STEREO yellow filter %s: colour gamma R %.3f G %.3f B %.3f (vr_yellow_filter=%s)",
            on ? "on (the game's)" : "off (No PP: neutral)", *ch[0], *ch[1], *ch[2], on ? "on" : "off");
}

// ---- hooks ----
void HookBuild(void* cam, float aspect, float guardX, float guardY) {
    ++g_count.build;
    // 1.4 (owner 2026-11-22 "remove it from desktop window"): grain off for EVERY render, not only the eye passes.
    // STATIC VIEWPORT::SetupCamera 0x14089fd10 calls this Build and then, in the same branch, its post-effect fill
    // camerasystem_1408e4550 (its only caller), which reads the noise switch +0x144 - cleared here it is off for the
    // headset, the desktop window and every flat screen. The game's scripts may set it again; it is cleared per Build.
    if (!g_vrGrain && cam) {
        auto* noise = static_cast<uint8_t*>(cam) + off::CamNoiseEnable;
        if (*noise) {
            *noise = 0;
            static bool logged = false;
            if (!logged) {
                logged = true;
                Log("STEREO film grain off on every camera: headset, desktop window and flat screens (vr_film_grain=off)");
            }
        }
    }
    ApplyYellowFilter();
    const int eye = g_eye;
    const bool stereo = eye >= 0 && IsStereoCam(cam);
    if (eye >= 0 && !stereo && g_smallHead && IsSmallCam(cam)) {
        if (uint8_t* node = CameraNode(cam)) SetNodeRelative(node, EyeRel(eye));
        g_origBuild(cam, aspect, guardX, guardY);  // its own projection: it is a window on the screen
        return;
    }
    float savedNear = -1.f;
    if (stereo) {
        uint8_t* node = CameraNode(cam);
        if (node) ApplyEye(node, eye);
        if (g_engineFov) ApplyEngineFov(cam);
        float h, v;
        HeadsetHalfTangents(h, v);
        aspect = h / v;  // the effective aspect (+0x11C) describes the headset frustum
        // First person: the eyes sit inside the character, and the game's near plane is set for cameras
        // metres away. Lowered for this Build only, then restored (game logic keeps the game's).
        pose::Mat4 base;
        const float wantNear = fpcam::NearPlane() * g_worldScale;
        auto* nearZ = reinterpret_cast<float*>(static_cast<uint8_t*>(cam) + 0x114);
        // The first-person near plane is set either way: lowered for the body (fp_near), or raised to the clip radius
        // around the eyes that keeps the character's own head out of view (fp_hidehead=clip).
        if (wantNear > 0.f && node && fpcam::Base(node, &base) && std::fabs(*nearZ - wantNear) > 1e-4f) {
            savedNear = *nearZ;
            *nearZ = wantNear;
            static bool logged = false;
            if (!logged) {
                logged = true;
                Log("FP near plane %.3f -> %.3f for the first-person eye passes", savedNear, wantNear);
            }
        }
    }
    g_origBuild(cam, aspect, guardX, guardY);
    if (savedNear >= 0.f) *reinterpret_cast<float*>(static_cast<uint8_t*>(cam) + 0x114) = savedNear;
    if (g_testProj && IsStereoCam(cam)) {
        // Gate D: the mod's projection model, from the engine's own FOV scalars, written over the
        // engine's matrix. If the model is right this changes nothing in the image.
        auto* c = static_cast<uint8_t*>(cam);
        float s = Read<float>(c, 0x10C) * Read<float>(c, 0x108);
        if (!(s > 0.01f && s < 100.f)) s = 1.f;
        const float h = std::tan(Read<float>(c, off::CamFov) * 0.5f) * s;
        const float v = h / Read<float>(c, off::CamAspect);
        float gx = Read<float>(c, off::CamGuardX), gy = Read<float>(c, off::CamGuardY);
        if (!(gx > 0.1f && gx < 10.f)) gx = 1.f;
        if (!(gy > 0.1f && gy < 10.f)) gy = 1.f;
        for (uintptr_t o : {off::CamProj, off::CamProjFar}) SetFrustum(reinterpret_cast<float*>(c + o), -h, h, v, -v, gx, gy);
        return;
    }
    if (!stereo) return;
    // The headset's (asymmetric) frustum; the engine's depth terms (p10, p14) and p11 = -1 stay.
    // Build divides p0 / p5 by the guard-band scales (+0x120 / +0x124): the render targets carry a
    // border around the visible image, and the frustum is widened to match.
    const xr::EyeView& ev = g_eyes[eye];
    float gx = Read<float>(cam, off::CamGuardX), gy = Read<float>(cam, off::CamGuardY);
    if (!(gx > 0.1f && gx < 10.f)) gx = 1.f;
    if (!(gy > 0.1f && gy < 10.f)) gy = 1.f;
    const float l = ev.tanLeft, r = ev.tanRight, u = ev.tanUp, d = ev.tanDown;
    for (uintptr_t o : {off::CamProj, off::CamProjFar})
        SetFrustum(reinterpret_cast<float*>(static_cast<uint8_t*>(cam) + o), l, r, u, d, gx, gy);
    static int logs = 0;
    if (logs++ < 4) {
        const auto* p = reinterpret_cast<const float*>(static_cast<uint8_t*>(cam) + off::CamProj);
        const auto* vw = reinterpret_cast<const float*>(static_cast<uint8_t*>(cam) + off::CamView);
        Log("STEREO eye %d camera %p: proj %.3f %.3f %.3f %.3f p11=%.1f guard %.3f %.3f view.t=(%.3f %.3f %.3f)", eye, cam,
            p[0], p[5], p[8], p[9], p[11], gx, gy, vw[12], vw[13], vw[14]);
    }
}

// 1.5 diagnostic (a player: the internal monologue's subtitle on the desktop window, never in the headset): where every
// 2D layer of a frame goes. Read-only: counts, a 10 s line, and each call of the diagnostic-shot frames.
struct Layer2DCensus {
    std::atomic<uint64_t> deferred{0}, otherVpInPass{0}, windowUi{0}, windowAfter{0}, other{0};
    void* otherVps[4] = {};
    int nOther = 0;
};
Layer2DCensus g_l2d;
int g_layerTrace = 0;      // Presents left whose 2D layer calls are logged one by one
uint64_t g_diagFrame = 0;  // the Present that asked for a diagnostic shot (saved from the next one)
void CensusLayer(void* layer, void* vp, uint64_t flag, bool mainVp, bool deferred) {
    const char* where;
    if (deferred) {
        ++g_l2d.deferred;
        where = "main viewport, held back for the UI capture";
    } else if (g_pass && vp) {
        ++g_l2d.otherVpInPass;
        where = "another viewport, drawn INSIDE the eye render (in the eye images)";
        bool known = false;
        for (int i = 0; i < g_l2d.nOther; ++i) known |= g_l2d.otherVps[i] == vp;
        if (!known && g_l2d.nOther < 4) {
            g_l2d.otherVps[g_l2d.nOther++] = vp;
            Log("STEREO 2D layer of another viewport %p (layer %p, flag %llu) drawn inside the eye render", vp, layer,
                static_cast<unsigned long long>(flag));
        }
    } else if (!vp && g_pendingUiSlot >= 0) {
        ++g_l2d.windowUi;
        where = "the window's own 2D layer, inside the UI capture";
    } else if (!vp && !g_pass) {
        ++g_l2d.windowAfter;
        where = g_uiPanel ? "the window's own 2D layer, OUTSIDE the UI capture (desktop only)"
                          : "the window's own 2D layer after the eye renders (ui=eyes: desktop only)";
    } else {
        ++g_l2d.other;
        where = mainVp ? "main viewport outside the eye renders" : "a viewport outside the eye renders";
    }
    if (g_layerTrace > 0)
        Log("STEREO diag 2D layer %p vp %p flag %llu: %s", layer, vp, static_cast<unsigned long long>(flag), where);
    static uint64_t last = 0;
    const uint64_t now = GetTickCount64();
    if (now - last >= 10000) {
        last = now;
        Log("STEREO 2D layers 10 s: main viewport held back %llu, another viewport inside the eye render %llu, window "
            "layer in the UI capture %llu, window layer outside it (desktop only) %llu, other %llu (ui=%s)",
            static_cast<unsigned long long>(g_l2d.deferred.exchange(0)),
            static_cast<unsigned long long>(g_l2d.otherVpInPass.exchange(0)),
            static_cast<unsigned long long>(g_l2d.windowUi.exchange(0)),
            static_cast<unsigned long long>(g_l2d.windowAfter.exchange(0)),
            static_cast<unsigned long long>(g_l2d.other.exchange(0)), g_uiPanel ? "panel" : "eyes");
    }
}

void HookLayer(void* layer, void* vp, uint64_t flag, void* a4) {
    ++g_count.layer;
    bool mainVp = false;
    for (int k = 0; k < g_mainVpCount; ++k) mainVp |= g_mainVps[k] == vp;
    if (xr::Running()) CensusLayer(layer, vp, flag, mainVp, g_pass && mainVp && (g_uiPanel || g_stage == Stage::Gate));
    if (g_pass && mainVp && (g_uiPanel || g_stage == Stage::Gate)) {
        // UI of the main view: held back during the eye passes, drawn once over transparent black
        // afterwards. Secondary viewports keep their 2D inside their own pass.
        for (const auto& d : g_deferred2D)
            if (d.layer == layer && d.vp == vp) return;
        if (g_deferred2D.size() < 32) g_deferred2D.push_back({layer, vp, flag, a4});
        return;
    }
    g_origLayer(layer, vp, flag, a4);
}

ID3D11Texture2D* TextureOfRenderTarget(void* rt) {
    __try {
        if (!rt) return nullptr;
        void* texObj = Read<void*>(rt, off::RtTexObject);
        if (!texObj) return nullptr;
        auto* unk = Read<IUnknown*>(texObj, off::TexD3D);
        if (!unk) return nullptr;
        ID3D11Texture2D* tex = nullptr;
        if (FAILED(unk->QueryInterface(IID_PPV_ARGS(&tex)))) return nullptr;
        tex->Release();  // the engine keeps it alive
        return tex;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

// The image the viewports rendered into: the render-state cache's current target, which the
// viewport passes point at the swapchain's colour target (SWAP_CHAIN+0x18, or +0x28 in the
// game's alternate mode).
ID3D11Texture2D* SceneTarget() {
    auto** rsc = Global<uint8_t*>(g::RenderStateCache);
    void* rt = rsc && *rsc ? Read<void*>(*rsc, off::RscCurrentRt) : nullptr;
    ID3D11Texture2D* tex = TextureOfRenderTarget(rt);
    static bool logged = false;
    if (!logged && tex) {
        logged = true;
        D3D11_TEXTURE2D_DESC d{};
        tex->GetDesc(&d);
        Log("STEREO scene target %ux%u fmt=%d samples=%u bind=0x%x", d.Width, d.Height, int(d.Format), d.SampleDesc.Count,
            d.BindFlags);
    }
    return tex;
}

void CopyEye(int eye, ID3D11Texture2D* src) {
    D3D11_TEXTURE2D_DESC d{};
    src->GetDesc(&d);
    if (d.Width != g_eyeDesc.Width || d.Height != g_eyeDesc.Height || d.Format != g_eyeDesc.Format ||
        d.SampleDesc.Count != g_eyeDesc.SampleDesc.Count) {
        for (auto*& t : g_eyeTex) {
            if (t) t->Release();
            t = nullptr;
        }
        g_eyeDesc = d;
        D3D11_TEXTURE2D_DESC ed = d;
        ed.SampleDesc = {1, 0};
        ed.MipLevels = 1;
        ed.ArraySize = 1;
        ed.Usage = D3D11_USAGE_DEFAULT;
        ed.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        ed.CPUAccessFlags = 0;
        ed.MiscFlags = 0;
        for (auto*& t : g_eyeTex) g_dev->CreateTexture2D(&ed, nullptr, &t);
        ++g_eyeTexGen;
        Log("STEREO eye textures %ux%u fmt=%d (%s)", d.Width, d.Height, int(d.Format), g_eyeTex[1] ? "ok" : "FAILED");
    }
    if (!g_eyeTex[eye]) return;
    if (d.SampleDesc.Count > 1) g_ctx->ResolveSubresource(g_eyeTex[eye], 0, src, 0, d.Format);
    else g_ctx->CopyResource(g_eyeTex[eye], src);
}

// The cameras of every enabled viewport of the list.
// The cameras of the list's MAIN viewports: enabled and covering the whole screen (rect in percent at
// +0x1B4 x, +0x1B8 y, +0x1BC w, +0x1C0 h). Smaller viewports (menu previews, picture-in-picture,
// render-to-texture) keep the game's own camera and are rendered identically in both eyes (playbook
// 23.4: never apply the player-view patch to secondary views). LIVE 2026-10-26: a scene with 5 viewports
// stereo-patched on every camera ended in the game's abort() within 2 s.
void* g_smallScanCams[kMaxCams] = {};  // filled by CollectCameras: the small viewports' cameras
int g_smallScan = 0;
int g_fullScreenCams = 0;  // main cameras of genuinely full-screen viewports (before the small_views fallback)
int CollectCameras(void* list, void* cams[kMaxCams], void* vps[kMaxCams], int* vpCount) {
    int n = 0;
    g_smallScan = 0;
    *vpCount = 0;
    const uint32_t count = Read<uint32_t>(list, off::VpListCount);
    auto** handles = Read<void**>(list, off::VpListArray);
    void* firstVp = nullptr;
    void* firstCam = nullptr;
    int enabled = 0;
    static uint64_t lastLayout = 0;
    uint64_t layout = 1469598103934665603ull;
    for (uint32_t i = 0; handles && i < count && i < 64; ++i) {
        void* vp = ResolveHandle(handles[i]);
        if (!vp || !(Read<uint32_t>(vp, off::VpFlags) & 1)) continue;
        layout = (layout ^ reinterpret_cast<uintptr_t>(vp)) * 1099511628211ull;
        layout = (layout ^ Read<uint32_t>(vp, 0x1BC) ^ (uint64_t(Read<uint32_t>(vp, 0x1B4)) << 32)) * 1099511628211ull;
    }
    const bool logLayout = layout != lastLayout;
    lastLayout = layout;
    for (uint32_t i = 0; handles && i < count && i < 64; ++i) {
        void* vp = ResolveHandle(handles[i]);
        if (!vp || !(Read<uint32_t>(vp, off::VpFlags) & 1)) continue;
        void* cam = ResolveHandle(Read<void*>(vp, off::VpCamera));
        if (!cam) continue;
        ++enabled;
        const float x = Read<float>(vp, 0x1B4), y = Read<float>(vp, 0x1B8), w = Read<float>(vp, 0x1BC),
                    h = Read<float>(vp, 0x1C0);
        if (logLayout && enabled <= 8)
            Log("STEREO viewport %d: vp=%p cam=%p rect x=%.1f y=%.1f w=%.1f h=%.1f %%", enabled, vp, cam, x, y, w, h);
        if (!firstVp) {
            firstVp = vp;
            firstCam = cam;
        }
        const bool full = x <= 0.5f && y <= 0.5f && w >= 99.f && h >= 99.f;
        if (!full) {
            bool dup = false;
            for (int k = 0; k < g_smallScan; ++k) dup |= g_smallScanCams[k] == cam;
            if (!dup && g_smallScan < kMaxCams) g_smallScanCams[g_smallScan++] = cam;
            continue;
        }
        if (*vpCount < kMaxCams) vps[(*vpCount)++] = vp;
        bool dup = false;
        for (int k = 0; k < n; ++k) dup |= cams[k] == cam;
        if (!dup && n < kMaxCams) cams[n++] = cam;
    }
    g_fullScreenCams = n;
    // No full-screen viewport (LIVE: a menu of 5 equal panels): no head-tracked main view. Without
    // small_views=head the screen is shown flat on the floating panel, rendered once.
    if (n == 0 && firstCam && g_smallHead) {
        cams[n++] = firstCam;
        vps[(*vpCount)++] = firstVp;
        int k = 0;
        for (int i = 0; i < g_smallScan; ++i)
            if (g_smallScanCams[i] != firstCam) g_smallScanCams[k++] = g_smallScanCams[i];
        g_smallScan = k;
    }
    return n;
}

// ---- the three-capture side-effect gate, plus a projection-model check (playbook RE-08/RE-09, A5) ----
// Runs in the flat game (stage=gate), no headset. Each gate frame renders the viewport list once
// (A) or twice in the same frame (B, C, D), capturing the scene target after every render:
//   A  stock single render                          -> scene noise floor between frames, capture sanity
//   B  second render, camera unchanged              -> the CONTROL: re-entry side effects (vetoes C/D)
//   C  second render, camera moved one IPD sideways -> the camera seam drives the image
//   D  second render, projection rebuilt by the mod -> the projection model reproduces the engine's
// Decision rules are preregistered in harness_stats.h and docs/STEREO_ROUTE.md.
enum class GateKind { A, B, C, D };
constexpr GateKind kGatePlan[] = {GateKind::A, GateKind::A, GateKind::B, GateKind::C, GateKind::D};
constexpr int kGateCycles = 6;
constexpr float kGateIpd = 0.065f;  // metres, scaled by world_scale
struct GateRun {
    bool armed = true, running = false;
    int eligibleFrames = 0, step = 0, cycle = 0;
    harness::Grid prevA;
    bool havePrevA = false;
    std::vector<double> floorAB, b, c, d, nonBlack;
    std::vector<int> shiftC;
    double msSingle = 0, msSecond = 0;
    int renders = 0;
    uint64_t buildPerRender = 0, layerPerRender = 0;
} g_gate;
std::wstring g_gateDir;

std::wstring GateFile(const char* name) {
    if (g_gateDir.empty()) return L"";
    std::wstring w(name, name + std::strlen(name));
    return g_gateDir + L"\\" + w + L".bmp";
}

double NowMs() {
    LARGE_INTEGER f, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return 1000.0 * double(t.QuadPart) / double(f.QuadPart);
}

// One mod-driven render of the list with 2D held back; returns the capture of the scene target.
bool GateRender(void* list, void* a2, void* a3, void* a4, harness::Grid* out, const char* bmp, double* ms) {
    const Counters before = g_count;
    const double t0 = NowMs();
    g_pass = true;
    g_origList(list, a2, a3, a4);
    g_pass = false;
    *ms += NowMs() - t0;
    g_gate.buildPerRender += g_count.build - before.build;
    g_gate.layerPerRender += g_count.layer - before.layer;
    ++g_gate.renders;
    ID3D11Texture2D* tex = SceneTarget();
    return tex && harness::Capture(g_dev, g_ctx, tex, out, (bmp && g_gate.cycle == 0) ? GateFile(bmp) : L"");
}

void DrawDeferred2D() {
    auto setViewport = reinterpret_cast<SetViewportFn>(Addr(Anchor::RenderSetViewport));
    const uint32_t w = *Global<uint32_t>(g::RenderWidth), h = *Global<uint32_t>(g::RenderHeight);
    for (const auto& d : g_deferred2D) {
        if (setViewport) setViewport(Global<void>(g::RenderContext), 0, 0, w, h);
        g_origLayer(d.layer, d.vp, d.flag, d.a4);
    }
    g_deferred2D.clear();
}

// ---- v1.3.1: MenusMaster's 2D menus (subtitles) into the headset's UI ----
// STATIC 2026-11-24 (HRVR v1.3.1): the subtitle (menu data 0x32c9:0xE) and every other 2D MenusMaster menu block
// are drawn by the master's 2D task 0x140835540 (task priority 0x35) straight into the swap chain's colour target - one
// step AFTER the frame display task (0x34: eye renders, UI capture, DISPLAY_WINDOW). So they reached only the desktop
// window. With ui=panel the mod now runs that draw itself inside the UI capture (right after the held-back 2D layers,
// over transparent black; RGB-only writes, the panel's brightness alpha keeps the text) and skips the game's own call of
// the same frame (no double draw on the desktop, which shows the UI capture anyway). hrvr.ini vr_subtitles (on).
using MenusDraw2DFn = void* (*)(void* master);
MenusDraw2DFn g_origMenus2D = nullptr;
std::atomic<void*> g_menusMaster{nullptr};
bool g_menus2DInUi = true;     // hrvr.ini vr_subtitles
bool g_menus2DDrawn = false;   // drawn into this frame's UI capture: the game's late call is skipped (render thread)
uint64_t g_menus2DEarly = 0, g_menus2DSkipped = 0, g_menus2DGame = 0;

void* HookMenus2D(void* master) {
    if (master) g_menusMaster.store(master, std::memory_order_relaxed);
    if (g_menus2DDrawn) {
        g_menus2DDrawn = false;
        ++g_menus2DSkipped;
        return master;
    }
    ++g_menus2DGame;
    return g_origMenus2D(master);
}

void DrawMenus2DIntoUi() {
    void* master = g_menusMaster.load(std::memory_order_relaxed);
    if (!g_menus2DInUi || !g_origMenus2D || !master) return;
    auto setViewport = reinterpret_cast<SetViewportFn>(Addr(Anchor::RenderSetViewport));
    const uint32_t w = *Global<uint32_t>(g::RenderWidth), h = *Global<uint32_t>(g::RenderHeight);
    if (setViewport) setViewport(Global<void>(g::RenderContext), 0, 0, w, h);
    g_origMenus2D(master);
    g_menus2DDrawn = true;
    ++g_menus2DEarly;
    static uint64_t last = 0;
    const uint64_t now = GetTickCount64();
    if (!last || now - last >= 10000) {
        Log("STEREO 2D menus (subtitles) drawn into the UI capture: %llu frames, the game's late draw skipped %llu, "
            "drawn by the game (desktop only) %llu, master %p (vr_subtitles=on)",
            static_cast<unsigned long long>(g_menus2DEarly), static_cast<unsigned long long>(g_menus2DSkipped),
            static_cast<unsigned long long>(g_menus2DGame), master);
        last = now;
    }
}

void GateReport() {
    using namespace harness;
    const double mb = Median(g_gate.b), mc = Median(g_gate.c), md = Median(g_gate.d);
    const Verdict vb = GateB(g_gate.b.empty() ? -1.0 : mb);
    const Verdict vc = GateC(vb, mb, g_gate.c.empty() ? -1.0 : mc);
    const Verdict vd = GateD(vb, mb, g_gate.d.empty() ? -1.0 : md);
    std::vector<double> shifts(g_gate.shiftC.begin(), g_gate.shiftC.end());
    const double nb = Median(g_gate.nonBlack);
    Log("GATE result (%d cycles, %d renders, units: mean |dLuma| 0..255 on a ~480x270 grid):", g_gate.cycle,
        g_gate.renders);
    Log("GATE   capture sanity: non-black %.2f (need >= %.2f) -> %s", nb, kSanityNonBlack,
        nb >= kSanityNonBlack ? "OK" : "SUSPECT (not the 3D scene?)");
    Log("GATE   A floor between consecutive stock frames: median %.4f MAD %.4f (n=%zu)", Median(g_gate.floorAB),
        Mad(g_gate.floorAB), g_gate.floorAB.size());
    Log("GATE   B re-entry, camera unchanged: median %.4f MAD %.4f max %.4f -> %s", mb, Mad(g_gate.b),
        g_gate.b.empty() ? 0.0 : *std::max_element(g_gate.b.begin(), g_gate.b.end()), VerdictName(vb));
    Log("GATE   C camera +%.3f m sideways: median %.4f (separation %.1fx over max(B, %.2f)), horizontal shift median %.1f "
        "cells -> %s",
        kGateIpd * g_worldScale, mc, mc / std::max(mb, kFloorC), kFloorC, Median(shifts), VerdictName(vc));
    Log("GATE   D projection rebuilt from FOV scalars: median %.4f -> %s", md, VerdictName(vd));
    Log("GATE   cost: single render %.2f ms, second render %.2f ms (mean); per render %.1f CAMERA::Build, %.1f 2D layer "
        "calls",
        g_gate.msSingle / std::max(1, g_gate.cycle * 5), g_gate.msSecond / std::max(1, g_gate.cycle * 3),
        double(g_gate.buildPerRender) / std::max(1, g_gate.renders),
        double(g_gate.layerPerRender) / std::max(1, g_gate.renders));
    Log("GATE   images (first cycle): %s", g_gateDir.empty() ? "(no folder)" : "hrvr_logs\\gate\\*.bmp");
}

void GateFrame(void* list, void* a2, void* a3, void* a4) {
    harness::Grid g1, g2;
    const GateKind kind = kGatePlan[g_gate.step];
    g_deferred2D.clear();
    if (kind == GateKind::A) {
        if (GateRender(list, a2, a3, a4, &g1, "gate_A", &g_gate.msSingle)) {
            g_gate.nonBlack.push_back(harness::NonBlack(g1));
            if (g_gate.havePrevA) g_gate.floorAB.push_back(harness::MeanAbsDiff(g_gate.prevA, g1));
            g_gate.prevA = g1;
            g_gate.havePrevA = true;
        }
    } else {
        const char* n1 = kind == GateKind::B ? "gate_B1" : kind == GateKind::C ? "gate_C1" : "gate_D1";
        const char* n2 = kind == GateKind::B ? "gate_B2" : kind == GateKind::C ? "gate_C2" : "gate_D2";
        ++g_renderPair;
        g_render = 0;
        const bool ok1 = GateRender(list, a2, a3, a4, &g1, n1, &g_gate.msSingle);
        g_render = 1;
        if (kind == GateKind::C) {
            pose::Mat4 rel = pose::Identity();
            rel.m[12] = kGateIpd * g_worldScale;
            for (int k = 0; k < g_camCount; ++k)
                if (uint8_t* node = CameraNode(g_cams[k])) SetNodeRelative(node, rel);
        }
        g_testProj = kind == GateKind::D;
        const bool ok2 = GateRender(list, a2, a3, a4, &g2, n2, &g_gate.msSecond);
        g_render = -1;
        g_testProj = false;
        RestoreNodes();
        if (ok1 && ok2) {
            const double diff = harness::MeanAbsDiff(g1, g2);
            if (kind == GateKind::B) g_gate.b.push_back(diff);
            if (kind == GateKind::C) {
                g_gate.c.push_back(diff);
                g_gate.shiftC.push_back(harness::BestHorizontalShift(g1, g2, 24));
            }
            if (kind == GateKind::D) g_gate.d.push_back(diff);
        }
    }
    DrawDeferred2D();
    if (++g_gate.step >= int(sizeof(kGatePlan) / sizeof(kGatePlan[0]))) {
        g_gate.step = 0;
        if (++g_gate.cycle >= kGateCycles) {
            g_gate.running = false;
            GateReport();
        }
    }
}

void StartGate(const char* why) {
    g_gate = GateRun{};
    g_gate.armed = false;
    g_gate.running = true;
    Log("GATE started (%s): %d cycles of A A B C D", why, kGateCycles);
}

// Bounded observation packet at the list boundary (RE-04): first calls and every camera-set change.
void* g_obsCam = nullptr;  // first viewport camera of the last list (read again after it was built)
void ObserveList(void* list, int camCount, int vpCount, void* const cams[]) {
    if (camCount > 0) g_obsCam = cams[0];
    static uint64_t logged = 0;
    static void* lastCams[kMaxCams] = {};
    static int lastCount = -1;
    const bool changed = camCount != lastCount || std::memcmp(cams, lastCams, sizeof(void*) * kMaxCams) != 0;
    if (!changed && logged >= 6) return;
    if (logged > 200) return;  // bounded
    ++logged;
    std::memcpy(lastCams, cams, sizeof(lastCams));
    lastCount = camCount;
    ID3D11Texture2D* tex = SceneTarget();
    D3D11_TEXTURE2D_DESC d{};
    if (tex) tex->GetDesc(&d);
    Log("OBSERVE list=%p thread=%lu frame=%llu viewports=%d cameras=%d paused=%d video=%d target=%p %ux%u fmt=%d",
        list, GetCurrentThreadId(), static_cast<unsigned long long>(g_frame), vpCount, camCount, int(Paused()),
        int(videowatch::Playing(GetTickCount64())), static_cast<void*>(tex), d.Width, d.Height, int(d.Format));
}

void HookList(void* list, void* a2, void* a3, void* a4) {
    ++g_count.list;
    std::unique_lock lock(g_lock);
    void* cams[kMaxCams] = {};
    void* vps[kMaxCams] = {};
    int vpCount = 0;
    const int camCount = list ? CollectCameras(list, cams, vps, &vpCount) : 0;
    std::memcpy(g_mainVps, vps, sizeof(g_mainVps));
    g_mainVpCount = vpCount;
    ObserveList(list, camCount, vpCount, cams);
    const bool first = !g_stereoThisFrame && g_eye < 0 && g_ctx && g_dev && list;

    if (g_stage == Stage::Observe || !first) {
        g_origList(list, a2, a3, a4);
        return;
    }

    if (g_stage == Stage::Gate) {
        g_stereoThisFrame = true;
        const bool eligible = camCount > 0 && !Paused() && !videowatch::Playing(GetTickCount64());
        std::memcpy(g_cams, cams, sizeof(g_cams));
        g_camCount = camCount;
        if (g_gate.running && eligible) {
            GateFrame(list, a2, a3, a4);
            return;
        }
        if (!g_gate.running) {
            g_gate.eligibleFrames = eligible ? g_gate.eligibleFrames + 1 : 0;
            if (g_gate.armed && g_gate.eligibleFrames >= 600) StartGate("10+ s of gameplay frames");
        }
        g_origList(list, a2, a3, a4);
        return;
    }

    // XR stages
    xr::EyeView eyes[2];
    xr::FrameInput input{};
    const int slot = xr::BeginGameFrame(eyes, &input);
    if (slot < 0 || !xr::Running()) {
        g_origList(list, a2, a3, a4);
        return;
    }
    vrinput::Publish(devtools::Filter(input));  // F7 menu open: the game gets no buttons
    if (camCount == 0) {
        // Nothing 3D in this list (2D-only screen): the waited frame is shown flat at Present.
        g_origList(list, a2, a3, a4);
        return;
    }
    g_stereoThisFrame = true;
    if (g_stage != Stage::Vr) {
        // Transport proofs: one ordinary (mono) render, the same image to both eyes (xrflat) or a
        // known solid colour per eye (xrcolor). Not stereo; proves the image path only.
        g_origList(list, a2, a3, a4);
        if (ID3D11Texture2D* src = SceneTarget()) {
            CopyEye(0, src);
            if (g_eyeTex[0]) {
                D3D11_TEXTURE2D_DESC d{};
                g_eyeTex[0]->GetDesc(&d);
                xr::OnEye(0, slot, g_ctx, g_eyeTex[0], 0, d.Height, false);
                xr::OnEye(1, slot, g_ctx, g_eyeTex[0], 0, d.Height, false);
                g_lastStereoMs = GetTickCount64();
            }
        }
        return;
    }
    if (camCount != g_camCount || std::memcmp(cams, g_cams, sizeof(void*) * camCount) != 0)
        for (int k = 0; k < camCount; ++k) Log("STEREO viewport camera %d/%d cam=%p", k + 1, camCount, cams[k]);
    std::memcpy(g_cams, cams, sizeof(g_cams));
    g_camCount = camCount;
    std::memcpy(g_smallCams, g_smallScanCams, sizeof(g_smallCams));
    g_smallCount = g_smallHead ? g_smallScan : 0;
    std::memcpy(g_eyes, eyes, sizeof(g_eyes));
    g_deferred2D.clear();
    // First person decides once per pair, from the game's camera (the node holds the game's world here:
    // every eye pass restores it) and this pair's input. Only a single full-screen view qualifies.
    {
        uint8_t* node = CameraNode(g_cams[0]);
        pose::Mat4 authored = pose::Identity();
        if (node) std::memcpy(authored.m, node + off::NodeWorld, sizeof(authored.m));
        const bool single = g_fullScreenCams == 1 && g_camCount == 1;
        fpcam::Prepare(node, authored, input, single);
        // The sound listener follows the head these eyes are drawn from (audio.h): on the first-person base
        // or on the game camera.
        pose::Mat4 base;
        const bool fp = node && fpcam::Base(node, &base);
        {
            const pose::Vec3 push = !fp && single && node ? ThirdPersonPush(authored) : pose::Vec3{};
            const bool had = g_pushOffset.x != 0.f || g_pushOffset.z != 0.f, has = push.x != 0.f || push.z != 0.f;
            g_pushOffset = push;
            static int logs = 0;
            if (had != has && logs++ < 20)
                Log("STEREO third-person camera %s (camera_forward %.2f m, this frame %.2f m)",
                    has ? "moved closer" : "at the game's place", g_cameraPush.load(), std::sqrt(push.x * push.x + push.z * push.z));
            authored.m[12] += push.x;  // the listener hears from where the eyes are
            authored.m[14] += push.z;
        }
        pose::Mat4 head = input.headRel;
        for (int i = 12; i < 15; ++i) head.m[i] *= g_worldScale;
        audio::Publish(pose::Mul(head, fp ? base : authored), authored, fp,
                       single && node && xr::FrameInputUsable(input, GetTickCount64()));
    }
    // AFR: this frame draws one eye, unless the other eye has no recent image to keep (the start, after a
    // flat screen): then both, as Native.
    int only = -1;
    bool aerStart = false, aerFinish = false;
    static uint64_t aerGen = 0;
    const int method = g_method.load(std::memory_order_relaxed);
    // Mono goes by the packet (its views were made the head centre in BeginGameFrame), not by the setting, so a
    // switch between the game and render threads never mixes a centre view with a stereo submission.
    const bool mono = xr::MonoSlot(slot);
    if (mono) {
        only = 0;  // the same eye slot every frame: one history
        static bool logged = false;
        if (!logged) {
            logged = true;
            Log("STEREO Mono: one render per frame from the centre of the head, the same picture to both eyes");
        }
    } else if (method == kAfr) {
        const int e = g_afrNext;
        if (xr::KeptEyeFresh(1 - e, 150)) only = e;
        g_afrNext = only >= 0 ? 1 - only : 0;
        static bool logged = false;
        if (!logged && only >= 0) {
            logged = true;
            Log("STEREO AFR: one eye per frame from now on (eye %d first)", only);
        }
    } else if (method == kAer) {
        // AER: finish the pair (eye 1 with the held views) or start one (eye 0, views held); both eyes of a
        // fresh pair must exist to show while a pair is in the making, otherwise this frame is Native.
        if (g_aerPhase == 1 && aerGen == g_eyeTexGen && xr::UseHeldViews(slot)) {
            only = 1;
            aerFinish = true;
            std::memcpy(g_eyes, g_aerEyes, sizeof(g_eyes));
            g_aerPhase = 0;
        } else if (xr::KeptEyeFresh(0, 200) && xr::KeptEyeFresh(1, 200)) {
            only = 0;
            aerStart = true;
            xr::HoldViews(slot);
            std::memcpy(g_aerEyes, g_eyes, sizeof(g_aerEyes));
            aerGen = g_eyeTexGen;
            g_aerPhase = 1;
        } else {
            g_aerPhase = 0;
        }
        static bool logged = false;
        if (!logged && aerStart) {
            logged = true;
            Log("STEREO AER: one eye per frame, both eyes of a pair from the same head pose");
        }
    }
    auto setup = reinterpret_cast<SetupCameraFn>(Addr(Anchor::ViewportSetupCamera));
    struct DofSave {
        uint8_t* cam;
        uint8_t dof, noise;  // the game's switches before the eye passes
    } dofSaved[2 * kMaxCams];
    int dofCount = 0;
    if (!g_vrDof || !g_vrGrain) {
        int dofOff = 0, grainOff = 0;
        auto clear = [&](void* camp) {
            auto* cam = static_cast<uint8_t*>(camp);
            if (!cam || dofCount >= 2 * kMaxCams) return;
            for (int i = 0; i < dofCount; ++i)
                if (dofSaved[i].cam == cam) return;
            const uint8_t dof = cam[off::CamDofEnable], noise = cam[off::CamNoiseEnable];
            const bool clearDof = !g_vrDof && dof, clearNoise = !g_vrGrain && noise;
            if (!clearDof && !clearNoise) return;
            dofSaved[dofCount++] = {cam, dof, noise};
            if (clearDof) {
                cam[off::CamDofEnable] = 0;
                ++dofOff;
            }
            if (clearNoise) {
                cam[off::CamNoiseEnable] = 0;
                ++grainOff;
            }
        };
        for (int k = 0; k < g_camCount; ++k) clear(g_cams[k]);
        for (int k = 0; k < g_smallCount; ++k) clear(g_smallCams[k]);
        static bool loggedDof = false, loggedGrain = false;
        if (dofOff && !loggedDof) {
            loggedDof = true;
            Log("STEREO depth of field off in the headset (vr_dof=off): %d camera(s)", dofOff);
        }
        if (grainOff && !loggedGrain) {
            loggedGrain = true;
            Log("STEREO film grain off in the headset (vr_film_grain=off): %d camera(s)", grainOff);
        }
    }
    bool copied = true;
    ++g_renderPair;
    if (only < 0) InstallDrawProbe(g_ctx);
    g_drawNow[0] = g_drawNow[1] = 0;
    g_diffActive = g_diffArmed && only < 0;
    g_diffArmed = false;
    if (g_diffActive) std::memset(g_drawSites, 0, sizeof(g_drawSites));
    for (int eye = 0; eye < 2; ++eye) {
        if (only >= 0 && eye != only) continue;
        g_eye = eye;
        g_render = only >= 0 ? 0 : eye;  // a single render is a first render: nothing to rewind
        const int poseEye = g_eyeSame.load(std::memory_order_relaxed) ? 0 : eye;  // `eyesame on` (draw probe)
        for (int k = 0; k < g_camCount; ++k) {
            if (uint8_t* node = CameraNode(g_cams[k])) ApplyEye(node, poseEye);
            if (g_engineFov) ApplyEngineFov(g_cams[k]);
        }
        for (int k = 0; k < g_smallCount; ++k)
            if (uint8_t* node = CameraNode(g_smallCams[k])) SetNodeRelative(node, EyeRel(poseEye));
        if (g_prime && setup)
            for (int k = 0; k < vpCount; ++k) setup(vps[k], static_cast<uint8_t*>(vps[k]) + off::VpViewParams, 0);
        // 0.36.5: the first of two renders leaves the close-up light lists to the second (the game's own switch, as its
        // tiled screenshot does); the second clears them as the game does. A single render (AFR / AER / Mono) is untouched.
        uint8_t* keep = only < 0 && eye == 0 && g_closeUpKeep.load(std::memory_order_relaxed) ? CloseUpKeepFlag() : nullptr;
        bool kept = false;
        if (keep) {
            if (*keep) {
                g_closeUpAlreadySet.fetch_add(1, std::memory_order_relaxed);
            } else {
                *keep = 1;
                kept = true;
            }
        }
        g_pass = true;
        g_origList(list, a2, a3, a4);
        g_pass = false;
        if (kept) {
            *keep = 0;
            g_closeUpKept.fetch_add(1, std::memory_order_relaxed);
        }
        RestoreNodes();
        RestoreEngineFov();
        ID3D11Texture2D* src = SceneTarget();
        if (src) {
            CopyEye(eye, src);
            if (g_eyeTex[eye]) {
                D3D11_TEXTURE2D_DESC d{};
                g_eyeTex[eye]->GetDesc(&d);
                if (mono) {
                    xr::OnMonoEye(slot, g_ctx, g_eyeTex[eye], 0, d.Height, /*uiFollows=*/g_uiPanel);
                } else if (aerStart) {
                    xr::OnKeepFrame(slot, g_ctx, /*uiFollows=*/g_uiPanel);  // shows the last full pair
                } else if (aerFinish && g_eyeTex[0]) {
                    D3D11_TEXTURE2D_DESC d0{};
                    g_eyeTex[0]->GetDesc(&d0);
                    xr::OnEye(0, slot, g_ctx, g_eyeTex[0], 0, d0.Height, false);
                    xr::OnEye(1, slot, g_ctx, g_eyeTex[1], 0, d.Height, /*uiFollows=*/g_uiPanel);
                } else if (only >= 0) {
                    xr::OnSingleEye(eye, slot, g_ctx, g_eyeTex[eye], 0, d.Height, /*uiFollows=*/g_uiPanel);
                } else {
                    xr::OnEye(eye, slot, g_ctx, g_eyeTex[eye], 0, d.Height, /*uiFollows=*/eye == 1 && g_uiPanel);
                }
            }
        } else {
            copied = false;
        }
    }
    g_eye = -1;
    g_render = -1;
    if (only < 0) NoteDrawPair();
    LogShadowProbe();
    LogLightGather();
    for (int i = 0; i < dofCount; ++i) {
        dofSaved[i].cam[off::CamDofEnable] = dofSaved[i].dof;
        dofSaved[i].cam[off::CamNoiseEnable] = dofSaved[i].noise;
    }
    ++g_pairs;
    g_lastStereoMs = GetTickCount64();
    if (!copied) {
        static int logs = 0;
        if (logs++ < 5) Log("STEREO the scene target was not readable: eye copy skipped");
        DrawDeferred2D();
        return;
    }
    static int lastSmall = -1;
    if (g_pairs <= 3 || g_pairs % 1800 == 0 || g_smallCount != lastSmall)
        Log("STEREO pair %llu: %d main camera(s), %d small view camera(s), %d main viewport(s), %zu 2D layer(s) to the "
            "panel, light-table rewinds %llu saves %llu, particle buffers (11 a frame) deepest left %d right %d, rewinds "
            "%llu saves %llu, thread %lu",
            static_cast<unsigned long long>(g_pairs), g_camCount, g_smallCount, vpCount, g_deferred2D.size(),
            static_cast<unsigned long long>(g_lightRewinds.load()), static_cast<unsigned long long>(g_lightSaves.load()),
            g_particleMost[0].exchange(0), g_particleMost[1].exchange(0),
            static_cast<unsigned long long>(g_particleRewinds.load()),
            static_cast<unsigned long long>(g_particleSaves.load()), GetCurrentThreadId());
    lastSmall = g_smallCount;
    if (g_uiPanel) {
        // UI over transparent black (2D layers held back above, then the window's own 2D layer,
        // drawn by DISPLAY_WINDOW::Display after we return); HookWindow lifts it onto the panel.
        if (ID3D11Texture2D* bb = SceneTarget()) xr::OnUiBegin(slot, g_ctx, bb);
        DrawDeferred2D();
        DrawMenus2DIntoUi();  // v1.3.1: subtitles / 2D HUD menus (otherwise drawn after the capture, desktop only)
        g_pendingUiSlot = slot;
    } else {
        DrawDeferred2D();
    }
}

void HookWindow(void* win, uint64_t flag, void* a3, void* a4) {
    ++g_count.window;
    fpbody::DrawBegin();  // 0.32.0 item seam: held objects take their drawn place only inside this draw
    g_origWindow(win, flag, a3, a4);
    fpbody::DrawEnd();
    std::unique_lock lock(g_lock);
    const int slot = g_pendingUiSlot;
    g_pendingUiSlot = -1;
    if (slot < 0) return;
    // Before the game blits this frame into its exposure history, the mirror is scene + UI again.
    if (ID3D11Texture2D* bb = SceneTarget()) xr::OnUiEnd(slot, g_ctx, bb, g_eyeTex[0]);  // the desktop: the left eye, steadily (08.2)
}

HRESULT STDMETHODCALLTYPE HookResize(IDXGISwapChain* sc, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT flags) {
    {
        std::unique_lock lock(g_lock);
        xr::ReleaseBackbufferViews();
    }
    const HRESULT hr = g_origResize(sc, n, w, h, f, flags);
    Log("STEREO ResizeBuffers %ux%u -> 0x%08lx", w, h, static_cast<unsigned long>(hr));
    return hr;
}

// Playbook 07.5 (GoW vr_lens_flares=0): Heavy Rain's lens flares are screen-space ghost chains placed along the line
// from the image centre through the projected light (STATIC: LENSE_FLARE::DisplayAll 0x140877d10 ->
// camerasystem_1409a9220). In a headset each eye has its own centre, so the chain lands in different directions in the
// two eyes. vr_lens_flares=off (default): not drawn in the eye passes; their occlusion queries still run.
using FlareDisplayFn = void (*)(void*, void*, void*, int);
FlareDisplayFn g_origFlares = nullptr;
bool g_vrFlares = false;
std::atomic<uint64_t> g_flaresSkipped{0};
void HookFlares(void* display, void* camera, void* a3, int a4) {
    if (g_pass && !g_vrFlares) {
        if (g_flaresSkipped.fetch_add(1, std::memory_order_relaxed) == 0)
            Log("STEREO lens flares not drawn in the headset (vr_lens_flares=off; screen-space sprites, playbook 07.5)");
        return;
    }
    g_origFlares(display, camera, a3, a4);
}

// ---- read-only shadow probe (0.30.1; owner: sun shadows only in the LEFT eye in Native) ----
// STATIC 2026-11-19 (TARGET_ADAPTER_MAP "Shadow maps"): every viewport pass - so each eye render - runs the shadow
// stage (SHADOW::Stage 0x1409ec450: a new pool round, the cascades refit, the projectors' jobs). A job gets no shadow
// in exactly two ways: no free map in the pool (SHADOW::AllocMap leaves job+0x578 = 0) or an empty caster list
// (SHADOW::RenderDirectional only clears the map). What makes one of them happen only in the second render is not
// visible statically, so each is counted per render of a pair; nothing is changed.
struct ShadowCounts {
    std::atomic<uint64_t> stages{0}, jobs{0}, allocs{0}, noMap{0}, drawn{0}, empty{0}, noZone{0};
};
ShadowCounts g_shadow[3];  // left render, right render, other (single renders, flat frames)
ShadowCounts& ShadowSlot() { return g_shadow[g_render == 0 ? 0 : g_render == 1 ? 1 : 2]; }
using ShadowStageFn = void (*)(void*, void*);
using ShadowAllocFn = void (*)(void*, void*, void*);
using ShadowRenderFn = void (*)(void*, void*, void*);
ShadowStageFn g_origShadowStage = nullptr;
ShadowAllocFn g_origShadowAlloc = nullptr;
ShadowRenderFn g_origShadowRender = nullptr;
void HookShadowStage(void* display, void* ctx) {
    ShadowCounts& c = ShadowSlot();
    c.stages.fetch_add(1, std::memory_order_relaxed);
    g_origShadowStage(display, ctx);
}
void HookShadowAlloc(void* light, void* a2, void* job) {
    g_origShadowAlloc(light, a2, job);
    ShadowCounts& c = ShadowSlot();
    c.allocs.fetch_add(1, std::memory_order_relaxed);
    void* map = nullptr;
    if (job && !obj::Get(job, 0x578, map)) map = nullptr;
    if (!map) c.noMap.fetch_add(1, std::memory_order_relaxed);
}
void HookShadowRender(void* light, void* display, void* job) {
    ShadowCounts& c = ShadowSlot();
    c.jobs.fetch_add(1, std::memory_order_relaxed);
    // the same test the game makes (0x14098f340): both caster spans empty -> the map is only cleared
    const uint64_t* spans = nullptr;
    uint64_t s[4] = {};
    bool empty = false;
    if (display && obj::Get(display, 0x260, spans) && spans && obj::Bytes(spans, 0, s, sizeof(s)))
        empty = s[1] <= s[0] && s[3] <= s[2];
    (empty ? c.empty : c.drawn).fetch_add(1, std::memory_order_relaxed);
    // STATIC (0x14098f870): the job's start zone job+0xd0 comes from the light's position (0x140918b20) or, for some
    // jobs, from the current viewport's visible-zone list (viewport+0x268 -> +0x1d0 / +0x1e8); 0 = none
    void* zone = nullptr;
    if (job && obj::Get(job, 0xd0, zone) && !zone) c.noZone.fetch_add(1, std::memory_order_relaxed);
    g_origShadowRender(light, display, job);
}
void LogShadowProbe() {
    static uint64_t last = 0;
    const uint64_t now = GetTickCount64();
    if (now - last < 10000) return;
    last = now;
    uint64_t v[3][7];
    uint64_t any = 0;
    for (int i = 0; i < 3; ++i) {
        ShadowCounts& c = g_shadow[i];
        const uint64_t x[7] = {c.stages.exchange(0), c.allocs.exchange(0), c.noMap.exchange(0), c.jobs.exchange(0),
                               c.drawn.exchange(0), c.empty.exchange(0), c.noZone.exchange(0)};
        for (int k = 0; k < 7; ++k) any += (v[i][k] = x[k]);
    }
    if (!any) return;
    char part[3][96];
    for (int i = 0; i < 3; ++i)
        sprintf_s(part[i], "%llu/%llu/%llu/%llu/%llu/%llu/%llu", (unsigned long long)v[i][0], (unsigned long long)v[i][1],
                  (unsigned long long)v[i][2], (unsigned long long)v[i][3], (unsigned long long)v[i][4],
                  (unsigned long long)v[i][5], (unsigned long long)v[i][6]);
    Log("STEREO shadows 10 s (stage calls / map requests / no map / map renders / drawn / EMPTY casters / no start "
        "zone): left %s | right %s | single %s", part[0], part[1], part[2]);
}

bool Hook(void* target, void* detour, void** orig, const char* name) {
    if (!target) return false;
    MH_STATUS s = MH_CreateHook(target, detour, orig);
    if (s == MH_OK) s = MH_EnableHook(target);
    Log("hook %-28s %p %s", name, target, MH_StatusToString(s));
    return s == MH_OK;
}

void FirstPresent() {
    void* scObj = *Global<void*>(g::MainSwapChain);
    if (!scObj) return;
    auto* sc = Read<IDXGISwapChain*>(scObj, off::SwapChainDxgi);
    if (!sc) return;
    sc->AddRef();
    g_sc = sc;
    sc->GetDevice(IID_PPV_ARGS(&g_dev));
    if (g_dev) g_dev->GetImmediateContext(&g_ctx);
    crash::NoteDevice(g_dev);
    void** vt = *reinterpret_cast<void***>(sc);
    Hook(vt[13], reinterpret_cast<void*>(&HookResize), reinterpret_cast<void**>(&g_origResize),
         "IDXGISwapChain::ResizeBuffers");
    const uint32_t* tid = Global<uint32_t>(g::RenderThreadId);
    Log("STEREO device ready at the first Present (thread %lu, engine render thread %lu)", GetCurrentThreadId(),
        tid ? *tid : 0u);
}

ID3D11Texture2D* PresentImage() {
    ID3D11Texture2D* bb = nullptr;
    if (!g_sc || FAILED(g_sc->GetBuffer(0, IID_PPV_ARGS(&bb)))) return nullptr;
    bb->Release();  // the swapchain keeps it alive until Present returns
    return bb;
}

// Command seam (playbook A5.6): <game>\hrvr_cmd.txt is polled about once a second; every line is a
// command, the file is consumed and <game>\hrvr_cmd.ack records what ran.
void PollCommands() {
    static uint64_t next = 0;
    const uint64_t now = GetTickCount64();
    if (now < next) return;
    next = now + 1000;
    const std::wstring dir = IniPath().substr(0, IniPath().find_last_of(L"\\/"));
    const std::wstring cmd = dir + L"\\hrvr_cmd.txt", ack = dir + L"\\hrvr_cmd.ack";
    FILE* f = nullptr;
    if (_wfopen_s(&f, cmd.c_str(), L"r") != 0 || !f) return;
    char line[128];
    std::string done;
    while (fgets(line, sizeof(line), f)) {
        std::string c(line);
        while (!c.empty() && (c.back() == '\n' || c.back() == '\r' || c.back() == ' ')) c.pop_back();
        if (c.empty() || c[0] == '#') continue;
        if (c == "gate") {
            if (g_stage == Stage::Gate) {
                g_gate = GateRun{};  // armed: starts after 10 s of gameplay frames
                Log("CMD gate: armed");
                done += "gate armed\n";
            } else {
                done += "gate refused: stage is not gate\n";
            }
        } else if (c == "eyesame on" || c == "eyesame off") {
            g_eyeSame = c == "eyesame on";
            g_drawStats = DrawStats{};
            Log("STEREO eyesame %s: %s", g_eyeSame ? "on" : "off",
                g_eyeSame ? "both renders use the left eye's pose (test; the picture is flat)" : "each eye its own pose");
            done += c + "\n";
        } else if (c == "closeupkeep on" || c == "closeupkeep off") {
            g_closeUpKeep = c == "closeupkeep on";
            Log("STEREO closeupkeep %s: %s", g_closeUpKeep ? "on" : "off",
                g_closeUpKeep ? "the close-up light lists are kept for the right render (Native)"
                              : "the game's own behaviour (the right render sees the lists emptied)");
            done += c + "\n";
        } else if (c == "prime on" || c == "prime off") {
            g_prime = c == "prime on";
            g_drawStats = DrawStats{};
            Log("STEREO prime %s (camera setup run before each eye pass)", g_prime ? "on" : "off");
            done += c + "\n";
        } else if (c == "diagshot") {
            xr::RequestDiagShot();
            g_diagFrame = g_frame;
            g_layerTrace = 3;
            done += c + "\n";
        } else if (c == "observe") {
            if (g_camCount) LogModel("CMD camera", g_cams[0]);
            done += "observe\n";
        } else if (c == "method native" || c == "method afr" || c == "method aer" || c == "method mono") {
            SetMethod(c == "method native" ? kNative : c == "method afr" ? kAfr : c == "method aer" ? kAer : kMono);
            done += c + "\n";
        } else if (g_stage == Stage::Vr && [&] {
                       const std::string verb = c.substr(0, c.find(' '));
                       const char* arg = c.find(' ') == std::string::npos ? nullptr : c.c_str() + c.find(' ') + 1;
                       return fpcam::Command(verb.c_str(), arg) || audio::Command(verb.c_str(), arg) ||
                              rendersize::Command(verb.c_str(), arg) || menus::Command(verb.c_str(), arg) ||
                              devtools::Command(verb.c_str(), arg);
                   }()) {
            done += c + "\n";
        } else {
            Log("CMD unknown '%s'", c.c_str());
            done += "unknown " + c + "\n";
        }
    }
    fclose(f);
    DeleteFileW(cmd.c_str());
    if (_wfopen_s(&f, ack.c_str(), L"w") == 0 && f) {
        fprintf(f, "%llu\n%s", static_cast<unsigned long long>(g_frame), done.c_str());
        fclose(f);
    }
}

// Bounded frame-boundary packet (RE-04): call rates per Present, phase, backbuffer identity.
void ObservePresent() {
    static uint64_t lastLog = 0;
    static double lastMs = 0;
    static Counters last;
    const bool due = g_frame <= 3 || g_frame - lastLog >= 300;
    if (!due) return;
    const double now = NowMs();
    const uint64_t frames = g_frame - lastLog;
    ID3D11Texture2D* bb = PresentImage();
    D3D11_TEXTURE2D_DESC d{};
    if (bb) bb->GetDesc(&d);
    const double perFrame = frames ? 1.0 / double(frames) : 0.0;
    Log("OBSERVE present frame=%llu thread=%lu fps=%.1f per-frame: window=%.2f list=%.2f build=%.2f layer2d=%.2f | "
        "paused=%d video=%d backbuffer=%p %ux%u fmt=%d samples=%u xr=%d",
        static_cast<unsigned long long>(g_frame), GetCurrentThreadId(),
        lastMs > 0 && frames ? 1000.0 * double(frames) / (now - lastMs) : 0.0,
        double(g_count.window - last.window) * perFrame, double(g_count.list - last.list) * perFrame,
        double(g_count.build - last.build) * perFrame, double(g_count.layer - last.layer) * perFrame, int(Paused()),
        int(videowatch::Playing(GetTickCount64())), static_cast<void*>(bb), d.Width, d.Height, int(d.Format),
        d.SampleDesc.Count, int(xr::Running()));
    lastLog = g_frame;
    lastMs = now;
    last = g_count;
    // After this frame's CAMERA::Build: the model check reads what the engine actually rendered with.
    // (Not in the XR stages: there the projection read back is the mod's own eye frustum.)
    if (g_obsCam && g_count.build > 0 && !XrStage()) LogModel("OBSERVE   camera", g_obsCam);
}

void HookPresent(void* a1, uint64_t sync, void* a3, void* a4) {
    {
        std::unique_lock lock(g_lock);
        ++g_count.present;
        ++g_frame;
        g_menus2DDrawn = false;  // v1.3.1: a UI-capture draw the game's 2D menu task did not consume ends with the frame
        if (!g_sc) {
            FirstPresent();
            Log("CANARY first Present hooked in stage=%s: the mod's code runs inside the game's frame", StageName(g_stage));
        }
        ObservePresent();
        PollCommands();
        if (XrStage()) {
            static hotkeys::Edge f9, f10, f11;
            if (f9.Feed(VK_F9)) {
                g_xrWanted = !g_xrWanted.load();
                Log("XR %s requested (F9)", g_xrWanted ? "on" : "off");
            }
            static hotkeys::Edge f12;
            if (f12.Feed(VK_F12)) {  // owner: just F12 (Steam's screenshot key too - both fire)
                xr::RequestDiagShot();
                g_diagFrame = g_frame;
                g_layerTrace = 3;
            }
            if (f10.Feed(VK_F10)) vrinput::SetPadType(2);
            if (f11.Feed(VK_F11)) vrinput::SetPadType(5);
            if (g_stage == Stage::Vr) {
                fpcam::Frame();
                devtools::Frame();  // developer build only (devtools.h): F7 / F8
            }
            sync = pacing::OnPresent(unsigned(sync), xr::Running());
            if (g_pendingUiSlot >= 0) {  // a UI frame that never reached the window hook
                if (ID3D11Texture2D* bb = SceneTarget()) xr::OnUiEnd(g_pendingUiSlot, g_ctx, bb, g_eyeTex[0]);
                g_pendingUiSlot = -1;
            }
            // Every Present completes one XR frame: 2D-only screens (menus, loading, videos) and
            // stalled pairs are shown flat (B2S FlatFrame).
            static uint64_t lastEnded = 0;
            if (g_ctx && xr::Running() && xr::FramesEnded() == lastEnded) {
                const bool recent = g_lastStereoMs && GetTickCount64() - g_lastStereoMs < 300;
                if (ID3D11Texture2D* bb = PresentImage()) {
                    xr::FrameInput input{};
                    xr::FlatFrame(g_ctx, bb, g_eyeTex, /*showPanel=*/!recent, &input);
                    vrinput::Publish(devtools::Filter(input));
                }
            }
            xr::Service(g_dev, g_xrWanted);
            if (!xr::Running()) vrinput::Suspend();
            else vrinput::LogStatus();
            lastEnded = xr::FramesEnded();
            // Square VR picture while the headset shows 3D frames; the game's own size otherwise.
            if (g_stage == Stage::Vr)
                rendersize::Frame(xr::Running(), g_lastStereoMs && GetTickCount64() - g_lastStereoMs < 300, xr::VideoMode());
            // 28.1: the summary block once VR runs, again when the frame size, 3D / flat or the controllers change
            summary::Tick(xr::Running(), g_lastStereoMs && GetTickCount64() - g_lastStereoMs < 300);
            // The desktop window's picture, the very last step (08.4): every headset copy is made; the square 3D view
            // cropped to fill the 16:9 window, flat screens kept whole.
            if (g_layerTrace > 0) --g_layerTrace;
            if (g_ctx && xr::Running() && xr::DiagShotPending() && g_frame > g_diagFrame)
                if (ID3D11Texture2D* bb = PresentImage()) xr::DiagShotAtPresent(g_ctx, bb, g_eyeTex[0]);
            int cw = 0, ch = 0;
            bool crop = false;
            if (g_stage == Stage::Vr && g_ctx && xr::Running() && rendersize::DesktopPicture(&cw, &ch, &crop))
                if (ID3D11Texture2D* bb = PresentImage()) {
                    D3D11_TEXTURE2D_DESC d{};
                    bb->GetDesc(&d);
                    const bool square3D = rendersize::Applied() && d.Width == d.Height &&
                                          g_lastStereoMs && GetTickCount64() - g_lastStereoMs < 300 && !xr::VideoMode();
                    xr::DesktopFit(g_ctx, bb, cw, ch, crop && square3D);
                }
        }
        g_stereoThisFrame = false;
    }
    g_origPresent(a1, sync, a3, a4);
}

}  // namespace

bool Install() {
    // 0.36.14 (owner: "both demos didnt turn into vr" - LIVE: a fresh install wrote the built-in hrvr.ini with
    // stage=observe, the read-only test stage): the shipped default is vr; the test stages stay for hrvr.ini.
    const std::wstring stage = IniString(L"stage", L"vr");
    g_stage = _wcsicmp(stage.c_str(), L"gate") == 0      ? Stage::Gate
              : _wcsicmp(stage.c_str(), L"xrcolor") == 0 ? Stage::XrColor
              : _wcsicmp(stage.c_str(), L"xrflat") == 0  ? Stage::XrFlat
              : _wcsicmp(stage.c_str(), L"vr") == 0      ? Stage::Vr
                                                         : Stage::Observe;
    g_xrWanted = XrStage() && IniOn(L"vr", true);
    g_uiPanel = _wcsicmp(IniString(L"ui", L"panel").c_str(), L"eyes") != 0;
    g_prime = IniOn(L"prime", false);
    g_smallHead = _wcsicmp(IniString(L"small_views", L"fixed").c_str(), L"head") == 0;
    g_engineFov = IniOn(L"enginefov", true);
    {
        // rendering_method wins; an older afr_rendering=on still means AFR.
        const std::wstring m = IniString(L"rendering_method", L"");
        g_method = _wcsicmp(m.c_str(), L"native") == 0 ? kNative
                   : _wcsicmp(m.c_str(), L"aer") == 0  ? kAer
                   : _wcsicmp(m.c_str(), L"mono") == 0 ? kMono
                   : _wcsicmp(m.c_str(), L"afr") == 0  ? kAfr
                   : m.empty() && IniString(L"afr_rendering", L"").size() ? (IniOn(L"afr_rendering", false) ? kAfr : kNative)
                                                                           : kNative;  // 1.0.2: Native default (owner)
    }
    xr::SetMono(g_method == kMono);
    g_vrDof = IniOn(L"vr_dof", false);
    g_vrGrain = IniOn(L"vr_film_grain", false);
    g_yellowFilter = IniOn(L"vr_yellow_filter", true);
    const float scale = float(_wtof(IniString(L"world_scale", L"1.0").c_str()));
    g_worldScale = scale > 0.001f && scale < 1000.f ? scale : 1.f;
    xr::SetSymmetricFov(IniOn(L"symfov", true));
    xr::SetKnownColorTest(g_stage == Stage::XrColor);
    if (g_stage == Stage::Gate) {
        g_gateDir = std::wstring(LogDir()) + L"\\gate";
        CreateDirectoryW(g_gateDir.c_str(), nullptr);
    }
    Log("STEREO stage=%s (observe -> gate -> xrcolor -> xrflat -> vr); small_views=%s vr=%d ui=%s prime=%d enginefov=%d world_scale=%.3f",
        StageName(g_stage), g_smallHead ? "head" : "fixed", int(g_xrWanted.load()), g_uiPanel ? "panel" : "eyes", int(g_prime), int(g_engineFov),
        g_worldScale);
    videowatch::Install();
    pacing::Install();
    bool ok = true;
    ok &= Hook(Addr(Anchor::CameraBuild), reinterpret_cast<void*>(&HookBuild), reinterpret_cast<void**>(&g_origBuild),
               "CAMERA::Build");
    ok &= Hook(Addr(Anchor::Layer2DDisplay), reinterpret_cast<void*>(&HookLayer), reinterpret_cast<void**>(&g_origLayer),
               "LAYER2D::Display");
    ok &= Hook(Addr(Anchor::DisplayWindowDisplay), reinterpret_cast<void*>(&HookWindow),
               reinterpret_cast<void**>(&g_origWindow), "DISPLAY_WINDOW::Display");
    ok &= Hook(Addr(Anchor::ViewportListDisplay), reinterpret_cast<void*>(&HookList), reinterpret_cast<void**>(&g_origList),
               "VIEWPORT_LIST::Display");
    ok &= Hook(Addr(Anchor::SwapChainPresent), reinterpret_cast<void*>(&HookPresent),
               reinterpret_cast<void**>(&g_origPresent), "SWAP_CHAIN::Present");
    // Required for the two-render stages: without it a scene with many viewports ends in the game's abort().
    if (g_stage == Stage::Gate || g_stage == Stage::Vr)
        ok &= Hook(Addr(Anchor::LightRecordAlloc), reinterpret_cast<void*>(&HookLightAlloc),
                   reinterpret_cast<void**>(&g_origLightAlloc), "LIGHT_RECORDS::Alloc");
    // 0.35.0: the particle buffers are rewound for the second render the same way (a missing anchor keeps the game's
    // own behaviour, which only matters in particle-heavy rooms; logged).
    if (ok && (g_stage == Stage::Gate || g_stage == Stage::Vr) &&
        !Hook(Addr(Anchor::ParticleVbNext), reinterpret_cast<void*>(&HookParticleVb),
              reinterpret_cast<void**>(&g_origParticleVb), "PARTICLE_VB::Next"))
        Log("STEREO particle buffer hook missing: two renders of a particle-heavy room can reach the game's abort()");
    if (!ok) Log("STEREO a render hook failed: the mod stays off");
    // v1.3.1: subtitles / 2D HUD menus into the headset's UI panel (missing anchor: they stay desktop only, logged).
    if (ok && g_stage == Stage::Vr) {
        g_menus2DInUi = IniOn(L"vr_subtitles", true);
        if (!g_uiPanel)
            Log("STEREO subtitles: ui=eyes has no UI capture - 2D menus (subtitles) stay on the desktop window only");
        else if (!Hook(Addr(Anchor::MenusMasterDraw2D), reinterpret_cast<void*>(&HookMenus2D),
                       reinterpret_cast<void**>(&g_origMenus2D), "MENUS_MASTER::Draw2D"))
            Log("STEREO subtitles: 2D menu draw not found in this build - subtitles stay on the desktop window only");
        else
            Log("STEREO subtitles / 2D menus into the UI panel %s (vr_subtitles)", g_menus2DInUi ? "on" : "off");
    }
    // First person writes nothing outside the eye passes, but its hooks only belong in the stereo stage
    // (observe stays read-only).
    if (ok && g_stage == Stage::Vr) {
        g_vrFlares = IniOn(L"vr_lens_flares", false);
        {
            const float push = float(_wtof(IniString(L"camera_forward", L"2").c_str()));
            g_cameraPush = std::isfinite(push) ? std::clamp(push, 0.f, kPushMax) : 2.f;
            Log("STEREO camera_forward=%.2f m (third-person camera closer; 0 = off)", g_cameraPush.load());
        }
        // 0.36.1: probe of the rig-light gather (the 0.36.0 light carry crashed and is removed). 0.36.2 .. 0.36.3 wrote
        // the rig group back and crashed (0.36.4: removed - its handles had been released). 0.36.5: the game's own
        // keep-lists switch around the first render (CloseUpKeepFlag); the probe stays read-only.
        g_closeUpKeep = IniOn(L"vr_closeup_keep", true);
        if (g_closeUpKeep) CloseUpKeepFlag();  // resolve and log once at start
        Hook(Addr(Anchor::LightGather), reinterpret_cast<void*>(&HookLightGather),
             reinterpret_cast<void**>(&g_origLightGather), "LIGHT_GATHER");
        // optional: a missing anchor only keeps the flares
        Hook(Addr(Anchor::LenseFlareDisplay), reinterpret_cast<void*>(&HookFlares), reinterpret_cast<void**>(&g_origFlares),
             "LENSE_FLARE::DisplayAll");
        // read-only shadow probe (optional; logs "STEREO shadows 10 s ...")
        Hook(Addr(Anchor::ShadowStage), reinterpret_cast<void*>(&HookShadowStage),
             reinterpret_cast<void**>(&g_origShadowStage), "SHADOW::Stage");
        Hook(Addr(Anchor::ShadowAllocMap), reinterpret_cast<void*>(&HookShadowAlloc),
             reinterpret_cast<void**>(&g_origShadowAlloc), "SHADOW::AllocMap");
        Hook(Addr(Anchor::ShadowRenderDirectional), reinterpret_cast<void*>(&HookShadowRender),
             reinterpret_cast<void**>(&g_origShadowRender), "SHADOW::RenderDirectional");
        fpcam::Install();
        audio::Install();
        rendersize::Install();
        menus::Install();    // the VR panel beside the game's own Options, at the main menu too (playbook 10.6)
        story::Install();    // the chapter and story variables for crash reports; owns the storyboard task's hook
        devtools::Install();  // developer build only (devtools.h); nothing in the release build
    }
    return ok;
}

float CameraPush() { return g_cameraPush.load(); }
void SetCameraPush(float metres) {
    metres = std::isfinite(metres) ? std::clamp(metres, 0.f, kPushMax) : 0.f;
    g_cameraPush = metres;
    wchar_t v[16];
    swprintf_s(v, L"%.2f", metres);
    WritePrivateProfileStringW(L"hrvr", L"camera_forward", v, IniPath().c_str());
    Log("STEREO third-person camera closer -> %.2f m (saved to hrvr.ini camera_forward)", metres);
}

bool YellowFilter() { return g_yellowFilter.load(); }
void SetYellowFilter(bool on) {
    g_yellowFilter = on;
    WritePrivateProfileStringW(L"hrvr", L"vr_yellow_filter", on ? L"on" : L"off", IniPath().c_str());
    Log("STEREO yellow filter -> %s (VR panel; saved to hrvr.ini vr_yellow_filter)", on ? "on" : "off");
}

void RequestVr(bool on) {
    g_xrWanted = on && XrStage();
    Log("XR %s requested (VR settings panel)", on ? "on" : "off");
}

int Method() { return g_method; }

void DescribeForCrash(char* out, size_t cap) {
    snprintf(out, cap, "rendering %s; render phase: %s", MethodName(g_method.load(std::memory_order_relaxed)),
             g_pass ? (g_eye == 0 ? "left eye pass" : g_eye == 1 ? "right eye pass" : "eye pass") : "outside the eye passes");
}

void SetMethod(int method) {
    if (method < kNative || method > kMono) return;
    g_method = method;
    g_afrNext = 0;
    g_aerPhase = 0;
    xr::SetMono(method == kMono);
    WritePrivateProfileStringW(L"hrvr", L"rendering_method",
                               method == kNative ? L"native" : method == kAfr ? L"afr" : method == kAer ? L"aer" : L"mono",
                               IniPath().c_str());
    Log("STEREO rendering method -> %s", method == kNative ? "Native (both eyes every frame)"
                                         : method == kAfr  ? "AFR (one eye per frame)"
                                         : method == kAer  ? "AER (one eye per frame, matched pairs)"
                                                           : "Mono (one render from the head centre, no stereo depth)");
}

bool EyeImageSize(uint32_t* width, uint32_t* height) {
    if (!g_eyeDesc.Width || !g_eyeDesc.Height) return false;
    *width = g_eyeDesc.Width;
    *height = g_eyeDesc.Height;
    return true;
}

}  // namespace hrvr::stereo
