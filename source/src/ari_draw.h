#pragma once
// 1.6: the first-time ARI glasses hint pictures (owner 2026-11-24: "an arrow or like a line that points to chest and
// just little above the line it says 'Grab AR Glasses' and then as you're holding the glasses ... 'Put on AR
// Glasses'"). Direct2D / DirectWrite into a premultiplied bitmap (transparent background), the built-in Montserrat,
// shared by the mod (ari.cpp) and tests/ari_hint_preview.cpp (pictures checked before the headset).
#include <cstdint>

namespace hrvr::ari {
constexpr int kHW = 640, kHH = 480;           // pixels of one hint picture
constexpr float kHintWm = 0.32f, kHintHm = 0.24f;  // its size in the headset (metres)
constexpr int kHintPictures = 4;
// picture 0: "Grab AR Glasses" above a line ending in an arrow pointing DOWN (bottom edge = the arrow tip) - to the chest
// picture 1: an arrow pointing UP (top edge = the tip) above "Put on AR Glasses" - from the holding hand
// picture 2: an arrow pointing UP above "Grab AR Glasses" - to the face (1.6.2, taking off)
// picture 3: "Take off AR Glasses" above an arrow pointing DOWN - from the holding hand to the chest
// 0xAARRGGBB premultiplied (B8G8R8A8 in memory); nullptr when Direct2D is unavailable.
const uint32_t* HintPixels(int picture);
}  // namespace hrvr::ari
