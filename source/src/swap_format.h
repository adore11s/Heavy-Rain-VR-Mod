// 1.0.1: the XR swapchain colour format and how the eye pictures are written into it.
//
// The game's pixels are sRGB-encoded bytes (made for a monitor). An OpenXR runtime reads an *_SRGB swapchain as
// sRGB and any other format as LINEAR light. So:
//   - sRGB swapchain, view with its UNORM twin (D3D11 swapchain images are typeless): copy the bytes as they are;
//   - sRGB swapchain whose image refuses the UNORM view: write through the sRGB view, shader linearises first;
//   - linear swapchain (no sRGB format offered): the shader must linearise, or the runtime encodes the bytes a
//     second time - the classic "washed out / greyish" double gamma (playbook ch09).
// Before 1.0.1 only R8G8B8A8_UNORM_SRGB was treated as sRGB: a runtime offering B8G8R8A8_UNORM_SRGB but not the
// RGBA one, or no sRGB at all, got raw sRGB bytes in a linear swapchain.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace hrvr::swapfmt {

// DXGI_FORMAT values (dxgiformat.h), kept numeric so the header has no Windows dependency.
constexpr int64_t kRGBA8 = 28, kRGBA8Srgb = 29, kBGRA8 = 87, kBGRA8Srgb = 91, kRGBA16F = 10, kRGB10A2 = 24;

struct Choice {
    int64_t format = kRGBA8;   // the swapchain format asked for
    int64_t rawView = kRGBA8;  // UNORM view that writes the bytes unchanged (sRGB formats only)
    bool srgb = false;         // the runtime decodes the swapchain as sRGB
};

inline bool IsSrgb(int64_t f) { return f == kRGBA8Srgb || f == kBGRA8Srgb; }
inline int64_t RawView(int64_t f) { return f == kRGBA8Srgb ? kRGBA8 : f == kBGRA8Srgb ? kBGRA8 : f; }

// Preference: 8-bit sRGB (RGBA, then BGRA); else a linear format, widest first so the linearised dark tones keep
// their steps (16-bit float, 10-bit, then 8-bit); else the runtime's first.
inline Choice Choose(const int64_t* offered, size_t n) {
    static const int64_t order[] = {kRGBA8Srgb, kBGRA8Srgb, kRGBA16F, kRGB10A2, kRGBA8, kBGRA8};
    Choice c;
    for (int64_t want : order)
        for (size_t i = 0; i < n; ++i)
            if (offered[i] == want) {
                c.format = want;
                c.rawView = RawView(want);
                c.srgb = IsSrgb(want);
                return c;
            }
    if (n) {
        c.format = c.rawView = offered[0];
        c.srgb = false;
    }
    return c;
}

// Whether the blit shaders convert sRGB bytes to linear before writing: always for a linear swapchain; for an sRGB
// one only when the raw UNORM view could not be made.
inline bool Linearize(const Choice& c, bool rawViewMade) { return !c.srgb || !rawViewMade; }

// The exact sRGB decode (IEC 61966-2-1), the same as the HLSL in xr.cpp - not pow(2.2), which crushes dark tones.
inline float SrgbToLinear(float v) {
    return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
}

}  // namespace hrvr::swapfmt
