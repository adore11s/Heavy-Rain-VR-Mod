#pragma once
// 1.7: the ARI glasses copy (ari_copy.h) drawn by the mod into an eye image (xr.cpp, after the game's picture is in the
// eye's swapchain image, before it is released): own shaders, own depth buffer, own device-context state (the game's
// state is untouched), two passes - the dark plastic opaque, then the lenses blended. Not occluded by the scene: it
// lies on top of the game's picture, in the hand nearest the eye.
#include <d3d11.h>

#include <string>

#include "posemath.h"

namespace hrvr::aricopy {

// v1.3: the glasses mesh comes from the player's own archives (ari_mesh_load.h), never from the mod's files.
// StartLoad reads <game folder>\Resources on a worker thread, once (logged); DrawCopy draws nothing until it is done.
void StartLoad();
// Reads it now from `resources` (the previews); true when the mesh is ready.
bool LoadNow(const std::wstring& resources);

// eye / copy: matrices in the same space (the projection layer's), row vectors; tangents of the image's frustum
// (OpenXR angles: left / down negative). False when the drawing could not be made (logged once).
bool DrawCopy(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv, unsigned width, unsigned height, const pose::Mat4& eye,
              float tanLeft, float tanRight, float tanUp, float tanDown, const pose::Mat4& copy);

}  // namespace hrvr::aricopy
