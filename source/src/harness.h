#pragma once
// In-process image capture for the flat harness (vr-modding-playbook A5.1: capture from inside the
// engine, never from the desktop). Render thread only; blocking readback, diagnostics only.
#include <string>

#include "harness_stats.h"

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;

namespace hrvr::harness {

// Reads `tex` back into a luminance grid of about 480 x 270 cells. When `bmpPath` is not empty a
// quarter-size 24-bit BMP of the image is saved as well. False for an unsupported format.
bool Capture(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, Grid* out,
             const std::wstring& bmpPath = L"");

}  // namespace hrvr::harness
