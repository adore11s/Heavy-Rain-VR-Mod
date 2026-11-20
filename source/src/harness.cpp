#include "harness.h"

#include <windows.h>
#include <d3d11.h>

#include <cstdio>
#include <cstring>
#include <share.h>

#include "log.h"

namespace hrvr::harness {
namespace {

ID3D11Texture2D* g_staging = nullptr;
ID3D11Texture2D* g_resolve = nullptr;
D3D11_TEXTURE2D_DESC g_desc{};

float Half(uint16_t h) {
    const uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
    float v;
    if (e == 0) v = std::ldexp(float(m), -24);
    else if (e == 31) v = 65504.f;
    else v = std::ldexp(float(m | 1024), int(e) - 25);
    return s ? -v : v;
}

// Pixel at x,y as 8-bit sRGB-ish RGB (float formats are clamped and gamma-encoded roughly).
bool ReadRgb(const D3D11_MAPPED_SUBRESOURCE& m, DXGI_FORMAT f, UINT x, UINT y, uint8_t rgb[3]) {
    const uint8_t* row = static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch;
    switch (f) {
        case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_R8G8B8A8_TYPELESS: {
            const uint8_t* p = row + size_t(x) * 4;
            rgb[0] = p[0]; rgb[1] = p[1]; rgb[2] = p[2];
            return true;
        }
        case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        case DXGI_FORMAT_B8G8R8X8_UNORM: {
            const uint8_t* p = row + size_t(x) * 4;
            rgb[0] = p[2]; rgb[1] = p[1]; rgb[2] = p[0];
            return true;
        }
        case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R10G10B10A2_TYPELESS: {
            uint32_t v;
            std::memcpy(&v, row + size_t(x) * 4, 4);
            rgb[0] = uint8_t((v & 1023) >> 2); rgb[1] = uint8_t(((v >> 10) & 1023) >> 2); rgb[2] = uint8_t(((v >> 20) & 1023) >> 2);
            return true;
        }
        case DXGI_FORMAT_R16G16B16A16_FLOAT: {
            uint16_t h[3];
            std::memcpy(h, row + size_t(x) * 8, 6);
            for (int i = 0; i < 3; ++i) {
                const float c = std::min(std::max(Half(h[i]), 0.f), 1.f);
                rgb[i] = uint8_t(std::pow(c, 1.f / 2.2f) * 255.f + 0.5f);
            }
            return true;
        }
        default: return false;
    }
}

void SaveBmp(const D3D11_MAPPED_SUBRESOURCE& m, DXGI_FORMAT f, UINT width, UINT height, const std::wstring& path) {
    const UINT w = width / 4, h = height / 4, stride = (w * 3 + 3) & ~3u;
    FILE* file = _wfsopen(path.c_str(), L"wb", _SH_DENYWR);
    if (!file) return;
    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    ih.biSize = sizeof(ih);
    ih.biWidth = LONG(w);
    ih.biHeight = -LONG(h);
    ih.biPlanes = 1;
    ih.biBitCount = 24;
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + stride * h;
    fwrite(&fh, sizeof(fh), 1, file);
    fwrite(&ih, sizeof(ih), 1, file);
    std::string row(stride, '\0');
    for (UINT y = 0; y < h; ++y) {
        for (UINT x = 0; x < w; ++x) {
            uint8_t rgb[3] = {};
            ReadRgb(m, f, x * 4, y * 4, rgb);
            row[x * 3 + 0] = char(rgb[2]);
            row[x * 3 + 1] = char(rgb[1]);
            row[x * 3 + 2] = char(rgb[0]);
        }
        fwrite(row.data(), 1, stride, file);
    }
    fclose(file);
}

}  // namespace

bool Capture(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, Grid* out, const std::wstring& bmpPath) {
    if (!dev || !ctx || !tex || !out) return false;
    D3D11_TEXTURE2D_DESC d{};
    tex->GetDesc(&d);
    if (d.Width != g_desc.Width || d.Height != g_desc.Height || d.Format != g_desc.Format ||
        d.SampleDesc.Count != g_desc.SampleDesc.Count) {
        if (g_staging) g_staging->Release();
        if (g_resolve) g_resolve->Release();
        g_staging = g_resolve = nullptr;
        g_desc = d;
        D3D11_TEXTURE2D_DESC sd = d;
        sd.SampleDesc = {1, 0};
        sd.MipLevels = 1;
        sd.ArraySize = 1;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.BindFlags = 0;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        sd.MiscFlags = 0;
        dev->CreateTexture2D(&sd, nullptr, &g_staging);
        if (d.SampleDesc.Count > 1) {
            D3D11_TEXTURE2D_DESC rd = sd;
            rd.Usage = D3D11_USAGE_DEFAULT;
            rd.CPUAccessFlags = 0;
            dev->CreateTexture2D(&rd, nullptr, &g_resolve);
        }
        Log("HARNESS capture target %ux%u fmt=%d samples=%u staging=%s", d.Width, d.Height, int(d.Format),
            d.SampleDesc.Count, g_staging ? "ok" : "FAILED");
    }
    if (!g_staging) return false;
    if (d.SampleDesc.Count > 1) {
        if (!g_resolve) return false;
        ctx->ResolveSubresource(g_resolve, 0, tex, 0, d.Format);
        ctx->CopyResource(g_staging, g_resolve);
    } else {
        ctx->CopySubresourceRegion(g_staging, 0, 0, 0, 0, tex, 0, nullptr);
    }
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx->Map(g_staging, 0, D3D11_MAP_READ, 0, &m))) return false;
    const UINT step = std::max(1u, d.Width / 480);
    out->w = int(d.Width / step);
    out->h = int(d.Height / step);
    out->lum.assign(size_t(out->w) * out->h, 0);
    bool ok = true;
    for (int y = 0; y < out->h && ok; ++y)
        for (int x = 0; x < out->w; ++x) {
            uint8_t rgb[3];
            if (!ReadRgb(m, d.Format, UINT(x) * step, UINT(y) * step, rgb)) {
                ok = false;
                break;
            }
            out->lum[size_t(y) * out->w + x] = uint8_t((299 * rgb[0] + 587 * rgb[1] + 114 * rgb[2]) / 1000);
        }
    if (ok && !bmpPath.empty()) SaveBmp(m, d.Format, d.Width, d.Height, bmpPath);
    ctx->Unmap(g_staging, 0);
    if (!ok) {
        static bool logged = false;
        if (!logged) Log("HARNESS unsupported capture format %d", int(d.Format));
        logged = true;
        *out = Grid{};
    }
    return ok;
}

}  // namespace hrvr::harness
