// 1.7: the ARI glasses copy drawn into an eye image (ari_copy_draw.h). Mesh: read at run time from the player's own
// archives (ari_mesh_load.h, v1.3) - the model is game data and never ships with the mod.
#include "ari_copy_draw.h"

#include <windows.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>

#include <atomic>
#include <cstring>
#include <thread>

#include "ari_copy.h"
#include "ari_mesh_load.h"
#include "log.h"

namespace hrvr::aricopy {
namespace {

// The mesh: written once by the loader, published by g_meshState (0 not started, 1 loading, 2 ready, 3 failed).
Mesh g_mesh;
std::atomic<int> g_meshState{0};

bool LoadInto(const std::wstring& resources) {
    const DWORD t0 = GetTickCount();
    std::string why;
    const bool ok = LoadGlasses(resources, &g_mesh, &why);
    if (ok)
        Log("ARI copy: glasses read from the game's own files (%s): %d vertices, %d triangles, %lu ms", load::kPackage,
            int(g_mesh.verts.size()), int(g_mesh.indices.size() / 3), GetTickCount() - t0);
    else
        Log("ARI copy: could not read the glasses from the game's files (%s) - no copy drawn", why.c_str());
    g_meshState.store(ok ? 2 : 3, std::memory_order_release);
    return ok;
}

std::wstring Resources() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring s(exe);
    return s.substr(0, s.find_last_of(L"\\/")) + L"\\Resources";
}

// Eye space: OpenXR view space (-Z forward, +Y up). Depth = near / distance (1 at the near plane, 0 far; GREATER).
// Colours in LINEAR light: the eye swapchain is either an sRGB format (the hardware encodes) or a linear one the mod's
// own blit already linearises into (swap_format.h) - linear is right for both.
constexpr char kHlsl[] = R"(
#pragma pack_matrix(row_major)
cbuffer C : register(b0) {
    float4x4 model;   // frame part -> layer space
    float4x4 view;    // layer space -> eye
    float4 tans;      // left, right, up, down
    float4 params;    // x pass (0 plastic, 1 lens), y near plane (m)
};
struct VSIn { float3 pos : POSITION; float3 nrm : NORMAL; float lens : TEXCOORD0; };
struct PSIn { float4 pos : SV_Position; float3 n : NORMAL; float3 v : TEXCOORD1; float lens : TEXCOORD0; };
PSIn VSMain(VSIn i) {
    PSIn o;
    float4 e = mul(mul(float4(i.pos, 1), model), view);
    float d = max(-e.z, 1e-4);
    float x = e.x / d, y = e.y / d;
    float nx = (2 * x - (tans.y + tans.x)) / (tans.y - tans.x);
    float ny = (2 * y - (tans.z + tans.w)) / (tans.z - tans.w);
    o.pos = float4(nx * d, ny * d, params.y, d);
    o.n = mul(mul(float4(i.nrm, 0), model), view).xyz;
    o.v = e.xyz;
    o.lens = i.lens;
    return o;
}
float4 PSMain(PSIn i) : SV_Target {
    bool lens = i.lens > 0.5;
    if ((params.x < 0.5) == lens) discard;
    float3 n = normalize(i.n), v = normalize(-i.v);
    if (dot(n, v) < 0) n = -n;                       // thin parts: lit from either side
    float3 l = normalize(float3(0.35, 0.85, 0.4));   // key light: above, a little right, toward the viewer
    float diff = saturate(dot(n, l));
    float spec = pow(saturate(dot(n, normalize(l + v))), lens ? 90 : 40);
    float rim = pow(1 - saturate(dot(n, v)), 3);
    float3 base = lens ? float3(0.020, 0.045, 0.070) : float3(0.016, 0.017, 0.020);  // smoky blue lens, dark carbon
    float3 c = base * (0.45 + 1.1 * diff) + spec * (lens ? 0.55 : 0.22) + rim * (lens ? float3(0.05, 0.12, 0.20) : 0.03);
    float a = lens ? saturate(0.42 + 0.45 * rim + spec) : 1;
    return float4(c, a);
}
)";

struct Gpu {
    bool tried = false, ok = false;
    ID3D11Device* device = nullptr;
    ID3DDeviceContextState* state = nullptr;
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11InputLayout* layout = nullptr;
    ID3D11Buffer *vb = nullptr, *ib = nullptr, *cb = nullptr;
    ID3D11RasterizerState* raster = nullptr;
    ID3D11DepthStencilState *depthWrite = nullptr, *depthTest = nullptr;
    ID3D11BlendState *opaque = nullptr, *blend = nullptr;
    ID3D11Texture2D* depthTex = nullptr;
    ID3D11DepthStencilView* dsv = nullptr;
    unsigned dw = 0, dh = 0;
} g;
std::atomic<uint64_t> g_draws{0};

template <class T>
void Drop(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

bool Create(ID3D11Device* dev) {
    g.tried = true;
    g.device = dev;
    ID3D11Device1* dev1 = nullptr;
    if (FAILED(dev->QueryInterface(IID_PPV_ARGS(&dev1)))) return false;
    const D3D_FEATURE_LEVEL fl[] = {dev->GetFeatureLevel()};
    const HRESULT hs = dev1->CreateDeviceContextState(0, fl, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device1), nullptr, &g.state);
    dev1->Release();
    if (FAILED(hs)) return false;
    ID3DBlob *vsb = nullptr, *psb = nullptr, *err = nullptr;
    if (FAILED(D3DCompile(kHlsl, sizeof(kHlsl) - 1, "hrvr_aricopy", nullptr, nullptr, "VSMain", "vs_5_0", 0, 0, &vsb, &err)) ||
        FAILED(D3DCompile(kHlsl, sizeof(kHlsl) - 1, "hrvr_aricopy", nullptr, nullptr, "PSMain", "ps_5_0", 0, 0, &psb, &err))) {
        Log("ARI copy: shader compile failed: %s", err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
        Drop(err), Drop(vsb), Drop(psb);
        return false;
    }
    bool ok = SUCCEEDED(dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &g.vs)) &&
              SUCCEEDED(dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &g.ps));
    const D3D11_INPUT_ELEMENT_DESC el[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0}};
    ok = ok && SUCCEEDED(dev->CreateInputLayout(el, 3, vsb->GetBufferPointer(), vsb->GetBufferSize(), &g.layout));
    Drop(vsb), Drop(psb), Drop(err);
    static_assert(sizeof(Vertex) == 28, "vertex layout");
    D3D11_BUFFER_DESC bd{UINT(g_mesh.verts.size() * sizeof(Vertex)), D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0};
    D3D11_SUBRESOURCE_DATA sd{g_mesh.verts.data(), 0, 0};
    ok = ok && SUCCEEDED(dev->CreateBuffer(&bd, &sd, &g.vb));
    bd = {UINT(g_mesh.indices.size() * sizeof(uint16_t)), D3D11_USAGE_IMMUTABLE, D3D11_BIND_INDEX_BUFFER, 0, 0, 0};
    sd = {g_mesh.indices.data(), 0, 0};
    ok = ok && SUCCEEDED(dev->CreateBuffer(&bd, &sd, &g.ib));
    bd = {160, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
    ok = ok && SUCCEEDED(dev->CreateBuffer(&bd, nullptr, &g.cb));
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    ok = ok && SUCCEEDED(dev->CreateRasterizerState(&rd, &g.raster));
    D3D11_DEPTH_STENCIL_DESC dd{};
    dd.DepthEnable = TRUE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dd.DepthFunc = D3D11_COMPARISON_GREATER;
    ok = ok && SUCCEEDED(dev->CreateDepthStencilState(&dd, &g.depthWrite));
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    ok = ok && SUCCEEDED(dev->CreateDepthStencilState(&dd, &g.depthTest));
    D3D11_BLEND_DESC bl{};
    bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN |
                                               D3D11_COLOR_WRITE_ENABLE_BLUE;  // the eye image's alpha stays
    ok = ok && SUCCEEDED(dev->CreateBlendState(&bl, &g.opaque));
    bl.RenderTarget[0].BlendEnable = TRUE;
    bl.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bl.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bl.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
    bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    ok = ok && SUCCEEDED(dev->CreateBlendState(&bl, &g.blend));
    return ok;
}

bool Depth(unsigned w, unsigned h) {
    if (g.dsv && g.dw == w && g.dh == h) return true;
    Drop(g.dsv), Drop(g.depthTex);
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w, td.Height = h, td.MipLevels = 1, td.ArraySize = 1;
    td.Format = DXGI_FORMAT_D32_FLOAT;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (FAILED(g.device->CreateTexture2D(&td, nullptr, &g.depthTex)) ||
        FAILED(g.device->CreateDepthStencilView(g.depthTex, nullptr, &g.dsv)))
        return false;
    g.dw = w, g.dh = h;
    return true;
}

}  // namespace

void StartLoad() {
    int expected = 0;
    if (!g_meshState.compare_exchange_strong(expected, 1)) return;
    std::thread([] { LoadInto(Resources()); }).detach();
}

bool LoadNow(const std::wstring& resources) {
    int expected = 0;
    if (g_meshState.compare_exchange_strong(expected, 1)) return LoadInto(resources);
    return g_meshState.load(std::memory_order_acquire) == 2;
}

bool DrawCopy(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv, unsigned width, unsigned height, const pose::Mat4& eye,
              float tanLeft, float tanRight, float tanUp, float tanDown, const pose::Mat4& copy) {
    if (!ctx || !rtv || !width || !height || !(tanRight > tanLeft) || !(tanUp > tanDown)) return false;
    if (g_meshState.load(std::memory_order_acquire) != 2) {  // not read yet (or not readable): nothing to draw
        StartLoad();
        return false;
    }
    if (!g.tried) {
        ID3D11Device* dev = nullptr;
        rtv->GetDevice(&dev);
        g.ok = dev && Create(dev);
        if (dev) dev->Release();
        Log("ARI copy: the glasses copy renderer %s (%d vertices, %d triangles)", g.ok ? "ready" : "FAILED - no copy drawn",
            int(g_mesh.verts.size()), int(g_mesh.indices.size() / 3));
    }
    if (!g.ok || !Depth(width, height)) return false;
    ID3D11DeviceContext1* ctx1 = nullptr;
    if (FAILED(ctx->QueryInterface(IID_PPV_ARGS(&ctx1)))) return false;
    struct {
        pose::Mat4 model, view;
        float tans[4], params[4];
    } cb{copy, pose::RigidInverse(eye), {tanLeft, tanRight, tanUp, tanDown}, {0.f, 0.08f, 0.f, 0.f}};
    ID3DDeviceContextState* previous = nullptr;
    ctx1->SwapDeviceContextState(g.state, &previous);
    const D3D11_VIEWPORT vp{0, 0, float(width), float(height), 0, 1};
    ctx1->RSSetViewports(1, &vp);
    ctx1->RSSetState(g.raster);
    ctx1->ClearDepthStencilView(g.dsv, D3D11_CLEAR_DEPTH, 0.f, 0);
    ctx1->OMSetRenderTargets(1, &rtv, g.dsv);
    const UINT stride = sizeof(Vertex), offset = 0;
    ctx1->IASetVertexBuffers(0, 1, &g.vb, &stride, &offset);
    ctx1->IASetIndexBuffer(g.ib, DXGI_FORMAT_R16_UINT, 0);
    ctx1->IASetInputLayout(g.layout);
    ctx1->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx1->VSSetShader(g.vs, nullptr, 0);
    ctx1->PSSetShader(g.ps, nullptr, 0);
    ctx1->VSSetConstantBuffers(0, 1, &g.cb);
    ctx1->PSSetConstantBuffers(0, 1, &g.cb);
    const float factor[4] = {0, 0, 0, 0};
    for (int pass = 0; pass < 2; ++pass) {  // plastic (opaque, writes depth), then the lenses (blended, depth tested)
        cb.params[0] = float(pass);
        ctx1->UpdateSubresource(g.cb, 0, nullptr, &cb, 0, 0);
        ctx1->OMSetDepthStencilState(pass ? g.depthTest : g.depthWrite, 0);
        ctx1->OMSetBlendState(pass ? g.blend : g.opaque, factor, 0xffffffff);
        ctx1->DrawIndexed(UINT(g_mesh.indices.size()), 0, 0);
    }
    ctx1->OMSetRenderTargets(0, nullptr, nullptr);
    ctx1->SwapDeviceContextState(previous, nullptr);
    if (previous) previous->Release();
    ctx1->Release();
    if (g_draws.fetch_add(1, std::memory_order_relaxed) == 0) Log("ARI copy: first glasses copy drawn in an eye image");
    return true;
}

}  // namespace hrvr::aricopy
