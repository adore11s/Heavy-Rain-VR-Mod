#pragma once
#include <cstddef>
#include <cstdint>

// Engine anchors for Heavy Rain (Steam build, 2020-05-12). Every anchor is the VA of the analysed
// build; at runtime it is mapped to this build's address through sigdb (verified in place on the
// analysed build, found by signature in any other). An anchor that cannot be found exactly once
// resolves to null, and callers leave that feature off.
// Evidence: reverse_engineering/docs/HR_VR_PORT_DESIGN.md.
namespace hrvr::game {

enum class Anchor {
    ViewportListDisplay,   // VIEWPORT_LIST::Display(list): every enabled viewport's passes, immediate
    DisplayWindowDisplay,  // DISPLAY_WINDOW::Display(win, flag): list, then the window's 2D layer
    CameraBuild,           // CAMERA::Build(cam, aspect, guardX, guardY): view +0x240, proj +0x200/+0x2A0
    Layer2DDisplay,        // 2D layer draw (layer, vp, flag): per viewport (vp+0x258) and per window
    SwapChainPresent,      // SWAP_CHAIN_DX11_PC::Present(?, syncInterval)
    ViewportSetupCamera,   // VIEWPORT::SetupCamera(vp, vp+0x70, flag): Build + matrix publish + frustums
    KcomResolveSlow,       // KCOM handle -> object (slow path)
    NodeUpdateWorld,       // NODE: recompute world (+0x90) when +0xE8 bit1 is clear
    RenderSetViewport,     // RENDER_CONTEXT::SetViewport(ctx, x, y, w, h)
    SteamEnumerate,        // PC_CONTROLLER_STEAM: GetConnectedControllers + action handle load
    SteamContextAccess,    // lea rcx,[ctx]; call [SteamInternal_ContextInit]
    LightRecordAlloc,      // copy a light into a 180-entry per-frame table (owner+0x6e4 count); abort() when full
    DirectorUpdate,        // CAMERA_DIRECTOR::UpdateChangeCamera(director): per-frame task; behaviour handle +0x1F0
    ThoughtsSetAnchor,     // THOUGHTS_GUI animator (gui+0x180) <- head anchor matrix (16-byte aligned), dt in xmm2
    SkinPaletteBuild,      // skinned record (type 3): palette = invBind(+0x48) x world(+0x58) x inv(node +0x18) -> *(+0x68)
    ListenerSetPosition,   // KSOUND listener (rcx) <- camera position (rdx: aligned vec4), eased 9 % per call
    ListenerSetOrientation,  // KSOUND listener (rcx) <- front (rdx), up (r8): aligned vec4s, slerped
    ComputeBackbufferSize,   // (bool windowed): g_BackbufferWidth/Height from the configured size and the window
    ComputeRenderSize,       // g_RenderWidth/Height from the back buffer (a 16:9 band when g_Force16x9)
    SwapChainResize,         // (SWAP_CHAIN_DX11_PC*): ResizeBuffers(g_SwapWidth x g_SwapHeight) + every render target
    MparUpdateElement,       // (owner, record): an action prompt's display matrix (*(record+0x10)+0x30) from its entity
    DisplayReflections,      // (viewport, render context, ?): every reflection plane's view, context +0x25 = 1 meanwhile
    CameraTextureRender,     // CAMERA_TEXTURE (this): a render-to-texture camera (mirrors, screens) draws its viewport
    NodeParentMatrix,        // (NODE*): address of the matrix a node's local is multiplied with: the parent's bone
                             // world (NODE+0x120 >= 0: *(parent skeleton+0x10) + bone * 0x40) or the parent's world
    // 0.21.0 (STATIC 2026-11-17): simulated cloth (Havok Cloth) reads the skeleton through transform sets.
    SkeletonUpdateWorlds,    // (instance): world[i] = local[i] x world[parent[i]] (+0x10), then +0x18 = transpose(inverse)
    TransformSetCreate,      // HAVOK_TRANSFORM_SET ctor (set, TRANSFORM_SETS*, ?, count): +0x10 -> instance worlds,
                             // +0x20 -> instance +0x18 (hkArrays of 0x40 matrices, 0x80000000 = not owned)
    TransformSetDestroy,     // HAVOK_TRANSFORM_SET vtable slot 0 (set, flags)
    ClothInstanceCreate,     // HAVOK_CLOTH_INSTANCE set-up (inst, cloth, buffers, TRANSFORM_SETS*, ?): makes its sets
    ClothInstanceDestroy,    // HAVOK_CLOTH_INSTANCE vtable slot 0 (inst, flags)
    ClothInstanceToSim,      // HAVOK_CLOTH_INSTANCE vtable slot 9 (inst): state "toSim1", the cloth starts again from
                             // its skinned pose (the engine's own reset)
    // 0.27.0 (STATIC 2026-11-17): MenusMaster's per-frame handler of one running menu (MmDk::CMenu* in rcx).
    MenuHandle,
    // 0.30.1 (STATIC 2026-11-19, playbook 07.5): the viewport pass's lens-flare sprite draw (display list +0x10f8 /
    // +0x1100, camera): screen-space ghost chains through the image centre - wrong in each eye of a headset.
    LenseFlareDisplay,
    // 0.30.1 (STATIC 2026-11-19, TARGET_ADAPTER_MAP "Shadow maps"): read-only shadow probe (owner: sun shadows in the
    // left eye only, Native).
    ShadowStage,             // (display, camera ctx): pool round += 1, projector jobs, map renders - every viewport pass
    ShadowAllocMap,          // (light, ?, job): job+0x578 = a free pool map, or 0 (no shadow for that job)
    ShadowRenderDirectional, // (light, display, job): *(display+0x260) caster spans empty -> the map is only cleared
    // 0.32.0 (STATIC 2026-11-20, playbook 19.8 point 11 Gate 3): the first-person item seam (item_seam.h).
    NodeSetWorld,            // (NODE*): local (+0x50) = world (+0x90) x inverse(ParentMatrix), clean
    NodeInvalidateSubtree,   // (NODE*, clear, set): +0xE8 = (+0xE8 & ~clear) | set on the node and every node below it
    // 0.33.0 (STATIC 2026-11-20, Gate 3 row 9): per frame per playing movie controller (rcx = controller, +0x110 the
    // movie's COM instance); movielink.cpp reads the movie's entity list after the game's update.
    MovieControllerUpdate,
    // 0.35.0 (STATIC 2026-11-20, crash 13:35 in _2501_THE_NURSE): the particle batch draw's dynamic vertex buffer
    // (rcx = the display scene *0x141a33aa8): ++scene+0x84, abort() when it reaches 12, else buffer
    // scene+0x90[(scene+0x80) * 12 + depth]. scene+0x84 is reset once per frame (0x14085ab50); Native's two renders
    // doubled the count. Mapped WRITE_DISCARD (0x140a72360), so a buffer may be reused within a frame.
    ParticleVbNext,
    // 0.36.1 (STATIC 2026-11-20): the visibility batch job's light gather of one culling record (query+0x200, record,
    // ?, rig): fills the record's light list (+0xc8, count byte +0x1e8); rig lights (key / fill / rim: rig+0x194 /
    // +0x196 / +0x198) only when query+0x252, rig+6, the record's zone (+0x98) in the rig slot's zone list (rig+8,
    // counts rig+0x188) and record+0xb8 bit 1. Read-only probe (the 0.36.0 light carry crashed and is removed).
    LightGather,
    // 0.36.15 (STATIC 2026-11-20): renderer start-up - user_setting.ini into the display globals (g::ConfigScreenMode);
    // the caller 0x140a716e0 then copies the configured mode into the current one before the window exists.
    LoadDisplaySettings,
    // 1.2 (STATIC 2026-11-22): the 3D-placed MenusMaster menus' draw in the viewport pass (rcx = display component:
    // +0x60 entries of 0x18 = menu-data handle, param block; +0x68 count). Param block +7 = 3D, +0x30 world matrix.
    IhmMenusDisplay,
    Count
};

// RTTI primary vtables (sigdb Vtable entries: found by mangled name on any other build).
namespace vt {
constexpr uint64_t ThirdPersonGameplay = 0x140c87a18;  // +0x54 IsActive, +0x1E8 Character handle, +0x200 CtrlMode
constexpr uint64_t Character = 0x140cb62b0;            // +0x160 entity handle -> +0x68 node handle
constexpr uint64_t CameraTargetAlias = 0x140d60d80;    // +0x18 name, +0x30 entity handle, +0x38 CRC32(bone)
constexpr uint64_t ThoughtsGui = 0x140d50ac8;          // +0xB0 character handle, +0xE0 bone name, +0x180 animator
constexpr uint64_t MokitSync = 0x140d52c38;            // pair walk: +0x110 entity, +0x128 EntitySync (handles)
constexpr uint64_t MparGui = 0x140d5f800;              // singleton; +0x20 / +0x38 counts of user actions offered
}  // namespace vt

// Recorded globals (resolved through sigdb Data entries).
namespace g {
constexpr uint64_t MainSwapChain = 0x141a1e7f8;     // SWAP_CHAIN_DX11_PC*
constexpr uint64_t RenderStateCache = 0x141a1e880;  // uint32_t*; +0xC8 current RENDER_TARGET
constexpr uint64_t RenderWidth = 0x141a1e808;
constexpr uint64_t RenderHeight = 0x141a1e80c;
constexpr uint64_t RenderContext = 0x141a1e870;     // the immediate RENDER_CONTEXT wrapper (by address)
constexpr uint64_t RenderThreadId = 0x141a1b020;
constexpr uint64_t TargetFps = 0x1417a5f80;         // float; EndFrame spins to 1000/fps when vsync is on
constexpr uint64_t VSync = 0x1417a523b;             // bool
constexpr uint64_t GamePaused = 0x1417c4da8;        // bool: script PauseGame sets, ResumeGame clears (STATIC)
constexpr uint64_t BackbufferWidth = 0x141a1e71c;   // u32 (ComputeBackbufferSize)
constexpr uint64_t BackbufferHeight = 0x141a1e7bc;
constexpr uint64_t SwapWidth = 0x141a1e6d8;         // u32: ResizeBuffers size (window client size)
constexpr uint64_t SwapHeight = 0x141a1e6dc;
constexpr uint64_t MainWnd = 0x141a1e800;           // HWND of the game window
constexpr uint64_t Force16x9 = 0x1417a523a;         // bool: scene into SWAP_CHAIN+0x28, letterboxed at EndFrame
constexpr uint64_t ScriptManager = 0x141a1f250;     // pointer: +0x18 count, +0x20 lua_State*, +0x198 variables name
// bool (0.36.5, STATIC): the viewport pass 0x1408a15e0 clears the close-up light group S = *(viewport+0x268) (three
// lists of AddRef'd handles, counts S+0x1d0, ClearList 0x14097ca70) at its end unless this is set. The game sets it
// only around its tiled screenshot's N x N renders of one frame (0x1408566b0); nothing else reads or writes it.
constexpr uint64_t KeepCloseUpLists = 0x141a1aeef;
// int (0.36.15, STATIC): the configured display mode - 0 exclusive full screen, 1 borderless screen-sized window,
// 2 window with a title bar (setter 0x140a6b420); ScreenMode from user_setting.ini, 1 when there is no file.
constexpr uint64_t ConfigScreenMode = 0x141a2e108;
}  // namespace g

bool ResolveAll(const char* textSha256, bool log);
int ResolvedAnchors();
bool KnownBuild();
uintptr_t Va(uint64_t recordedVa);
void* Addr(Anchor a);
const char* AnchorName(Anchor a);
bool SteamAnchorsOk();

// Engine handle idiom: (h+0x30 & 0x202000) marks a cached object at h+0x50.
void* ResolveHandle(void* handle);

namespace off {
constexpr uintptr_t VpListArray = 0x48;  // VIEWPORT_LIST: KCOM handle array
constexpr uintptr_t VpListCount = 0x50;  // VIEWPORT_LIST: u32 count
constexpr uintptr_t VpFlags = 0x1B0;     // VIEWPORT: bit0 enabled
constexpr uintptr_t VpCamera = 0x230;    // VIEWPORT: camera handle
constexpr uintptr_t VpLayer2D = 0x258;   // VIEWPORT: 2D layer
constexpr uintptr_t VpViewParams = 0x70; // VIEWPORT: SetupCamera's second argument
constexpr uintptr_t CamNode = 0x68;
constexpr uintptr_t CamFov = 0x104;      // horizontal FOV, radians (p0 = cot(fov/2) / (scale*aspectParam))
constexpr uintptr_t CamAspect = 0x11C;   // effective aspect (Build's argument): p5 = p0 * aspect
constexpr uintptr_t CamGuardX = 0x120;   // guard-band scale: p0 /= guardX
constexpr uintptr_t CamGuardY = 0x124;
// bool: depth of field on (script ViewportDepthOfFieldEnable, kernelrenderer_140264050). Read by
// VIEWPORT::SetupCamera's post-effect fill (camerasystem_1408e4550: DoF params[0] = +0x128 && !options+0x12) and
// by the viewport pass (camerasystem_1408a15e0: the DoF pass flag 0x141a1eff0). +0x12C..+0x140 focus parameters.
constexpr uintptr_t CamDofEnable = 0x128;
// bool: film grain / viewport noise on. Read by the same post-effect fill (camerasystem_1408e4550: noise on =
// +0x144 && !options+0x13; +0x148 / +0x14C / +0x150 its parameters, +0x154 its rate as 1 / x).
constexpr uintptr_t CamNoiseEnable = 0x144;
constexpr uintptr_t CamProj = 0x200;     // row-major, RH, row-vector, p[11] = -1
constexpr uintptr_t CamView = 0x240;     // inverse(node world)
constexpr uintptr_t CamProjFar = 0x2A0;
constexpr uintptr_t NodeWorld = 0x90;
constexpr uintptr_t NodeFlags = 0xE8;    // bit1 world valid
constexpr uintptr_t SwapChainDxgi = 0x08;  // IDXGISwapChain*
constexpr uintptr_t RscCurrentRt = 0xC8;   // render state cache: current RENDER_TARGET_DX11_PC*
constexpr uintptr_t RtTexObject = 0x18;    // RENDER_TARGET -> texture object
constexpr uintptr_t TexD3D = 0x08;         // texture object -> ID3D11Texture2D*
}  // namespace off

template <typename T>
T Read(const void* base, uintptr_t offset) {
    return *reinterpret_cast<const T*>(reinterpret_cast<const uint8_t*>(base) + offset);
}
template <typename T>
T* Global(uint64_t recordedVa) {
    return reinterpret_cast<T*>(Va(recordedVa));
}

}  // namespace hrvr::game
