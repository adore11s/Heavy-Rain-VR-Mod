#include "game.h"

#include <windows.h>

#include <cstring>
#include <string>

#include "log.h"
#include "sigdb.h"

namespace hrvr::game {
namespace {

struct AnchorDef {
    const char* name;
    uint64_t va;
};

constexpr AnchorDef kAnchors[] = {
    {"VIEWPORT_LIST::Display", 0x140889650},   {"DISPLAY_WINDOW::Display", 0x1408e6040},
    {"CAMERA::Build", 0x1408e4bd0},            {"LAYER2D::Display", 0x140a056d0},
    {"SWAP_CHAIN::Present", 0x140a70a40},      {"VIEWPORT::SetupCamera", 0x14089fd10},
    {"KCOM::ResolveSlow", 0x140ac2f30},        {"NODE::UpdateWorld", 0x140893860},
    {"RENDER::SetViewport", 0x140a6ea50},      {"STEAM_CONTROLLER::Enumerate", 0x140b4e4b0},
    {"STEAM::ContextAccess", 0x1401e0014},     {"LIGHT_RECORDS::Alloc", 0x1408ed630},
    {"CAMERA_DIRECTOR::UpdateChangeCamera", 0x1403d32b0}, {"THOUGHTS_ANIMATOR::SetAnchor", 0x1404fecf0},
    {"SKIN::PaletteBuild", 0x140a61920},
    {"SOUND_LISTENER::SetPosition", 0x140bb5fe0}, {"SOUND_LISTENER::SetOrientation", 0x140bb61e0},
    {"RENDER::ComputeBackbufferSize", 0x140a6ac30}, {"RENDER::ComputeRenderSize", 0x140a6ae00},
    {"SWAP_CHAIN::Resize", 0x140a70560}, {"MPAR::UpdateElement", 0x14023fdb0},
    {"VIEWPORT::DisplayReflections", 0x14087e600}, {"CAMERA_TEXTURE::Render", 0x1409a9bf0},
    {"NODE::ParentMatrix", 0x14017b750},
    {"SKELETON_INSTANCE::UpdateWorlds", 0x1409fd270}, {"HAVOK_TRANSFORM_SET::Create", 0x140b8f400},
    {"HAVOK_TRANSFORM_SET::Destroy", 0x1401ecf00},    {"HAVOK_CLOTH_INSTANCE::Create", 0x140b8ed50},
    {"HAVOK_CLOTH_INSTANCE::Destroy", 0x1401ec910},   {"HAVOK_CLOTH_INSTANCE::ToSim", 0x140b8e8a0},
    {"MENU::Handle", 0x14059b3f0},           {"LENSE_FLARE::DisplayAll", 0x140877d10},
    {"SHADOW::Stage", 0x1409ec450},          {"SHADOW::AllocMap", 0x140992c10},
    {"SHADOW::RenderDirectional", 0x14098f340},
    {"NODE::SetWorld", 0x140893700},          {"NODE::InvalidateSubtree", 0x140893960},
    {"MOVIE_CONTROLLER_PLAY::Update", 0x1403c74e0},
    {"PARTICLE_VB::Next", 0x1408ab530},
    {"LIGHT_GATHER", 0x140a0d160},
    {"RENDER::LoadDisplaySettings", 0x140a70cc0},
    {"IHM_MENUS::Display3D", 0x14087b8f0},
    {"USER_ACTION::Activate", 0x14023d180},
    {"MPAR_AXIS_ROTATION::Check", 0x14045df90},
    {"MENUS_MASTER::Draw2D", 0x140835540},
};
static_assert(sizeof(kAnchors) / sizeof(kAnchors[0]) == size_t(Anchor::Count));

// Mapped .text SHA-256 of the analysed Steam build (exe sha256 0a362969...ef70; no relocations in .text).
constexpr const char* kVerifiedTextSha = "eac8444567b5534160f3c3acb8ec8a553564d5a761479b124c70b50cbbd443c5";

// The render / camera path cannot run without these (fail closed: the game stays vanilla).
constexpr Anchor kCore[] = {Anchor::ViewportListDisplay, Anchor::DisplayWindowDisplay, Anchor::CameraBuild,
                            Anchor::Layer2DDisplay,      Anchor::SwapChainPresent,     Anchor::KcomResolveSlow,
                            Anchor::NodeUpdateWorld,     Anchor::RenderSetViewport};
constexpr uint64_t kCoreGlobals[] = {g::MainSwapChain, g::RenderStateCache, g::RenderWidth, g::RenderHeight,
                                     g::RenderContext};

void* g_resolved[size_t(Anchor::Count)] = {};
bool g_known = false;
int g_count = 0;

std::wstring IniPath() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring s = exe;
    return s.substr(0, s.find_last_of(L"\\/")) + L"\\hrvr.ini";
}
bool IniFlag(const wchar_t* key) {
    wchar_t v[16];
    GetPrivateProfileStringW(L"hrvr", key, L"off", v, 16, IniPath().c_str());
    return _wcsicmp(v, L"on") == 0 || _wcsicmp(v, L"force") == 0;
}

}  // namespace

bool KnownBuild() { return g_known; }
int ResolvedAnchors() { return g_count; }
uintptr_t Va(uint64_t recordedVa) { return sigdb::Va(recordedVa); }
void* Addr(Anchor a) { return g_resolved[size_t(a)]; }
const char* AnchorName(Anchor a) { return kAnchors[size_t(a)].name; }
bool SteamAnchorsOk() {
    return Addr(Anchor::SteamEnumerate) && Addr(Anchor::SteamContextAccess);
}

bool ResolveAll(const char* textSha256, bool log) {
    const bool forceScan = IniFlag(L"signature_scan");
    g_known = textSha256 && std::strcmp(textSha256, kVerifiedTextSha) == 0 && !forceScan;
    if (log) {
        if (g_known) Log("Game build: VERIFIED (Heavy Rain Steam 2020-05-12, addresses recorded for this exact build)");
        else Log("Game build: UNVERIFIED (.text %s) - addresses are found by signature%s", textSha256 ? textSha256 : "?",
                 forceScan ? " (signature scan forced)" : "");
    }
    sigdb::Resolve(g_known, log);
    g_count = 0;
    for (size_t i = 0; i < size_t(Anchor::Count); ++i) {
        g_resolved[i] = reinterpret_cast<void*>(sigdb::Va(kAnchors[i].va));
        g_count += g_resolved[i] != nullptr;
        if (log && !g_resolved[i]) Log("anchor %-28s NOT FOUND in this build (feature off)", kAnchors[i].name);
    }
    bool core = true;
    for (Anchor a : kCore)
        if (!Addr(a)) {
            core = false;
            if (log) Log("core anchor missing: %s", AnchorName(a));
        }
    for (uint64_t va : kCoreGlobals)
        if (!sigdb::Va(va)) {
            core = false;
            if (log) Log("core global missing: recorded 0x%llx", static_cast<unsigned long long>(va));
        }
    return core;
}

void* ResolveHandle(void* handle) {
    if (!handle) return nullptr;
    if ((Read<uint32_t>(handle, 0x30) & 0x202000) != 0)
        if (void* cached = Read<void*>(handle, 0x50)) return cached;
    using SlowFn = void* (*)(void*, int, int);
    auto slow = reinterpret_cast<SlowFn>(Addr(Anchor::KcomResolveSlow));
    return slow ? slow(handle, 0, 0) : nullptr;
}

}  // namespace hrvr::game
